# dynamic_vrs_proxy_probe.exe: checks the proxy exports; build.ps1 expects it and
# scripts/openvr/stage.ps1 stages it next to the proxy.
add_executable(dynamic_vrs_proxy_probe proxy_probe.cpp)
dvr_configure_target(dynamic_vrs_proxy_probe)
