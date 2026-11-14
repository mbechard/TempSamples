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

#include "MetalTOP.h"

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <cmath>
#include <random>
#include <chrono>

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

	// Change this to change the executeMode behavior of this plugin.
	info->executeMode = TOP_ExecuteMode::Metal;

	// The opType is the unique name for this TOP. It must start with a
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
	return new MetalTOP(info, context);
}

DLLEXPORT
void
DestroyTOPInstance(TOP_CPlusPlusBase* instance, TOP_Context *context)
{
	// Delete the instance here, this will be called when
	// Touch is shutting down, when the TOP using that instance is deleted, or
	// if the TOP loads a different DLL
	delete (MetalTOP*)instance;
}

};

MetalTOP::MetalTOP(const OP_NodeInfo* info, TOP_Context* context) :
	myNodeInfo(info),
	myContext(context)
{
	myExecuteCount = 0;
	myStep = 0.0;
	myBrightness = 1.0;
}

MetalTOP::~MetalTOP()
{
#if !__has_feature(objc_arc)
	#error "This example is written assuming ARC is turned on as a compile option."
#endif
	myFillPipeline = nil;
	myMultiplyPipeline = nil;
}

void
MetalTOP::getGeneralInfo(TOP_GeneralInfo* ginfo, const OP_Inputs* inputs, void* reserved1)
{
	ginfo->cookEveryFrameIfAsked = true;
}

// Must match FillParams in the MSL source below.
struct FillParams
{
	float	step;
	float	brightness;
	// How to turn a sampled input texel into TD's RGBA meaning for its format.
	// rgbMode:  0 = rgb as sampled, 1 = replicate red (mono), 2 = rgb from alpha (alpha-only)
	// alphaSrc: 0 = .a, 1 = .g, 2 = .r
	int32_t	rgbMode;
	int32_t	alphaSrc;
};

#ifdef __APPLE__
static const char* kFillKernelSrc = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct FillParams
{
	float step;
	float brightness;
	int   rgbMode;
	int   alphaSrc;
};

// Scrolling diagonal bands over a horizontal hue gradient
static float3 animatedColor(float2 uv, constant FillParams& p)
{
	float bands = 0.5 + 0.5 * sin((uv.x + uv.y) * 20.0 - p.step * 4.0);
	float3 hue  = 0.5 + 0.5 * cos(6.28318 * (uv.x + p.step * 0.1 + float3(0.0, 0.33, 0.67)));
	return hue * mix(0.4, 1.0, bands) * p.brightness;
}

kernel void fillKernel(texture2d<float, access::write> outTex [[texture(0)]],
					   constant FillParams&             p      [[buffer(0)]],
					   uint2                            gid    [[thread_position_in_grid]])
{
	const uint w = outTex.get_width();
	const uint h = outTex.get_height();
	if (gid.x >= w || gid.y >= h)
		return;

	float2 uv = (float2(gid) + 0.5) / float2(w, h);
	outTex.write(float4(animatedColor(uv, p), 1.0), gid);
}

kernel void multiplyKernel(texture2d<float, access::write>  outTex [[texture(0)]],
						   texture2d<float, access::sample> inTex  [[texture(1)]],
						   constant FillParams&              p      [[buffer(0)]],
						   uint2                             gid    [[thread_position_in_grid]])
{
	const uint w = outTex.get_width();
	const uint h = outTex.get_height();
	if (gid.x >= w || gid.y >= h)
		return;

	float2 uv = (float2(gid) + 0.5) / float2(w, h);

	// Normalized coords + linear filtering, so the input can differ in size from the output.
	constexpr sampler s(coord::normalized, filter::linear, address::clamp_to_edge);
	float4 src = inTex.sample(s, uv);

	float  a   = p.alphaSrc == 1 ? src.g : (p.alphaSrc == 2 ? src.r : src.a);
	float3 rgb = p.rgbMode  == 1 ? src.rrr : (p.rgbMode == 2 ? float3(a) : src.rgb);

	// TD images are premultiplied; scaling rgb and keeping alpha stays premultiplied.
	outTex.write(float4(rgb * animatedColor(uv, p), a), gid);
}
)MSL";

