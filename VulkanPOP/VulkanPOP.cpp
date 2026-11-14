/* Shared Use License: This file is owned by Derivative Inc. (Derivative)
* and can only be used, and/or modified for use, in conjunction with
* Derivative's TouchDesigner software, and only if you are a licensee who has
* accepted Derivative's TouchDesigner license or assignment agreement
* (which also govern the use of this file). You may share or redistribute
* a modified version of this file provided the following conditions are met:
*
* 1. The shared file or redistribution must retain the information set out
* above and this list of conditions.
* 2. Derivative's name (Derivative Inc.) or its trademarks may not be used
* to endorse or promote products derived from this file without specific
* prior written permission from Derivative.
*/

#include "VulkanPOP.h"

#include <assert.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>

// The compute shaders, compiled from shaders/*.comp to SPIR-V at build time
// (see CMakeLists.txt). Each header defines a uint32_t array.
#include "TransformPosition.spv.h"
#include "MakeCube.spv.h"

// These functions are basic C function, which the DLL loader can find
// much easier than finding a C++ Class.
// The DLLEXPORT prefix is needed so the compile exports these functions from the .dll
// you are creating
extern "C"
{

DLLEXPORT
void
FillPOPPluginInfo(POP_PluginInfo *info)
{
	// This must always be set to this constant
	if (!info->setAPIVersion(POPCPlusPlusAPIVersion))
		return;

	// GPU buffers are exchanged as VkBuffers, and the GPU work is recorded into
	// command buffers TouchDesigner provides (see VulkanPOPRecorder below).
	info->deviceAPI = OP_DeviceAPI::Vulkan;

	// The opType is the unique name for this POP. It must start with a 
	// capital A-Z character, and all the following characters must lower case
	// or numbers (a-z, 0-9)
	info->customOPInfo.opType->setString("Vulkansample");

	// The opLabel is the text that will show up in the OP Create Dialog
	info->customOPInfo.opLabel->setString("Vulkan Sample");

	// Will be turned into a 3 letter icon on the nodes
	info->customOPInfo.opIcon->setString("VLK");

	// Information about the author of this OP
	info->customOPInfo.authorName->setString("Author Name");
	info->customOPInfo.authorEmail->setString("email@email.com");

	// With an input, the input is copied to new buffers with P transformed.
	// With no input, a cube is generated. The Scale and Shift Z parameters
	// transform P in both cases.
	info->customOPInfo.minInputs = 0;
	info->customOPInfo.maxInputs = 1;
}

DLLEXPORT
POP_CPlusPlusBase*
CreatePOPInstance(const OP_NodeInfo* info, POP_Context *context)
{
	// Return a new instance of your class every time this is called.
	// It will be called once per POP that is using the .dll
	return new VulkanPOP(info, context);
}

DLLEXPORT
void
DestroyPOPInstance(POP_CPlusPlusBase* instance, POP_Context *context)
{
	// Delete the instance here, this will be called when
	// Touch is shutting down, when the POP using that instance is deleted, or
	// if the POP loads a different DLL
	delete (VulkanPOP*)instance;
}

};

// The generated cube: 8 shared corner points and 6 quads.
static constexpr uint32_t kCubePoints = 8;
static constexpr uint32_t kCubeQuads = 6;

// Corner i of the unit cube is at (x, y, z) = (bit0, bit1, bit2) mapped to -0.5 / +0.5, so
//   0(-,-,-) 1(+,-,-) 2(-,+,-) 3(+,+,-) 4(-,-,+) 5(+,-,+) 6(-,+,+) 7(+,+,+)
// Each quad is counter-clockwise when viewed from outside the cube.
static const uint32_t kCubeIndices[kCubeQuads * 4] =
{
	0, 2, 3, 1,		// -Z
	4, 5, 7, 6,		// +Z
	0, 4, 6, 2,		// -X
	1, 3, 7, 5,		// +X
	0, 1, 5, 4,		// -Y
	2, 6, 7, 3,		// +Y
};

// Push constants for both shaders. Must match the Params block in
// shaders/TransformPosition.comp and shaders/MakeCube.comp (20 bytes, no padding).
struct PositionParams
{
	uint32_t	maxPoints;		// size of the buffers, in points
	float		scale;			// applied to x, y and z
	float		shift;			// added to z after scaling
	uint32_t	numPointsOnGPU;	// 1: the shader reads the point count from the point info buffer
	uint32_t	numPoints;		// the point count, when it is known on the CPU
};
static_assert(sizeof(PositionParams) == 20, "PositionParams must match the shaders' push constants");

// Local work group sizes, as declared in the shaders.
static constexpr uint32_t kTransformGroupSize = 64;
static constexpr uint32_t kCubeGroupSize = 8;

