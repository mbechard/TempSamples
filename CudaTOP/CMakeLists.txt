cmake_minimum_required(VERSION 4.4)

# Before project(): macOS deployment target and CUDA Toolkit selection.
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/TouchDesignerPreProject.cmake)
project(CudaTOP LANGUAGES CXX)

# Shared TouchDesigner custom-operator build logic (repository root /cmake).
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/TouchDesignerCustomOP.cmake)

td_enable_cuda()

td_add_custom_op(CudaTOP
	FAMILY TOP
	CXX_STANDARD 20
	WINDOWS_ONLY
	CUDA
	INCLUDE_DIRECTORIES include
	SOURCES
		CudaTOP.cpp
		kernel.cu
		CudaTOP.h
		include/BitMaskFlags.h
		include/CPlusPlus_Common.h
		include/TOP_CPlusPlusBase.h
)
