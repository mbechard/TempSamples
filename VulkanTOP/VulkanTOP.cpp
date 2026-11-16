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

#include "VulkanTOP.h"

#include <assert.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <tuple>
#include <vector>

// The shaders, compiled to SPIR-V at build time (see CMakeLists.txt). Each header
// defines a uint32_t array.
#include "FullScreenVertSpv.h"
#include "FillFragSpv.h"
#include "MultiplyFragSpv.h"

// These functions are basic C function, which the DLL loader can find
// much easier than finding a C++ Class.
// The DLLEXPORT prefix is needed so the compile exports these functions from the .dll
// you are creating
extern "C"
{

DLLEXPORT
void
FillTOPPluginInfo(TOP_PluginInfo *info)
{
	// This must always be set to this constant
	if (!info->setAPIVersion(TOPCPlusPlusAPIVersion))
		return;

	// Textures are given as VkImages, and the GPU work is recorded into command
	// buffers TouchDesigner provides (see VulkanTOPRecorder below).
	info->executeMode = TOP_ExecuteMode::Vulkan;

	// The opType is the unique name for this TOP. It must start with a
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

	// This TOP works with 0 or 1 inputs connected
	info->customOPInfo.minInputs = 0;
	info->customOPInfo.maxInputs = 1;

	// Custom website URL that the Operator Help can point to
	info->customOPInfo.opHelpURL->setString("yourwebsiteurl.com");
}

DLLEXPORT
TOP_CPlusPlusBase*
CreateTOPInstance(const OP_NodeInfo* info, TOP_Context* context)
{
	// Return a new instance of your class every time this is called.
	// It will be called once per TOP that is using the .dll
	return new VulkanTOP(info, context);
}

DLLEXPORT
void
DestroyTOPInstance(TOP_CPlusPlusBase* instance, TOP_Context *context)
{
	// Delete the instance here, this will be called when
	// Touch is shutting down, when the TOP using that instance is deleted, or
	// if the TOP loads a different DLL
	delete (VulkanTOP*)instance;
}

};

// Push constants for both fragment shaders. Must match FillParams in
// AnimatedColor.glsl (16 bytes, no padding).
struct FillParams
{
	float	step;
	float	brightness;
	float	outputWidth;
	float	outputHeight;
};
static_assert(sizeof(FillParams) == 16, "FillParams must match the shaders' push constants");

// How we use the input image: sampled in the fragment shader. TouchDesigner uses
// this to put the right barrier (and layout) before our commands.
static constexpr vk::PipelineStageFlags2 InputStages = vk::PipelineStageFlagBits2::eFragmentShader;
static constexpr vk::AccessFlags2 InputAccess = vk::AccessFlagBits2::eShaderSampledRead;

// How we write the output image: as a color attachment. TouchDesigner uses this to
// put the right barrier after our commands.
static constexpr vk::PipelineStageFlags2 OutputStages = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
static constexpr vk::AccessFlags2 OutputAccess = vk::AccessFlagBits2::eColorAttachmentWrite;

// ---------------------------------------------------------------------------------
// Vulkan resources
// ---------------------------------------------------------------------------------

// The render pass and pipelines for one kind of output image. Render passes and
// pipelines depend on the output's format and sample count, and the render pass
// also on the layout TouchDesigner wants the image left in.
struct VulkanTOPTargets
{
	vk::RenderPass	renderPass;
	vk::Pipeline	fillPipeline;		// no input: the animated colors
	vk::Pipeline	multiplyPipeline;	// input * animated colors
};

// Everything one VulkanTOP needs to record its GPU work: the dispatcher (all Vulkan
// calls go through TouchDesigner's vkGetInstanceProcAddr), the shaders, the
// sampler, and the targets for each output format seen so far.
//
// Each recording also creates objects that must stay alive until the GPU has run
// its command buffer (image views, a framebuffer, a descriptor pool). Those are
// destroyed once the command buffer has completed (see recycle()).
class VulkanTOPResources
{
public:
	// Per-recording objects, destroyed once their command buffer completes.
	struct PerRecording
	{
		vk::ImageView		outputView;
		vk::ImageView		inputView;
		vk::Framebuffer		framebuffer;
		vk::DescriptorPool	descriptorPool;
	};

