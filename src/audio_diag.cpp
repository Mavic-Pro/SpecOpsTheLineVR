// Audio: l'uscita del gioco segue il dispositivo predefinito di Windows.
//
// Il gioco (FMOD Ex 4.32, uscita WASAPI, tipo 8) apre il dispositivo
// predefinito all'avvio e non lo segue piu'. Virtual Desktop rende
// predefinito il proprio dispositivo quando parte lo streaming: avviato il
// gioco prima (dal PC), l'audio restava sulle casse del PC (22/10). Ne' la
// preferenza per app di Windows ne' il cambio del predefinito spostano un
// flusso WASAPI aperto su un endpoint esplicito.
//
// FMOD sa riaprire l'uscita dopo l'init (System::setDriver: chiude, rienumera
// e riapre; driver 0 = il predefinito di quel momento) ma rifiuta il nuovo
// dispositivo se il formato del mix cambia (0x3e: Realtek 7.1 = 8 canali,
// Virtual Desktop = stereo). Per questo l'audio client WASAPI, durante
// l'init di FMOD e le riaperture, dichiara sempre lo stesso formato (48 kHz
// float stereo, quello che arriva al visore) e Windows converte verso il
// dispositivo (AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM): cosi' ogni riapertura
// riesce. Thread di uscita VR: predefinito cambiato -> richiesta; thread del
// gioco (hook dell'ascoltatore 3D): setDriver(0).
#include "audio_diag.h"
#include "logger.h"
#define INITGUID
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ks.h>
#include <ksmedia.h>
#include <MinHook.h>
#include <atomic>
#include <string>

namespace vr::audio {
namespace {

std::string ToUtf8(const wchar_t* w) {
    if (!w) return std::string();
    char buf[512];
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf), nullptr, nullptr);
    return n > 0 ? std::string(buf) : std::string();
}

std::string DeviceName(IMMDevice* dev) {
    std::string name = "?";
    IPropertyStore* props = nullptr;
    if (dev && SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) && props) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) name = ToUtf8(v.pwszVal);
        PropVariantClear(&v);
        props->Release();
    }
    return name;
}

// Nome e id MMDevice del predefinito.
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
            if (name) *name = DeviceName(dev);
            dev->Release();
        }
        en->Release();
    }
    return ok;
}

// Dispositivi di riproduzione con una sessione audio attiva (che suona) di
// questo processo: dove sta davvero l'audio del gioco.
std::string GameSessionDevices() {
    std::string out;
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&en))) || !en)
        return "(enumerator unavailable)";
    IMMDeviceCollection* coll = nullptr;
    if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll)) && coll) {
        UINT n = 0;
        coll->GetCount(&n);
        for (UINT i = 0; i < n; ++i) {
            IMMDevice* dev = nullptr;
            if (FAILED(coll->Item(i, &dev)) || !dev) continue;
            IAudioSessionManager2* mgr = nullptr;
            if (SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&mgr))) && mgr) {
                IAudioSessionEnumerator* se = nullptr;
                if (SUCCEEDED(mgr->GetSessionEnumerator(&se)) && se) {
                    int count = 0;
                    se->GetCount(&count);
                    bool mine = false;
                    for (int s = 0; s < count && !mine; ++s) {
                        IAudioSessionControl* sc = nullptr;
                        if (FAILED(se->GetSession(s, &sc)) || !sc) continue;
                        IAudioSessionControl2* sc2 = nullptr;
                        if (SUCCEEDED(sc->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void**>(&sc2))) && sc2) {
                            DWORD pid = 0;
                            AudioSessionState st = AudioSessionStateInactive;
                            sc2->GetProcessId(&pid);
                            sc2->GetState(&st);
                            // Attivare il gestore crea una sessione (inattiva) di questo
                            // processo su ogni dispositivo: conta solo quella che suona.
                            if (pid == GetCurrentProcessId() && st == AudioSessionStateActive) mine = true;
                            sc2->Release();
                        }
                        sc->Release();
                    }
                    if (mine) {
                        if (!out.empty()) out += ", ";
                        out += "\"" + DeviceName(dev) + "\"";
                    }
                    se->Release();
                }
                mgr->Release();
            }
            dev->Release();
        }
        coll->Release();
    }
    en->Release();
    return out.empty() ? "(no session)" : out;
}

// --- WASAPI: formato fisso per FMOD -------------------------------------------
std::atomic<bool> g_formatOverride{false};  // vero durante System::init e setDriver di FMOD