// How the input buffers will be used by our commands. TouchDesigner uses this to
// put the right barrier before them: the copies read them in the transfer stage,
// the TransformPosition shader reads P and the point info in the compute stage.
static constexpr VkPipelineStageFlags2 kInputStages = VK_PIPELINE_STAGE_2_TRANSFER_BIT |
													  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
static constexpr VkAccessFlags2 kInputAccess = VK_ACCESS_2_TRANSFER_READ_BIT |
											   VK_ACCESS_2_SHADER_STORAGE_READ_BIT;

// ---------------------------------------------------------------------------------
// Vulkan resources
// ---------------------------------------------------------------------------------

// Everything one VulkanPOP needs to record its GPU work, created once: the
// dispatcher (all Vulkan calls go through TouchDesigner's vkGetInstanceProcAddr),
// and a compute pipeline per shader.
//
// Descriptor sets have to stay valid until the GPU has run the command buffer
// they're used in, so each recording gets its own descriptor pool, which is
// destroyed once its command buffer has completed (see recycle()).
class VulkanPOPResources
{
public:
	~VulkanPOPResources()
	{
		// Normally everything has completed by now (the POP waits for it in its
		// destructor). If a command buffer is still running, destroying the pools
		// and pipelines it uses would be invalid, so they're leaked instead.
		if (!recycle(0))
		{
			fprintf(stderr, "VulkanPOP: GPU work still running at shutdown, leaking its Vulkan objects\n");
			return;
		}
		if (!device)
			return;
		device.destroyPipeline(transformPipeline, nullptr, dl);
		device.destroyPipeline(cubePipeline, nullptr, dl);
		device.destroyPipelineLayout(transformLayout, nullptr, dl);
		device.destroyPipelineLayout(cubeLayout, nullptr, dl);
		device.destroyDescriptorSetLayout(transformSetLayout, nullptr, dl);
		device.destroyDescriptorSetLayout(cubeSetLayout, nullptr, dl);
	}

	// Sets up the dispatcher and builds the pipelines. On failure, err says why.
	bool
	init(const OP_VulkanInfo* info, std::string& err)
	{
		if (!info || !info->device || !info->getInstanceProcAddr)
		{
			err = "TouchDesigner did not provide a Vulkan device";
			return false;
		}
		dl.init(info->getInstanceProcAddr);
		dl.init(vk::Instance(info->instance));
		dl.init(vk::Device(info->device));
		device = vk::Device(info->device);

		// Set layouts: TransformPosition reads src (0), writes dst (1) and reads the
		// point info (2); MakeCube writes P (0). All storage buffers.
		auto makeSetLayout = [&](uint32_t numBindings, vk::DescriptorSetLayout& layout) {
			vk::DescriptorSetLayoutBinding bindings[3];
			for (uint32_t i = 0; i < numBindings; i++)
				bindings[i] = vk::DescriptorSetLayoutBinding(i, vk::DescriptorType::eStorageBuffer, 1,
															 vk::ShaderStageFlagBits::eCompute);
			vk::DescriptorSetLayoutCreateInfo ci({}, numBindings, bindings);
			auto r = device.createDescriptorSetLayout(ci, nullptr, dl);
			layout = r.value;
			return r.result == vk::Result::eSuccess;
		};
		if (!makeSetLayout(3, transformSetLayout) || !makeSetLayout(1, cubeSetLayout))
		{
			err = "vkCreateDescriptorSetLayout failed";
			return false;
		}

		const vk::PushConstantRange pushRange(vk::ShaderStageFlagBits::eCompute, 0, sizeof(PositionParams));
		auto makeLayout = [&](const vk::DescriptorSetLayout& setLayout, vk::PipelineLayout& layout) {
			vk::PipelineLayoutCreateInfo ci({}, 1, &setLayout, 1, &pushRange);
			auto r = device.createPipelineLayout(ci, nullptr, dl);
			layout = r.value;
			return r.result == vk::Result::eSuccess;
		};
		if (!makeLayout(transformSetLayout, transformLayout) || !makeLayout(cubeSetLayout, cubeLayout))
		{
			err = "vkCreatePipelineLayout failed";
			return false;
		}

		auto makePipeline = [&](const uint32_t* spv, size_t spvSize, vk::PipelineLayout layout,
								vk::Pipeline& pipeline) {
			vk::ShaderModuleCreateInfo smci({}, spvSize, spv);
			auto sm = device.createShaderModule(smci, nullptr, dl);
			if (sm.result != vk::Result::eSuccess)
				return false;
			vk::ComputePipelineCreateInfo ci({}, vk::PipelineShaderStageCreateInfo(
												 {}, vk::ShaderStageFlagBits::eCompute, sm.value, "main"),
											 layout);
			auto r = device.createComputePipeline(nullptr, ci, nullptr, dl);
			// The module is only needed to create the pipeline.
			device.destroyShaderModule(sm.value, nullptr, dl);
			pipeline = r.value;
			return r.result == vk::Result::eSuccess;
		};
		if (!makePipeline(kTransformPositionSpv, sizeof(kTransformPositionSpv), transformLayout, transformPipeline) ||
			!makePipeline(kMakeCubeSpv, sizeof(kMakeCubeSpv), cubeLayout, cubePipeline))
		{
			err = "creating the compute pipelines failed";
			return false;
		}
		return true;
	}