	~VulkanTOPResources()
	{
		// Normally everything has completed by now (the TOP waits for it in its
		// destructor). If a command buffer is still running, destroying what it
		// uses would be invalid, so it's all leaked instead.
		if (!recycle(0))
		{
			fprintf(stderr, "VulkanTOP: GPU work still running at shutdown, leaking its Vulkan objects\n");
			return;
		}
		if (!device)
			return;
		for (auto& [key, t] : myTargets)
		{
			device.destroyPipeline(t.fillPipeline, nullptr, dl);
			device.destroyPipeline(t.multiplyPipeline, nullptr, dl);
			device.destroyRenderPass(t.renderPass, nullptr, dl);
		}
		device.destroyPipelineLayout(fillLayout, nullptr, dl);
		device.destroyPipelineLayout(multiplyLayout, nullptr, dl);
		device.destroyDescriptorSetLayout(multiplySetLayout, nullptr, dl);
		device.destroySampler(sampler, nullptr, dl);
		device.destroyShaderModule(myVertModule, nullptr, dl);
		device.destroyShaderModule(myFillModule, nullptr, dl);
		device.destroyShaderModule(myMultiplyModule, nullptr, dl);
	}

	// Sets up the dispatcher and creates everything that doesn't depend on the
	// output format. On failure, err says why.
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

		auto makeModule = [&](const uint32_t* spv, size_t size, vk::ShaderModule& module) {
			auto r = device.createShaderModule(vk::ShaderModuleCreateInfo({}, size, spv), nullptr, dl);
			module = r.value;
			return r.result == vk::Result::eSuccess;
		};
		if (!makeModule(FullScreenVertSpv, sizeof(FullScreenVertSpv), myVertModule) ||
			!makeModule(FillFragSpv, sizeof(FillFragSpv), myFillModule) ||
			!makeModule(MultiplyFragSpv, sizeof(MultiplyFragSpv), myMultiplyModule))
		{
			err = "vkCreateShaderModule failed";
			return false;
		}

		// Normalized coordinates and linear filtering, so the input can differ in
		// size from the output; clamped at the edges. Like the Metal sampler.
		vk::SamplerCreateInfo sci;
		sci.magFilter    = vk::Filter::eLinear;
		sci.minFilter    = vk::Filter::eLinear;
		sci.mipmapMode   = vk::SamplerMipmapMode::eNearest;
		sci.addressModeU = vk::SamplerAddressMode::eClampToEdge;
		sci.addressModeV = vk::SamplerAddressMode::eClampToEdge;
		sci.addressModeW = vk::SamplerAddressMode::eClampToEdge;
		auto s = device.createSampler(sci, nullptr, dl);
		sampler = s.value;
		if (s.result != vk::Result::eSuccess)
		{
			err = "vkCreateSampler failed";
			return false;
		}

		// The multiply shader samples the input (binding 0).
		vk::DescriptorSetLayoutBinding binding(0, vk::DescriptorType::eCombinedImageSampler, 1,
											   vk::ShaderStageFlagBits::eFragment);
		auto sl = device.createDescriptorSetLayout(vk::DescriptorSetLayoutCreateInfo({}, 1, &binding), nullptr, dl);
		multiplySetLayout = sl.value;
		if (sl.result != vk::Result::eSuccess)
		{
			err = "vkCreateDescriptorSetLayout failed";
			return false;
		}

