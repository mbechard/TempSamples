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

// Objective-C++, compiled with ARC.

#include "MetalPOP.h"

#include <assert.h>
#include <algorithm>
#include <cstdio>
#include <new>

#if !__has_feature(objc_arc)
	#error "This example is written assuming ARC is turned on as a compile option."
#endif

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

	// GPU buffers are exchanged as MTLBuffers. This is already the default on macOS,
	// but setting it makes the intent explicit.
	info->gpuAPI = OP_DeviceAPI::Metal;

	// The opType is the unique name for this POP. It must start with a 
	// capital A-Z character, and all the following characters must lower case
	// or numbers (a-z, 0-9)
	info->customOPInfo.opType->setString("Metalsample");

	// The opLabel is the text that will show up in the OP Create Dialog
	info->customOPInfo.opLabel->setString("Metal Sample");

	// Will be turned into a 3 letter icon on the nodes
	info->customOPInfo.opIcon->setString("MTL");

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
	return new MetalPOP(info, context);
}

DLLEXPORT
void
DestroyPOPInstance(POP_CPlusPlusBase* instance, POP_Context *context)
{
	// Delete the instance here, this will be called when
	// Touch is shutting down, when the POP using that instance is deleted, or
	// if the POP loads a different DLL
	delete (MetalPOP*)instance;
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

// Must match PositionParams in the MSL source below (12 bytes, no padding).
// Used by both transformPosition and makeCube, so P is transformed the same way in both modes.
struct PositionParams
{
	uint32_t	maxPoints;
	float		scale;		// applied to x, y and z
	float		shift;		// added to z after scaling
};

static const char* kKernelSrc = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct PositionParams
{
	uint  maxPoints;
	float scale;
	float shift;
};

// The same transform for both kernels: scale x, y, z, then add the shift to z.
static float3 transformP(float3 v, constant PositionParams& p)
{
	return v * p.scale + float3(0.0f, 0.0f, p.shift);
}

// P is a tightly packed 3-component float attribute (12 bytes per point), which is
// exactly packed_float3. Load into a float3 to do the math, store back as packed.
// src is the input's buffer (read-only); dst is our own output buffer.
kernel void transformPosition(device const packed_float3* src       [[buffer(0)]],
							  device packed_float3*       dst       [[buffer(1)]],
							  device const uint*          numPoints [[buffer(2)]],   // POP_PointInfo::numPoints
							  constant PositionParams&    p         [[buffer(3)]],
							  uint                        id        [[thread_position_in_grid]])
{
	if (id >= p.maxPoints)
		return;

	// Elements past the real point count (which may only be known on the GPU) are
	// copied unchanged, so the whole output buffer is initialized.
	const float3 v = src[id];
	dst[id] = (id < numPoints[0]) ? transformP(v, p) : v;
}

// Writes the 8 corners of a unit cube centered at the origin, transformed like
// transformPosition. Only p.scale and p.shift are used.
kernel void makeCube(device packed_float3*    outP [[buffer(0)]],
					 constant PositionParams& p    [[buffer(1)]],
					 uint                     id   [[thread_position_in_grid]])
{
	if (id >= 8)
		return;

	const float3 corner = float3((id & 1) ? 0.5f : -0.5f,
								 (id & 2) ? 0.5f : -0.5f,
								 (id & 4) ? 0.5f : -0.5f);
	outP[id] = transformP(corner, p);
}
)MSL";

MetalPOP::MetalPOP(const OP_NodeInfo* info, POP_Context *context) :
	myNodeInfo(info), myExecuteCount(0),
	myContext(context),
	myError(nullptr)
{
}

MetalPOP::~MetalPOP()
{
	myPositionPipeline = nil;
	myCubePipeline = nil;
}

void
MetalPOP::getGeneralInfo(POP_GeneralInfo* ginfo, const OP_Inputs *inputs, void* reserved)
{
}