	// Called by a recorder once it has recorded commands that use 'pool'.
	void
	track(const OP_SmartRef<OP_VulkanCommandBuffer>& cmd, vk::DescriptorPool pool)
	{
		std::lock_guard<std::mutex> lock(myMutex);
		myInFlight.push_back({ cmd, pool });
	}

	// Destroys the descriptor pools of command buffers that have completed, waiting
	// up to timeoutNs for each. Returns true if nothing is left running.
	// Must not be called while TouchDesigner is recording or submitting the command
	// buffers (see OP_VulkanCommandBuffer::waitForCompletion()), so it's only called
	// from execute() and the destructors.
	bool
	recycle(uint64_t timeoutNs)
	{
		std::lock_guard<std::mutex> lock(myMutex);
		OP_VulkanWaitInfo wait;
		wait.timeout = timeoutNs;
		for (auto it = myInFlight.begin(); it != myInFlight.end(); )
		{
			if (it->cmd->waitForCompletion(&wait, nullptr))
			{
				device.destroyDescriptorPool(it->pool, nullptr, dl);
				it = myInFlight.erase(it);
			}
			else
			{
				++it;
			}
		}
		return myInFlight.empty();
	}

	vk::detail::DispatchLoaderDynamic	dl;
	vk::Device							device;
	vk::DescriptorSetLayout				transformSetLayout;
	vk::DescriptorSetLayout				cubeSetLayout;
	vk::PipelineLayout					transformLayout;
	vk::PipelineLayout					cubeLayout;
	vk::Pipeline						transformPipeline;
	vk::Pipeline						cubePipeline;

private:
	struct InFlight
	{
		OP_SmartRef<OP_VulkanCommandBuffer>	cmd;
		vk::DescriptorPool					pool;
	};
	std::mutex				myMutex;
	std::vector<InFlight>	myInFlight;
};

// ---------------------------------------------------------------------------------
// Command recording
// ---------------------------------------------------------------------------------

// Records one cook's GPU work. Created in execute() with everything it needs, and
// handed to TouchDesigner with POP_Output::addVulkanCommandRecorder(). TouchDesigner
// calls recordCommands() later, possibly on another thread, with a command buffer
// it has begun and will submit, and takes care of ordering against the work that
// produced the inputs and the work that uses the outputs. So there are no barriers
// or synchronization objects to manage here.
class VulkanPOPRecorder : public OP_RefCountImpl<POP_VulkanCommandRecorder>
{
public:
	VulkanPOPRecorder(std::shared_ptr<VulkanPOPResources> resources,
					  std::vector<VulkanPOP::GPUCopy>&& jobs,
					  const VulkanPOP::PositionTransform& xform) :
		myResources(std::move(resources)),
		myJobs(std::move(jobs)),
		myXform(xform)
	{
	}

	// Input mode: the transform of P needs the number of points. If the input's
	// point info is on the GPU, the shader reads it from there; otherwise numPoints
	// is the count read on the CPU.
	void
	setPointCount(uint32_t maxPoints, OP_SmartRef<POP_Buffer> gpuPointInfo, uint32_t numPoints)
	{
		myMaxPoints = maxPoints;
		myGPUPointInfo = std::move(gpuPointInfo);
		myNumPoints = numPoints;
	}

	// Cube mode: compute the cube's corners into this buffer.
	void
	setCube(OP_SmartRef<POP_Buffer> dst)
	{
		myCubeDst = std::move(dst);
	}

