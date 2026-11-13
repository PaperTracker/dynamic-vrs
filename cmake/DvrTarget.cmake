# dvr_configure_target(<target> [CXX_STANDARD <n>])
#
# Applies the settings every target in this project shares: static CRT, Unicode Win32 APIs, strict MSVC
# warnings and Control Flow Guard.
function(dvr_configure_target target)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "CXX_STANDARD" "")
    if(NOT ARG_CXX_STANDARD)
        set(ARG_CXX_STANDARD 20)
    endif()
    set_target_properties(${target} PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
    target_compile_features(${target} PRIVATE cxx_std_${ARG_CXX_STANDARD})
    target_compile_definitions(${target} PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
    if(MSVC)
        target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /EHsc /utf-8>)
        target_link_options(${target} PRIVATE /guard:cf)
    endif()
endfunction()