bool
MetalTOP::buildPipelines(id<MTLDevice> device)
{
	NSError* err = nil;

	id<MTLLibrary> lib = [device newLibraryWithSource:@(kFillKernelSrc) options:nil error:&err];
	if (!lib)
	{
		NSLog(@"MetalTOP: shader compile failed: %@", err);
		return false;
	}

	id<MTLFunction> fillFn = [lib newFunctionWithName:@"fillKernel"];
	id<MTLFunction> mulFn  = [lib newFunctionWithName:@"multiplyKernel"];

	myFillPipeline     = [device newComputePipelineStateWithFunction:fillFn error:&err];
	if (myFillPipeline)
		myMultiplyPipeline = [device newComputePipelineStateWithFunction:mulFn error:&err];

	if (!myFillPipeline || !myMultiplyPipeline)
	{
		NSLog(@"MetalTOP: pipeline creation failed: %@", err);
		myFillPipeline = nil;
		myMultiplyPipeline = nil;
		return false;
	}
	return true;
}

static bool
isTwoChannelFormat(MTLPixelFormat f)
{
	switch (f)
	{
		case MTLPixelFormatRG8Unorm:
		case MTLPixelFormatRG8Snorm:
		case MTLPixelFormatRG16Unorm:
		case MTLPixelFormatRG16Snorm:
		case MTLPixelFormatRG16Float:
		case MTLPixelFormatRG32Float:
			return true;
		default:
			return false;
	}
}

// Work out how the kernel should interpret the input's channels. Metal samples a
// one-channel texture as (r, 0, 0, 1) and a two-channel one as (r, g, 0, 1), which
// isn't what TD's mono / mono+alpha / alpha-only formats mean.
static void
setInputChannelModes(OP_PixelFormat fmt, id<MTLTexture> tex, FillParams& p)
{
	p.rgbMode = 0;
	p.alphaSrc = 0;

	// If TD already put a channel swizzle on the texture, sampling is already correct.
	MTLTextureSwizzleChannels sw = tex.swizzle;
	if (sw.red   != MTLTextureSwizzleRed   || sw.green != MTLTextureSwizzleGreen ||
		sw.blue  != MTLTextureSwizzleBlue  || sw.alpha != MTLTextureSwizzleAlpha)
		return;

	switch (fmt)
	{
		case OP_PixelFormat::Mono8Fixed:
		case OP_PixelFormat::Mono16Fixed:
		case OP_PixelFormat::Mono16Float:
		case OP_PixelFormat::Mono32Float:
			p.rgbMode = 1;		// alpha already samples as 1 for a one-channel format
			break;

		case OP_PixelFormat::MonoA8Fixed:
		case OP_PixelFormat::MonoA16Fixed:
		case OP_PixelFormat::MonoA16Float:
		case OP_PixelFormat::MonoA32Float:
			p.rgbMode = 1;
			p.alphaSrc = isTwoChannelFormat(tex.pixelFormat) ? 1 : 0;
			break;

		case OP_PixelFormat::A8Fixed:
		case OP_PixelFormat::A16Fixed:
		case OP_PixelFormat::A16Float:
		case OP_PixelFormat::A32Float:
			// Metal's only true alpha format is A8Unorm; the others must be stored in red.
			p.rgbMode = 2;		// premultiplied white
			p.alphaSrc = (tex.pixelFormat == MTLPixelFormatA8Unorm) ? 0 : 2;
			break;

		default:
			break;
	}
}
#endif