	virtual void
	recordCommands(const OP_VulkanCommandRecordInfo* info, void* reserved1) override
	{
		const VulkanPOPResources& res = *myResources;
		const vk::detail::DispatchLoaderDynamic& dl = res.dl;
		vk::CommandBuffer cmd(info->commandBuffer->getHandle(nullptr));

		// Native Vulkan copies for everything we pass through unchanged
		for (const VulkanPOP::GPUCopy& job : myJobs)
		{
			if (job.transformP)
				continue;
			const uint64_t size = std::min(job.src->info.size, job.dst->info.size);
			if (size == 0)
				continue;
			vk::BufferCopy region(0, 0, size);
			cmd.copyBuffer(vk::Buffer(job.src->getVkBuffer(nullptr)), vk::Buffer(job.dst->getVkBuffer(nullptr)),
						   1, &region, dl);
		}

		// The compute work: the position shader for P, or the cube. Each dispatch
		// needs a descriptor set; they all come from one pool for this recording.
		uint32_t numSets = myCubeDst ? 1 : 0;
		for (const VulkanPOP::GPUCopy& job : myJobs)
		{
			if (job.transformP)
				numSets++;
		}
		if (numSets == 0)
			return;

		vk::DescriptorPoolSize poolSize(vk::DescriptorType::eStorageBuffer, numSets * 3);
		vk::DescriptorPoolCreateInfo poolInfo({}, numSets, 1, &poolSize);
		auto pool = res.device.createDescriptorPool(poolInfo, nullptr, dl);
		if (pool.result != vk::Result::eSuccess)
		{
			// There's no way to report an error from here, so the outputs keep
			// whatever the buffers held. The next cook will try again.
			fprintf(stderr, "VulkanPOP: vkCreateDescriptorPool failed, the GPU work was skipped\n");
			return;
		}
		// The pool (and the sets in it) is destroyed once this command buffer has
		// completed, by VulkanPOPResources::recycle().
		myResources->track(info->commandBuffer, pool.value);

		auto allocSet = [&](vk::DescriptorSetLayout layout) {
			vk::DescriptorSetAllocateInfo ai(pool.value, 1, &layout);
			vk::DescriptorSet set;
			if (res.device.allocateDescriptorSets(&ai, &set, dl) != vk::Result::eSuccess)
				return vk::DescriptorSet();
			return set;
		};

		PositionParams params{};
		params.scale = myXform.scale;
		params.shift = myXform.shiftZ;

		// The position shader for P
		for (const VulkanPOP::GPUCopy& job : myJobs)
		{
			if (!job.transformP || myMaxPoints == 0)
				continue;
			vk::DescriptorSet set = allocSet(res.transformSetLayout);
			if (!set)
				continue;

			const vk::Buffer src(job.src->getVkBuffer(nullptr));
			const vk::Buffer dst(job.dst->getVkBuffer(nullptr));
			// Binding 2 must always hold a valid buffer. When the point count is
			// known on the CPU the shader doesn't read it, so src stands in.
			const vk::Buffer pointInfo = myGPUPointInfo ? vk::Buffer(myGPUPointInfo->getVkBuffer(nullptr)) : src;
			vk::DescriptorBufferInfo bufs[3] = {
				vk::DescriptorBufferInfo(src, 0, VK_WHOLE_SIZE),
				vk::DescriptorBufferInfo(dst, 0, VK_WHOLE_SIZE),
				vk::DescriptorBufferInfo(pointInfo, 0, VK_WHOLE_SIZE),
			};
			vk::WriteDescriptorSet writes[3];
			for (uint32_t i = 0; i < 3; i++)
				writes[i] = vk::WriteDescriptorSet(set, i, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &bufs[i]);
			res.device.updateDescriptorSets(3, writes, 0, nullptr, dl);

			params.maxPoints      = myMaxPoints;
			params.numPointsOnGPU = myGPUPointInfo ? 1 : 0;
			params.numPoints      = myNumPoints;

			cmd.bindPipeline(vk::PipelineBindPoint::eCompute, res.transformPipeline, dl);
			cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, res.transformLayout, 0, 1, &set, 0, nullptr, dl);
			cmd.pushConstants(res.transformLayout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(params), &params, dl);
			cmd.dispatch((myMaxPoints + kTransformGroupSize - 1) / kTransformGroupSize, 1, 1, dl);
		}

		// The cube
		if (myCubeDst)
		{
			vk::DescriptorSet set = allocSet(res.cubeSetLayout);
			if (set)
			{
				vk::DescriptorBufferInfo buf(vk::Buffer(myCubeDst->getVkBuffer(nullptr)), 0, VK_WHOLE_SIZE);
				vk::WriteDescriptorSet write(set, 0, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &buf);
				res.device.updateDescriptorSets(1, &write, 0, nullptr, dl);

				params.maxPoints = kCubePoints;
				cmd.bindPipeline(vk::PipelineBindPoint::eCompute, res.cubePipeline, dl);
				cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, res.cubeLayout, 0, 1, &set, 0, nullptr, dl);
				cmd.pushConstants(res.cubeLayout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(params), &params, dl);
				cmd.dispatch((kCubePoints + kCubeGroupSize - 1) / kCubeGroupSize, 1, 1, dl);
			}
		}
	}

