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

#include "TOP_CPlusPlusBase.h"

#include <memory>
#include <string>

using namespace TD;

// Vulkan objects shared by all cooks of one VulkanTOP: the device and dispatcher,
// the sampler, and the render passes and pipelines for each output format. Held
// through a shared_ptr by the TOP and by every command recorder, because a
// recorder can outlive the cook that created it. See VulkanTOP.cpp.
class VulkanTOPResources;

class VulkanTOP : public TOP_CPlusPlusBase
{
public:
	VulkanTOP(const OP_NodeInfo *info, TOP_Context* context);
	virtual ~VulkanTOP();

	virtual void		getGeneralInfo(TOP_GeneralInfo *, const OP_Inputs*, void*) override;

	virtual void		execute(TOP_Output*,
							const OP_Inputs*,
							void* reserved1) override;

	virtual int32_t		getNumInfoCHOPChans(void *reserved1) override;
	virtual void		getInfoCHOPChan(int32_t index,
								OP_InfoCHOPChan *chan, void* reserved1) override;

	virtual bool		getInfoDATSize(OP_InfoDATSize *infoSize, void *reserved1) override;
	virtual void		getInfoDATEntries(int32_t index,
									int32_t nEntries,
									OP_InfoDATEntries *entries,
									void *reserved1) override;

	virtual void		setupParameters(OP_ParameterManager *manager, void *reserved1) override;
	virtual void		pulsePressed(const char *name, void *reserved1) override;

	virtual void		getWarningString(OP_String *warning, void *reserved1) override;
	virtual void		getErrorString(OP_String *error, void *reserved1) override;

private:
	// Creates the device-wide Vulkan resources (dispatcher, sampler, shaders) the
	// first time. Returns false, with myError set, if Vulkan isn't available or
	// creation failed.
	bool				ensureResources();

	std::shared_ptr<VulkanTOPResources>	myResources;
	// Don't retry failed creation every cook.
	bool				myResourcesFailed = false;

	// We don't need to store this pointer, but we do for the example.
	// The OP_NodeInfo class store information about the node that's using
	// this instance of the class (like its name).
	const OP_NodeInfo*	myNodeInfo;

	// In this example this value will be incremented each time the execute()
	// function is called, then passes back to the TOP
	int					myExecuteCount;

	double				myStep;
	double				mySpeed;
	double				myBrightness;

	std::string			myWarning;
	const char*			myError = nullptr;

	TOP_Context*		myContext;
};
