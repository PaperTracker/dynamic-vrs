# Code every binary shares: logging, module paths and the dynamic-vrs.ini reader, plus the header-only
# gaze and control protocols.
add_library(dynamic_vrs_common STATIC
    gaze_packet.hpp
    gaze_udp_receiver.hpp
    ini_file.cpp
    ini_file.hpp
    log.cpp
    log.hpp
    openvr_abi.hpp
    paths.cpp
    paths.hpp
    vrs_control_protocol.hpp
)
# Consumers include with the src/ root, e.g. "common/log.hpp".
target_include_directories(dynamic_vrs_common PUBLIC "${PROJECT_SOURCE_DIR}/src")
dvr_configure_target(dynamic_vrs_common)