private:
	// Keeps the pipelines alive until this recording is done with them.
	std::shared_ptr<VulkanPOPResources>	myResources;
	// The input references and our new output buffers, kept alive until recorded.
	std::vector<VulkanPOP::GPUCopy>		myJobs;
	VulkanPOP::PositionTransform		myXform;

	uint32_t							myMaxPoints = 0;
	OP_SmartRef<POP_Buffer>				myGPUPointInfo;
	uint32_t							myNumPoints = 0;

	OP_SmartRef<POP_Buffer>				myCubeDst;
};

// ---------------------------------------------------------------------------------
// VulkanPOP
// ---------------------------------------------------------------------------------

VulkanPOP::VulkanPOP(const OP_NodeInfo* info, POP_Context *context) :
	myNodeInfo(info), myExecuteCount(0),
	myContext(context),
	myError(nullptr)
{
}

VulkanPOP::~VulkanPOP()
{
	// Wait (a bounded time) for our GPU work, so its descriptor pools and the
	// pipelines can be destroyed. Safe here: we're not inside TouchDesigner's
	// recording of a command buffer.
	if (myResources)
		myResources->recycle(5ull * 1000 * 1000 * 1000);
}

void
VulkanPOP::getGeneralInfo(POP_GeneralInfo* ginfo, const OP_Inputs *inputs, void* reserved)
{
}

bool
VulkanPOP::ensureResources()
{
	OP_VulkanUtils* vulkanUtils = myContext->getVulkanUtils(nullptr);
	if (!vulkanUtils)
	{
		myError = "Vulkan is not available";
		return false;
	}
	if (myResources)
		return true;

	// Build once; don't retry a failed creation every cook
	if (myResourcesFailed)
	{
		myError = "Failed to create the Vulkan compute pipelines (see console)";
		return false;
	}

	auto resources = std::make_shared<VulkanPOPResources>();
	std::string err;
	if (!resources->init(vulkanUtils->getVulkanInfo(nullptr), err))
	{
		fprintf(stderr, "VulkanPOP: %s\n", err.c_str());
		myResourcesFailed = true;
		myError = "Failed to create the Vulkan compute pipelines (see console)";
		return false;
	}
	myResources = std::move(resources);
	return true;
}

// ---------------------------------------------------------------------------------
// Cooking
// ---------------------------------------------------------------------------------

// References to the input's attribute buffers. They're read-only: ReferenceInput gives
// us the input POP's own memory instead of a copy.
static VulkanPOP::AttributeList
getAllAttributes(POP_AttributeClass c, const OP_POPInput* input)
{
	VulkanPOP::AttributeList res;
	for (uint32_t i = 0; i < input->getNumAttributes(c); i++)
	{
		const POP_Attribute* attr = input->getAttribute(c, i, nullptr);

		POP_GetBufferInfo info;
		info.location      = POP_BufferLocation::Vulkan;
		info.mode          = POP_GetBufferMode::ReferenceInput;
		info.dstStageMask  = kInputStages;
		info.dstAccessMask = kInputAccess;
		OP_SmartRef<POP_Buffer> buf = attr->getBuffer(info, nullptr);
		res.emplace_back(std::string(attr->info.name), buf);
	}
	return res;
}

OP_SmartRef<POP_Buffer>
VulkanPOP::makeOutputBuffer(const OP_SmartRef<POP_Buffer>& src, std::vector<GPUCopy>& jobs,
							bool& ok, bool transformP)
{
	if (!src)
		return {};

	// CPU buffers are always returned as new copies, so they're already ours to output.
	if (src->info.location != POP_BufferLocation::Vulkan)
		return src;

	POP_BufferInfo createInfo = src->info;
	createInfo.location = POP_BufferLocation::Vulkan;
	OP_SmartRef<POP_Buffer> dst = myContext->createBuffer(createInfo, nullptr);
	if (!dst)
	{
		myError = "Failed to create output buffers";
		ok = false;
		return {};
	}

	if (!src->getVkBuffer(nullptr) || !dst->getVkBuffer(nullptr))
	{
		myError = "A buffer is not available as a Vulkan buffer";
		ok = false;
		return {};
	}

	GPUCopy job;
	job.src        = src;
	job.dst        = dst;
	job.transformP = transformP;
	jobs.push_back(std::move(job));
	return dst;
}

void
VulkanPOP::execute(POP_Output* output, const OP_Inputs* inputs, void* reserved)
{
	myError = nullptr;
	myExecuteCount++;

	const OP_POPInput* input = inputs->getNumInputs() > 0 ? inputs->getInputPOP(0) : nullptr;

	if (!ensureResources())
		return;

	// Free the descriptor pools of earlier cooks whose GPU work has finished.
	myResources->recycle(0);

	PositionTransform xform;
	xform.scale  = (float)inputs->getParDouble("Scale");
	xform.shiftZ = (float)inputs->getParDouble("Shiftz");

	if (input)
		processInput(output, input, xform);
	else
		generateCube(output, xform);
}

