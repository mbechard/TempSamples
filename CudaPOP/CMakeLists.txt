cmake_minimum_required(VERSION 4.4)

# Before project(): macOS deployment target and CUDA Toolkit selection.
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/TouchDesignerPreProject.cmake)
project(CudaPOP LANGUAGES CXX)

# Shared TouchDesigner custom-operator build logic (repository root /cmake).
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/TouchDesignerCustomOP.cmake)

td_enable_cuda()

td_add_custom_op(CudaPOP
	FAMILY POP
	CXX_STANDARD 20
	WINDOWS_ONLY
	CUDA
	INCLUDE_DIRECTORIES include
	SOURCES
		CudaPOP.cpp
		kernel.cu
		CudaPOP.h
		include/BitMaskFlags.h
		include/CPlusPlus_Common.h
		include/POP_CPlusPlusBase.h
)