		const vk::PushConstantRange pushRange(vk::ShaderStageFlagBits::eFragment, 0, sizeof(FillParams));
		auto fl = device.createPipelineLayout(vk::PipelineLayoutCreateInfo({}, 0, nullptr, 1, &pushRange), nullptr, dl);
		fillLayout = fl.value;
		auto ml = device.createPipelineLayout(vk::PipelineLayoutCreateInfo({}, 1, &multiplySetLayout, 1, &pushRange),
											  nullptr, dl);
		multiplyLayout = ml.value;
		if (fl.result != vk::Result::eSuccess || ml.result != vk::Result::eSuccess)
		{
			err = "vkCreatePipelineLayout failed";
			return false;
		}
		return true;
	}

	// The render pass and pipelines for this kind of output image, created the
	// first time. Only called from execute(), on the main thread.
	bool
	getTargets(vk::Format format, vk::SampleCountFlagBits samples, vk::ImageLayout finalLayout,
			   VulkanTOPTargets& targets, std::string& err)
	{
		const auto key = std::make_tuple(format, samples, finalLayout);
		auto it = myTargets.find(key);
		if (it != myTargets.end())
		{
			targets = it->second;
			return true;
		}

		// One color attachment, fully overwritten (so its old contents don't need
		// loading), left in the layout TouchDesigner wants it in at the end.
		vk::AttachmentDescription attachment({}, format, samples,
											 vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eStore,
											 vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eDontCare,
											 vk::ImageLayout::eUndefined, finalLayout);
		vk::AttachmentReference colorRef(0, vk::ImageLayout::eColorAttachmentOptimal);
		vk::SubpassDescription subpass({}, vk::PipelineBindPoint::eGraphics, 0, nullptr, 1, &colorRef);

		// TouchDesigner orders its own work on the image against the stage we said
		// we write it in (OutputStages), with barriers before and after our commands.
		// These dependencies chain onto those barriers through the same stage, and
		// cover the render pass' layout transitions at the start and the end.
		vk::SubpassDependency dependencies[2] = {
			vk::SubpassDependency(VK_SUBPASS_EXTERNAL, 0,
								  vk::PipelineStageFlagBits::eColorAttachmentOutput,
								  vk::PipelineStageFlagBits::eColorAttachmentOutput,
								  {}, vk::AccessFlagBits::eColorAttachmentWrite),
			vk::SubpassDependency(0, VK_SUBPASS_EXTERNAL,
								  vk::PipelineStageFlagBits::eColorAttachmentOutput,
								  vk::PipelineStageFlagBits::eColorAttachmentOutput,
								  vk::AccessFlagBits::eColorAttachmentWrite, {}),
		};
		auto rp = device.createRenderPass(vk::RenderPassCreateInfo({}, 1, &attachment, 1, &subpass, 2, dependencies),
										  nullptr, dl);
		if (rp.result != vk::Result::eSuccess)
		{
			err = "vkCreateRenderPass failed";
			return false;
		}
		VulkanTOPTargets t;
		t.renderPass = rp.value;

		auto makePipeline = [&](vk::ShaderModule frag, vk::PipelineLayout layout, vk::Pipeline& pipeline) {
			vk::PipelineShaderStageCreateInfo stages[2] = {
				vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eVertex, myVertModule, "main"),
				vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eFragment, frag, "main"),
			};
			vk::PipelineVertexInputStateCreateInfo vertexInput;
			vk::PipelineInputAssemblyStateCreateInfo inputAssembly({}, vk::PrimitiveTopology::eTriangleList);
			// The viewport and scissor are set per recording, to the output's size.
			vk::PipelineViewportStateCreateInfo viewport({}, 1, nullptr, 1, nullptr);
			vk::PipelineRasterizationStateCreateInfo raster;
			raster.cullMode  = vk::CullModeFlagBits::eNone;
			raster.lineWidth = 1.0f;
			vk::PipelineMultisampleStateCreateInfo multisample({}, samples);
			vk::PipelineColorBlendAttachmentState blend;
			blend.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
								   vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
			vk::PipelineColorBlendStateCreateInfo colorBlend({}, false, vk::LogicOp::eCopy, 1, &blend);
			vk::DynamicState dynamicStates[2] = { vk::DynamicState::eViewport, vk::DynamicState::eScissor };
			vk::PipelineDynamicStateCreateInfo dynamic({}, 2, dynamicStates);

			vk::GraphicsPipelineCreateInfo ci({}, 2, stages, &vertexInput, &inputAssembly, nullptr, &viewport,
											  &raster, &multisample, nullptr, &colorBlend, &dynamic, layout,
											  t.renderPass, 0);
			auto r = device.createGraphicsPipeline(nullptr, ci, nullptr, dl);
			pipeline = r.value;
			return r.result == vk::Result::eSuccess;
		};
		if (!makePipeline(myFillModule, fillLayout, t.fillPipeline) ||
			!makePipeline(myMultiplyModule, multiplyLayout, t.multiplyPipeline))
		{
			device.destroyPipeline(t.fillPipeline, nullptr, dl);
			device.destroyRenderPass(t.renderPass, nullptr, dl);
			err = "creating the graphics pipelines failed (is the output pixel format renderable?)";
			return false;
		}

		myTargets.emplace(key, t);
		targets = t;
		return true;
	}

	// Called by a recorder once it has recorded commands that use 'objects'.
	void
	track(const OP_SmartRef<OP_VulkanCommandBuffer>& cmd, const PerRecording& objects)
	{
		std::lock_guard<std::mutex> lock(myMutex);
		myInFlight.push_back({ cmd, objects });
	}

	// Destroys the per-recording objects of command buffers that have completed,
	// waiting up to timeoutNs for each. Returns true if nothing is left running.
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
				destroy(it->objects);
				it = myInFlight.erase(it);
			}
			else
			{
				++it;
			}
		}
		return myInFlight.empty();
	}

	// Destroys objects that were created for a recording (null handles are fine).
	void
	destroy(const PerRecording& o)
	{
		device.destroyFramebuffer(o.framebuffer, nullptr, dl);
		device.destroyImageView(o.outputView, nullptr, dl);
		device.destroyImageView(o.inputView, nullptr, dl);
		device.destroyDescriptorPool(o.descriptorPool, nullptr, dl);
	}

	vk::detail::DispatchLoaderDynamic	dl;
	vk::Device							device;
	vk::Sampler							sampler;
	vk::DescriptorSetLayout				multiplySetLayout;
	vk::PipelineLayout					fillLayout;
	vk::PipelineLayout					multiplyLayout;

