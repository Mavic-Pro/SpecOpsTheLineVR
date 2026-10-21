cmake_minimum_required(VERSION 3.16)
project(SpecOpsVR LANGUAGES CXX C)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "SpecOpsTheLine.exe e' a 32 bit: configura con -A Win32 (es. cmake -B build -A Win32)")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(vendor/minhook)

# Ini predefinito incorporato nella dll (solo le righe attive): le voci che
# mancano nell'ini dell'utente prendono il valore distribuito, cosi' i
# predefiniti del codice e quelli dell'ini non divergono.
file(STRINGS "${CMAKE_SOURCE_DIR}/deploy/specopsvr.ini" SPECOPSVR_INI_LINES ENCODING UTF-8)
set(SPECOPSVR_DEFAULT_INI "")
foreach(line IN LISTS SPECOPSVR_INI_LINES)
    string(STRIP "${line}" line)
    if(line AND NOT line MATCHES "^#")
        string(APPEND SPECOPSVR_DEFAULT_INI "${line}\\n")
    endif()
endforeach()
configure_file("${CMAKE_SOURCE_DIR}/src/default_ini.h.in" "${CMAKE_BINARY_DIR}/generated/default_ini.h" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/deploy/specopsvr.ini")

# Ponte ProTubeVR a 64 bit (bridge/): sotto-progetto x64 costruito a parte e
# incorporato in d3d9.dll come risorsa, cosi' l'aggiornamento automatico (che
# scarica solo d3d9.dll) lo porta con se'.
include(ExternalProject)
set(PROTUBE_BRIDGE_EXE "${CMAKE_BINARY_DIR}/bridge-x64/Release/specopsvr_protube.exe")
ExternalProject_Add(protube_bridge
    SOURCE_DIR "${CMAKE_SOURCE_DIR}/bridge"
    BINARY_DIR "${CMAKE_BINARY_DIR}/bridge-x64"
    CMAKE_GENERATOR "${CMAKE_GENERATOR}"
    CMAKE_GENERATOR_PLATFORM x64
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config Release
    INSTALL_COMMAND ""
    BUILD_ALWAYS ON
    BUILD_BYPRODUCTS "${PROTUBE_BRIDGE_EXE}"
)
file(TO_CMAKE_PATH "${PROTUBE_BRIDGE_EXE}" PROTUBE_BRIDGE_EXE_RC)
configure_file("${CMAKE_SOURCE_DIR}/src/resources.rc.in" "${CMAKE_BINARY_DIR}/generated/resources.rc" @ONLY)

add_library(d3d9proxy SHARED
    ${CMAKE_BINARY_DIR}/generated/resources.rc
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
    src/pure_utils.h
    src/vr_toast.cpp
    src/vr_toast.h
    src/haptics_ext.cpp
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
    ${CMAKE_BINARY_DIR}/generated
    ${CMAKE_SOURCE_DIR}/src
)
add_dependencies(d3d9proxy protube_bridge)
set_source_files_properties(${CMAKE_BINARY_DIR}/generated/resources.rc PROPERTIES OBJECT_DEPENDS "${PROTUBE_BRIDGE_EXE}")

target_link_libraries(d3d9proxy PRIVATE minhook d3d11 dxgi winhttp bcrypt delayimp
    ${CMAKE_SOURCE_DIR}/vendor/openvr/lib/win32/openvr_api.lib)
# openvr_api.dll caricata alla prima chiamata (vr_session.cpp la cerca accanto
# al mod): se manca, il gioco parte senza VR invece di non avviarsi.
target_link_options(d3d9proxy PRIVATE /DELAYLOAD:openvr_api.dll)

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

# Test offline delle funzioni pure (updater, ini): build/Release/unit_tests.exe,
# nessun gioco e nessun visore (R-T2). tools/regress.py li lancia per primo.
add_executable(unit_tests tests/unit_tests.cpp)
target_include_directories(unit_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