typedef HRESULT(STDMETHODCALLTYPE* AcInitialize_t)(IAudioClient*, AUDCLNT_SHAREMODE, DWORD, REFERENCE_TIME, REFERENCE_TIME, const WAVEFORMATEX*, LPCGUID);
typedef HRESULT(STDMETHODCALLTYPE* AcIsFormatSupported_t)(IAudioClient*, AUDCLNT_SHAREMODE, const WAVEFORMATEX*, WAVEFORMATEX**);
typedef HRESULT(STDMETHODCALLTYPE* AcGetMixFormat_t)(IAudioClient*, WAVEFORMATEX**);
AcInitialize_t g_AcInitialize_Original = nullptr;
AcIsFormatSupported_t g_AcIsFormatSupported_Original = nullptr;
AcGetMixFormat_t g_AcGetMixFormat_Original = nullptr;

bool IsOurFormat(const WAVEFORMATEX* f) {
    if (!f || f->nChannels != 2 || f->nSamplesPerSec != 48000 || f->wBitsPerSample != 32) return false;
    if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (f->wFormatTag != WAVE_FORMAT_EXTENSIBLE || f->cbSize < 22) return false;
    return IsEqualGUID(reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f)->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
}

WAVEFORMATEX* OurFormat() {
    auto* x = static_cast<WAVEFORMATEXTENSIBLE*>(CoTaskMemAlloc(sizeof(WAVEFORMATEXTENSIBLE)));
    if (!x) return nullptr;
    x->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    x->Format.nChannels = 2;
    x->Format.nSamplesPerSec = 48000;
    x->Format.wBitsPerSample = 32;
    x->Format.nBlockAlign = 8;
    x->Format.nAvgBytesPerSec = 48000 * 8;
    x->Format.cbSize = 22;
    x->Samples.wValidBitsPerSample = 32;
    x->dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    x->SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    return &x->Format;
}