void
VulkanPOP::processInput(POP_Output* output, const OP_POPInput* input, const PositionTransform& xform)
{
	// ---- References to the input's buffers (read-only) -----------------------------
	// All inputs must be requested, and all output buffers created, before the
	// command recorder is added below.
	AttributeList pointAttrs = getAllAttributes(POP_AttributeClass::Point, input);
	AttributeList vertAttrs  = getAllAttributes(POP_AttributeClass::Vertex, input);
	AttributeList primAttrs  = getAllAttributes(POP_AttributeClass::Primitive, input);

	// Some info buffers always live on the CPU, so ask for CPUOrVulkan. ReferenceInput
	// only applies to the ones on the GPU; CPU ones are always new copies.
	POP_GetBufferInfo ginfo;
	ginfo.location      = POP_BufferLocation::CPUOrVulkan;
	ginfo.mode          = POP_GetBufferMode::ReferenceInput;
	ginfo.dstStageMask  = kInputStages;
	ginfo.dstAccessMask = kInputAccess;
	POP_InfoBuffers inInfo;
	input->getAllInfoBuffers(&inInfo, ginfo, nullptr);

	POP_GetBufferInfo indexGInfo;
	indexGInfo.location      = POP_BufferLocation::Vulkan;
	indexGInfo.mode          = POP_GetBufferMode::ReferenceInput;
	indexGInfo.dstStageMask  = kInputStages;
	indexGInfo.dstAccessMask = kInputAccess;
	OP_SmartRef<POP_Buffer> inIndex = input->getIndexBuffer(nullptr)->getBuffer(indexGInfo, nullptr);

	// The position shader works on 3 floats per point, so P must be exactly that.
	// Any other layout is passed through with a plain copy.
	const POP_Attribute* posAttr = input->getAttribute(POP_AttributeClass::Point, "P", nullptr);
	const bool transformP = posAttr &&
						posAttr->info.type == POP_AttributeType::Float &&
						posAttr->info.numComponents == 3 &&
						posAttr->info.numColumns == 1 &&
						posAttr->info.arraySize == 0 &&
						inInfo.maxInfo.points > 0;

	// ---- Create every output buffer --------------------------------------------------
	// Each output is a new buffer from createBuffer(), filled on the GPU from the
	// corresponding input reference. P gets the position shader (scale + shift),
	// everything else a buffer copy.
	std::vector<GPUCopy> jobs;
	bool ok = true;

	auto makeOutputs = [&](const AttributeList& in, POP_AttributeClass c) {
		AttributeList out;
		for (const auto& [name, buf] : in)
		{
			const bool isP = transformP && c == POP_AttributeClass::Point && name == "P";
			out.emplace_back(name, makeOutputBuffer(buf, jobs, ok, isP));
		}
		return out;
	};
	AttributeList outPoint = makeOutputs(pointAttrs, POP_AttributeClass::Point);
	AttributeList outVert  = makeOutputs(vertAttrs, POP_AttributeClass::Vertex);
	AttributeList outPrim  = makeOutputs(primAttrs, POP_AttributeClass::Primitive);

	OP_SmartRef<POP_Buffer> outIndex = makeOutputBuffer(inIndex, jobs, ok);

	POP_InfoBuffers outInfo;
	outInfo.maxInfo               = inInfo.maxInfo;
	outInfo.pointInfo             = makeOutputBuffer(inInfo.pointInfo, jobs, ok);
	outInfo.topoInfo              = makeOutputBuffer(inInfo.topoInfo, jobs, ok);
	outInfo.lineStripsInfo        = makeOutputBuffer(inInfo.lineStripsInfo, jobs, ok);
	outInfo.lineStripsPrimIndices = makeOutputBuffer(inInfo.lineStripsPrimIndices, jobs, ok);
	outInfo.gridInfo              = makeOutputBuffer(inInfo.gridInfo, jobs, ok);   // always CPU

	// No recorder has been added yet, so returning here leaves no GPU work behind.
	if (!ok)
		return;   // myError was set by makeOutputBuffer()

	// ---- Vulkan work ---------------------------------------------------------------
	// The point info may be on the CPU or on the GPU (we asked for CPUOrVulkan). On
	// the GPU the shader reads the count itself; on the CPU we read it here.
	OP_SmartRef<POP_Buffer> gpuPointInfo;
	uint32_t numPoints = inInfo.maxInfo.points;
	if (inInfo.pointInfo)
	{
		if (inInfo.pointInfo->info.location == POP_BufferLocation::Vulkan)
		{
			gpuPointInfo = inInfo.pointInfo;
		}
		else
		{
			const POP_PointInfo* pi = (const POP_PointInfo*)inInfo.pointInfo->getData(nullptr);
			if (pi)
				numPoints = pi->numPoints;
		}
	}

	// Every job reads only the input references and writes its own new buffer, so the
	// copies and dispatches don't depend on each other.
	if (!jobs.empty())
	{
		auto recorder = new VulkanPOPRecorder(myResources, std::move(jobs), xform);
		recorder->setPointCount(inInfo.maxInfo.points, std::move(gpuPointInfo), numPoints);
		output->addVulkanCommandRecorder(OP_SmartRef<POP_VulkanCommandRecorder>(recorder));
	}

	// ---- Assign the outputs --------------------------------------------------------
	// Only our new buffers go to the output, never the input references. Ordering
	// against the GPU work is handled by TouchDesigner.
	POP_SetBufferInfo sinfo;
	sinfo.srcStageMask = VkPipelineStageFlags2(vk::PipelineStageFlagBits2::eComputeShader);
	sinfo.srcAccessMask = VkAccessFlags2(vk::AccessFlagBits2::eShaderWrite);

	auto setOutputs = [&](AttributeList& out, POP_AttributeClass c) {
		for (auto& [name, buf] : out)
		{
			const POP_Attribute* attr = input->getAttribute(c, name.c_str(), nullptr);
			if (attr && buf)
				output->setAttribute(&buf, attr->info, sinfo, nullptr);
		}
	};
	setOutputs(outPoint, POP_AttributeClass::Point);
	setOutputs(outVert, POP_AttributeClass::Vertex);
	setOutputs(outPrim, POP_AttributeClass::Primitive);

	if (outIndex)
		output->setIndexBuffer(&outIndex, input->getIndexBuffer(nullptr)->info, sinfo, nullptr);

	// These are set on the CPU
	sinfo.srcStageMask = 0;
	sinfo.srcAccessMask = 0;
	output->setInfoBuffers(&outInfo, sinfo, nullptr);
}

