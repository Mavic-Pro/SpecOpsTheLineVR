#include "audio_diag.h"
#include <windows.h>
#include <d3d9.h>

#include <atomic>
#include <cstdio>
#include <string>

#include "logger.h"
#include "real_d3d9.h"
#include "hooks.h"
#include "display_spoof.h"
#include "vr_input.h"
#include "updater.h"
#include "haptics_ext.h"
#include "control.h"
#include "game_addrs.h"
#include "vr_toast.h"

namespace {

std::atomic<bool> g_createDeviceHookInstalled = false;

// La mod e' stata scritta sull'eseguibile Steam (build del 12/07/2012). Dal
// 28/10 gli indirizzi del gioco si risolvono all'avvio (game_addrs.cpp: byte
// attesi all'indirizzo Steam, altrimenti ricerca per firma), quindi una copia
// non Steam funziona se le firme si trovano; quelle mancanti spengono solo le
// funzioni che ne dipendono. L'impronta dell'exe resta nel log e l'avviso nel
// visore (vr::control::g_nonSteamExe) dice quante funzioni mancano.
constexpr DWORD kSupportedTimeDateStamp = 0x50041F0C;
constexpr DWORD kSupportedSizeOfImage = 0x012C7000;
bool g_supportedExe = true;

bool CheckGameExecutable() {
    auto base = reinterpret_cast<const BYTE*>(GetModuleHandleA(nullptr));
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    DWORD stamp = nt->FileHeader.TimeDateStamp, size = nt->OptionalHeader.SizeOfImage;
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char b[400];
    std::snprintf(b, sizeof(b), "Executable: %s, fingerprint 0x%08lX / 0x%08lX (supported 0x%08lX / 0x%08lX)", exe,
                  stamp, size, kSupportedTimeDateStamp, kSupportedSizeOfImage);
    vr::log::Line(b);
    return stamp == kSupportedTimeDateStamp && size == kSupportedSizeOfImage;
}

void EnsureCreateDeviceHooked(IDirect3D9* d3d9) {
    bool already = g_createDeviceHookInstalled.exchange(true);
    if (already || !d3d9) return;
    vr::hooks::InstallIDirect3D9Hook(d3d9);
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        vr::log::Init();
        vr::log::InstallCrashHandler();
        vr::log::Line("SpecOpsVR proxy d3d9.dll loaded into the game.");
        if (!vr::realdll::Load()) {
            vr::log::Line("FATAL ERROR: cannot load the real d3d9.dll, the proxy will not work.");
        }
        vr::updater::Start();  // anche con un eseguibile non supportato: una versione nuova potrebbe supportarlo
        g_supportedExe = CheckGameExecutable();
        vr::addr::Resolve();
        if (!g_supportedExe || !vr::addr::AllAtSteamAddresses()) {
            vr::control::g_nonSteamExe = true;
            vr::log::Line(std::string("NOTE: this is not the Steam executable the mod was built against (non-Steam "
                                      "copy or other version): ") +
                          (vr::addr::MissingList().empty()
                               ? "every game address was found, everything should work."
                               : "some game addresses were not found, the features that need them stay off (see the "
                                 "ADDR lines above). Please send this log to the mod's GitHub issues."));
        }
        vr::display::InstallDesktopSizeSpoof();
        vr::input::InstallXInputHooks();
        vr::haptics::Start();
    }
    return TRUE;
}

IDirect3D9* WINAPI Direct3DCreate9(UINT SDKVersion) {
    vr::log::Line("Direct3DCreate9 called by the game (SDKVersion=" + std::to_string(SDKVersion) + ").");
    if (!vr::realdll::Direct3DCreate9_Real) {
        vr::log::Line("ERROR: real Direct3DCreate9 not available.");
        return nullptr;
    }
    if (vr::hooks::GpuShareRequested() && vr::realdll::Direct3DCreate9Ex_Real) {
        IDirect3D9Ex* ex = nullptr;
        if (SUCCEEDED(vr::realdll::Direct3DCreate9Ex_Real(SDKVersion, &ex)) && ex) {
            vr::log::Line("GPUSHARE: D3D9 object created as D3D9Ex.");
            vr::hooks::g_d3d9ex = true;
            EnsureCreateDeviceHooked(ex);
            return ex;
        }
        vr::log::Line("GPUSHARE: Direct3DCreate9Ex failed, normal mode.");
    }
    IDirect3D9* real = vr::realdll::Direct3DCreate9_Real(SDKVersion);
    EnsureCreateDeviceHooked(real);
    return real;
}

int WINAPI D3DPERF_BeginEvent(D3DCOLOR col, LPCWSTR wszName) {
    return vr::realdll::D3DPERF_BeginEvent_Real ? vr::realdll::D3DPERF_BeginEvent_Real(col, wszName) : 0;
}

int WINAPI D3DPERF_EndEvent(void) {
    return vr::realdll::D3DPERF_EndEvent_Real ? vr::realdll::D3DPERF_EndEvent_Real() : 0;
}

void WINAPI D3DPERF_SetMarker(D3DCOLOR col, LPCWSTR wszName) {
    if (vr::realdll::D3DPERF_SetMarker_Real) vr::realdll::D3DPERF_SetMarker_Real(col, wszName);
}

void WINAPI D3DPERF_SetRegion(D3DCOLOR col, LPCWSTR wszName) {
    if (vr::realdll::D3DPERF_SetRegion_Real) vr::realdll::D3DPERF_SetRegion_Real(col, wszName);
}

BOOL WINAPI D3DPERF_QueryRepeatFrame(void) {
    return vr::realdll::D3DPERF_QueryRepeatFrame_Real ? vr::realdll::D3DPERF_QueryRepeatFrame_Real() : FALSE;
}

void WINAPI D3DPERF_SetOptions(DWORD dwOptions) {
    if (vr::realdll::D3DPERF_SetOptions_Real) vr::realdll::D3DPERF_SetOptions_Real(dwOptions);
}

DWORD WINAPI D3DPERF_GetStatus(void) {
    return vr::realdll::D3DPERF_GetStatus_Real ? vr::realdll::D3DPERF_GetStatus_Real() : 0;
}

HRESULT WINAPI Direct3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex** ppD3D) {
    vr::log::Line("Direct3DCreate9Ex called by the game (SDKVersion=" + std::to_string(SDKVersion) + ").");
    if (!vr::realdll::Direct3DCreate9Ex_Real) {
        vr::log::Line("ERROR: real Direct3DCreate9Ex not available.");
        return E_NOTIMPL;
    }
    HRESULT hr = vr::realdll::Direct3DCreate9Ex_Real(SDKVersion, ppD3D);
    if (SUCCEEDED(hr) && ppD3D) {
        EnsureCreateDeviceHooked(reinterpret_cast<IDirect3D9*>(*ppD3D));
    }
    return hr;
}