HRESULT STDMETHODCALLTYPE Hooked_AcGetMixFormat(IAudioClient* self, WAVEFORMATEX** fmt) {
    HRESULT hr = g_AcGetMixFormat_Original(self, fmt);
    if (SUCCEEDED(hr) && g_formatOverride && fmt && *fmt) {
        WAVEFORMATEX* ours = OurFormat();
        if (ours) {
            static unsigned logged = 0;
            if (logged < 4) {
                ++logged;
                vr::log::Line("AUDIO: WASAPI mix format " + std::to_string((*fmt)->nChannels) + " ch " + std::to_string((*fmt)->nSamplesPerSec) +
                              " Hz reported to FMOD as 2 ch 48000 Hz float.");
            }
            CoTaskMemFree(*fmt);
            *fmt = ours;
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Hooked_AcIsFormatSupported(IAudioClient* self, AUDCLNT_SHAREMODE mode, const WAVEFORMATEX* fmt, WAVEFORMATEX** closest) {
    if (g_formatOverride && mode == AUDCLNT_SHAREMODE_SHARED && IsOurFormat(fmt)) {
        if (closest) *closest = nullptr;
        return S_OK;
    }
    return g_AcIsFormatSupported_Original(self, mode, fmt, closest);
}

HRESULT STDMETHODCALLTYPE Hooked_AcInitialize(IAudioClient* self, AUDCLNT_SHAREMODE mode, DWORD flags, REFERENCE_TIME duration, REFERENCE_TIME period,
                                              const WAVEFORMATEX* fmt, LPCGUID session) {
    bool override = g_formatOverride && mode == AUDCLNT_SHAREMODE_SHARED;
    if (override) flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    HRESULT hr = g_AcInitialize_Original(self, mode, flags, duration, period, fmt, session);
    if (g_formatOverride) {
        char b[200];
        snprintf(b, sizeof(b), "AUDIO: WASAPI Initialize(%s, flags 0x%08lX, %u ch %lu Hz, %.1f ms) -> 0x%08lX",
                 mode == AUDCLNT_SHAREMODE_SHARED ? "shared" : "exclusive", static_cast<unsigned long>(flags), fmt ? fmt->nChannels : 0,
                 fmt ? static_cast<unsigned long>(fmt->nSamplesPerSec) : 0UL, duration / 10000.0, static_cast<unsigned long>(hr));
        vr::log::Line(b);
    }
    return hr;
}

std::wstring g_lastDefaultId;  // predefinito all'avvio, poi l'ultimo notificato
void RequestReopenFor(const wchar_t* deviceId, const char* why);

// Notifica di Windows quando cambia il predefinito (nessun polling, anche
// senza uscita VR).
class DefaultDeviceListener : public IMMNotificationClient {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, __uuidof(IMMNotificationClient))) {
            *out = this;
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR id) override {
        // Una chiamata per ruolo (console, multimedia, comunicazioni): basta la prima.
        if (flow == eRender && role == eConsole) RequestReopenFor(id, "Windows default playback device changed");
        return S_OK;
    }
};
DefaultDeviceListener g_listener;

// La tabella virtuale di IAudioClient e' una per tutto il processo: si
// prende da un client qualsiasi. Thread a parte (COM MTA senza toccare
// l'apartment del thread del gioco) che poi resta vivo per le notifiche.
struct WasapiHookState {
    bool hooks = false;
    bool notify = false;
    HANDLE done = nullptr;
};
DWORD WINAPI WasapiHookThread(LPVOID p) {
    WasapiHookState* st = static_cast<WasapiHookState*>(p);
    HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                   reinterpret_cast<void**>(&en))) && en) {
        IMMDevice* dev = nullptr;
        if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) || !dev) {
            IMMDeviceCollection* coll = nullptr;
            if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll)) && coll) {
                coll->Item(0, &dev);
                coll->Release();
            }
        }
        LPWSTR wid = nullptr;
        if (dev && SUCCEEDED(dev->GetId(&wid)) && wid) {
            g_lastDefaultId = wid;  // una notifica per lo stesso dispositivo non riapre
            CoTaskMemFree(wid);
        }
        IAudioClient* client = nullptr;
        if (dev && SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client))) && client) {
            void** vt = *reinterpret_cast<void***>(client);
            st->hooks = MH_CreateHook(vt[3], reinterpret_cast<void*>(&Hooked_AcInitialize), reinterpret_cast<void**>(&g_AcInitialize_Original)) == MH_OK &&
                        MH_EnableHook(vt[3]) == MH_OK &&
                        MH_CreateHook(vt[7], reinterpret_cast<void*>(&Hooked_AcIsFormatSupported), reinterpret_cast<void**>(&g_AcIsFormatSupported_Original)) == MH_OK &&
                        MH_EnableHook(vt[7]) == MH_OK &&
                        MH_CreateHook(vt[8], reinterpret_cast<void*>(&Hooked_AcGetMixFormat), reinterpret_cast<void**>(&g_AcGetMixFormat_Original)) == MH_OK &&
                        MH_EnableHook(vt[8]) == MH_OK;
            client->Release();
        }
        if (dev) dev->Release();
        st->notify = SUCCEEDED(en->RegisterEndpointNotificationCallback(&g_listener));
    }
    SetEvent(st->done);
    if (st->notify) {
        // Resta registrato (e l'apartment MTA vivo) per tutta la partita.
        Sleep(INFINITE);
    }
    if (en) en->Release();
    if (SUCCEEDED(init)) CoUninitialize();
    return 0;
}

// --- FMOD -------------------------------------------------------------------
typedef int(__stdcall* SysInit_t)(void* system, int maxChannels, unsigned flags, void* extraDriverData);
typedef int(__stdcall* EvGetSystem_t)(void* eventSystem, void** system);
typedef int(__stdcall* SysGetDriver_t)(void* system, int* driver);
typedef int(__stdcall* SysSetDriver_t)(void* system, int driver);
SysInit_t g_SysInit_Original = nullptr;
std::atomic<int> g_reopenDriver{-1};  // >= 0: riapertura in attesa (eseguita dal thread del gioco)

int __stdcall Hooked_SysInit(void* system, int maxChannels, unsigned flags, void* extraDriverData) {
    g_formatOverride = true;
    int r = g_SysInit_Original(system, maxChannels, flags, extraDriverData);
    g_formatOverride = false;
    vr::log::Line("AUDIO: FMOD System::init -> " + std::to_string(r) + (r == 0 ? " (ok)" : " (FMOD error)") +
                  "; the game's audio follows the Windows default playback device.");
    return r;
}

ULONGLONG g_lastPoll = 0;
ULONGLONG g_whereDue = 0;  // qualche secondo dopo una riapertura: dove sta la sessione del gioco
bool g_comInit = false;

void EnsureCom() {
    if (!g_comInit) g_comInit = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) || true;
}

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
}

