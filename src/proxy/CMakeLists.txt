# vrclient_x64.dll: forwards every OpenVR export to the real SteamVR client and loads the layer.
add_library(dynamic_vrs_vrclient_proxy SHARED
    vrclient_proxy.cpp
    vrclient_proxy.def
)
set_target_properties(dynamic_vrs_vrclient_proxy PROPERTIES
    OUTPUT_NAME "vrclient_x64"
    PREFIX ""
)
target_link_libraries(dynamic_vrs_vrclient_proxy PRIVATE dynamic_vrs_common)
dvr_configure_target(dynamic_vrs_vrclient_proxy)
