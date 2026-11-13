# External SDK roots. Every SDK is optional: the layer builds without NVAPI (no variable rate shading) and
# without the OpenVR or OpenXR headers (that API's support is left out). A root that is given must be valid.

set(OPENVR_SDK_ROOT "" CACHE PATH "Path to the official Valve OpenVR SDK checkout")
set(NVAPI_ROOT "" CACHE PATH "Path to the official NVIDIA NVAPI SDK checkout")
set(OPENXR_SDK_ROOT "" CACHE PATH "Path to the official Khronos OpenXR SDK checkout")

if(NVAPI_ROOT)
    set(NVAPI_LIBRARY "${NVAPI_ROOT}/amd64/nvapi64.lib")
    foreach(required_file IN ITEMS "${NVAPI_ROOT}/nvapi.h" "${NVAPI_LIBRARY}")
        if(NOT EXISTS "${required_file}")
            message(FATAL_ERROR "Official NVAPI SDK file not found: ${required_file}")
        endif()
    endforeach()
endif()

if(OPENVR_SDK_ROOT)
    set(OPENVR_INCLUDE_DIR "${OPENVR_SDK_ROOT}/headers")
    if(NOT EXISTS "${OPENVR_INCLUDE_DIR}/openvr.h")
        message(FATAL_ERROR "Official OpenVR SDK file not found: ${OPENVR_INCLUDE_DIR}/openvr.h")
    endif()
endif()

if(OPENXR_SDK_ROOT)
    set(OPENXR_INCLUDE_DIR "${OPENXR_SDK_ROOT}/include")
    if(NOT EXISTS "${OPENXR_INCLUDE_DIR}/openxr/openxr_loader_negotiation.h")
        message(FATAL_ERROR "Official OpenXR SDK headers not found in ${OPENXR_SDK_ROOT}")
    endif()
    if(NOT NVAPI_ROOT)
        message(FATAL_ERROR "OpenXR support requires NVAPI_ROOT")
    endif()
endif()
