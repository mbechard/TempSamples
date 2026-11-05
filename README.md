# TempSamples

TouchDesigner custom operator samples, built with [CMake](https://cmake.org/) 4.4 or newer. CMake generates Visual Studio solutions on Windows and Xcode projects (or Makefiles) on macOS from the same `CMakeLists.txt` files. The shared build logic lives in `cmake/` and is the same as in [CustomOperatorSamples](https://github.com/TouchDesigner/CustomOperatorSamples).

| Sample | Type | Needs |
| --- | --- | --- |
| [CHOP](CHOP) | CHOP | |
| [CHOPWithPythonClass](CHOPWithPythonClass) | CHOP | TouchDesigner's Python |
| [CPUMemoryTOP](CPUMemoryTOP) | TOP | |
| [CudaPOP](CudaPOP) | POP | CUDA 13 Toolkit, Windows only |
| [CudaTOP](CudaTOP) | TOP | CUDA 13 Toolkit, Windows only |
| [DAT](DAT) | DAT | |
| [SimpleShapesPOP](SimpleShapesPOP) | POP | |
| [SimpleShapesSOP](SimpleShapesSOP) | SOP | |
| [VulkanTexturePanelCOMP](VulkanTexturePanelCOMP) | COMP | TouchDesigner's Python, Vulkan SDK 1.2+ |

## Building

All samples at once, from the repository root:

```bash
cmake -S . -B build
cmake --build build --config Release
```

Samples whose dependencies aren't available, or that don't support the current platform, are skipped with a warning. To build one sample, configure its folder instead, e.g. `cmake -S CPUMemoryTOP -B CPUMemoryTOP/build`; a missing dependency is then an error.

The built plugins (`.dll` on Windows, `.plugin` bundles on macOS) are written to `build/plugins`. `cmake --install build --config Release` copies them into TouchDesigner's Plugins folder (`Documents/Derivative/Plugins` on Windows, `~/Library/Application Support/Derivative/TouchDesigner099/Plugins` on macOS). Visual Studio and Xcode are set up to launch TouchDesigner when you debug a plugin.

## TouchDesigner installation

These samples need a TouchDesigner 2026 or later series build, which uses CUDA 13 and ships OpenCV 5. Python and the debugger executable are taken from the TouchDesigner installation set by `TD_INSTALLATION_PATH`. If TouchDesigner is in its default location you don't need to set it:

* Windows: the install folder, default `C:/Program Files/Derivative/TouchDesigner`
* macOS: the application bundle, default `/Applications/TouchDesigner.app`

```bash
cmake -S . -B build -DTD_INSTALLATION_PATH="D:/Apps/TouchDesigner"
```

### Python

Samples that use Python are built against the Python that ships with TouchDesigner, so they match the interpreter they're loaded into. No separate Python install is needed.

* Windows: headers from `<TD_INSTALLATION_PATH>/bin/include` and the import library from `<TD_INSTALLATION_PATH>/bin/libs`. Whether TouchDesigner's Python is the free-threaded build is detected from the interpreter DLL it ships (`python314t.dll`), and `Py_GIL_DISABLED` is defined to match. These samples always use the release C runtime, because the debug runtime would make `pyconfig.h` switch to a debug Python.
* macOS: headers from `<TD_INSTALLATION_PATH>/Contents/Frameworks/Python.framework`. The plugin isn't linked against a Python library; it uses the Python that TouchDesigner has already loaded.

The Python version found is shown in the configure output, e.g. `Adding CHOP sample: CHOPWithPythonClass (Python 3.14t from TouchDesigner)`.

### CUDA (Windows only)

The CUDA samples need a CUDA 13 toolkit, the CUDA version TouchDesigner uses: the plugins link the shared CUDA runtime, and TouchDesigner only ships `cudart64_13.dll`. If several toolkits are installed, CMake picks the newest 13.x automatically, and it reports a toolkit of another major version instead of building with it. To choose one yourself, pass `-DCMAKE_CUDA_COMPILER=<path to nvcc.exe>`, or `-T cuda=13.0` with the Visual Studio generator, in a fresh build folder.

CUDA 13 supports GPUs from the Turing generation (GeForce GTX 16 / RTX 20 series) and newer, so the CUDA samples are compiled for Turing through Blackwell and won't run on older NVIDIA GPUs.

### Vulkan

VulkanTexturePanelCOMP needs the [Vulkan SDK](https://vulkan.lunarg.com/), found through the `VULKAN_SDK` environment variable the SDK installer sets (or `Vulkan_ROOT`). By default it only uses the Vulkan headers and resolves entry points at runtime from TouchDesigner's Vulkan; `-DTD_VULKAN_DISPATCH=LOADER` links the Vulkan loader instead (see the comments in its `CMakeLists.txt`).

## Adding a sample

Copy a sample folder, rename it, update its `CMakeLists.txt`, and add an `add_subdirectory()` line for it to the root `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 4.4)
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/TouchDesignerPreProject.cmake)   # before project()
project(MyTOP LANGUAGES CXX)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/TouchDesignerCustomOP.cmake)
# td_enable_cuda()      # only if the sample has .cu files

td_add_custom_op(MyTOP
	FAMILY TOP
	CXX_STANDARD 20
	# PYTHON            # build against TouchDesigner's Python
	# CUDA              # compile .cu files, link the CUDA runtime
	# WINDOWS_ONLY
	INCLUDE_DIRECTORIES include
	SOURCES
		MyTOP.cpp
		MyTOP.h
		include/BitMaskFlags.h
		include/CPlusPlus_Common.h
		include/TOP_CPlusPlusBase.h
)
```

On macOS, samples are compiled as Objective-C++ and linked with the Foundation and Metal frameworks automatically, because `CPlusPlus_Common.h` imports `<Metal/Metal.h>`.
