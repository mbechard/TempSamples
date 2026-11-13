cmake_minimum_required(VERSION 4.4)

# Before project(): macOS deployment target and CUDA Toolkit selection.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/TouchDesignerPreProject.cmake)
project(CPlusPlusSamples LANGUAGES CXX)

# Builds every sample as one project/solution.
#
# Each sample folder also has its own CMakeLists.txt and can be configured on
# its own instead, e.g.:  cmake -S CPUMemoryTOP -B CPUMemoryTOP/build
#
# Samples whose dependencies (CUDA, TouchDesigner's Python, the Vulkan SDK) are
# not found, or that don't support the current platform, are skipped with a
# warning.

# Tells td_add_custom_op() to skip (not fail on) samples with missing dependencies.
set(TD_CUSTOM_OPS_ROOT_BUILD TRUE)

include(${CMAKE_CURRENT_LIST_DIR}/cmake/TouchDesignerCustomOP.cmake)

# CUDA must be enabled in the top-level directory for any sample to use it.
td_enable_cuda()

add_subdirectory(CHOP)
add_subdirectory(CHOPWithPythonClass)
add_subdirectory(CPUMemoryTOP)
if(WIN32)
	add_subdirectory(CudaPOP)
	add_subdirectory(CudaTOP)
endif()
if(APPLE)
	add_subdirectory(MetalPOP)
	add_subdirectory(MetalTOP)
endif()
add_subdirectory(DAT)
add_subdirectory(SimpleShapesPOP)
add_subdirectory(SimpleShapesSOP)
add_subdirectory(VulkanTexturePanelCOMP)

get_property(_td_skipped GLOBAL PROPERTY TD_SKIPPED_SAMPLES)
if(_td_skipped)
	list(JOIN _td_skipped ", " _td_skipped)
	message(STATUS "Skipped samples (see warnings above): ${_td_skipped}")
endif()