void
MetalTOP::execute(TOP_Output* output, const OP_Inputs* inputs, void* reserved1)
{
	myExecuteCount++;
	mySpeed      = inputs->getParDouble("Speed");
	myBrightness = inputs->getParDouble("Brightness");
	myStep      += mySpeed * 0.01;
	myWarning.clear();

	@autoreleasepool
	{
		OP_MetalUtils* metalUtils = myContext->getMetalUtils(nullptr);
		OP_MetalInfo* metalInfo = metalUtils->getMetalInfo(nullptr);
		
		id<MTLDevice>  device = metalInfo->device;

		// Build once; don't retry a failed compile every frame
		if (!myFillPipeline && !myPipelineFailed)
			myPipelineFailed = !buildPipelines(device);
		if (!myFillPipeline || !myMultiplyPipeline)
			return;

		FillParams params{ (float)myStep, (float)myBrightness, 0, 0 };

		// Gather the optional input. This must happen before finalizeMetalResources().
		// inTex stays empty if there's no usable input, and we fall back to the plain fill.
		OP_SmartRef<OP_MetalTexture> inTex;
		if (inputs->getNumInputs() > 0)
		{
			const OP_TOPInput* top = inputs->getInputTOP(0);
			if (top)
			{
				OP_SmartRef<OP_MetalTexture> t = top->getMetalTexture(nullptr);
				if (t)
				{
					id<MTLTexture> mtlTexture = t->getMTLTexture(nullptr);
					if (mtlTexture)
					{
						if (top->textureDesc.texDim != OP_TexDim::e2D)
						{
							myWarning = "Input is not a 2D texture, it will not be used in this example.";
						}
						else
						{
							setInputChannelModes(top->textureDesc.pixelFormat, mtlTexture, params);
							inTex = t;
						}
					}
				}
			}
		}

		TOP_TextureOutputInfo outInfo;
		output->getSuggestedOutputDesc(&outInfo.textureDesc, nullptr);
		outInfo.textureDesc.texDim = OP_TexDim::e2D;
		outInfo.colorBufferIndex = 0;

		OP_SmartRef<OP_MetalTexture> tex = output->outputMetalTexture(outInfo, nullptr);
		if (!tex)
			return;
			
		id<MTLTexture> mtlTex = tex->getMTLTexture(nullptr);
		if (!mtlTex)
			return;

		// If we call this then we *must* commit signal the syncs.signalSync at the very least.
		// This must be called after we are doing getting inputs and allocating our outputs.
		OP_SmartRef<OP_MetalOperationSync> syncs = output->finalizeMetalResources(nullptr);

		// An error occured
		if (!syncs)
			return;

		id<MTLComputePipelineState> pipeline = inTex ? myMultiplyPipeline : myFillPipeline;

		id<MTLCommandQueue> queue = metalUtils->getSharedCommandQueue(nullptr);

		id<MTLCommandBuffer> cmd = [queue commandBuffer];
		syncs->addWaitSyncs(cmd, nullptr);

		id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
		[enc setComputePipelineState:pipeline];
		[enc setTexture:mtlTex atIndex:0];
		if (inTex)
			[enc setTexture:inTex->getMTLTexture(nullptr) atIndex:1];
		[enc setBytes:&params length:sizeof(params) atIndex:0];

		const NSUInteger tw = pipeline.threadExecutionWidth;
		const NSUInteger th = pipeline.maxTotalThreadsPerThreadgroup / tw;
		[enc dispatchThreads:MTLSizeMake(mtlTex.width, mtlTex.height, 1)
			threadsPerThreadgroup:MTLSizeMake(tw, th, 1)];
		[enc endEncoding];

		syncs->addSignalSyncs(cmd, nullptr);

		[cmd commit];
		syncs->allCommandsCommitted(nullptr);
	}
}

void
MetalTOP::getWarningString(OP_String* warning, void* reserved1)
{
	if (!myWarning.empty())
		warning->setString(myWarning.c_str());
}

int32_t
MetalTOP::getNumInfoCHOPChans(void *reserved1)
{
	// We return the number of channel we want to output to any Info CHOP
	// connected to the TOP. In this example we are just going to send one channel.
	return 2;
}

void
MetalTOP::getInfoCHOPChan(int32_t index, OP_InfoCHOPChan* chan, void* reserved1)
{
	// This function will be called once for each channel we said we'd want to return
	// In this example it'll only be called once.

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
MetalTOP::getInfoDATSize(OP_InfoDATSize* infoSize, void* reserved1)
{
	infoSize->rows = 2;
	infoSize->cols = 2;
	// Setting this to false means we'll be assigning values to the table
	// one row at a time. True means we'll do it one column at a time.
	infoSize->byColumn = false;
	return true;
}

void
MetalTOP::getInfoDATEntries(int32_t index,
								int32_t nEntries,
								OP_InfoDATEntries* entries,
								void *reserved1)
{
	char tempBuffer[4096];

	if (index == 0)
	{
		// Set the value for the first column
#ifdef _WIN32
		strcpy_s(tempBuffer, "executeCount");
#else // macOS
		strlcpy(tempBuffer, "executeCount", sizeof(tempBuffer));
#endif
		entries->values[0]->setString(tempBuffer);

		// Set the value for the second column
#ifdef _WIN32
		sprintf_s(tempBuffer, "%d", myExecuteCount);
#else // macOS
		snprintf(tempBuffer, sizeof(tempBuffer), "%d", myExecuteCount);
#endif
		entries->values[1]->setString(tempBuffer);
	}

	if (index == 1)
	{
#ifdef _WIN32
		strcpy_s(tempBuffer, "step");
#else // macOS
		strlcpy(tempBuffer, "step", sizeof(tempBuffer));
#endif
		entries->values[0]->setString(tempBuffer);

#ifdef _WIN32
		sprintf_s(tempBuffer, "%g", myStep);
#else // macOS
		snprintf(tempBuffer, sizeof(tempBuffer), "%g", myStep);
#endif
		entries->values[1]->setString(tempBuffer);
	}
}

void
MetalTOP::setupParameters(OP_ParameterManager* manager, void *reserved1)
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
MetalTOP::pulsePressed(const char* name, void *reserved1)
{
	if (!strcmp(name, "Reset"))
	{
		myStep = 0.0;
	}


}