private:
	vk::ShaderModule	myVertModule;
	vk::ShaderModule	myFillModule;
	vk::ShaderModule	myMultiplyModule;

	std::map<std::tuple<vk::Format, vk::SampleCountFlagBits, vk::ImageLayout>, VulkanTOPTargets>	myTargets;

	struct InFlight
	{
		OP_SmartRef<OP_VulkanCommandBuffer>	cmd;
		PerRecording						objects;
	};
	std::mutex				myMutex;
	std::vector<InFlight>	myInFlight;
};

// ---------------------------------------------------------------------------------
// Input channels
// ---------------------------------------------------------------------------------

static bool
isTwoChannelFormat(vk::Format f)
{
	switch (f)
	{
		case vk::Format::eR8G8Unorm:
		case vk::Format::eR8G8Snorm:
		case vk::Format::eR16G16Unorm:
		case vk::Format::eR16G16Snorm:
		case vk::Format::eR16G16Sfloat:
		case vk::Format::eR32G32Sfloat:
			return true;
		default:
			return false;
	}
}

// How to turn a sampled input texel into TD's RGBA meaning for its format. Vulkan
// samples a one-channel image as (r, 0, 0, 1) and a two-channel one as (r, g, 0, 1),
// which isn't what TD's mono / mono+alpha / alpha-only formats mean. The image view
// we create for the input applies this mapping, so the shader can use the texel
// as-is. Same rules as MetalTOP's setInputChannelModes().
static vk::ComponentMapping
inputChannelMapping(OP_PixelFormat fmt, vk::Format format)
{
	using S = vk::ComponentSwizzle;
	switch (fmt)
	{
		case OP_PixelFormat::Mono8Fixed:
		case OP_PixelFormat::Mono16Fixed:
		case OP_PixelFormat::Mono16Float:
		case OP_PixelFormat::Mono32Float:
			// Replicate red; alpha already samples as 1 for a one-channel format.
			return vk::ComponentMapping(S::eR, S::eR, S::eR, S::eA);

		case OP_PixelFormat::MonoA8Fixed:
		case OP_PixelFormat::MonoA16Fixed:
		case OP_PixelFormat::MonoA16Float:
		case OP_PixelFormat::MonoA32Float:
			return vk::ComponentMapping(S::eR, S::eR, S::eR, isTwoChannelFormat(format) ? S::eG : S::eA);

		case OP_PixelFormat::A8Fixed:
		case OP_PixelFormat::A16Fixed:
		case OP_PixelFormat::A16Float:
		case OP_PixelFormat::A32Float:
		{
			// Premultiplied white: rgb = alpha. Vulkan's only true alpha format is
			// A8 (VK_KHR_maintenance5); the others are stored in red.
			const S a = (format == vk::Format::eA8UnormKHR) ? S::eA : S::eR;
			return vk::ComponentMapping(a, a, a, a);
		}

		default:
			return vk::ComponentMapping();	// identity
	}
}