// ---------------------------------------------------------------------------------
// Metal helpers
// ---------------------------------------------------------------------------------
bool
MetalPOP::ensurePipelines(OP_MetalUtils* metalUtils)
{
	if (myPositionPipeline && myCubePipeline)
		return true;
	// Build once; don't retry a failed compile every cook
	if (myPipelineFailed)
		return false;
	myPipelineFailed = true;

	OP_MetalInfo* metalInfo = metalUtils->getMetalInfo(nullptr);
	id<MTLDevice> device = metalInfo ? metalInfo->device : nil;
	if (!device)
		return false;

	NSError* err = nil;
	id<MTLLibrary> lib = [device newLibraryWithSource:@(kKernelSrc) options:nil error:&err];
	if (!lib)
	{
		NSLog(@"MetalPOP: shader compile failed: %@", err);
		return false;
	}

	auto build = [&](NSString* name) -> id<MTLComputePipelineState> {
		id<MTLComputePipelineState> pso =
			[device newComputePipelineStateWithFunction:[lib newFunctionWithName:name] error:&err];
		if (!pso)
			NSLog(@"MetalPOP: pipeline creation for %@ failed: %@", name, err);
		return pso;
	};

	myPositionPipeline = build(@"transformPosition");
	myCubePipeline     = build(@"makeCube");
	if (!myPositionPipeline || !myCubePipeline)
	{
		myPositionPipeline = nil;
		myCubePipeline = nil;
		return false;
	}

	myPipelineFailed = false;
	return true;
}

bool
MetalPOP::submit(OP_MetalUtils* metalUtils, const OP_SmartRef<OP_MetalOperationSync>& sync,
				 const std::function<void(id<MTLCommandBuffer>)>& encode)
{
	id<MTLCommandQueue>  queue = metalUtils->getSharedCommandQueue(nullptr);
	id<MTLCommandBuffer> cmd   = [queue commandBuffer];

	if (!cmd)
	{
		// There's no command buffer to put TD's signal in, and OP_MetalOperationSync has no
		// way to signal without one, so TD will wait on this operation. Reporting the commit
		// at least keeps TD from also blocking before present.
		sync->allCommandsCommitted(nullptr);
		return false;
	}

	// Must come before anything that reads TD's input buffers.
	sync->addWaitSyncs(cmd, nullptr);

	// encode creates and ends its own encoders, so none is open when the signal is added.
	if (encode)
		encode(cmd);

	// After all our commands: TD uses this to know when the outputs are ready and
	// when it may reuse the input buffers we referenced.
	sync->addSignalSyncs(cmd, nullptr);
	[cmd commit];

	// This operation only uses one command buffer, so everything is committed now.
	sync->allCommandsCommitted(nullptr);
	return true;
}

void
MetalPOP::encodeCopy(id<MTLBlitCommandEncoder> blit,
					 const OP_SmartRef<POP_Buffer>& src,
					 const OP_SmartRef<POP_Buffer>& dst)
{
	// POP buffers are pooled: the data lives at an offset inside the MTLBuffer.
	// makeOutputBuffer() already checked both buffers and their alignment.
	uint64_t srcOffset = 0, dstOffset = 0;
	id<MTLBuffer> srcBuf = src->getMetalBuffer(&srcOffset, nullptr);
	id<MTLBuffer> dstBuf = dst->getMetalBuffer(&dstOffset, nullptr);

	const uint64_t size = std::min(src->info.size, dst->info.size);
	if (!srcBuf || !dstBuf || size == 0)
		return;

	[blit copyFromBuffer:srcBuf
			sourceOffset:srcOffset
				toBuffer:dstBuf
	   destinationOffset:dstOffset
					size:size];
}

