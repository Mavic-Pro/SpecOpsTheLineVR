// Audio: dispositivo di riproduzione predefinito di Windows nel log e
// instradamento dell'audio del gioco quando il predefinito cambia.
//
// Il gioco (FMOD) apre il dispositivo predefinito all'avvio e non lo segue
// piu'. Virtual Desktop rende predefinito il proprio dispositivo quando parte
// lo streaming: avviato il gioco prima (dal PC, per i test), l'audio restava
// sulle casse del PC ("mancava l'audio", 22/10: nel log "Realtek Digital
// Output" all'avvio). Windows sa spostare una app su un altro dispositivo
// (Impostazioni > Audio > preferenze per app): la stessa API, non documentata
// ma stabile dal 1803 (EarTrumpet, SoundSwitch), sposta qui il processo del
// gioco sul nuovo predefinito; all'uscita la preferenza viene tolta.
#include "audio_diag.h"
#include "logger.h"
#define INITGUID
#include <windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <roapi.h>
#include <winstring.h>
#include <inspectable.h>
#include <string>

namespace vr::audio {
namespace {

std::string ToUtf8(const wchar_t* w) {
    if (!w) return std::string();
    char buf[512];
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf), nullptr, nullptr);
    return n > 0 ? std::string(buf) : std::string();
}

// Nome e id MMDevice ("{0.0.0.00000000}.{guid}") del predefinito.
bool DefaultDevice(std::string* name, std::wstring* id) {
    bool ok = false;
    IMMDeviceEnumerator* en = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                   reinterpret_cast<void**>(&en))) && en) {
        IMMDevice* dev = nullptr;
        if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) && dev) {
            LPWSTR wid = nullptr;
            if (SUCCEEDED(dev->GetId(&wid)) && wid) {
                if (id) *id = wid;
                CoTaskMemFree(wid);
                ok = true;
            }
            IPropertyStore* props = nullptr;
            if (name && SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) && props) {
                PROPVARIANT v;
                PropVariantInit(&v);
                if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) *name = ToUtf8(v.pwszVal);
                PropVariantClear(&v);
                props->Release();
            }
            dev->Release();
        }
        en->Release();
    }
    return ok;
}

// Windows.Media.Internal.AudioPolicyConfig (EarTrumpet): la tabella virtuale
// dopo IInspectable; servono solo le ultime tre voci.
struct __declspec(uuid("2a59116d-6c4f-45e0-a74f-707e3fef9258")) IAudioPolicyConfigFactory10 : IInspectable {};
struct __declspec(uuid("ab3d4648-e242-459f-b02f-541c70306324")) IAudioPolicyConfigFactory11 : IInspectable {};
struct IAudioPolicyConfigFactory : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE add_CtxVolumeChange() = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_CtxVolumeChanged() = 0;
    virtual HRESULT STDMETHODCALLTYPE add_RingerVibrateStateChanged() = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_RingerVibrateStateChange() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetVolumeGroupGainForId() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetVolumeGroupGainForId() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetActiveVolumeGroupForEndpointId() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetVolumeGroupsForEndpoint() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentVolumeContext() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetVolumeGroupMuteForId() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetVolumeGroupMuteForId() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetRingerVibrateState() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetRingerVibrateState() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPreferredChatApplication() = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetPreferredChatApplication() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPreferredChatApplication() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentChatApplications() = 0;
    virtual HRESULT STDMETHODCALLTYPE add_ChatContextChanged() = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_ChatContextChanged() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPersistedDefaultAudioEndpoint(UINT processId, EDataFlow flow, ERole role, HSTRING deviceId) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPersistedDefaultAudioEndpoint(UINT processId, EDataFlow flow, ERole role, HSTRING* deviceId) = 0;
    virtual HRESULT STDMETHODCALLTYPE ClearAllPersistedApplicationDefaultEndpoints() = 0;
};

IAudioPolicyConfigFactory* Factory() {
    static IAudioPolicyConfigFactory* factory = nullptr;
    static bool tried = false;
    if (tried) return factory;
    tried = true;
    HSTRING cls = nullptr;
    if (FAILED(WindowsCreateString(L"Windows.Media.Internal.AudioPolicyConfig", 40, &cls))) return nullptr;
    IInspectable* f = nullptr;
    HRESULT hr = RoGetActivationFactory(cls, __uuidof(IAudioPolicyConfigFactory11), reinterpret_cast<void**>(&f));
    if (FAILED(hr)) hr = RoGetActivationFactory(cls, __uuidof(IAudioPolicyConfigFactory10), reinterpret_cast<void**>(&f));
    WindowsDeleteString(cls);
    if (FAILED(hr) || !f) {
        char b[96];
        snprintf(b, sizeof(b), "AUDIO: per-app routing API not available (0x%08lX).", static_cast<unsigned long>(hr));
        vr::log::Line(b);
        return nullptr;
    }
    factory = static_cast<IAudioPolicyConfigFactory*>(f);
    return factory;
}

