cmake_minimum_required(VERSION 3.25)

project(DynamicVRS
    DESCRIPTION "Eye-tracked variable rate shading layer for OpenVR and OpenXR D3D11 applications"
    HOMEPAGE_URL "https://github.com/PaperTracker/dynamic-vrs"
    LANGUAGES CXX RC
)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Dynamic VRS only supports 64-bit Windows.")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(DvrSdks)
include(DvrTarget)

# Every binary lands in <build>/<config>, where build.ps1 and scripts/ look for them.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>")

# Shipped binaries. Source files include each other from the src/ root, for example "common/log.hpp" or
# "layer/vrs/vrs_state.hpp"; headers in the same folder are included by bare name.
add_subdirectory(src/common)          # dynamic_vrs_common: logging and paths, plus the shared protocol headers
add_subdirectory(src/proxy)           # vrclient_x64.dll
add_subdirectory(src/layer)           # DynamicVRSLayer64.dll
add_subdirectory(src/control_panel)   # DynamicVRSPanel64.exe

# The layer starts the panel, so a layer build produces both.
add_dependencies(dynamic_vrs_layer dynamic_vrs_control_panel)

# Developer tools.
add_subdirectory(tools/diagnostics)   # dynamic_vrs_proxy_probe.exe
