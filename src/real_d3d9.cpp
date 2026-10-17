#include "real_d3d9.h"
#include "logger.h"

#include <windows.h>
#include <string>

namespace vr::realdll {

Direct3DCreate9_t Direct3DCreate9_Real = nullptr;
Direct3DCreate9Ex_t Direct3DCreate9Ex_Real = nullptr;
D3DPERF_BeginEvent_t D3DPERF_BeginEvent_Real = nullptr;
D3DPERF_EndEvent_t D3DPERF_EndEvent_Real = nullptr;
D3DPERF_SetMarker_t D3DPERF_SetMarker_Real = nullptr;
D3DPERF_SetRegion_t D3DPERF_SetRegion_Real = nullptr;
D3DPERF_QueryRepeatFrame_t D3DPERF_QueryRepeatFrame_Real = nullptr;
D3DPERF_SetOptions_t D3DPERF_SetOptions_Real = nullptr;
D3DPERF_GetStatus_t D3DPERF_GetStatus_Real = nullptr;

namespace {

std::string ModuleDirectory() {
    char path[MAX_PATH] = {};
    HMODULE self = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCSTR>(&ModuleDirectory),
        &self);
    GetModuleFileNameA(self, path, MAX_PATH);
    std::string full(path);
    size_t pos = full.find_last_of("\\/");
    return pos == std::string::npos ? "." : full.substr(0, pos);
}

}  // namespace

bool Load() {
    // Il vero d3d9.dll si carica dalla cartella di sistema (per un processo a
    // 32 bit GetSystemDirectory restituisce SysWOW64): cosi' il pacchetto
    // della mod non deve contenere file di Windows. d3d9_orig.dll accanto al
    // proxy resta come ripiego (installazioni di sviluppo precedenti). Le
    // funzioni D3DPERF_* sono implementate in dllmain.cpp chiamando queste.
    char sysDir[MAX_PATH] = {};
    GetSystemDirectoryA(sysDir, MAX_PATH);
    std::string path = std::string(sysDir) + "\\d3d9.dll";

    HMODULE real = LoadLibraryA(path.c_str());
    if (!real) {
        path = ModuleDirectory() + "\\d3d9_orig.dll";
        real = LoadLibraryA(path.c_str());
    }
    if (!real) {
        vr::log::Line("ERROR: cannot load the real d3d9.dll from " + path);
        return false;
    }

    Direct3DCreate9_Real = reinterpret_cast<Direct3DCreate9_t>(
        GetProcAddress(real, "Direct3DCreate9"));
    Direct3DCreate9Ex_Real = reinterpret_cast<Direct3DCreate9Ex_t>(
        GetProcAddress(real, "Direct3DCreate9Ex"));

    if (!Direct3DCreate9_Real) {
        vr::log::Line("ERROR: Direct3DCreate9 not found in the real d3d9.dll");
        return false;
    }

    D3DPERF_BeginEvent_Real = reinterpret_cast<D3DPERF_BeginEvent_t>(
        GetProcAddress(real, "D3DPERF_BeginEvent"));
    D3DPERF_EndEvent_Real = reinterpret_cast<D3DPERF_EndEvent_t>(
        GetProcAddress(real, "D3DPERF_EndEvent"));
    D3DPERF_SetMarker_Real = reinterpret_cast<D3DPERF_SetMarker_t>(
        GetProcAddress(real, "D3DPERF_SetMarker"));
    D3DPERF_SetRegion_Real = reinterpret_cast<D3DPERF_SetRegion_t>(
        GetProcAddress(real, "D3DPERF_SetRegion"));
    D3DPERF_QueryRepeatFrame_Real = reinterpret_cast<D3DPERF_QueryRepeatFrame_t>(
        GetProcAddress(real, "D3DPERF_QueryRepeatFrame"));
    D3DPERF_SetOptions_Real = reinterpret_cast<D3DPERF_SetOptions_t>(
        GetProcAddress(real, "D3DPERF_SetOptions"));
    D3DPERF_GetStatus_Real = reinterpret_cast<D3DPERF_GetStatus_t>(
        GetProcAddress(real, "D3DPERF_GetStatus"));

    vr::log::Line("real d3d9.dll loaded from " + path);
    return true;
}

}  // namespace vr::realdll