// ---------------------------------------------------------------------------------
// Command recording
// ---------------------------------------------------------------------------------

// Records one cook's GPU work. Created in execute() with everything it needs, and
// handed to TouchDesigner with TOP_Output::addVulkanCommandRecorder(). TouchDesigner
// calls recordCommands() later, possibly on another thread, with a command buffer
// it has begun and will submit, and orders it against the work that produced the
// input and the work that uses the output. The render pass leaves the output in
// the layout TouchDesigner gave it in.
class VulkanTOPRecorder : public OP_RefCountImpl<OP_VulkanCommandRecorder>
{
public:
	VulkanTOPRecorder(std::shared_ptr<VulkanTOPResources> resources, const VulkanTOPTargets& targets,
					  OP_SmartRef<OP_VulkanImage> output, OP_SmartRef<OP_VulkanImage> input,
					  vk::ComponentMapping inputMapping, const FillParams& params) :
		myResources(std::move(resources)),
		myTargets(targets),
		myOutput(std::move(output)),
		myInput(std::move(input)),
		myInputMapping(inputMapping),
		myParams(params)
	{
	}

	virtual void
	recordCommands(const OP_VulkanCommandRecordInfo* info, void* reserved1) override
	{
		VulkanTOPResources& res = *myResources;
		const vk::detail::DispatchLoaderDynamic& dl = res.dl;
		vk::CommandBuffer cmd(info->commandBuffer->getHandle(nullptr));

		VulkanTOPResources::PerRecording objects;
		const vk::Extent2D extent(myOutput->createInfo.extent.width, myOutput->createInfo.extent.height);

		// The output: a view of its first mip level and layer, as the framebuffer's
		// one attachment.
		//
		// A view inherits all of its image's usage flags unless they're narrowed
		// with vk::ImageViewUsageCreateInfo. TouchDesigner's images can also have
		// storage usage, which formats such as B8G8R8A8_SRGB don't support for
		// views, so each view is limited to how we use it: here, as a color attachment.
		const vk::ImageSubresourceRange outRange(myOutput->range);
		vk::StructureChain<vk::ImageViewCreateInfo, vk::ImageViewUsageCreateInfo> ovci(
			vk::ImageViewCreateInfo({}, vk::Image(myOutput->image), vk::ImageViewType::e2D,
									vk::Format(myOutput->createInfo.format), vk::ComponentMapping(),
									vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,
															  outRange.baseMipLevel, 1, outRange.baseArrayLayer, 1)),
			vk::ImageViewUsageCreateInfo(vk::ImageUsageFlagBits::eColorAttachment));
		auto ov = res.device.createImageView(ovci.get<vk::ImageViewCreateInfo>(), nullptr, dl);
		objects.outputView = ov.value;
		auto fb = res.device.createFramebuffer(vk::FramebufferCreateInfo({}, myTargets.renderPass, 1, &objects.outputView,
																		 extent.width, extent.height, 1),
											   nullptr, dl);
		objects.framebuffer = fb.value;

