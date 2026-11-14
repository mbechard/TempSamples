cmake_minimum_required(VERSION 4.4)

# Before project(): macOS deployment target.
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/TouchDesignerPreProject.cmake)
project(VulkanPOP LANGUAGES CXX)

# Shared TouchDesigner custom-operator build logic (repository root /cmake).
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/TouchDesignerCustomOP.cmake)

# -----------------------------------------------------------------------------
# Vulkan SDK: headers and the shader compiler
# -----------------------------------------------------------------------------
# Headers only: no Vulkan loader is linked. All Vulkan calls go through a
# vulkan.hpp dispatcher initialized from the vkGetInstanceProcAddr TouchDesigner
# provides (see VulkanPOP.h), so the plugin uses the Vulkan TouchDesigner is
# running. VK_NO_PROTOTYPES makes an accidental direct vk* call a compile error.
#
# 1.4 or newer: vulkan.hpp's dispatcher is vk::detail::DispatchLoaderDynamic
# from SDK 1.4 on. Uses $VULKAN_SDK (on macOS, the SDK's macOS/ subfolder) or
# Vulkan_ROOT.
set(VULKANPOP_VULKAN_MIN_VERSION 1.4)
find_package(Vulkan ${VULKANPOP_VULKAN_MIN_VERSION} QUIET COMPONENTS glslangValidator)
if(NOT Vulkan_FOUND OR NOT TARGET Vulkan::glslangValidator)
	td_skip_custom_op(VulkanPOP
		"the Vulkan SDK (${VULKANPOP_VULKAN_MIN_VERSION} or newer, with glslangValidator) was not found (install it and set VULKAN_SDK, or set Vulkan_ROOT)")
	return()
endif()

add_library(VulkanPOPVulkan INTERFACE)
target_link_libraries(VulkanPOPVulkan INTERFACE Vulkan::Headers)
target_compile_definitions(VulkanPOPVulkan INTERFACE VK_NO_PROTOTYPES)

# -----------------------------------------------------------------------------
# Compute shaders: GLSL -> SPIR-V, embedded as C arrays (<Name>Spv)
# -----------------------------------------------------------------------------
# SPIR-V 1.0 (Vulkan 1.0 environment) runs on every Vulkan TouchDesigner
# supports; OP_VulkanInfo::spirvMajorVersion/spirvMinorVersion give the maximum.
set(_spv_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
set(_spv_headers "")
foreach(shader IN ITEMS TransformPosition MakeCube MakeCubeIndices)
	set(_src "${CMAKE_CURRENT_SOURCE_DIR}/${shader}.comp")
	set(_out "${_spv_dir}/${shader}.spv.h")
	add_custom_command(
		OUTPUT "${_out}"
		COMMAND ${CMAKE_COMMAND} -E make_directory "${_spv_dir}"
		COMMAND Vulkan::glslangValidator -V --target-env vulkan1.0 --vn ${shader}Spv -o "${_out}" "${_src}"
		DEPENDS "${_src}"
		COMMENT "Compiling ${shader}.comp to SPIR-V"
		VERBATIM)
	list(APPEND _spv_headers "${_out}")
endforeach()

# -----------------------------------------------------------------------------
# Plugin
# -----------------------------------------------------------------------------
td_add_custom_op(VulkanPOP
	FAMILY POP
	CXX_STANDARD 20
	INCLUDE_DIRECTORIES include "${_spv_dir}"
	LINK_LIBRARIES VulkanPOPVulkan
	SOURCES
		VulkanPOP.cpp
		VulkanPOP.h
		TransformPosition.comp
		MakeCube.comp
		MakeCubeIndices.comp
		${_spv_headers}
		include/BitMaskFlags.h
		include/CPlusPlus_Common.h
		include/POP_CPlusPlusBase.h
)