// SEH in una funzione senza oggetti C++ (C2712).
bool CallSetEndpoint(IAudioPolicyConfigFactory* f, HSTRING hs, HRESULT* h1, HRESULT* h2) {
    __try {
        *h1 = f->SetPersistedDefaultAudioEndpoint(GetCurrentProcessId(), eRender, eMultimedia, hs);
        *h2 = f->SetPersistedDefaultAudioEndpoint(GetCurrentProcessId(), eRender, eConsole, hs);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Id come lo vuole l'API: \\?\SWD#MMDEVAPI#<id>#{e6327cad-dcec-4949-ae8a-991e976a79d2}
bool SetGameEndpoint(const std::wstring& mmId) {
    IAudioPolicyConfigFactory* f = Factory();
    if (!f) return false;
    HSTRING hs = nullptr;
    if (!mmId.empty()) {
        std::wstring full = L"\\\\?\\SWD#MMDEVAPI#" + mmId + L"#{e6327cad-dcec-4949-ae8a-991e976a79d2}";
        if (FAILED(WindowsCreateString(full.c_str(), static_cast<UINT32>(full.size()), &hs))) return false;
    }
    HRESULT h1 = E_FAIL, h2 = E_FAIL;
    if (!CallSetEndpoint(f, hs, &h1, &h2)) {
        vr::log::Line("AUDIO: exception in the per-app routing API, routing disabled.");
        if (hs) WindowsDeleteString(hs);
        return false;
    }
    if (hs) WindowsDeleteString(hs);
    if (FAILED(h1) || FAILED(h2)) {
        char b[120];
        snprintf(b, sizeof(b), "AUDIO: per-app routing failed (0x%08lX / 0x%08lX).", static_cast<unsigned long>(h1), static_cast<unsigned long>(h2));
        vr::log::Line(b);
        return false;
    }
    return true;
}

std::wstring g_startId;   // predefinito all'avvio del gioco (quello che FMOD ha aperto)
std::wstring g_routedId;  // dove abbiamo spostato il gioco
bool g_routed = false;
ULONGLONG g_lastPoll = 0;
bool g_comInit = false;

}  // namespace

std::string DefaultDeviceName() {
    std::string name = "(unknown)";
    HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DefaultDevice(&name, nullptr);
    if (SUCCEEDED(init)) CoUninitialize();
    return name;
}

void LogDefaultDevice(const char* when) {
    static std::string last;
    std::string now = DefaultDeviceName();
    if (now == last) return;
    last = now;
    vr::log::Line(std::string("AUDIO: Windows default playback device ") + when + ": " + now);
    if (g_startId.empty()) {
        HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        DefaultDevice(nullptr, &g_startId);
        if (SUCCEEDED(init)) CoUninitialize();
    }
}

void Tick() {
    ULONGLONG now = GetTickCount64();
    if (now - g_lastPoll < 2000) return;
    g_lastPoll = now;
    if (!g_comInit) g_comInit = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) || true;
    std::string name;
    std::wstring id;
    if (!DefaultDevice(&name, &id) || id.empty()) return;
    if (g_startId.empty()) {
        g_startId = id;
        return;
    }
    // Predefinito cambiato (Virtual Desktop partito dopo il gioco): il gioco
    // va sul nuovo; tornato quello dell'avvio, la preferenza si toglie.
    std::wstring want = id == g_startId ? std::wstring() : id;
    if (want == g_routedId && (g_routed || want.empty())) return;
    if (SetGameEndpoint(want)) {
        g_routedId = want;
        g_routed = !want.empty();
        vr::log::Line(want.empty() ? "AUDIO: default device back to the one at start: game audio follows the default again."
                                   : "AUDIO: default playback device changed to \"" + name + "\": game audio routed there.");
    } else {
        g_routedId = want;  // non riprovare ogni 2 s
        g_routed = false;
    }
}

// "audioroute test": prova dell'API sul predefinito di adesso (diagnostica).
void RouteTest() {
    if (!g_comInit) g_comInit = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) || true;
    std::string name;
    std::wstring id;
    if (!DefaultDevice(&name, &id)) {
        vr::log::Line("AUDIO: route test: no default device.");
        return;
    }
    bool ok = SetGameEndpoint(id);
    vr::log::Line(std::string("AUDIO: route test to \"") + name + "\": " + (ok ? "ok (per-app routing works)" : "failed"));
    if (ok) {
        g_routed = true;
        g_routedId = id;
    }
}

void Shutdown() {
    if (g_routed) {
        SetGameEndpoint(std::wstring());
        g_routed = false;
    }
}

}  // namespace vr::audio
