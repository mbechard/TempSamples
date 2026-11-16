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

// This should only appear in one .cpp file, before the other includes

#include "VulkanTexturePanelCOMP.h"

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <cmath>
#include <random>
#include <chrono>

//#define THREADING_SIGNALED_PRODUCER

// These functions are basic C function, which the DLL loader can find
// much easier than finding a C++ Class.
// The DLLEXPORT prefix is needed so the compile exports these functions from the .dll
// you are creating
extern "C"
{

DLLEXPORT
void
FillPanelCOMPPluginInfo(COMP_PanelPluginInfo *info)
{
	// This must always be set to this constant
	if (!info->setAPIVersion(COMPCPlusPlusPanelAPIVersion))
		return;

	// Change this to change the executeMode behavior of this plugin.
	info->renderMode = COMP_PanelRenderMode::VulkanTexture;

	// The opType is the unique name for this TOP. It must start with a
	// capital A-Z character, and all the following characters must lower case
	// or numbers (a-z, 0-9)
	info->customOPInfo.opType->setString("Vulkantexturesample");

	// The opLabel is the text that will show up in the OP Create Dialog
	info->customOPInfo.opLabel->setString("Vulkan Texture Sample");

	// Will be turned into a 3 letter icon on the nodes
	info->customOPInfo.opIcon->setString("VTx");

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
COMP_CPlusPlusPanelBase*
CreatePanelCOMPInstance(const OP_NodeInfo* info)
{
	// Return a new instance of your class every time this is called.
	// It will be called once per TOP that is using the .dll
	return new VulkanTexturePanelCOMP(info);
}

DLLEXPORT
void
DestroyPanelCOMPInstance(COMP_CPlusPlusPanelBase* instance)
{
	// Delete the instance here, this will be called when
	// Touch is shutting down, when the TOP using that instance is deleted, or
	// if the TOP loads a different DLL
	delete (VulkanTexturePanelCOMP*)instance;
}

};

VulkanTexturePanelCOMP::VulkanTexturePanelCOMP(const OP_NodeInfo* info) :
	myNodeInfo(info),
	myContext((COMP_PanelContext*)info->context),
	myMouseX(0.0f),
	myMouseY(0.0f),
	myDropX(-1.0f),
	myDropY(-1.0f),
	myMouseLeftButton(0.0f),
	myMouseMiddleButton(0.0f),
	myMouseRightButton(0.0f)
{
	myVulkanUtils = myContext->getVulkanUtils(nullptr);
	const OP_VulkanInfo* vulkanInfo = myVulkanUtils->getVulkanInfo(nullptr);
	myDispatchLoader = std::make_shared<vk::detail::DispatchLoaderDynamic>();
	myDispatchLoader->init(vulkanInfo->getInstanceProcAddr);
	myDispatchLoader->init(vk::Instance(vulkanInfo->instance));
	myDispatchLoader->init(vk::Device(vulkanInfo->device));
}

VulkanTexturePanelCOMP::~VulkanTexturePanelCOMP()
{
}

void
VulkanTexturePanelCOMP::getGeneralInfo(COMP_PanelGeneralInfo* ginfo, const OP_Inputs* inputs, void* reserved1)
{
	ginfo->cookEveryFrameIfAsked = true;
}

class ExampleRecorder : public OP_RefCountImpl<OP_VulkanCommandRecorder>
{
public:

	ExampleRecorder(OP_SmartRef<COMP_VulkanTextureRenderInfo>&& info, const std::shared_ptr<vk::detail::DispatchLoaderDynamic> &loader,
					const std::array<float, 4>& color) :
		myInfo(std::move(info)),
		myDispatchLoader(loader),
		myColor(color)
	{
	}

	virtual void
	recordCommands(const OP_VulkanCommandRecordInfo* recordInfo, void*) override
	{
		vk::CommandBuffer cmdBuf(recordInfo->commandBuffer->getHandle(nullptr));

		int w = myInfo->renderPixelWidth;
		int h = myInfo->renderPixelHeight;

		vk::DependencyInfo depInfo;
		vk::ImageMemoryBarrier2 m;
		m.image = myInfo->targetImage;
		// We don't care what the contents were beforehand
		m.oldLayout = vk::ImageLayout::eUndefined;
		m.newLayout = vk::ImageLayout::eTransferDstOptimal;
		m.srcAccessMask = vk::AccessFlags2();
		m.dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
		m.srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe;
		m.dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
		m.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		m.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

		m.subresourceRange = myInfo->targetImageSubresourceRange;

		depInfo.imageMemoryBarrierCount = 1;
		depInfo.pImageMemoryBarriers = &m;
		cmdBuf.pipelineBarrier2(depInfo, *myDispatchLoader);

		vk::ClearColorValue c(myColor);
		vk::ImageSubresourceRange range;
		range.layerCount = 1;
		range.levelCount = 1;
		range.aspectMask = vk::ImageAspectFlagBits::eColor;
		cmdBuf.clearColorImage(myInfo->targetImage, vk::ImageLayout::eTransferDstOptimal, c, range, *myDispatchLoader);

		m.oldLayout = vk::ImageLayout::eTransferDstOptimal;
		m.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
		m.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;

		m.newLayout = vk::ImageLayout(myInfo->targetImageFinalLayout);
		m.dstAccessMask = vk::AccessFlags2(myInfo->targetImageFinalDstAccessMask);
		m.dstStageMask = vk::PipelineStageFlags2(myInfo->targetImageFinalDstStageMask);

		cmdBuf.pipelineBarrier2(depInfo, *myDispatchLoader);
	}

private:
	OP_SmartRef<COMP_VulkanTextureRenderInfo>	myInfo;
	std::shared_ptr<vk::detail::DispatchLoaderDynamic>		myDispatchLoader;
	std::array<float, 4>						myColor;
};

OP_SmartRef<OP_VulkanCommandRecorder>
VulkanTexturePanelCOMP::prepareRenderForVulkanTexture(const OP_Inputs* inputs,
													OP_SmartRef<COMP_VulkanTextureRenderInfo>&& info,
													void* reserved1)
{
	return OP_SmartRef<OP_VulkanCommandRecorder>(new ExampleRecorder(std::move(info), myDispatchLoader, myColor));
}

void
VulkanTexturePanelCOMP::execute(const OP_Inputs* inputs, void* reserved1)
{
	double r, g, b, a;
	inputs->getParRGBA("Color", r, g, b, a);
	myColor[0] = (float)r;
	myColor[1] = (float)g;
	myColor[2] = (float)b;
	myColor[3] = (float)a;
}

bool
VulkanTexturePanelCOMP::mouseEvent(const COMP_PanelMouseEvent& mouse, void*)
{
	auto setState =
		[this, &mouse](float state)
		{
			switch (mouse.button)
			{
				default:
					break;
				case COMP_PanelMouseButton::Left:
					myMouseLeftButton = state;
					break;
				case COMP_PanelMouseButton::Middle:
					myMouseMiddleButton = state;
					break;
				case COMP_PanelMouseButton::Right:
					myMouseRightButton = state;
					break;
			}
		};
	
	auto setPosition =
		[this, &mouse]()
		{
			myMouseX = mouse.x;
			myMouseY = mouse.y;
		};
	switch (mouse.action)
	{
		default:
			break;
		case COMP_PanelMouseAction::Down:
			setPosition();
			setState(1.0f);
			break;
		case COMP_PanelMouseAction::Up:
			setPosition();
			setState(0.0f);
			break;
		case COMP_PanelMouseAction::Move:
			setPosition();
			break;
	}
	return true;
}

bool
VulkanTexturePanelCOMP::keyEvent(const COMP_PanelKeyEvent& key, void* reserved1)
{
	switch (key.action)
	{
		default:
			break;
		case COMP_KeyEventAction::Down:
		{
			std::string s;
			s = key.text;
			s += ":  Down ";
			myKeyEvents.push_back(std::move(s));
			break;
		}
		case COMP_KeyEventAction::Up:
		{
			std::string s;
			s = key.text;
			s += ":  Up";
			myKeyEvents.push_back(std::move(s));
			break;
		}
	}
	while (myKeyEvents.size() > 20)
		myKeyEvents.erase(myKeyEvents.begin());
	return true;
}

int32_t
VulkanTexturePanelCOMP::getNumInfoCHOPChans(void *reserved1)
{
	// We return the number of channel we want to output to any Info CHOP
	// connected to the TOP. In this example we are just going to send one channel.
	return 7;
}

void
VulkanTexturePanelCOMP::getInfoCHOPChan(int32_t index, OP_InfoCHOPChan* chan, void* reserved1)
{
	// This function will be called once for each channel we said we'd want to return
	// In this example it'll only be called once.

	switch (index)
	{
		case 0:
			chan->name->setString("mouse_x");
			chan->value = myMouseX;
			break;
		case 1:
			chan->name->setString("mouse_y");
			chan->value = myMouseY;
			break;
		case 2:
			chan->name->setString("mouse_lbutton");
			chan->value = myMouseLeftButton;
			break;
		case 3:
			chan->name->setString("mouse_middle_button");
			chan->value = myMouseMiddleButton;
			break;
		case 4:
			chan->name->setString("mouse_right_button");
			chan->value = myMouseRightButton;
			break;
		case 5:
			chan->name->setString("drop_x");
			chan->value = myDropX;
			break;
		case 6:
			chan->name->setString("drop_y");
			chan->value = myDropY;
			break;
	}
}

bool		
VulkanTexturePanelCOMP::getInfoDATSize(OP_InfoDATSize* infoSize, void* reserved1)
{
	infoSize->rows = (int)myKeyEvents.size();
	infoSize->cols = 1;
	// Setting this to false means we'll be assigning values to the table
	// one row at a time. True means we'll do it one column at a time.
	infoSize->byColumn = false;
	return true;
}

void
VulkanTexturePanelCOMP::getInfoDATEntries(int32_t index,
								int32_t nEntries,
								OP_InfoDATEntries* entries,
								void *reserved1)
{
	entries->values[0]->setString(myKeyEvents[index].c_str());
}

void
VulkanTexturePanelCOMP::setupParameters(OP_ParameterManager* manager, void *reserved1)
{
	{
		OP_NumericParameter	np;

		np.name = "Color";
		np.label = "Color";
		for (int i = 0; i < 4; i++)
		{
			np.defaultValues[i] = 1.0;
			np.minSliders[i] =  0.0;
			np.maxSliders[i] =  1.0;
			np.minValues[i] = 0.0;
			np.maxValues[i] = 1.0;
			np.clampMins[i] = false;
			np.clampMaxes[i] = false;
		}
		
		OP_ParAppendResult res = manager->appendRGBA(np);
		assert(res == OP_ParAppendResult::Success);
	}
}

void
VulkanTexturePanelCOMP::pulsePressed(const char* name, void *reserved1)
{
}

bool
VulkanTexturePanelCOMP::dropHoverStart(const COMP_DropEvent& drop, void* reserved1)
{
	myDropX = drop.x;
	myDropY = drop.y;
	return true;
}

void
VulkanTexturePanelCOMP::dropHoverMove(const COMP_DropEvent& drop, void* reserved1)
{
	myDropX = drop.x;
	myDropY = drop.y;
}

COMP_DropResult
VulkanTexturePanelCOMP::dropHoverEnd(const COMP_DropEvent& drop, void* reserved1)
{
	myDropX = -1.0;
	myDropY = -1.0;
	return COMP_DropResult();
}

// Return true if you want to allow a drag operation to start.
bool
VulkanTexturePanelCOMP::allowDrag(const COMP_DragEvent& drag, void* reserved1)
{
	return true;
}

COMP_DragResult
VulkanTexturePanelCOMP::dragStart(const COMP_DragEvent& drag, void* reserved1)
{
	PY_SmartRef list(PyList_New(1));
	PyList_SetItem(list.get(), 0, PyUnicode_FromString("TestString"));

	COMP_DragResult res;
	res.dragList = std::move(list);
	return res;
}

void
VulkanTexturePanelCOMP::dragEnd(const COMP_DropResult& dropResult, void* reserved1)
{
}