void
VulkanPOP::generateCube(POP_Output* output, const PositionTransform& xform)
{
	// ---- Create every output buffer before adding the command recorder -------------
	// P is computed on the GPU, so it's a Vulkan buffer.
	POP_BufferInfo posInfo;
	posInfo.size     = kCubePoints * 3 * sizeof(float);
	posInfo.usage    = POP_BufferUsage::Attribute;
	posInfo.location = POP_BufferLocation::Vulkan;
	OP_SmartRef<POP_Buffer> posBuf = myContext->createBuffer(posInfo, nullptr);

	// The indices and info buffers are small and fixed, so they're filled on the CPU.
	// Mixing CPU and GPU buffers in one output is fine.
	POP_BufferInfo indexInfo;
	indexInfo.size     = sizeof(kCubeIndices);
	indexInfo.usage    = POP_BufferUsage::IndexBuffer;
	indexInfo.location = POP_BufferLocation::CPU;
	OP_SmartRef<POP_Buffer> indexBuf = myContext->createBuffer(indexInfo, nullptr);

	POP_BufferInfo pointInfoInfo;
	pointInfoInfo.size     = sizeof(POP_PointInfo);
	pointInfoInfo.usage    = POP_BufferUsage::PointInfoBuffer;
	pointInfoInfo.location = POP_BufferLocation::CPU;
	OP_SmartRef<POP_Buffer> pointInfoBuf = myContext->createBuffer(pointInfoInfo, nullptr);

	POP_BufferInfo topoInfoInfo;
	topoInfoInfo.size     = sizeof(POP_TopologyInfo);
	topoInfoInfo.usage    = POP_BufferUsage::TopologyInfoBuffer;
	topoInfoInfo.location = POP_BufferLocation::CPU;
	OP_SmartRef<POP_Buffer> topoInfoBuf = myContext->createBuffer(topoInfoInfo, nullptr);

	// No recorder has been added yet, so returning here leaves no GPU work behind.
	if (!posBuf || !indexBuf || !pointInfoBuf || !topoInfoBuf || !posBuf->getVkBuffer(nullptr))
	{
		myError = "Failed to create output buffers";
		return;
	}

	// Fill the CPU buffers (sequentially, as POP_BufferMode::SequentialWrite expects).
	memcpy(indexBuf->getData(nullptr), kCubeIndices, sizeof(kCubeIndices));

	POP_PointInfo* pointInfo = new (pointInfoBuf->getData(nullptr)) POP_PointInfo();
	pointInfo->numPoints = kCubePoints;

	POP_TopologyInfo* topoInfo = new (topoInfoBuf->getData(nullptr)) POP_TopologyInfo();
	topoInfo->quadsStartIndex = 0;
	topoInfo->quadsCount      = kCubeQuads;

	// ---- Vulkan work ---------------------------------------------------------------
	auto recorder = new VulkanPOPRecorder(myResources, {}, xform);
	recorder->setCube(posBuf);
	output->addVulkanCommandRecorder(OP_SmartRef<POP_VulkanCommandRecorder>(recorder));

	// ---- Assign the outputs --------------------------------------------------------
	POP_SetBufferInfo sinfo;

	POP_AttributeInfo posAttr;
	posAttr.name          = "P";
	posAttr.numComponents = 3;
	posAttr.type          = POP_AttributeType::Float;
	posAttr.attribClass   = POP_AttributeClass::Point;
	sinfo.srcStageMask = VkPipelineStageFlags2(vk::PipelineStageFlagBits2::eComputeShader);
	sinfo.srcAccessMask = VkAccessFlags2(vk::AccessFlagBits2::eShaderWrite);
	output->setAttribute(&posBuf, posAttr, sinfo, nullptr);

	// The others are set from the CPU.
	sinfo.srcStageMask = 0;
	sinfo.srcAccessMask = 0;

	POP_IndexBufferInfo indexBufInfo;
	output->setIndexBuffer(&indexBuf, indexBufInfo, sinfo, nullptr);

	// Both info buffers are on the CPU, so maxInfo isn't needed.
	POP_InfoBuffers infoBufs;
	infoBufs.pointInfo = std::move(pointInfoBuf);
	infoBufs.topoInfo  = std::move(topoInfoBuf);
	output->setInfoBuffers(&infoBufs, sinfo, nullptr);
}

