# DynamicVRSPanel64.exe: the desktop control panel, started by the layer from its own directory.
# Win32 + Direct2D/DirectWrite only; the UI strings are compiled in as a resource.
add_executable(dynamic_vrs_control_panel WIN32
    control_endpoint.cpp
    control_endpoint.hpp
    control_panel.rc
    ini_defaults.cpp
    ini_defaults.hpp
    panel.cpp
    panel.hpp
    panel_common.hpp
    panel_control.cpp
    panel_input.cpp
    panel_layout.cpp
    panel_main.cpp
    panel_model.cpp
    panel_model.hpp
    panel_paint.cpp
    panel_renderer.cpp
    panel_renderer.hpp
    panel_resources.h
    panel_strings.cpp
    panel_strings.hpp
    panel_theme.cpp
    panel_theme.hpp
)
set_target_properties(dynamic_vrs_control_panel PROPERTIES OUTPUT_NAME "DynamicVRSPanel64")
set_source_files_properties(control_panel.rc PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/control_panel_strings.tsv"
)
target_include_directories(dynamic_vrs_control_panel PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(dynamic_vrs_control_panel PRIVATE dynamic_vrs_common d2d1 dwrite dwmapi dxgi)
dvr_configure_target(dynamic_vrs_control_panel)