void
MetalPOP::encodeTransformPosition(id<MTLComputeCommandEncoder> enc,
								  const OP_SmartRef<POP_Buffer>& src,
								  const OP_SmartRef<POP_Buffer>& dst,
								  const OP_SmartRef<POP_Buffer>& pointInfo,
								  uint32_t maxPoints,
								  const PositionTransform& xform)
{
	uint64_t srcOffset = 0, dstOffset = 0;
	id<MTLBuffer> srcBuf = src->getMetalBuffer(&srcOffset, nullptr);
	id<MTLBuffer> dstBuf = dst->getMetalBuffer(&dstOffset, nullptr);
	if (!srcBuf || !dstBuf)
	{
		myError = "Position attribute is not available as a Metal buffer";
		return;
	}

	[enc setComputePipelineState:myPositionPipeline];
	[enc setBuffer:srcBuf offset:srcOffset atIndex:0];
	[enc setBuffer:dstBuf offset:dstOffset atIndex:1];

	// The point info may be on the CPU or on the GPU (we asked for CPUOrMetal).
	// Either way the kernel just reads the first uint32, POP_PointInfo::numPoints.
	uint64_t infoOffset = 0;
	id<MTLBuffer> infoBuf = nil;
	if (pointInfo && pointInfo->info.location == POP_BufferLocation::Metal)
		infoBuf = pointInfo->getMetalBuffer(&infoOffset, nullptr);

	if (infoBuf)
	{
		[enc setBuffer:infoBuf offset:infoOffset atIndex:2];
	}
	else
	{
		uint32_t numPoints = maxPoints;
		if (pointInfo)
		{
			const POP_PointInfo* pi = (const POP_PointInfo*)pointInfo->getData(nullptr);
			if (pi)
				numPoints = pi->numPoints;
		}
		[enc setBytes:&numPoints length:sizeof(numPoints) atIndex:2];
	}

	PositionParams params{ maxPoints, xform.scale, xform.shiftZ };
	[enc setBytes:&params length:sizeof(params) atIndex:3];

	const NSUInteger tg = std::min<NSUInteger>(myPositionPipeline.maxTotalThreadsPerThreadgroup, maxPoints);
	[enc dispatchThreads:MTLSizeMake(maxPoints, 1, 1)
		threadsPerThreadgroup:MTLSizeMake(tg, 1, 1)];
}

void
MetalPOP::encodeCube(id<MTLComputeCommandEncoder> enc, const OP_SmartRef<POP_Buffer>& dst,
					 const PositionTransform& xform)
{
	uint64_t dstOffset = 0;
	id<MTLBuffer> dstBuf = dst->getMetalBuffer(&dstOffset, nullptr);
	if (!dstBuf)
	{
		myError = "Created position buffer is not a Metal buffer";
		return;
	}

	[enc setComputePipelineState:myCubePipeline];
	[enc setBuffer:dstBuf offset:dstOffset atIndex:0];
	PositionParams params{ kCubePoints, xform.scale, xform.shiftZ };
	[enc setBytes:&params length:sizeof(params) atIndex:1];
	[enc dispatchThreads:MTLSizeMake(kCubePoints, 1, 1)
		threadsPerThreadgroup:MTLSizeMake(kCubePoints, 1, 1)];
}

// ---------------------------------------------------------------------------------
// Cooking
// ---------------------------------------------------------------------------------

// References to the input's attribute buffers. They're read-only: ReferenceInput gives
// us the input POP's own memory instead of a copy.
static MetalPOP::AttributeList
getAllAttributes(POP_AttributeClass c, const OP_POPInput* input)
{
	MetalPOP::AttributeList res;
	for (uint32_t i = 0; i < input->getNumAttributes(c); i++)
	{
		const POP_Attribute* attr = input->getAttribute(c, i, nullptr);
		POP_GetBufferInfo info;
		info.location = POP_BufferLocation::Metal;
		info.mode     = POP_GetBufferMode::ReferenceInput;
		OP_SmartRef<POP_Buffer> buf = attr->getBuffer(info, nullptr);
		res.emplace_back(std::string(attr->info.name), buf);
	}
	return res;
}

