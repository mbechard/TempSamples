cmake_minimum_required(VERSION 4.4)

# Before project(): macOS deployment target.
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/TouchDesignerPreProject.cmake)
project(VulkanTexturePanelCOMP LANGUAGES CXX)

# Shared TouchDesigner custom-operator build logic (repository root /cmake).
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/TouchDesignerCustomOP.cmake)

# -----------------------------------------------------------------------------
# Vulkan
# -----------------------------------------------------------------------------
# The plugin lives inside TouchDesigner's process, so any VkInstance/VkDevice it
# is handed belongs to TD and must be driven through TD's own entry points.
#
#   DYNAMIC (default): headers only, no loader dependency. Entry points are
#     resolved at runtime from the vkGetInstanceProcAddr of the Vulkan TD is
#     running (vulkan-hpp's DispatchLoaderDynamic). Calling a host-owned handle
#     through a *different* loader's trampolines is undefined -- and on macOS TD
#     may be talking to MoltenVK with no loader at all.
#   LOADER: link the Vulkan loader (vulkan-1.dll / libvulkan.1.dylib) and call
#     prototypes directly. Only safe for objects the plugin creates itself.
set(TD_VULKAN_DISPATCH "DYNAMIC" CACHE STRING
	"DYNAMIC = headers only, resolve entry points at runtime; LOADER = link the Vulkan loader")
set_property(CACHE TD_VULKAN_DISPATCH PROPERTY STRINGS DYNAMIC LOADER)
if(NOT TD_VULKAN_DISPATCH MATCHES "^(DYNAMIC|LOADER)$")
	message(FATAL_ERROR "TD_VULKAN_DISPATCH must be DYNAMIC or LOADER (got '${TD_VULKAN_DISPATCH}').")
endif()

# Header version, not a runtime requirement -- check apiVersion at runtime too.
set(TD_VULKAN_MIN_VERSION 1.2)

# Uses $VULKAN_SDK (on macOS, the SDK's macOS/ subfolder) or Vulkan_ROOT.
find_package(Vulkan ${TD_VULKAN_MIN_VERSION} QUIET)
if(NOT Vulkan_FOUND)
	td_skip_custom_op(VulkanTexturePanelCOMP
		"the Vulkan SDK (${TD_VULKAN_MIN_VERSION} or newer) was not found (install it and set VULKAN_SDK, or set Vulkan_ROOT)")
	return()
endif()
message(STATUS "Vulkan ${Vulkan_VERSION} (${TD_VULKAN_DISPATCH}): ${Vulkan_INCLUDE_DIRS}")

add_library(TDVulkan INTERFACE)

if(TD_VULKAN_DISPATCH STREQUAL "DYNAMIC")
	target_link_libraries(TDVulkan INTERFACE Vulkan::Headers)
	target_compile_definitions(TDVulkan INTERFACE
		# No global vk* prototypes: any accidental direct call fails to compile
		# instead of silently binding to whatever loader happens to be around.
		VK_NO_PROTOTYPES
		VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1)
else()
	target_link_libraries(TDVulkan INTERFACE Vulkan::Vulkan)
endif()

if(WIN32)
	# VK_KHR_external_memory_win32 / external_semaphore_win32 (shared handles).
	# vulkan.h then includes <windows.h>, so keep min/max macros out of C++ code.
	target_compile_definitions(TDVulkan INTERFACE VK_USE_PLATFORM_WIN32_KHR)
endif()

# -----------------------------------------------------------------------------
# Plugin
# -----------------------------------------------------------------------------
td_add_custom_op(VulkanTexturePanelCOMP
	FAMILY COMP
	CXX_STANDARD 20
	PYTHON
	INCLUDE_DIRECTORIES include
	LINK_LIBRARIES TDVulkan
	SOURCES
		VulkanTexturePanelCOMP.cpp
		VulkanTexturePanelCOMP.h
		include/BitMaskFlags.h
		include/CPlusPlus_Common.h
		include/COMP_CPlusPlusPanelBase.h
)

if(TARGET VulkanTexturePanelCOMP AND APPLE AND TD_VULKAN_DISPATCH STREQUAL "LOADER")
	# The SDK's libvulkan.1.dylib has install name @rpath/libvulkan.1.dylib.
	# Point the plugin at the SDK so it loads on this machine; this is a
	# development convenience, not something to ship (a distributed plugin
	# would need the loader + MoltenVK ICD bundled, or DYNAMIC mode).
	get_filename_component(_vk_libdir "${Vulkan_LIBRARY}" DIRECTORY)
	set_target_properties(VulkanTexturePanelCOMP PROPERTIES
		BUILD_RPATH "${_vk_libdir}"
		INSTALL_RPATH "${_vk_libdir}")
endif()
