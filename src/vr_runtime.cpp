#include "vr_runtime.h"
#include "vr_openxr.h"
#include "control.h"
#include "logger.h"

#include <windows.h>
#include <tlhelp32.h>
#include <openvr.h>

#include <atomic>
#include <cmath>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace vr::rt {

namespace {

std::atomic<Backend> g_active{Backend::None};
std::mutex g_cfgMutex;
std::string g_choice;      // "" = non ancora letto
bool g_autoSettled = false;  // runtime auto: backend scelto (un Init riuscito)
std::string g_openxrJson;  // override del json del runtime OpenXR

// La sessione parte da CreateDevice, prima che l'ini sia caricato dai hook:
// "runtime" e "openxrjson" si leggono direttamente dal file (come fa
// l'updater per autoupdate).
void ReadChoiceFromIni() {
    std::lock_guard<std::mutex> lock(g_cfgMutex);
    if (!g_choice.empty()) return;
    g_choice = "auto";  // predefinito dal 22/10 (configversion 9)
    std::ifstream f(vr::control::GameDirectory() + "\\specopsvr.ini");
    std::string line;
    while (f && std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);
        std::istringstream in(line);
        std::string key, v;
        in >> key;
        for (auto& c : key) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (key == "runtime") {
            in >> v;
            for (auto& c : v) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            if (v == "openxr" || v == "auto" || v == "steamvr" || v == "openvr") g_choice = v == "openvr" ? "steamvr" : v;
        } else if (key == "openxrjson") {
            std::string rest;
            std::getline(in, rest);
            size_t a = rest.find_first_not_of(" \t");
            g_openxrJson = a == std::string::npos ? "" : rest.substr(a);
            if (g_openxrJson == "auto" || g_openxrJson == "off") g_openxrJson.clear();
        }
    }
}

// --- OpenVR (SteamVR) ---------------------------------------------------------

// openvr_api.dll e' caricata a richiesta (/DELAYLOAD): se manca o e' stata
// messa in quarantena il gioco parte lo stesso e il mod resta inattivo, invece
// di non avviarsi affatto per un import mancante (M6).
bool OpenVrDllAvailable() {
    static int state = 0;  // 0 non provato, 1 ok, -1 mancante
    if (state) return state > 0;
    std::string path = vr::control::GameDirectory() + "\\openvr_api.dll";
    HMODULE h = LoadLibraryA(path.c_str());
    if (!h) h = LoadLibraryA("openvr_api.dll");
    state = h ? 1 : -1;
    if (!h) vr::log::Line("ERROR: openvr_api.dll not found next to d3d9.dll (error " + std::to_string(GetLastError()) +
                          "): no SteamVR, the game runs normally. Reinstall the mod.");
    return h != nullptr;
}

bool OpenVrInit(vr::EVRApplicationType type, std::string* error) {
    if (!OpenVrDllAvailable()) {
        if (error) *error = "openvr_api.dll missing";
        return false;
    }
    vr::EVRInitError err = vr::VRInitError_None;
    vr::VR_Init(&err, type);
    if (err != vr::VRInitError_None) {
        if (error) *error = vr::VR_GetVRInitErrorAsEnglishDescription(err);
        return false;
    }
    return true;
}

// vrserver.exe in esecuzione (per accorgersi di SteamVR morto senza evento).
bool VrServerRunning() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return true;  // nel dubbio: vivo
    PROCESSENTRY32W e = {sizeof(e)};
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &e); ok && !found; ok = Process32NextW(snap, &e))
        found = _wcsicmp(e.szExeFile, L"vrserver.exe") == 0;
    CloseHandle(snap);
    return found;
}

float OpenVrSecondsToPhotons() {
    vr::IVRSystem* sys = vr::VRSystem();
    if (!sys) return 0.0f;
    static float hz = 0.0f, vsyncToPhotons = 0.0f;
    static ULONGLONG lastProps = 0;
    if (GetTickCount64() - lastProps > 2000) {
        lastProps = GetTickCount64();
        hz = sys->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_DisplayFrequency_Float);
        vsyncToPhotons = sys->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,
                                                            vr::Prop_SecondsFromVsyncToPhotons_Float);
    }
    float frame = hz > 30.0f ? 1.0f / hz : 1.0f / 90.0f;
    float sinceVsync = 0.0f;
    uint64_t counter = 0;
    if (!sys->GetTimeSinceLastVsync(&sinceVsync, &counter)) sinceVsync = 0.0f;
    // Con Link il valore puo' crescere oltre un frame (vsync non aggiornato):
    // conta solo la posizione dentro il frame corrente.
    sinceVsync = std::fmod(sinceVsync < 0.0f ? 0.0f : sinceVsync, frame);
    return frame - sinceVsync + vsyncToPhotons;
}

}  // namespace

Backend Active() { return g_active; }
const char* BackendName() {
    switch (g_active) {
    case Backend::OpenVR: return "SteamVR (OpenVR)";
    case Backend::OpenXR: return "OpenXR";
    default: return "none";
    }
}

void Configure(const std::string& choice, const std::string& openxrJson) {
    std::lock_guard<std::mutex> lock(g_cfgMutex);
    if (!choice.empty()) g_choice = choice;
    if (openxrJson != "\x01") g_openxrJson = openxrJson;
}
std::string Choice() {
    std::lock_guard<std::mutex> lock(g_cfgMutex);
    return g_choice;
}

