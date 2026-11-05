cmake_minimum_required(VERSION 4.4.0)

# Must be set before project() to take effect.
set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version")

project(VulkanTexturePanelCOMP VERSION 1.0 LANGUAGES CXX)

# CPlusPlus_Common.h does `#import <Metal/Metal.h>` and uses id<MTL...> on macOS,
# so the plugin source has to be compiled as Objective-C++ there.
if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_EXTENSIONS OFF)

# -----------------------------------------------------------------------------
# Options
# -----------------------------------------------------------------------------
set(TD_CPLUSPLUS_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include" CACHE PATH
    "Directory containing CPlusPlus_Common.h, COMP_CPlusPlusPanelBase.h and BitMaskFlags.h")

option(TD_PLUGIN_INSTALL "Add an install step that copies the plugin into TouchDesigner's plugin folder" ON)
# option() leaves an existing cache entry's type alone, so one created earlier as
# a STRING (e.g. via the GUI's "Add Entry") would show as a text field.
# Force BOOL so it is always a checkbox.
set_property(CACHE TD_PLUGIN_INSTALL PROPERTY TYPE BOOL)

set(TD_PYTHON_VERSION_MAJOR 3)
set(TD_PYTHON_VERSION_MINOR 14)

if(NOT EXISTS "${TD_CPLUSPLUS_INCLUDE_DIR}/CPlusPlus_Common.h")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/CPlusPlus_Common.h")
        set(TD_CPLUSPLUS_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE PATH "" FORCE)
    else()
        message(FATAL_ERROR
            "CPlusPlus_Common.h not found in TD_CPLUSPLUS_INCLUDE_DIR (${TD_CPLUSPLUS_INCLUDE_DIR}).")
    endif()
endif()

# -----------------------------------------------------------------------------
# Python 3.14, free-threaded (python3.14t)
# -----------------------------------------------------------------------------
# Optional manual location, shown in the CMake GUI. Leave empty to let
# FindPython search the standard places. Python_EXECUTABLE can also be set.
set(Python_ROOT_DIR "" CACHE PATH
    "Root of the free-threaded Python 3.14 install to build against (empty = search)")
#
# ABI tuple: pydebug, pymalloc, unicode, gil_disabled
#   pydebug OFF     -> TouchDesigner's interpreter is a release build
#   gil_disabled ON -> only python3.14t is acceptable
set(Python_FIND_ABI "OFF" "ANY" "ANY" "ON")
# Stop at the first suitable install (honours hints) rather than the newest anywhere.
set(Python_FIND_STRATEGY LOCATION)

# Development.Module: headers everywhere, import lib on Windows, and
# -undefined dynamic_lookup on macOS (no libpython install name baked into
# the plugin, so it binds to TD's already-loaded Python).
find_package(Python
    ${TD_PYTHON_VERSION_MAJOR}.${TD_PYTHON_VERSION_MINOR}...<${TD_PYTHON_VERSION_MAJOR}.15
    REQUIRED COMPONENTS Development.Module)

if(NOT Python_FREE_THREADED)
    message(FATAL_ERROR "Found Python ${Python_VERSION}, but it is not a free-threaded build.")
endif()
message(STATUS "Python ${Python_VERSION} (free-threaded): ${Python_INCLUDE_DIRS}")

add_library(TDPython INTERFACE)
target_link_libraries(TDPython INTERFACE Python::Module)

if(WIN32)
    # Python::Module already carries Py_GIL_DISABLED=1 (Windows pyconfig.h is
    # shared between builds, so it can't say so itself) and Py_NO_LINK_LIB
    # (stops pyconfig.h's #pragma comment(lib) auto-linking).

    # Never let a Debug configuration pick up python314t_d.lib.
    get_target_property(_py_cfgs Python::Module IMPORTED_CONFIGURATIONS)
    if(_py_cfgs AND "RELEASE" IN_LIST _py_cfgs)
        set_target_properties(Python::Module PROPERTIES MAP_IMPORTED_CONFIG_DEBUG RELEASE)
    endif()
endif()

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
find_package(Vulkan ${TD_VULKAN_MIN_VERSION} REQUIRED)
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
elseif(APPLE)
    # VK_EXT_metal_objects: import/export MTLTexture / MTLSharedEvent, which is
    # how Vulkan output reaches TD's Metal execute mode. The plugin source is
    # already Objective-C++, so vulkan_metal.h gets real id<MTL...> types.
    target_compile_definitions(TDVulkan INTERFACE VK_USE_PLATFORM_METAL_EXT)
endif()

# -----------------------------------------------------------------------------
# Plugin
# -----------------------------------------------------------------------------
set(PLUGIN_SOURCES VulkanTexturePanelCOMP.cpp)
set(PLUGIN_HEADERS
    VulkanTexturePanelCOMP.h
    "${TD_CPLUSPLUS_INCLUDE_DIR}/COMP_CPlusPlusPanelBase.h"
    "${TD_CPLUSPLUS_INCLUDE_DIR}/CPlusPlus_Common.h"
    "${TD_CPLUSPLUS_INCLUDE_DIR}/BitMaskFlags.h")

add_library(VulkanTexturePanelCOMP MODULE ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})

