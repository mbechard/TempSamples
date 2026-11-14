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

#pragma once

// vulkan.hpp without the global vk* prototypes: every Vulkan call goes through a
// dispatcher initialized from the vkGetInstanceProcAddr TouchDesigner gives us,
// so the plugin uses the same Vulkan as TouchDesigner (see CMakeLists.txt).
#define VULKAN_HPP_DISABLE_IMPLICIT_RESULT_VALUE_CAST
#define VULKAN_HPP_NO_SMART_HANDLE
#define VULKAN_HPP_NO_EXCEPTIONS
#define VULKAN_HPP_TYPESAFE_CONVERSION 1
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#define VULKAN_HPP_NO_DEFAULT_DISPATCHER

// Included before the TouchDesigner headers, so they use the real Vulkan types.
#include <vulkan/vulkan.hpp>

#include "POP_CPlusPlusBase.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace TD;

// Vulkan objects shared by all cooks of one VulkanPOP: the device and dispatcher,
// and the compute pipelines. Held through a shared_ptr by the POP and by every
// command recorder, because a recorder can outlive the cook that created it.
// See VulkanPOP.cpp.
class VulkanPOPResources;

class VulkanPOP : public POP_CPlusPlusBase
{
public:
	using AttributeList = std::vector<std::pair<std::string, OP_SmartRef<POP_Buffer>>>;

	VulkanPOP(const OP_NodeInfo *info, POP_Context *context);
	virtual ~VulkanPOP();

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

	// One GPU copy from a (read-only) input buffer into a new output buffer.
	struct GPUCopy
	{
		OP_SmartRef<POP_Buffer>	src;
		OP_SmartRef<POP_Buffer>	dst;

		// false: plain buffer copy (vkCmdCopyBuffer).
		// true:  run the TransformPosition shader (scale, then shift along Z). Used for P.
		bool					transformP = false;
	};

	// The Scale and Shift Z parameters. Applied to P in both modes: P is scaled by
	// 'scale', then 'shiftZ' is added to z.
	struct PositionTransform
	{
		float	scale = 1.0f;
		float	shiftZ = 0.0f;
	};

private:
	// Input connected: copy the input to new buffers, with P transformed on the GPU.
	void				processInput(POP_Output* output, const OP_POPInput* input,
									 const PositionTransform& xform);

	// No input: generate a unit cube, transformed, with its points computed on the GPU.
	void				generateCube(POP_Output* output, const PositionTransform& xform);

	// Returns a new Vulkan buffer to receive src's data and records the copy in jobs.
	// CPU buffers are always our own copies already, so they're returned as-is.
	// Sets ok = false and myError on failure.
	OP_SmartRef<POP_Buffer>	makeOutputBuffer(const OP_SmartRef<POP_Buffer>& src, std::vector<GPUCopy>& jobs,
											 bool& ok, bool transformP = false);

	// Creates the Vulkan resources (dispatcher, shaders, pipelines) the first time.
	// Returns false, with myError set, if Vulkan isn't available or creation failed.
	bool				ensureResources();

	std::shared_ptr<VulkanPOPResources>	myResources;
	// Don't retry failed pipeline creation every cook.
	bool				myResourcesFailed = false;

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