OP_SmartRef<POP_Buffer>
MetalPOP::makeOutputBuffer(const OP_SmartRef<POP_Buffer>& src, std::vector<GPUCopy>& jobs,
						   bool& ok, bool transformP)
{
	if (!src)
		return {};

	// CPU buffers are always returned as new copies, so they're already ours to output.
	if (src->info.location != POP_BufferLocation::Metal)
		return src;

	POP_BufferInfo createInfo = src->info;
	createInfo.location = POP_BufferLocation::Metal;

	OP_SmartRef<POP_Buffer> dst = myContext->createBuffer(createInfo, nullptr);
	if (!dst)
	{
		myError = "Failed to create output buffers";
		ok = false;
		return {};
	}

	uint64_t srcOffset = 0, dstOffset = 0;
	if (!src->getMetalBuffer(&srcOffset, nullptr) || !dst->getMetalBuffer(&dstOffset, nullptr))
	{
		myError = "A buffer is not available as a Metal buffer";
		ok = false;
		return {};
	}

	// Blit copies on macOS need offsets and size that are multiples of 4. POP data is
	// made of 4-byte elements, so this should always hold; check rather than assume.
	if (!transformP && ((srcOffset | dstOffset | src->info.size) & 3) != 0)
	{
		myError = "Buffer offset or size is not 4-byte aligned for a blit copy";
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
MetalPOP::execute(POP_Output* output, const OP_Inputs* inputs, void* reserved)
{
	myError = nullptr;
	myExecuteCount++;

	const OP_POPInput* input = inputs->getNumInputs() > 0 ? inputs->getInputPOP(0) : nullptr;

	@autoreleasepool
	{
		OP_MetalUtils* metalUtils = myContext->getMetalUtils(nullptr);
		if (!metalUtils)
		{
			myError = "Metal is not available";
			return;
		}

		if (!ensurePipelines(metalUtils))
		{
			myError = "Failed to build the Metal compute pipelines (see console)";
			return;
		}

		PositionTransform xform;
		xform.scale  = (float)inputs->getParDouble("Scale");
		xform.shiftZ = (float)inputs->getParDouble("Shiftz");

		if (input)
			processInput(output, input, xform, metalUtils);
		else
			generateCube(output, xform, metalUtils);
	}
}

void
MetalPOP::processInput(POP_Output* output, const OP_POPInput* input,
					   const PositionTransform& xform, OP_MetalUtils* metalUtils)
{
	// ---- References to the input's buffers (read-only) -----------------------------
	// All inputs must be requested, and all output buffers created, before
	// finalizeMetalResources() below.
	AttributeList pointAttrs = getAllAttributes(POP_AttributeClass::Point, input);
	AttributeList vertAttrs  = getAllAttributes(POP_AttributeClass::Vertex, input);
	AttributeList primAttrs  = getAllAttributes(POP_AttributeClass::Primitive, input);

	// Some info buffers always live on the CPU, so ask for CPUOrMetal. ReferenceInput
	// only applies to the ones on the GPU; CPU ones are always new copies.
	POP_GetBufferInfo ginfo;
	ginfo.location = POP_BufferLocation::CPUOrMetal;
	ginfo.mode     = POP_GetBufferMode::ReferenceInput;
	POP_InfoBuffers inInfo;
	input->getAllInfoBuffers(&inInfo, ginfo, nullptr);

	POP_GetBufferInfo indexGInfo;
	indexGInfo.location = POP_BufferLocation::Metal;
	indexGInfo.mode     = POP_GetBufferMode::ReferenceInput;
	OP_SmartRef<POP_Buffer> inIndex = input->getIndexBuffer(nullptr)->getBuffer(indexGInfo, nullptr);

	// The position kernel works on packed_float3, so P must be exactly 3 floats per point.
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
	// corresponding input reference. P gets the position kernel (scale + shift),
	// everything else a blit copy.
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

	// Nothing has been finalized yet, so returning here leaves no signal owed.
	if (!ok)
		return;   // myError was set by makeOutputBuffer()

	// ---- Metal work ----------------------------------------------------------------
	OP_SmartRef<OP_MetalOperationSync> sync = output->finalizeMetalResources(nullptr);
	if (!sync)
	{
		myError = "finalizeMetalResources() failed";
		return;
	}

	// Every job reads only the input references and writes its own new buffer, so the
	// blit and compute passes don't depend on each other.
	const bool submitted = submit(metalUtils, sync, [&](id<MTLCommandBuffer> cmd) {
		bool anyCopy = false, anyTransform = false;
		for (const GPUCopy& job : jobs)
			(job.transformP ? anyTransform : anyCopy) = true;

		// Native Metal copies for everything we pass through unchanged
		if (anyCopy)
		{
			id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
			for (const GPUCopy& job : jobs)
			{
				if (!job.transformP)
					encodeCopy(blit, job.src, job.dst);
			}
			[blit endEncoding];
		}

		// The position kernel for P
		if (anyTransform)
		{
			id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
			for (const GPUCopy& job : jobs)
			{
				if (job.transformP)
					encodeTransformPosition(enc, job.src, job.dst, inInfo.pointInfo,
											inInfo.maxInfo.points, xform);
			}
			[enc endEncoding];
		}
	});
	if (!submitted)
	{
		myError = "Failed to create a Metal command buffer";
		return;
	}

	// ---- Assign the outputs --------------------------------------------------------
	// Only our new buffers go to the output, never the input references. Ordering
	// against the GPU work is handled by the signal sync, so there's no stream to set.
	POP_SetBufferInfo sinfo;

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
	output->setInfoBuffers(&outInfo, sinfo, nullptr);
}

void
MetalPOP::generateCube(POP_Output* output, const PositionTransform& xform, OP_MetalUtils* metalUtils)
{
	// ---- Create every output buffer before finalizeMetalResources() ------------------

	// P is computed on the GPU, so it's a Metal buffer.
	POP_BufferInfo posInfo;
	posInfo.size     = kCubePoints * 3 * sizeof(float);
	posInfo.usage    = POP_BufferUsage::Attribute;
	posInfo.location = POP_BufferLocation::Metal;
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

	// Nothing has been finalized yet, so returning here leaves no signal owed.
	if (!posBuf || !indexBuf || !pointInfoBuf || !topoInfoBuf)
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

	// ---- Metal work ----------------------------------------------------------------
	OP_SmartRef<OP_MetalOperationSync> sync = output->finalizeMetalResources(nullptr);
	if (!sync)
	{
		myError = "finalizeMetalResources() failed";
		return;
	}

	if (!submit(metalUtils, sync, [&](id<MTLCommandBuffer> cmd) {
		id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
		encodeCube(enc, posBuf, xform);
		[enc endEncoding];
	}))
	{
		myError = "Failed to create a Metal command buffer";
		return;
	}

	// ---- Assign the outputs --------------------------------------------------------
	POP_SetBufferInfo sinfo;

	POP_AttributeInfo posAttr;
	posAttr.name          = "P";
	posAttr.numComponents = 3;
	posAttr.type          = POP_AttributeType::Float;
	posAttr.attribClass   = POP_AttributeClass::Point;
	output->setAttribute(&posBuf, posAttr, sinfo, nullptr);

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
MetalPOP::getNumInfoCHOPChans(void* reserved)
{
	// We return the number of channel we want to output to any Info CHOP
	// connected to the POP. In this example we are just going to send one channel.
	return 1;
}

void
MetalPOP::getInfoCHOPChan(int32_t index,
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
MetalPOP::getInfoDATSize(OP_InfoDATSize* infoSize, void* reserved)
{
	infoSize->rows = 1;
	infoSize->cols = 2;
	// Setting this to false means we'll be assigning values to the table
	// one row at a time. True means we'll do it one column at a time.
	infoSize->byColumn = false;
	return true;
}

void
MetalPOP::getInfoDATEntries(int32_t index,
										int32_t nEntries,
										OP_InfoDATEntries* entries,
										void* reserved)
{
	char tempBuffer[4096];

	if (index == 0)
	{
		// Set the value for the first column
		strlcpy(tempBuffer, "executeCount", sizeof(tempBuffer));
		entries->values[0]->setString(tempBuffer);

		// Set the value for the second column
		snprintf(tempBuffer, sizeof(tempBuffer), "%d", myExecuteCount);
		entries->values[1]->setString(tempBuffer);
	}
}

void
MetalPOP::getErrorString(OP_String *error, void* reserved)
{
	if (myError)
		error->setString(myError);
}

void
MetalPOP::setupParameters(OP_ParameterManager* manager, void* reserved)
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
MetalPOP::pulsePressed(const char* name, void* reserved)
{
}
