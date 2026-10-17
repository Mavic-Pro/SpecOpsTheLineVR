cmake_minimum_required(VERSION 3.16)
project(SpecOpsVR LANGUAGES CXX C)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "SpecOpsTheLine.exe e' a 32 bit: configura con -A Win32 (es. cmake -B build -A Win32)")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(vendor/minhook)

add_library(d3d9proxy SHARED
    src/dllmain.cpp
    src/logger.cpp
    src/logger.h
    src/real_d3d9.cpp
    src/real_d3d9.h
    src/hooks.cpp
    src/hooks.h
    src/control.cpp
    src/control.h
    src/screenshot.cpp
    src/screenshot.h
    src/vr_output.cpp
    src/vr_output.h
    src/display_spoof.cpp
    src/display_spoof.h
    src/vr_input.cpp
    src/vr_session.cpp
    src/script_call.cpp
    src/updater.cpp
    src/updater.h
    src/version.h
    src/vr_input.h
    src/d3d9proxy.def
)

set_target_properties(d3d9proxy PROPERTIES
    OUTPUT_NAME "d3d9"
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/deploy"
)

target_include_directories(d3d9proxy PRIVATE
    ${CMAKE_SOURCE_DIR}/vendor/openvr/headers
)

target_link_libraries(d3d9proxy PRIVATE minhook d3d11 dxgi winhttp
    ${CMAKE_SOURCE_DIR}/vendor/openvr/lib/win32/openvr_api.lib)

add_custom_command(TARGET d3d9proxy POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_SOURCE_DIR}/vendor/openvr/bin/win32/openvr_api.dll"
        "${CMAKE_SOURCE_DIR}/deploy/openvr_api.dll"
)

# Copia il vero d3d9.dll a 32 bit (SysWOW64, cmake.exe e' un processo a 64
# bit quindi niente redirect automatico qui) accanto al proxy: serve ai
# forward D3DPERF_* ecc. definiti in d3d9proxy.def.
add_custom_command(TARGET d3d9proxy POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        "$ENV{WINDIR}/SysWOW64/d3d9.dll"
        "${CMAKE_SOURCE_DIR}/deploy/d3d9_orig.dll"
)

# Eseguibile di test standalone per OpenVR (nessuna dipendenza dal gioco,
# serve solo a verificare che SteamVR/Quest 3 siano raggiungibili prima di
# integrare il tracking nella DLL del mod).
add_executable(vrtest tools/vrtest/vrtest.cpp)
target_include_directories(vrtest PRIVATE ${CMAKE_SOURCE_DIR}/vendor/openvr/headers)
target_link_libraries(vrtest PRIVATE ${CMAKE_SOURCE_DIR}/vendor/openvr/lib/win32/openvr_api.lib)
set_target_properties(vrtest PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/deploy")
add_custom_command(TARGET vrtest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_SOURCE_DIR}/vendor/openvr/bin/win32/openvr_api.dll"
        "${CMAKE_SOURCE_DIR}/deploy/openvr_api.dll"
)