		// The input: a view that maps its channels to TD's RGBA meaning, sampled
		// through a descriptor set.
		vk::DescriptorSet set;
		bool ok = ov.result == vk::Result::eSuccess && fb.result == vk::Result::eSuccess;
		if (ok && myInput)
		{
			// Limited to sampling (see the output view above).
			const vk::ImageSubresourceRange inRange(myInput->range);
			vk::StructureChain<vk::ImageViewCreateInfo, vk::ImageViewUsageCreateInfo> ivci(
				vk::ImageViewCreateInfo({}, vk::Image(myInput->image), vk::ImageViewType::e2D,
										vk::Format(myInput->createInfo.format), myInputMapping,
										vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,
																  inRange.baseMipLevel, inRange.levelCount,
																  inRange.baseArrayLayer, 1)),
				vk::ImageViewUsageCreateInfo(vk::ImageUsageFlagBits::eSampled));
			auto iv = res.device.createImageView(ivci.get<vk::ImageViewCreateInfo>(), nullptr, dl);
			objects.inputView = iv.value;

			vk::DescriptorPoolSize poolSize(vk::DescriptorType::eCombinedImageSampler, 1);
			auto pool = res.device.createDescriptorPool(vk::DescriptorPoolCreateInfo({}, 1, 1, &poolSize), nullptr, dl);
			objects.descriptorPool = pool.value;

			ok = iv.result == vk::Result::eSuccess && pool.result == vk::Result::eSuccess;
			if (ok)
			{
				vk::DescriptorSetAllocateInfo ai(objects.descriptorPool, 1, &res.multiplySetLayout);
				ok = res.device.allocateDescriptorSets(&ai, &set, dl) == vk::Result::eSuccess;
			}
			if (ok)
			{
				// Sampled in the layout TouchDesigner has the input in.
				vk::DescriptorImageInfo imageInfo(res.sampler, objects.inputView, vk::ImageLayout(myInput->layout));
				vk::WriteDescriptorSet write(set, 0, 0, 1, vk::DescriptorType::eCombinedImageSampler, &imageInfo);
				res.device.updateDescriptorSets(1, &write, 0, nullptr, dl);
			}
		}

		if (!ok)
		{
			// There's no way to report an error from here, so the output keeps
			// whatever it held. The next cook will try again.
			fprintf(stderr, "VulkanTOP: creating the per-frame Vulkan objects failed, the frame was skipped\n");
			res.destroy(objects);
			return;
		}
		// Destroyed once this command buffer has completed, by VulkanTOPResources::recycle().
		res.track(info->commandBuffer, objects);

		// One full-screen triangle; the fragment shader computes every pixel.
		vk::RenderPassBeginInfo rpbi(myTargets.renderPass, objects.framebuffer, vk::Rect2D({ 0, 0 }, extent));
		cmd.beginRenderPass(rpbi, vk::SubpassContents::eInline, dl);

		vk::Viewport viewport(0.0f, 0.0f, float(extent.width), float(extent.height), 0.0f, 1.0f);
		vk::Rect2D scissor({ 0, 0 }, extent);
		cmd.setViewport(0, 1, &viewport, dl);
		cmd.setScissor(0, 1, &scissor, dl);

		if (myInput)
		{
			cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, myTargets.multiplyPipeline, dl);
			cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, res.multiplyLayout, 0, 1, &set, 0, nullptr, dl);
			cmd.pushConstants(res.multiplyLayout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(myParams), &myParams, dl);
		}
		else
		{
			cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, myTargets.fillPipeline, dl);
			cmd.pushConstants(res.fillLayout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(myParams), &myParams, dl);
		}
		cmd.draw(3, 1, 0, 0, dl);
		cmd.endRenderPass(dl);
	}

private:
	// Keeps the pipelines alive until this recording is done with them.
	std::shared_ptr<VulkanTOPResources>	myResources;
	VulkanTOPTargets					myTargets;
	OP_SmartRef<OP_VulkanImage>			myOutput;
	OP_SmartRef<OP_VulkanImage>			myInput;
	vk::ComponentMapping				myInputMapping;
	FillParams							myParams;
};

// ---------------------------------------------------------------------------------
// VulkanTOP
// ---------------------------------------------------------------------------------

VulkanTOP::VulkanTOP(const OP_NodeInfo* info, TOP_Context* context) :
	myNodeInfo(info),
	myContext(context)
{
	myExecuteCount = 0;
	myStep = 0.0;
	mySpeed = 0.0;
	myBrightness = 1.0;
}