target_include_directories(VulkanTexturePanelCOMP PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${TD_CPLUSPLUS_INCLUDE_DIR}")

target_link_libraries(VulkanTexturePanelCOMP PRIVATE TDPython TDVulkan)

if(APPLE AND TD_VULKAN_DISPATCH STREQUAL "LOADER")
    # The SDK's libvulkan.1.dylib has install name @rpath/libvulkan.1.dylib.
    # Point the plugin at the SDK so it loads on this machine; this is a
    # development convenience, not something to ship (a distributed plugin
    # would need the loader + MoltenVK ICD bundled, or DYNAMIC mode).
    get_filename_component(_vk_libdir "${Vulkan_LIBRARY}" DIRECTORY)
    set_target_properties(VulkanTexturePanelCOMP PROPERTIES
        BUILD_RPATH "${_vk_libdir}"
        INSTALL_RPATH "${_vk_libdir}")
endif()

set_target_properties(VulkanTexturePanelCOMP PROPERTIES
    PREFIX "")

if(MSVC)
    target_compile_definitions(VulkanTexturePanelCOMP PRIVATE
        WIN32 _WINDOWS _USRDLL UNICODE _UNICODE)
    target_compile_options(VulkanTexturePanelCOMP PRIVATE /W3 /permissive- /utf-8)

    set_target_properties(VulkanTexturePanelCOMP PROPERTIES
        # /MD in every configuration: /MDd defines _DEBUG, which makes pyconfig.h
        # define Py_DEBUG and changes the object layout TD's interpreter expects.
        MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
        MSVC_DEBUG_INFORMATION_FORMAT "ProgramDatabase"
        INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)

    # PDBs for optimized builds too, like the original project.
    target_link_options(VulkanTexturePanelCOMP PRIVATE
        /DEBUG
        $<$<NOT:$<CONFIG:Debug>>:/OPT:REF /OPT:ICF /INCREMENTAL:NO>)

    set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT VulkanTexturePanelCOMP)
endif()

if(APPLE)
    set_source_files_properties(${PLUGIN_SOURCES} PROPERTIES LANGUAGE OBJCXX)
    target_compile_options(VulkanTexturePanelCOMP PRIVATE
        $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    target_link_libraries(VulkanTexturePanelCOMP PRIVATE
        "-framework Foundation" "-framework Metal")

    set_target_properties(VulkanTexturePanelCOMP PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "plugin"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME "VulkanTexturePanelCOMP"
        MACOSX_BUNDLE_GUI_IDENTIFIER "ca.derivative.cpp.VulkanTexturePanelCOMP"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "ca.derivative.cpp.VulkanTexturePanelCOMP"
        # Product > Archive only copies targets that install into the archive.
        # Bundle targets default to SKIP_INSTALL=YES with no install path, which
        # leaves the archive without the .plugin.
        XCODE_ATTRIBUTE_SKIP_INSTALL NO
        # Archives place products at Products/$(INSTALL_PATH); "/" puts the
        # .plugin directly under Products.
        XCODE_ATTRIBUTE_INSTALL_PATH "/"
        # Own scheme, so Archive can be run on this target rather than ALL_BUILD.
        XCODE_GENERATE_SCHEME TRUE)
endif()

# Plugin source at the project root in VS/Xcode, TD API headers in their own folder.
source_group("" FILES ${PLUGIN_SOURCES} VulkanTexturePanelCOMP.h)
source_group("TD API" FILES
    "${TD_CPLUSPLUS_INCLUDE_DIR}/COMP_CPlusPlusPanelBase.h"
    "${TD_CPLUSPLUS_INCLUDE_DIR}/CPlusPlus_Common.h"
    "${TD_CPLUSPLUS_INCLUDE_DIR}/BitMaskFlags.h")

# CMake always creates CMAKE_INSTALL_PREFIX, but nothing here uses it (the
# install destination below is absolute), so hide it from the GUI's basic view.
mark_as_advanced(CMAKE_INSTALL_PREFIX)

# -----------------------------------------------------------------------------
# Install: copy the plugin into TouchDesigner's per-user plugin folder
#   Windows: Documents/Derivative/Plugins                         (.dll)
#   macOS:   ~/Library/Application Support/Derivative/TouchDesigner099/Plugins (.plugin)
#
#   cmake --install build --config Release
#   (or build the INSTALL / install target in Visual Studio / Xcode)
#
# Turn off with -DTD_PLUGIN_INSTALL=OFF; change the folder with
# -DTD_PLUGIN_INSTALL_DIR=...
#
# This deliberately doesn't go through CMAKE_INSTALL_PREFIX: that is cached on
# the first configure, so an existing build directory would keep the old value.
# -----------------------------------------------------------------------------
if(TD_PLUGIN_INSTALL AND (WIN32 OR APPLE))
    if(NOT TD_PLUGIN_INSTALL_DIR)
        if(WIN32)
            # Ask the shell for the real Documents folder, which may be redirected
            # (e.g. to OneDrive) rather than living at %USERPROFILE%\Documents.
            execute_process(
                COMMAND powershell -NoProfile -NonInteractive -Command
                        "[Environment]::GetFolderPath('MyDocuments')"
                OUTPUT_VARIABLE _td_documents
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
            if(NOT _td_documents)
                set(_td_documents "$ENV{USERPROFILE}/Documents")
            endif()
            file(TO_CMAKE_PATH "${_td_documents}/Derivative/Plugins" _td_plugins_dir)
        else()
            set(_td_plugins_dir
                "$ENV{HOME}/Library/Application Support/Derivative/TouchDesigner099/Plugins")
        endif()
        set(TD_PLUGIN_INSTALL_DIR "${_td_plugins_dir}" CACHE PATH
            "Where the install step copies the plugin" FORCE)
    endif()
    message(STATUS "Plugin install directory: ${TD_PLUGIN_INSTALL_DIR}")

    # A MODULE library (.dll, or the .plugin bundle on macOS) installs through
    # the LIBRARY destination.
    install(TARGETS VulkanTexturePanelCOMP
        LIBRARY DESTINATION "${TD_PLUGIN_INSTALL_DIR}"
        RUNTIME DESTINATION "${TD_PLUGIN_INSTALL_DIR}")
endif()