// ---------------------------------------------------------------------------------
// Info / parameters
// ---------------------------------------------------------------------------------

int32_t
VulkanPOP::getNumInfoCHOPChans(void* reserved)
{
	// We return the number of channel we want to output to any Info CHOP
	// connected to the POP. In this example we are just going to send one channel.
	return 1;
}

void
VulkanPOP::getInfoCHOPChan(int32_t index,
						OP_InfoCHOPChan* chan,
						void* reserved)
{
	// This function will be called once for each channel we said we'd want to return
	// In this example it'll only be called once.

	if (index == 0)
	{
		chan->name->setString("executeCount");
		chan->value = (float)myExecuteCount;
	}
}

bool		
VulkanPOP::getInfoDATSize(OP_InfoDATSize* infoSize, void* reserved)
{
	infoSize->rows = 1;
	infoSize->cols = 2;
	// Setting this to false means we'll be assigning values to the table
	// one row at a time. True means we'll do it one column at a time.
	infoSize->byColumn = false;
	return true;
}

void
VulkanPOP::getInfoDATEntries(int32_t index,
										int32_t nEntries,
										OP_InfoDATEntries* entries,
										void* reserved)
{
	char tempBuffer[4096];

	if (index == 0)
	{
		// Set the value for the first column
		snprintf(tempBuffer, sizeof(tempBuffer), "%s", "executeCount");
		entries->values[0]->setString(tempBuffer);

		// Set the value for the second column
		snprintf(tempBuffer, sizeof(tempBuffer), "%d", myExecuteCount);
		entries->values[1]->setString(tempBuffer);
	}
}

void
VulkanPOP::getErrorString(OP_String *error, void* reserved)
{
	if (myError)
		error->setString(myError);
}

void
VulkanPOP::setupParameters(OP_ParameterManager* manager, void* reserved)
{
	// Scale and Shift Z transform P: the input's points when an input is connected,
	// or the generated unit cube when there's no input. P is scaled first, then shifted.

	// Uniform scale applied to x, y and z
	{
		OP_NumericParameter np;

		np.name = "Scale";
		np.label = "Scale";
		np.defaultValues[0] = 1.0;
		np.minSliders[0] = 0.0;
		np.maxSliders[0] = 10.0;
		np.minValues[0] = 0.0;
		np.clampMins[0] = true;

		OP_ParAppendResult res = manager->appendFloat(np);
		assert(res == OP_ParAppendResult::Success);
	}

	// Distance added to z, after scaling
	{
		OP_NumericParameter np;

		np.name = "Shiftz";
		np.label = "Shift Z";
		np.defaultValues[0] = 1.0;
		np.minSliders[0] = -10.0;
		np.maxSliders[0] = 10.0;

		OP_ParAppendResult res = manager->appendFloat(np);
		assert(res == OP_ParAppendResult::Success);
	}
}

void
VulkanPOP::pulsePressed(const char* name, void* reserved)
{
}