VulkanTOP::~VulkanTOP()
{
	// Wait (a bounded time) for our GPU work, so its per-frame objects and the
	// pipelines can be destroyed. Safe here: we're not inside TouchDesigner's
	// recording of a command buffer.
	if (myResources)
		myResources->recycle(5ull * 1000 * 1000 * 1000);
}

void
VulkanTOP::getGeneralInfo(TOP_GeneralInfo* ginfo, const OP_Inputs* inputs, void* reserved1)
{
	ginfo->cookEveryFrameIfAsked = true;
}

bool
VulkanTOP::ensureResources()
{
	OP_VulkanUtils* vulkanUtils = myContext->getVulkanUtils(nullptr);
	if (!vulkanUtils)
	{
		myError = "Vulkan is not available";
		return false;
	}
	if (myResources)
		return true;

	// Build once; don't retry a failed creation every frame
	if (myResourcesFailed)
	{
		myError = "Failed to create the Vulkan resources (see console)";
		return false;
	}

	auto resources = std::make_shared<VulkanTOPResources>();
	std::string err;
	if (!resources->init(vulkanUtils->getVulkanInfo(nullptr), err))
	{
		fprintf(stderr, "VulkanTOP: %s\n", err.c_str());
		myResourcesFailed = true;
		myError = "Failed to create the Vulkan resources (see console)";
		return false;
	}
	myResources = std::move(resources);
	return true;
}

void
VulkanTOP::execute(TOP_Output* output, const OP_Inputs* inputs, void* reserved1)
{
	myExecuteCount++;

	mySpeed      = inputs->getParDouble("Speed");
	myBrightness = inputs->getParDouble("Brightness");
	myStep      += mySpeed * 0.01;

	myWarning.clear();
	myError = nullptr;

	if (!ensureResources())
		return;

	// Free the per-frame objects of earlier frames whose GPU work has finished.
	myResources->recycle(0);

	// Gather the optional input. This must happen before the output is allocated and
	// the command recorder is added. inImage stays empty if there's no usable
	// input, and we fall back to the plain fill.
	OP_SmartRef<OP_VulkanImage> inImage;
	vk::ComponentMapping inMapping;
	if (inputs->getNumInputs() > 0)
	{
		const OP_TOPInput* top = inputs->getInputTOP(0);
		if (top)
		{
			OP_GetVulkanImageInfo ginfo;
			ginfo.dstStageMask  = static_cast<VkPipelineStageFlags2>(InputStages);
			ginfo.dstAccessMask = static_cast<VkAccessFlags2>(InputAccess);
			OP_SmartRef<OP_VulkanImage> img = top->getVulkanImage(ginfo, nullptr);
			if (img && img->image)
			{
				if (top->textureDesc.texDim != OP_TexDim::e2D)
				{
					myWarning = "Input is not a 2D texture, it will not be used in this example.";
				}
				else
				{
					inMapping = inputChannelMapping(top->textureDesc.pixelFormat, vk::Format(img->createInfo.format));
					inImage = std::move(img);
				}
			}
		}
	}

	TOP_TextureOutputInfo outInfo;
	output->getSuggestedOutputDesc(&outInfo.textureDesc, nullptr);
	outInfo.textureDesc.texDim = OP_TexDim::e2D;
	outInfo.colorBufferIndex = 0;
	outInfo.srcStageMask  = static_cast<VkPipelineStageFlags2>(OutputStages);
	outInfo.srcAccessMask = static_cast<VkAccessFlags2>(OutputAccess);
	outInfo.finalLayout = static_cast<VkImageLayout>(vk::ImageLayout::eColorAttachmentOptimal);

	OP_SmartRef<OP_VulkanImage> outImage = output->outputVulkanImage(outInfo, nullptr);
	if (!outImage || !outImage->image)
		return;

	// We render into the output, so it must be usable as a color attachment.
	const vk::ImageCreateInfo& outCreate = reinterpret_cast<const vk::ImageCreateInfo&>(outImage->createInfo);
	if (!(outCreate.usage & vk::ImageUsageFlagBits::eColorAttachment))
	{
		myError = "The output texture can't be rendered to (no color attachment usage)";
		return;
	}

	VulkanTOPTargets targets;
	std::string err;
	if (!myResources->getTargets(outCreate.format, outCreate.samples, vk::ImageLayout(outInfo.finalLayout), targets, err))
	{
		fprintf(stderr, "VulkanTOP: %s\n", err.c_str());
		myError = "Failed to create the Vulkan pipelines for the output format (see console)";
		return;
	}

	FillParams params;
	params.step         = (float)myStep;
	params.brightness   = (float)myBrightness;
	params.outputWidth  = (float)outCreate.extent.width;
	params.outputHeight = (float)outCreate.extent.height;

	// Added after getting the input and allocating the output.
	output->addVulkanCommandRecorder(OP_SmartRef<OP_VulkanCommandRecorder>(
		new VulkanTOPRecorder(myResources, targets, std::move(outImage), std::move(inImage), inMapping, params)));
}

