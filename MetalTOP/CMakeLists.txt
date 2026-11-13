cmake_minimum_required(VERSION 4.4)

# Before project(): macOS deployment target.
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/TouchDesignerPreProject.cmake)
project(SimpleShapesPOP LANGUAGES CXX)

# Shared TouchDesigner custom-operator build logic (repository root /cmake).
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/TouchDesignerCustomOP.cmake)

td_add_custom_op(MetalTOP
	FAMILY TOP
	CXX_STANDARD 20
	INCLUDE_DIRECTORIES include
	SOURCES
		MetalTOP.cpp
		MetalTOP.h
		include/BitMaskFlags.h
		include/CPlusPlus_Common.h
		include/TOP_CPlusPlusBase.h
)
