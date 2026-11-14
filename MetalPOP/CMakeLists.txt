cmake_minimum_required(VERSION 4.4)

# Before project(): macOS deployment target.
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/TouchDesignerPreProject.cmake)
project(MetalPOP LANGUAGES CXX)

# Shared TouchDesigner custom-operator build logic (repository root /cmake).
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/TouchDesignerCustomOP.cmake)

td_add_custom_op(MetalPOP
	FAMILY POP
	CXX_STANDARD 20
	INCLUDE_DIRECTORIES include
	SOURCES
		MetalPOP.cpp
		MetalPOP.h
		include/BitMaskFlags.h
		include/CPlusPlus_Common.h
		include/POP_CPlusPlusBase.h
)