void
VulkanTOP::getWarningString(OP_String* warning, void* reserved1)
{
	if (!myWarning.empty())
		warning->setString(myWarning.c_str());
}

void
VulkanTOP::getErrorString(OP_String* error, void* reserved1)
{
	if (myError)
		error->setString(myError);
}

int32_t
VulkanTOP::getNumInfoCHOPChans(void *reserved1)
{
	// We return the number of channel we want to output to any Info CHOP
	// connected to the TOP. In this example we are just going to send two channels.
	return 2;
}

void
VulkanTOP::getInfoCHOPChan(int32_t index, OP_InfoCHOPChan* chan, void* reserved1)
{
	// This function will be called once for each channel we said we'd want to return
	if (index == 0)
	{
		chan->name->setString("executeCount");
		chan->value = (float)myExecuteCount;
	}

	if (index == 1)
	{
		chan->name->setString("step");
		chan->value = (float)myStep;
	}
}

bool
VulkanTOP::getInfoDATSize(OP_InfoDATSize* infoSize, void* reserved1)
{
	infoSize->rows = 2;
	infoSize->cols = 2;
	// Setting this to false means we'll be assigning values to the table
	// one row at a time. True means we'll do it one column at a time.
	infoSize->byColumn = false;
	return true;
}

void
VulkanTOP::getInfoDATEntries(int32_t index,
								int32_t nEntries,
								OP_InfoDATEntries* entries,
								void *reserved1)
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

	if (index == 1)
	{
		snprintf(tempBuffer, sizeof(tempBuffer), "%s", "step");
		entries->values[0]->setString(tempBuffer);

		snprintf(tempBuffer, sizeof(tempBuffer), "%g", myStep);
		entries->values[1]->setString(tempBuffer);
	}
}

void
VulkanTOP::setupParameters(OP_ParameterManager* manager, void *reserved1)
{
	// brightness
	{
		OP_NumericParameter	np;

		np.name = "Brightness";
		np.label = "Brightness";
		np.defaultValues[0] = 1.0;

		np.minSliders[0] =  0.0;
		np.maxSliders[0] =  1.0;

		np.minValues[0] = 0.0;
		np.maxValues[0] = 1.0;

		np.clampMins[0] = true;
		np.clampMaxes[0] = true;

		OP_ParAppendResult res = manager->appendFloat(np);
		assert(res == OP_ParAppendResult::Success);
	}

	// speed
	{
		OP_NumericParameter	np;

		np.name = "Speed";
		np.label = "Speed";
		np.defaultValues[0] = 1.0;
		np.minSliders[0] = -10.0;
		np.maxSliders[0] =  10.0;

		OP_ParAppendResult res = manager->appendFloat(np);
		assert(res == OP_ParAppendResult::Success);
	}

	// pulse
	{
		OP_NumericParameter	np;

		np.name = "Reset";
		np.label = "Reset";

		OP_ParAppendResult res = manager->appendPulse(np);
		assert(res == OP_ParAppendResult::Success);
	}
}

void
VulkanTOP::pulsePressed(const char* name, void *reserved1)
{
	if (!strcmp(name, "Reset"))
	{
		myStep = 0.0;
	}
}
