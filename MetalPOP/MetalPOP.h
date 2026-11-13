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

#include "POP_CPlusPlusBase.h"
#include <functional>
#include <vector>
#include <string>

using namespace TD;

class MetalPOP : public POP_CPlusPlusBase
{
public:
	using AttributeList = std::vector<std::pair<std::string, OP_SmartRef<POP_Buffer>>>;

	MetalPOP(const OP_NodeInfo *info, POP_Context *context);
	virtual ~MetalPOP();

	virtual void		getGeneralInfo(POP_GeneralInfo*, const OP_Inputs*, void* reserved1) override;

	virtual void		execute(POP_Output*, const OP_Inputs*, void* reserved) override;

	virtual int32_t		getNumInfoCHOPChans(void* reserved) override;
	virtual void		getInfoCHOPChan(int32_t index,
										OP_InfoCHOPChan *chan,
										void* reserved) override;

	virtual bool		getInfoDATSize(OP_InfoDATSize *infoSize, void* reserved) override;
	virtual void		getInfoDATEntries(int32_t index,
											int32_t nEntries,
											OP_InfoDATEntries *entries,
											void* reserved) override;

	virtual void		getErrorString(OP_String *error, void* reserved) override;

	virtual void		setupParameters(OP_ParameterManager *manager, void* reserved) override;
	virtual void		pulsePressed(const char *name, void* reserved) override;

private:
	// One GPU copy from a (read-only) input buffer into a new output buffer.
	struct GPUCopy
	{
		OP_SmartRef<POP_Buffer>	src;
		OP_SmartRef<POP_Buffer>	dst;
		// false: plain blit copy.
		// true:  run the transformPosition kernel (scale, then shift along Z). Used for P.
		bool					transformP = false;
	};

	// The Scale and Shift Z parameters. Applied to P in both modes: P is scaled by
	// 'scale', then 'shiftZ' is added to z.
	struct PositionTransform
	{
		float	scale = 1.0f;
		float	shiftZ = 0.0f;
	};

	// Input connected: copy the input to new buffers, with P transformed on the GPU.
	void				processInput(POP_Output* output, const OP_POPInput* input,
									 const PositionTransform& xform, OP_MetalUtils* metalUtils);

	// No input: generate a unit cube, transformed, with its points computed on the GPU.
	void				generateCube(POP_Output* output, const PositionTransform& xform,
									 OP_MetalUtils* metalUtils);

	// Returns a new Metal buffer to receive src's data and records the copy in jobs.
	// CPU buffers are always our own copies already, so they're returned as-is.
	// Must be called before finalizeMetalResources(). Sets ok = false and myError on failure.
	OP_SmartRef<POP_Buffer>	makeOutputBuffer(const OP_SmartRef<POP_Buffer>& src, std::vector<GPUCopy>& jobs,
											 bool& ok, bool transformP = false);

#ifdef __APPLE__
	bool				ensurePipelines(OP_MetalUtils* metalUtils);

	// Commits one command buffer: TD's wait syncs, then encode (which creates and ends
	// its own encoders), then TD's signal syncs, commit, and allCommandsCommitted().
	// Returns false if the GPU work wasn't submitted.
	bool				submit(OP_MetalUtils* metalUtils, const OP_SmartRef<OP_MetalOperationSync>& sync,
							   const std::function<void(id<MTLCommandBuffer>)>& encode);

	void				encodeCopy(id<MTLBlitCommandEncoder> blit,
								   const OP_SmartRef<POP_Buffer>& src,
								   const OP_SmartRef<POP_Buffer>& dst);

	void				encodeTransformPosition(id<MTLComputeCommandEncoder> enc,
												const OP_SmartRef<POP_Buffer>& src,
												const OP_SmartRef<POP_Buffer>& dst,
												const OP_SmartRef<POP_Buffer>& pointInfo,
												uint32_t maxPoints,
												const PositionTransform& xform);

	void				encodeCube(id<MTLComputeCommandEncoder> enc, const OP_SmartRef<POP_Buffer>& dst,
								   const PositionTransform& xform);

	id<MTLComputePipelineState>	myPositionPipeline = nil;
	id<MTLComputePipelineState>	myCubePipeline = nil;
	bool						myPipelineFailed = false;
#endif

	// We don't need to store this pointer, but we do for the example.
	// The OP_NodeInfo class store information about the node that's using
	// this instance of the class (like its name).
	const OP_NodeInfo*	myNodeInfo;

	// In this example this value will be incremented each time the execute()
	// function is called, then passes back to the POP
	int32_t				myExecuteCount;

	POP_Context*		myContext;
	const char*			myError;
};