void InstallAudioHooks() {
    static WasapiHookState st;
    st.done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE t = st.done ? CreateThread(nullptr, 0, &WasapiHookThread, &st, 0, nullptr) : nullptr;
    if (t) {
        WaitForSingleObject(st.done, 5000);
        CloseHandle(t);
    }
    HMODULE ex = GetModuleHandleW(L"fmodex.dll");
    void* init = ex ? reinterpret_cast<void*>(GetProcAddress(ex, "?init@System@FMOD@@QAG?AW4FMOD_RESULT@@HIPAX@Z")) : nullptr;
    bool fmod = init && MH_CreateHook(init, reinterpret_cast<void*>(&Hooked_SysInit), reinterpret_cast<void**>(&g_SysInit_Original)) == MH_OK &&
                MH_EnableHook(init) == MH_OK;
    vr::log::Line(std::string("AUDIO: hooks on the WASAPI client ") + (st.hooks ? "installed" : "FAILED") + ", on FMOD System::init " +
                  (fmod ? "installed" : "FAILED") + ", default device notifications " + (st.notify ? "on" : "FAILED") +
                  " (game audio as 48 kHz stereo on the Windows default device).");
}

std::string GameAudioDevices() {
    EnsureCom();
    return GameSessionDevices();
}

void RequestReopen(int driver) { g_reopenDriver = driver; }

bool ReopenPending() { return g_reopenDriver.load() >= 0; }

void ReopenOutput(void* eventSystem) {
    int driver = g_reopenDriver.exchange(-1);
    if (driver < 0 || !eventSystem) return;
    HMODULE ev = GetModuleHandleW(L"fmod_event.dll");
    HMODULE ex = GetModuleHandleW(L"fmodex.dll");
    EvGetSystem_t getSystem = ev ? reinterpret_cast<EvGetSystem_t>(GetProcAddress(ev, "?getSystemObject@EventSystem@FMOD@@QAG?AW4FMOD_RESULT@@PAPAVSystem@2@@Z")) : nullptr;
    SysGetDriver_t getDriver = ex ? reinterpret_cast<SysGetDriver_t>(GetProcAddress(ex, "?getDriver@System@FMOD@@QAG?AW4FMOD_RESULT@@PAH@Z")) : nullptr;
    SysSetDriver_t setDriver = ex ? reinterpret_cast<SysSetDriver_t>(GetProcAddress(ex, "?setDriver@System@FMOD@@QAG?AW4FMOD_RESULT@@H@Z")) : nullptr;
    if (!getSystem || !getDriver || !setDriver) {
        vr::log::Line("AUDIO: FMOD entry points for reopening the output not found.");
        return;
    }
    void* sys = nullptr;
    int r0 = getSystem(eventSystem, &sys);
    if (r0 != 0 || !sys) {
        vr::log::Line("AUDIO: FMOD getSystemObject failed (" + std::to_string(r0) + ").");
        return;
    }
    // Driver 0 = il predefinito di Windows in questo momento (FMOD rienumera).
    g_formatOverride = true;
    int r = setDriver(sys, driver);
    g_formatOverride = false;
    int now = -1;
    getDriver(sys, &now);
    vr::log::Line("AUDIO: FMOD output reopened on driver " + std::to_string(driver) + " (now " + std::to_string(now) + "): result " +
                  std::to_string(r) + (r == 0 ? " (ok)." : " (FMOD error)."));
    g_whereDue = GetTickCount64() + 4000;
}

namespace {
// Predefinito cambiato (Virtual Desktop partito dopo il gioco, o chiuso):
// l'uscita di FMOD si riapre li', dal thread del gioco. Thread di notifica COM.
void RequestReopenFor(const wchar_t* deviceId, const char* why) {
    std::wstring id = deviceId ? deviceId : L"";
    if (id.empty() || id == g_lastDefaultId) return;
    g_lastDefaultId = id;
    std::string name;
    DefaultDevice(&name, nullptr);
    vr::log::Line(std::string("AUDIO: ") + why + " to \"" + name + "\": reopening the game's audio there.");
    RequestReopen(0);
}
}  // namespace

void Tick() {
    ULONGLONG now = GetTickCount64();
    if (now - g_lastPoll < 2000) return;
    g_lastPoll = now;
    if (g_whereDue && now >= g_whereDue) {
        g_whereDue = 0;
        EnsureCom();
        vr::log::Line("AUDIO: game audio session now on " + GameSessionDevices() + ".");
    }
}

}  // namespace vr::audio
