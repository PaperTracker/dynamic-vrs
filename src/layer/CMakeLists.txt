# DynamicVRSLayer64.dll: the layer itself, loaded by the proxy (OpenVR) or the OpenXR loader.
#
#   config/       settings read from dynamic-vrs.ini
#   control/      the shared-memory channel to the control panel, and starting the panel
#   vrs/          variable rate shading: rate images, gaze tracking, the D3D11 hooks
#   diagnostics/  debug blur, radius rings and the target alignment probe
#   openvr/       IVRCompositor::Submit hooks and the DLL entry points
#   openxr/       the OpenXR API layer
add_library(dynamic_vrs_layer SHARED
    config/layer_config.cpp
    config/layer_config.hpp
    control/control_server.cpp
    control/control_server.hpp
    control/panel_launcher.cpp
    control/panel_launcher.hpp
    diagnostics/alignment_probe.cpp
    diagnostics/alignment_probe.hpp
    diagnostics/debug_blur.cpp
    diagnostics/debug_blur.hpp
    openvr/openvr_layer.cpp
)
set_target_properties(dynamic_vrs_layer PROPERTIES
    OUTPUT_NAME "DynamicVRSLayer64"
    PREFIX ""
)
target_link_libraries(dynamic_vrs_layer PRIVATE dynamic_vrs_common d3dcompiler ws2_32)
dvr_configure_target(dynamic_vrs_layer)

if(NVAPI_ROOT)
    target_compile_definitions(dynamic_vrs_layer PRIVATE DVR_HAS_NVAPI=1)
    # Everything that creates or binds shading-rate images.
    target_sources(dynamic_vrs_layer PRIVATE
        control/frame_control.cpp
        control/frame_control.hpp
        diagnostics/gpu_frame_probe.hpp
        diagnostics/submit_diagnostics.cpp
        diagnostics/submit_diagnostics.hpp
        vrs/eye_projection.cpp
        vrs/eye_projection.hpp
        vrs/eye_viewport.hpp
        vrs/gaze_tracking.cpp
        vrs/gaze_tracking.hpp
        vrs/rate_image.cpp
        vrs/rate_image.hpp
        vrs/rate_pattern.cpp
        vrs/rate_pattern.hpp
        vrs/render_target_hook.cpp
        vrs/render_target_hook.hpp
        vrs/vrs_binding.cpp
        vrs/vrs_binding.hpp
        vrs/vrs_state.cpp
        vrs/vrs_state.hpp
    )
    target_include_directories(dynamic_vrs_layer PRIVATE "${NVAPI_ROOT}")
    target_link_libraries(dynamic_vrs_layer PRIVATE "${NVAPI_LIBRARY}")
endif()

# Per-eye gaze projection needs the OpenVR headers only. Interfaces are resolved at runtime through the
# openvr_api.dll exports, so linking openvr_api.lib is not required.
if(OPENVR_SDK_ROOT)
    target_compile_definitions(dynamic_vrs_layer PRIVATE DVR_HAS_OPENVR=1)
    target_include_directories(dynamic_vrs_layer PRIVATE "${OPENVR_INCLUDE_DIR}")
endif()

if(OPENXR_SDK_ROOT)
    target_sources(dynamic_vrs_layer PRIVATE
        openxr/openxr_layer.cpp
        openxr/view_geometry.cpp
        openxr/view_geometry.hpp
    )
    target_compile_definitions(dynamic_vrs_layer PRIVATE DVR_HAS_OPENXR=1 XR_USE_GRAPHICS_API_D3D11)
    target_include_directories(dynamic_vrs_layer PRIVATE "${OPENXR_INCLUDE_DIR}")
endif()
