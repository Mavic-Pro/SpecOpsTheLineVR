cmake_minimum_required(VERSION 3.16)
project(SpecOpsVRProTubeBridge LANGUAGES CXX)

# Ponte ProTubeVR a 64 bit: costruito come sotto-progetto x64 dal CMakeLists
# principale (ExternalProject) e incorporato come risorsa in d3d9.dll.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "il ponte ProTube va costruito a 64 bit (-A x64)")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Runtime statico: nessuna dipendenza dai redistribuibili VC per l'eseguibile.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(specopsvr_protube WIN32 protube_bridge.cpp)
set_target_properties(specopsvr_protube PROPERTIES LINK_FLAGS "/ENTRY:wmainCRTStartup")
target_compile_definitions(specopsvr_protube PRIVATE UNICODE _UNICODE)