bool Init(vr::EVRApplicationType type, std::string* error) {
    ReadChoiceFromIni();
    std::string choice, json;
    {
        std::lock_guard<std::mutex> lock(g_cfgMutex);
        choice = g_choice;
        json = g_openxrJson;
    }
    if (!g_autoSettled && choice == "auto" && json.empty()) {
        // Virtual Desktop preferito quando l'utente lo usa (streamer avviato,
        // richiesta 22/10): VDXR regge 120 Hz Godlike, VD attraverso SteamVR no.
        // Altrimenti SteamVR: il backend OpenXR su Link (runtime Oculus) non e'
        // ancora provato nel visore, e con la migrazione a "auto" tutti i
        // giocatori Link ci passerebbero (scelta prudente, 22/10). VDXR che non
        // parte (visore non ancora collegato) -> SteamVR, e finche' nessuno e'
        // partito ogni nuovo tentativo (ogni 5 s) riprova VDXR: il visore
        // collegato a VD dopo l'avvio del gioco prende VDXR. Log solo quando cambia.
        static std::string lastLog;
        std::string log;
        std::vector<std::pair<std::string, std::string>> tries;  // json, motivo
        std::string vd = vr::xr::VirtualDesktopRunning() ? vr::xr::VirtualDesktopJson() : std::string();
        if (!vd.empty()) tries.push_back({vd, "Virtual Desktop streamer running"});
        for (const auto& t : tries) {
            std::string e;
            g_active = Backend::OpenXR;
            if (vr::xr::Init(t.first, &e)) {
                vr::log::Line(std::string("VR runtime: OpenXR (runtime auto: ") + t.second + ").");
                std::lock_guard<std::mutex> lock(g_cfgMutex);
                g_openxrJson = t.first;  // i tentativi successivi restano su questo runtime
                g_autoSettled = true;
                return true;
            }
            log += "VR runtime: OpenXR (runtime auto: " + t.second + ") not available (" + e + "), using SteamVR.\n";
            vr::xr::Unload();
        }
        g_active = Backend::OpenVR;
        log += std::string("VR runtime: ") + BackendName() + " (runtime auto: " +
               (vd.empty() ? "Virtual Desktop streamer not running" : "Virtual Desktop not connected") + ").";
        if (log != lastLog) {
            lastLog = log;
            for (size_t a = 0, b; a < log.size(); a = b + 1) {
                b = log.find('\n', a);
                if (b == std::string::npos) b = log.size();
                vr::log::Line(log.substr(a, b - a));
            }
        }
        bool ok = OpenVrInit(type, error);
        if (ok) g_autoSettled = true;
        return ok;
    }
    if (g_active == Backend::None) {
        // Scelta del backend una volta sola per partita (loggata).
        Backend b = Backend::OpenVR;
        std::string why;
        if (choice == "openxr") {
            b = Backend::OpenXR;
            why = "runtime openxr";
        } else if (choice == "auto") {
            // OpenXR se il runtime attivo e' nativo (Oculus, Virtual Desktop,
            // Pimax...): senza passare da SteamVR. SteamVR come runtime OpenXR
            // non conviene: OpenVR e' la sua via diretta.
            std::string name;
            if (vr::xr::ActiveRuntime(json, &name) && name.find("steamxr") == std::string::npos) {
                b = Backend::OpenXR;
                why = "runtime auto: OpenXR runtime " + name;
            } else {
                why = name.empty() ? "runtime auto: no OpenXR runtime, SteamVR" : "runtime auto: OpenXR runtime is " + name + ", using SteamVR directly";
            }
        } else {
            why = "runtime steamvr";
        }
        g_active = b;
        vr::log::Line(std::string("VR runtime: ") + BackendName() + " (" + why + ").");
    }
    if (g_active == Backend::OpenXR) return vr::xr::Init(json, error);
    return OpenVrInit(type, error);
}

void Shutdown() {
    if (g_active == Backend::OpenXR) vr::xr::Shutdown();
    else if (g_active == Backend::OpenVR) vr::VR_Shutdown();
}

bool Alive() {
    if (g_active == Backend::OpenXR) return vr::xr::Alive();
    return VrServerRunning();
}

bool SceneOnly() { return g_active == Backend::OpenXR; }

vr::IVRSystem* System() { return g_active == Backend::OpenXR ? vr::xr::System() : vr::VRSystem(); }
vr::IVRCompositor* Compositor() { return g_active == Backend::OpenXR ? vr::xr::Compositor() : vr::VRCompositor(); }
vr::IVROverlay* Overlay() { return g_active == Backend::OpenXR ? vr::xr::Overlay() : vr::VROverlay(); }
vr::IVRInput* Input() { return g_active == Backend::OpenXR ? vr::xr::Input() : vr::VRInput(); }
vr::IVRRenderModels* RenderModels() {
    return g_active == Backend::OpenXR ? vr::xr::RenderModels() : vr::VRRenderModels();
}
vr::IVRSettings* Settings() { return g_active == Backend::OpenXR ? vr::xr::Settings() : vr::VRSettings(); }
vr::IVRApplications* Applications() {
    return g_active == Backend::OpenXR ? vr::xr::Applications() : vr::VRApplications();
}

ID3D11Device* SharedD3D11() { return g_active == Backend::OpenXR ? vr::xr::D3D11Device() : nullptr; }

float SecondsToPhotons() {
    if (g_active == Backend::OpenXR) return vr::xr::SecondsToPhotons();
    if (g_active == Backend::OpenVR) return OpenVrSecondsToPhotons();
    return 0.0f;
}

}  // namespace vr::rt
