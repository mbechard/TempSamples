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

// Configuration options for vulkan.hpp
#define VULKAN_HPP_DISABLE_IMPLICIT_RESULT_VALUE_CAST
#define VULKAN_HPP_NO_SMART_HANDLE
#define VULKAN_HPP_NO_EXCEPTIONS
#define VK_PROTOTYPES
#define VULKAN_HPP_TYPESAFE_CONVERSION 1
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#define VULKAN_HPP_NO_DEFAULT_DISPATCHER

#include <Python.h>

// This needs to be included before the COMP_CPlusPlusPanelBase, to avoid duplicate definitions
// of some fallback Vulkan types.
#include <vulkan/vulkan.hpp>

#include <COMP_CPlusPlusPanelBase.h>
#include <vector>
#include <string>

using namespace TD;

class VulkanTexturePanelCOMP : public COMP_CPlusPlusPanelBase
{
public:
	VulkanTexturePanelCOMP(const OP_NodeInfo *info);
	virtual ~VulkanTexturePanelCOMP();

	virtual void		getGeneralInfo(COMP_PanelGeneralInfo *, const OP_Inputs*, void*) override;

	virtual void		execute(const OP_Inputs*, void* reserved1) override;
	virtual OP_SmartRef<COMP_VulkanCommandRecorder> prepareRenderForVulkanTexture(const OP_Inputs* inputs,
																					OP_SmartRef<COMP_VulkanTextureRenderInfo>&& info,
																					void* reserved1) override;
	virtual bool		mouseEvent(const COMP_PanelMouseEvent& mouse, void* reserved1) override;
	virtual bool		keyEvent(const COMP_PanelKeyEvent& key, void* reserved1) override;

	virtual int32_t		getNumInfoCHOPChans(void *reserved1) override;
	virtual void		getInfoCHOPChan(int32_t index, OP_InfoCHOPChan *chan, void* reserved1) override;

	virtual bool		getInfoDATSize(OP_InfoDATSize *infoSize, void *reserved1) override;
	virtual void		getInfoDATEntries(int32_t index, int32_t nEntries,
									OP_InfoDATEntries *entries, void *reserved1) override;

	virtual void		setupParameters(OP_ParameterManager *manager, void *reserved1) override;
	virtual void		pulsePressed(const char *name, void *reserved1) override;

	virtual bool				dropHoverStart(const COMP_DropEvent& drop, void* reserved1) override;
	virtual void				dropHoverMove(const COMP_DropEvent& drop, void* reserved1) override;
	virtual COMP_DropResult		dropHoverEnd(const COMP_DropEvent& drop, void* reserved1) override;

	virtual bool				allowDrag(const COMP_DragEvent& drag, void* reserved1) override;
	virtual COMP_DragResult		dragStart(const COMP_DragEvent& drag, void* reserved1) override;
	virtual void				dragEnd(const COMP_DropResult& dropResult, void* reserved1) override;

private:

	const OP_NodeInfo*	myNodeInfo;

	float				myMouseX, myMouseY;
	float				myDropX, myDropY;
	float				myMouseLeftButton;
	float				myMouseMiddleButton;
	float				myMouseRightButton;

	std::vector<std::string> myKeyEvents;

	std::shared_ptr<vk::detail::DispatchLoaderDynamic>		myDispatchLoader;

	COMP_PanelContext*	myContext;
	OP_VulkanUtils*		myVulkanUtils;

	std::array<float, 4>	myColor;
};
