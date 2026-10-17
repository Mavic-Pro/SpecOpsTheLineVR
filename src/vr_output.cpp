#include "vr_output.h"
#include "control.h"
#include "logger.h"
#include "vr_session.h"
#include "hooks.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <openvr.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <cmath>
#include <cstdio>
#include <vector>
#include <string>

namespace vr::output {

namespace {

// --- Uscita verso SteamVR via Direct3D 11 -----------------------------------
//
// OpenVR non accetta texture D3D9. Storia dei tentativi (16/10):
// - WGL_NV_DX_interop (GPU->GPU): rifiutato su device D3D9 non-Ex (0xC007006E).
// - Copia via CPU + texture OpenGL: funzionava, ma fragile (contesto GL legato
//   al thread, UE3 ricrea il thread di rendering al Reset) e da un certo punto
//   Submit ha iniziato a restituire 105 (TextureIsOnWrongDevice) anche in mono.
// Ora: copia via CPU in una texture D3D11 creata sulla STESSA scheda video a
// cui e' collegato il visore (GetDXGIOutputInfo), il percorso principale di
// OpenVR. D3D11 non ha affinita' di thread.
bool g_disabled = false;  // errore permanente: il gioco continua sul monitor
ID3D11Device* g_d3d11 = nullptr;
ID3D11DeviceContext* g_d3d11Ctx = nullptr;
ID3D11Texture2D* g_eyeTexture[2] = {};

// Lettura in pipeline, con un thread dedicato all'uscita (16/10).
// Thread di rendering del gioco, per ogni occhio: StretchRect del ritaglio e
// GetRenderTargetData (asincrona) in una superficie in memoria di sistema;
// al tick seguente la superficie, ormai pronta, viene bloccata (LockRect) e
// consegnata al thread di uscita, che la copia nella texture D3D11, attende il
// visore (WaitGetPoses) e invia. Prima tutto questo stava sul thread di
// rendering: ~7,5 ms per tick su 13,9 disponibili a 72 Hz, piu' il Present
// del monitor. Tre superfici per occhio: una in lettura GPU, una pronta,
// una in copia nel thread di uscita.
constexpr int kSlots = 3;
enum SlotState { kFree, kGpu, kLocked };
IDirect3DSurface9* g_sharedSurface = nullptr;  // ritaglio dell'occhio (GPU, D3D9)
IDirect3DSurface9* g_sysmem[2][kSlots] = {};
SlotState g_slotState[2][kSlots] = {};
UINT g_slotW[2][kSlots] = {}, g_slotH[2][kSlots] = {};
bool g_slotCinema[2][kSlots] = {};  // lettura fatta in modalita' schermo virtuale
bool g_slotGpu[2][kSlots] = {};     // slot nella texture condivisa (niente LockRect)
// Percorso GPU (device D3D9Ex): texture render target condivise con D3D11,
// una query evento per sapere quando la GPU ha finito lo StretchRect.
IDirect3DTexture9* g_shTex9[2][kSlots] = {};
IDirect3DSurface9* g_shSurf9[2][kSlots] = {};
IDirect3DQuery9* g_shQuery[2][kSlots] = {};
ID3D11Texture2D* g_shTex11[2][kSlots] = {};
bool g_gpuReady = false;  // risorse condivise create
int g_gpuSlot[2] = {-1, -1};  // superficie con GetRenderTargetData in corso
UINT g_width = 0;
UINT g_height = 0;

// Consegna al thread di uscita (protetta da g_mutex).
struct EyePacket {
    int slot = -1;
    const uint8_t* bits = nullptr;
    UINT pitch = 0, w = 0, h = 0;
    bool cinema = false;  // solo occhio 0: immagine 16:9 per lo schermo virtuale
    ID3D11Texture2D* gpuSrc = nullptr;  // percorso GPU: texture condivisa da copiare
};
std::mutex g_mutex;
std::condition_variable g_cv;
EyePacket g_ready[2];            // ultimo frame bloccato per occhio, non ancora preso
bool g_frameReady = false;       // coppia completa pronta
bool g_workerBusy = false;       // il thread di uscita sta leggendo superfici bloccate
std::vector<int> g_doneSlots[2];  // superfici lette, da sbloccare sul thread di rendering
std::thread g_worker;
std::atomic<bool> g_workerStop{false};

double g_copyMsAccum = 0;
// Ripartizione dei tempi della copia (ms accumulati): StretchRect,
// GetRenderTargetData, LockRect, UpdateSubresource.
double g_stageMs[4] = {};
double g_waitMs = 0.0;  // tempo passato in WaitGetPoses
double QpcMs() {
    static LARGE_INTEGER f = {};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return 1000.0 * t.QuadPart / f.QuadPart;
}

// Ritaglio per occhio. Il gioco disegna un'immagine simmetrica con FOV
// abbastanza largo da contenere i frustum (asimmetrici) di entrambi gli
// occhi; la proiezione e' lineare nelle tangenti, quindi ogni occhio usa
// esattamente la sua porzione. La porzione viene ritagliata in GPU
// (StretchRect con rettangolo sorgente) prima della lettura dalla CPU, cosi'
// si copiano solo i pixel utili. Ricalcolato quando cambia il FOV o la forma
// del back buffer.
vr::VRTextureBounds_t g_eyeBounds[2] = {{0, 0, 1, 1}, {0, 0, 1, 1}};
RECT g_eyeCrop[2] = {};
UINT g_cropW = 0, g_cropH = 0;
float g_boundsForFov = -1.0f;

std::string Hex(unsigned long v) {
    char b[16];
    std::snprintf(b, sizeof(b), "0x%08lX", v);
    return b;
}

void Disable(const std::string& why) {
    vr::log::Line("VR-OUTPUT disabled: " + why);
    g_disabled = true;
}

bool UpdateFovAndBounds(UINT bbW, UINT bbH) {
    float aspect = static_cast<float>(bbW) / static_cast<float>(bbH);
    vr::session::Shared vrLock(true);
    vr::IVRSystem* sys = vrLock.ok() ? vr::VRSystem() : nullptr;
    if (!sys) return false;
    float l[2], r[2], t[2], b[2];
    float maxH = 0.0f, maxV = 0.0f;
    for (int e = 0; e < 2; ++e) {
        sys->GetProjectionRaw(static_cast<vr::EVREye>(e), &l[e], &r[e], &t[e], &b[e]);
        maxH = std::fmax(maxH, std::fmax(std::fabs(l[e]), std::fabs(r[e])));
        maxV = std::fmax(maxV, std::fmax(std::fabs(t[e]), std::fabs(b[e])));
    }
    // Tangente orizzontale necessaria: deve coprire sia i limiti
    // orizzontali sia quelli verticali (scalati dall'aspect dell'immagine).
    float th = std::fmax(maxH, maxV * aspect);
    float fovDeg = 2.0f * std::atan(th) * 57.2957795f;
    if (fovDeg > 170.0f) fovDeg = 170.0f;
    if (vr::control::g_fovAuto) vr::control::g_fovOverride = fovDeg;
    float used = vr::control::g_fovOverride > 0 ? vr::control::g_fovOverride.load() : 90.0f;
    th = std::tan(used * 0.5f / 57.2957795f);
    float tv = th / aspect;
    auto clamp01 = [](float v) { return v < 0 ? 0.0f : (v > 1 ? 1.0f : v); };
    g_cropW = g_cropH = 0;
    for (int e = 0; e < 2; ++e) {
        // Coordinate immagine D3D (0 = alto/sinistra) -> rettangolo in pixel.
        float u0 = clamp01(0.5f + l[e] / (2 * th)), u1 = clamp01(0.5f + r[e] / (2 * th));
        float d0 = clamp01(0.5f + t[e] / (2 * tv)), d1 = clamp01(0.5f + b[e] / (2 * tv));
        g_eyeCrop[e] = {static_cast<LONG>(u0 * bbW), static_cast<LONG>(d0 * bbH),
                        static_cast<LONG>(u1 * bbW), static_cast<LONG>(d1 * bbH)};
        g_cropW = std::max<UINT>(g_cropW, g_eyeCrop[e].right - g_eyeCrop[e].left);
        g_cropH = std::max<UINT>(g_cropH, g_eyeCrop[e].bottom - g_eyeCrop[e].top);
    }
    for (int e = 0; e < 2; ++e) {
        // Il ritaglio dell'occhio sta in alto a sinistra della texture (le due
        // porzioni possono differire di un pixel). Convenzione D3D: v = 0 in alto.
        float uw = float(g_eyeCrop[e].right - g_eyeCrop[e].left) / g_cropW;
        float vh = float(g_eyeCrop[e].bottom - g_eyeCrop[e].top) / g_cropH;
        g_eyeBounds[e] = {0.0f, 0.0f, uw, vh};
    }
    g_boundsForFov = vr::control::g_fovOverride;
    char buf[400];
    std::snprintf(buf, sizeof(buf),
        "VR-FOV: tangents L(%.3f %.3f %.3f %.3f) R(%.3f %.3f %.3f %.3f) aspect=%.3f -> game FOV %.1f degrees; "
        "crop L x[%ld..%ld] R x[%ld..%ld] y[%ld..%ld] = %ux%u per eye (back buffer %ux%u)",
        l[0], r[0], t[0], b[0], l[1], r[1], t[1], b[1], aspect, used,
        g_eyeCrop[0].left, g_eyeCrop[0].right, g_eyeCrop[1].left, g_eyeCrop[1].right,
        g_eyeCrop[0].top, g_eyeCrop[0].bottom, g_cropW, g_cropH, bbW, bbH);
    vr::log::Line(buf);
    return true;
}

bool InitD3D11() {
    int32_t adapterIndex = 0;
    {
        vr::session::Shared vrLock;
        vr::IVRSystem* sys = vr::VRSystem();
        if (sys) sys->GetDXGIOutputInfo(&adapterIndex);
    }
    IDXGIFactory1* factory = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) return false;
    if (FAILED(factory->EnumAdapters1(adapterIndex < 0 ? 0 : adapterIndex, &adapter))) {
        factory->Release();
        return false;
    }
    DXGI_ADAPTER_DESC1 desc;
    adapter->GetDesc1(&desc);
    char name[128];
    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                   &g_d3d11, &level, &g_d3d11Ctx);
    adapter->Release();
    factory->Release();
    vr::log::Line(std::string("VR-OUTPUT: D3D11 device on the headset GPU (index ") +
                  std::to_string(adapterIndex) + ", " + name + ") " + (SUCCEEDED(hr) ? "created" : "FAILED " + Hex(hr)));
    return SUCCEEDED(hr);
}

// Attende che il thread di uscita non stia leggendo superfici e annulla le
// consegne in sospeso; da chiamare prima di rilasciare o ricreare risorse.
void QuiesceWorker() {
    std::unique_lock<std::mutex> lock(g_mutex);
    g_cv.wait_for(lock, std::chrono::milliseconds(500), [] { return !g_workerBusy; });
    g_ready[0] = g_ready[1] = EyePacket{};
    g_frameReady = false;
    g_doneSlots[0].clear();
    g_doneSlots[1].clear();
}

// Solo risorse D3D9 in pool DEFAULT/SYSTEMMEM: sicuro da qualunque thread
// (anche da Reset, che il gioco puo' chiamare da un altro thread).
void ReleaseD3d9Surfaces() {
    QuiesceWorker();
    if (g_sharedSurface) {
        g_sharedSurface->Release();
        g_sharedSurface = nullptr;
    }
    for (int e = 0; e < 2; ++e) {
        for (int k = 0; k < kSlots; ++k) {
            if (g_shTex11[e][k]) g_shTex11[e][k]->Release();
            if (g_shQuery[e][k]) g_shQuery[e][k]->Release();
            if (g_shSurf9[e][k]) g_shSurf9[e][k]->Release();
            if (g_shTex9[e][k]) g_shTex9[e][k]->Release();
            g_shTex11[e][k] = nullptr;
            g_shQuery[e][k] = nullptr;
            g_shSurf9[e][k] = nullptr;
            g_shTex9[e][k] = nullptr;
            if (g_sysmem[e][k]) {
                if (g_slotState[e][k] == kLocked && !g_slotGpu[e][k]) g_sysmem[e][k]->UnlockRect();
                g_sysmem[e][k]->Release();
            }
            g_sysmem[e][k] = nullptr;
            g_slotState[e][k] = kFree;
            g_slotGpu[e][k] = false;
        }
        g_gpuSlot[e] = -1;
    }
    g_gpuReady = false;
    g_width = g_height = 0;
}

void ReleaseEyeTextures() {
    for (ID3D11Texture2D*& t : g_eyeTexture) {
        if (t) t->Release();
        t = nullptr;
    }
}

void StartWorker();

// --- Schermo virtuale (solo thread di uscita) --------------------------------
ID3D11Texture2D* g_cinemaTex = nullptr;   // immagine 16:9 mostrata dall'overlay
ID3D11Texture2D* g_blackTex = nullptr;    // inviata agli occhi durante il cinema
UINT g_cinemaTexW = 0, g_cinemaTexH = 0;
vr::VROverlayHandle_t g_cinemaOverlay = vr::k_ulOverlayHandleInvalid;
bool g_cinemaShown = false;

bool EnsureCinemaTextures(UINT w, UINT h) {
    if (!g_blackTex) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = td.Height = 16;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        std::vector<uint32_t> px(16 * 16, 0xFF000000u);
        D3D11_SUBRESOURCE_DATA init = {px.data(), 16 * 4, 0};
        if (FAILED(g_d3d11->CreateTexture2D(&td, &init, &g_blackTex))) return false;
    }
    if (!g_cinemaTex || g_cinemaTexW != w || g_cinemaTexH != h) {
        if (g_cinemaTex) g_cinemaTex->Release();
        g_cinemaTex = nullptr;
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w;
        td.Height = h;
        td.MipLevels = td.ArraySize = 1;
        // Senza alfa: il back buffer del gioco ha un alfa qualsiasi e l'overlay
        // lo userebbe come trasparenza (immagine scura, cielo nero).
        td.Format = DXGI_FORMAT_B8G8R8X8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(g_d3d11->CreateTexture2D(&td, nullptr, &g_cinemaTex))) return false;
        g_cinemaTexW = w;
        g_cinemaTexH = h;
    }
    return true;
}

// Mostra lo schermo davanti alla direzione in cui guarda il giocatore
// all'inizio della scena (solo yaw), all'altezza degli occhi, fisso nella
// stanza. L'overlay e' un quadrato nel piano XY visibile da +Z.
void ShowCinemaOverlay() {
    vr::IVROverlay* ov = vr::VROverlay();
    vr::IVRSystem* sys = vr::VRSystem();
    if (!ov || !sys) return;
    if (g_cinemaOverlay == vr::k_ulOverlayHandleInvalid) {
        vr::EVROverlayError err = ov->CreateOverlay("specopsvr.cinema", "SpecOpsVR schermo", &g_cinemaOverlay);
        if (err != vr::VROverlayError_None) {
            vr::log::Line("CINEMA: CreateOverlay failed, error " + std::to_string(err));
            g_cinemaOverlay = vr::k_ulOverlayHandleInvalid;
            return;
        }
    }
    vr::TrackedDevicePose_t pose;
    sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, &pose, 1);
    const vr::HmdMatrix34_t& m = pose.mDeviceToAbsoluteTracking;
    float fx = -m.m[0][2], fz = -m.m[2][2];
    float fl = std::sqrt(fx * fx + fz * fz);
    if (fl < 1e-3f) { fx = 0.0f; fz = -1.0f; fl = 1.0f; }
    fx /= fl;
    fz /= fl;
    float dist = vr::control::g_cinemaDistM;
    float zx = -fx, zz = -fz;  // asse Z dell'overlay: verso il giocatore
    vr::HmdMatrix34_t t = {};
    t.m[0][0] = zz;   t.m[0][1] = 0.0f; t.m[0][2] = zx; t.m[0][3] = m.m[0][3] + fx * dist;
    t.m[1][0] = 0.0f; t.m[1][1] = 1.0f; t.m[1][2] = 0.0f; t.m[1][3] = pose.bPoseIsValid ? m.m[1][3] : 1.6f;
    t.m[2][0] = -zx;  t.m[2][1] = 0.0f; t.m[2][2] = zz; t.m[2][3] = m.m[2][3] + fz * dist;
    ov->SetOverlayTransformAbsolute(g_cinemaOverlay, vr::TrackingUniverseStanding, &t);
    ov->SetOverlayWidthInMeters(g_cinemaOverlay, vr::control::g_cinemaWidthM);
    ov->ShowOverlay(g_cinemaOverlay);
    g_cinemaShown = true;
    vr::log::Line("CINEMA: virtual screen shown.");
}

// Righe con contenuto (senza le bande nere sopra e sotto): campiona 16 pixel
// per riga dall'alto e dal basso.
void DetectContentRows(const uint8_t* bits, UINT pitch, UINT w, UINT h, UINT* top, UINT* bottom) {
    auto blackRow = [&](UINT y) {
        const uint8_t* row = bits + static_cast<size_t>(y) * pitch;
        for (UINT i = 0; i < 16; ++i) {
            const uint8_t* px = row + ((w - 1) * i / 15) * 4;
            if (px[0] > 10 || px[1] > 10 || px[2] > 10) return false;
        }
        return true;
    };
    UINT t = 0, b = h;
    // Il gioco non centra sempre il 16:9 (visto in basso): fino al 70% per lato.
    while (t < h * 7 / 10 && blackRow(t)) ++t;
    while (b > t + h / 4 && blackRow(b - 1)) --b;
    if (b <= t + 16) {  // immagine tutta nera (es. dissolvenza): area intera
        t = 0;
        b = h;
    }
    *top = t;
    *bottom = b;
}

void HideCinemaOverlay() {
    if (!g_cinemaShown) return;
    if (vr::VROverlay() && g_cinemaOverlay != vr::k_ulOverlayHandleInvalid) vr::VROverlay()->HideOverlay(g_cinemaOverlay);
    g_cinemaShown = false;
    vr::log::Line("CINEMA: virtual screen hidden.");
}

bool EnsureResources(IDirect3DDevice9* device, UINT w, UINT h) {
    if (g_sharedSurface && g_width == w && g_height == h) return true;
    ReleaseD3d9Surfaces();
    {
        // Le texture D3D11 sono usate solo dal thread di uscita, fermo qui.
        std::lock_guard<std::mutex> lock(g_mutex);
        ReleaseEyeTextures();
    }
    HRESULT hr = device->CreateRenderTarget(w, h, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE,
                                            &g_sharedSurface, nullptr);
    if (FAILED(hr)) {
        Disable("CreateRenderTarget fallita hr=" + Hex(hr));
        return false;
    }
    for (int e = 0; e < 2; ++e) {
        for (int k = 0; k < kSlots; ++k) {
            hr = device->CreateOffscreenPlainSurface(w, h, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                                                     &g_sysmem[e][k], nullptr);
            if (FAILED(hr)) {
                Disable("CreateOffscreenPlainSurface fallita hr=" + Hex(hr));
                return false;
            }
        }
    }
    // D3DFMT_A8R8G8B8 in memoria = byte B,G,R,A = DXGI_FORMAT_B8G8R8A8_UNORM.
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (ID3D11Texture2D*& t : g_eyeTexture) {
            hr = g_d3d11->CreateTexture2D(&td, nullptr, &t);
            if (FAILED(hr)) {
                Disable("CreateTexture2D (D3D11) fallita hr=" + Hex(hr));
                return false;
            }
        }
    }
    // Percorso GPU: solo con il device D3D9Ex (le superfici condivise
    // richiedono D3D9Ex). Se qualcosa fallisce resta la lettura via CPU.
    if (vr::hooks::g_d3d9ex) {
        bool ok = true;
        for (int e = 0; ok && e < 2; ++e) {
            for (int k = 0; ok && k < kSlots; ++k) {
                HANDLE sh = nullptr;
                ok = SUCCEEDED(device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                                     D3DPOOL_DEFAULT, &g_shTex9[e][k], &sh)) && sh &&
                     SUCCEEDED(g_shTex9[e][k]->GetSurfaceLevel(0, &g_shSurf9[e][k])) &&
                     SUCCEEDED(device->CreateQuery(D3DQUERYTYPE_EVENT, &g_shQuery[e][k])) &&
                     SUCCEEDED(g_d3d11->OpenSharedResource(sh, __uuidof(ID3D11Texture2D),
                                                           reinterpret_cast<void**>(&g_shTex11[e][k])));
            }
        }
        g_gpuReady = ok;
        vr::log::Line(ok ? "VR-OUTPUT: D3D9Ex -> D3D11 shared surfaces ready (GPU copy)."
                         : "VR-OUTPUT: shared surfaces not available, CPU readback.");
    }
    g_width = w;
    g_height = h;
    StartWorker();
    vr::log::Line("VR-OUTPUT: per-eye resources " + std::to_string(w) + "x" + std::to_string(h) +
                  " ready (dedicated output thread).");
    return true;
}

}  // namespace

void OnBeforeReset() { ReleaseD3d9Surfaces(); }

namespace {

// Salva le texture specchio del compositor (occhio sinistro e destro, cioe'
// cio' che il visore sta mostrando) affiancate in un BMP a 24 bit, ridotte a
// meta' risoluzione.
void SaveMirrorShot(vr::IVRCompositor* compositor, const std::string& path) {
    ID3D11Texture2D* staging[2] = {};
    UINT w = 0, h = 0;
    for (int e = 0; e < 2; ++e) {
        void* srvPtr = nullptr;
        vr::EVRCompositorError err = compositor->GetMirrorTextureD3D11(static_cast<vr::EVREye>(e), g_d3d11, &srvPtr);
        if (err != vr::VRCompositorError_None || !srvPtr) {
            vr::log::Line("MIRRORSHOT: GetMirrorTextureD3D11 failed, error " + std::to_string(err));
            for (auto* t : staging) if (t) t->Release();
            return;
        }
        auto* srv = static_cast<ID3D11ShaderResourceView*>(srvPtr);
        ID3D11Resource* res = nullptr;
        srv->GetResource(&res);
        ID3D11Texture2D* tex = nullptr;
        res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex));
        D3D11_TEXTURE2D_DESC td;
        tex->GetDesc(&td);
        w = td.Width;
        h = td.Height;
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        td.MiscFlags = 0;
        td.MipLevels = 1;
        td.ArraySize = 1;
        if (SUCCEEDED(g_d3d11->CreateTexture2D(&td, nullptr, &staging[e]))) {
            g_d3d11Ctx->CopySubresourceRegion(staging[e], 0, 0, 0, 0, tex, 0, nullptr);
        }
        tex->Release();
        res->Release();
        compositor->ReleaseMirrorTextureD3D11(srvPtr);
        if (!staging[e]) return;
        vr::log::Line("MIRRORSHOT: eye " + std::to_string(e) + " " + std::to_string(td.Width) + "x" +
                      std::to_string(td.Height) + " format " + std::to_string(td.Format));
    }
    UINT ow = w / 2, oh = h / 2;
    std::vector<uint8_t> rgb(static_cast<size_t>(ow) * 2 * oh * 3);
    for (int e = 0; e < 2; ++e) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(g_d3d11Ctx->Map(staging[e], 0, D3D11_MAP_READ, 0, &m))) continue;
        D3D11_TEXTURE2D_DESC td;
        staging[e]->GetDesc(&td);
        bool bgra = td.Format == DXGI_FORMAT_B8G8R8A8_UNORM || td.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        for (UINT y = 0; y < oh; ++y) {
            const uint8_t* row = static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y * 2) * m.RowPitch;
            for (UINT x = 0; x < ow; ++x) {
                const uint8_t* px = row + x * 2 * 4;
                uint8_t* out = &rgb[(static_cast<size_t>(y) * ow * 2 + e * ow + x) * 3];
                out[0] = bgra ? px[0] : px[2];  // BMP = B, G, R
                out[1] = px[1];
                out[2] = bgra ? px[2] : px[0];
            }
        }
        g_d3d11Ctx->Unmap(staging[e], 0);
        staging[e]->Release();
    }
    UINT bw = ow * 2;
    int rowBytes = (bw * 3 + 3) & ~3;
    BITMAPFILEHEADER fh = {};
    BITMAPINFOHEADER ih = {};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + rowBytes * oh;
    ih.biSize = sizeof(ih);
    ih.biWidth = bw;
    ih.biHeight = oh;
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) return;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    std::vector<uint8_t> pad(rowBytes - bw * 3, 0);
    for (int y = static_cast<int>(oh) - 1; y >= 0; --y) {
        fwrite(&rgb[static_cast<size_t>(y) * bw * 3], 1, bw * 3, f);
        if (!pad.empty()) fwrite(pad.data(), 1, pad.size(), f);
    }
    fclose(f);
    vr::log::Line("MIRRORSHOT saved: " + path);
}

// Thread di uscita: aspetta una coppia di occhi pronta, attende il ritmo del
// visore, prende la coppia piu' recente, la copia nelle texture D3D11 e la
// invia. Anche le chiamate al compositor non legate al frame (finestra
// specchio, cattura) stanno qui, cosi' il contesto D3D11 ha un solo thread.
void WorkerLoop() {
    vr::log::Line("VR-OUTPUT: output thread started.");
    int submits = 0;
    double windowStart = QpcMs();
    double copyMs = 0.0, waitMs = 0.0, submitMs = 0.0;
    while (!g_workerStop) {
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            if (!g_cv.wait_for(lock, std::chrono::milliseconds(100),
                               [] { return g_frameReady || g_workerStop.load(); }))
                continue;
        }
        if (g_workerStop) break;
        vr::session::Shared vrLock;
        vr::IVRCompositor* compositor = vr::session::IsScene() ? vr::VRCompositor() : nullptr;
        if (!compositor || g_disabled) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_frameReady = false;  // niente compositor (Background): coppia scartata
            g_cv.notify_all();
            continue;
        }
        {
            std::string mirrorPath;
            if (vr::control::TakeMirrorShotRequest(&mirrorPath)) SaveMirrorShot(compositor, mirrorPath);
            int mirror = vr::control::g_mirrorRequest.exchange(-1);
            if (mirror == 1) compositor->ShowMirrorWindow();
            if (mirror == 0) compositor->HideMirrorWindow();
        }
        double tw = QpcMs();
        if (vr::control::g_outputTest < 2) {
            vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
            compositor->WaitGetPoses(poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
        }
        waitMs += QpcMs() - tw;
        EyePacket pk[2];
        ID3D11Texture2D* tex[2] = {};
        vr::VRTextureBounds_t bounds[2];
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!g_frameReady) continue;  // annullata (Reset / ricreazione)
            pk[0] = g_ready[0];
            pk[1] = g_ready[1];
            g_ready[0] = g_ready[1] = EyePacket{};
            g_frameReady = false;
            g_workerBusy = true;
            tex[0] = g_eyeTexture[0];
            tex[1] = g_eyeTexture[1];
            bounds[0] = g_eyeBounds[0];
            bounds[1] = g_eyeBounds[1];
        }
        g_cv.notify_all();  // il thread di rendering puo' preparare il frame seguente
        double tc = QpcMs();
        bool cinema = pk[0].cinema;
        UINT cinemaTop = 0, cinemaBottom = pk[0].h;
        if (cinema) {
            // Schermo virtuale: l'immagine 16:9 va nella texture dell'overlay.
            if (pk[0].bits && EnsureCinemaTextures(pk[0].w, pk[0].h)) {
                D3D11_BOX box = {0, 0, 0, pk[0].w, pk[0].h, 1};
                g_d3d11Ctx->UpdateSubresource(g_cinemaTex, 0, &box, pk[0].bits, pk[0].pitch, 0);
                DetectContentRows(pk[0].bits, pk[0].pitch, pk[0].w, pk[0].h, &cinemaTop, &cinemaBottom);
            }
        } else {
            for (int e = 0; e < 2; ++e) {
                if (!tex[e] || vr::control::g_outputTest == 1 || vr::control::g_outputTest == 2) continue;
                if (pk[e].gpuSrc) {
                    g_d3d11Ctx->CopyResource(tex[e], pk[e].gpuSrc);  // GPU -> GPU
                    continue;
                }
                if (!pk[e].bits) continue;
                D3D11_BOX box = {0, 0, 0, pk[e].w, pk[e].h, 1};
                g_d3d11Ctx->UpdateSubresource(tex[e], 0, &box, pk[e].bits, pk[e].pitch, 0);
            }
        }
        copyMs += QpcMs() - tc;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            for (int e = 0; e < 2; ++e)
                if (pk[e].slot >= 0) g_doneSlots[e].push_back(pk[e].slot);
            g_workerBusy = false;
        }
        g_cv.notify_all();
        if (cinema && g_cinemaTex) {
            if (!g_cinemaShown) ShowCinemaOverlay();
            if (vr::VROverlay() && g_cinemaOverlay != vr::k_ulOverlayHandleInvalid) {
                vr::Texture_t ot = {g_cinemaTex, vr::TextureType_DirectX, vr::ColorSpace_Gamma};
                vr::VROverlay()->SetOverlayTexture(g_cinemaOverlay, &ot);
                // Solo l'area con contenuto: 16:9 nelle cinematiche, intera nei menu.
                if (cinemaBottom > cinemaTop + 16) {
                    vr::VRTextureBounds_t ob = {0.0f, float(cinemaTop) / g_cinemaTexH, 1.0f,
                                                float(cinemaBottom) / g_cinemaTexH};
                    vr::VROverlay()->SetOverlayTextureBounds(g_cinemaOverlay, &ob);
                }
            }
        } else if (!cinema) {
            HideCinemaOverlay();
        }
        if (!tex[0] || !tex[1]) continue;
        double ts = QpcMs();
        vr::Texture_t texL = {cinema && g_blackTex ? g_blackTex : tex[0], vr::TextureType_DirectX, vr::ColorSpace_Gamma};
        vr::Texture_t texR = {cinema && g_blackTex ? g_blackTex : tex[1], vr::TextureType_DirectX, vr::ColorSpace_Gamma};
        vr::VRTextureBounds_t full = {0.0f, 0.0f, 1.0f, 1.0f};
        vr::EVRCompositorError errL = compositor->Submit(vr::Eye_Left, &texL, cinema ? &full : &bounds[0]);
        vr::EVRCompositorError errR = compositor->Submit(vr::Eye_Right, &texR, cinema ? &full : &bounds[1]);
        submitMs += QpcMs() - ts;
        ++submits;
        if (submits == 1 || submits % 300 == 0 || (errL != vr::VRCompositorError_None && submits % 120 == 0)) {
            double now = QpcMs();
            double n = submits == 1 ? 1.0 : 300.0;
            char b[300];
            std::snprintf(b, sizeof(b),
                          "VR-OUTPUT: submit %d L=%d R=%d [D3D11, dedicated thread] %.1f submits/s; per submit ms: "
                          "copy=%.2f headset wait=%.2f Submit=%.2f",
                          submits, static_cast<int>(errL), static_cast<int>(errR), 1000.0 * n / (now - windowStart),
                          copyMs / n, waitMs / n, submitMs / n);
            vr::log::Line(b);
            windowStart = now;
            copyMs = waitMs = submitMs = 0.0;
        }
    }
    vr::log::Line("VR-OUTPUT: output thread stopped.");
}

void StartWorker() {
    // Un solo thread per tutta la vita del processo (dopo detach joinable()
    // e' sempre falso: serve un indicatore a parte).
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;
    g_workerStop = false;
    g_worker = std::thread(WorkerLoop);
    g_worker.detach();  // termina con il processo; mai unito (DllMain)
}

}  // namespace

RECT EyeCropRect(int eye) {
    RECT r = g_eyeCrop[eye == 1 ? 1 : 0];
    if (r.right <= r.left) {
        // Senza visore (nessun ritaglio calcolato): tutto il back buffer.
        r = {0, 0, static_cast<LONG>(g_width ? g_width : 0), static_cast<LONG>(g_height ? g_height : 0)};
    }
    return r;
}

void OnPresent(IDirect3DDevice9* device, int eye) {
    if (g_disabled || !vr::control::g_vrOutput) return;
    // Solo da applicazione Scene (visore indossato, vedi vr_session.h).
    if (!vr::session::IsScene()) return;
    // Visore non indossato / in standby: niente invio ne' WaitGetPoses. Il 16/10,
    // con SteamVR appena riavviato e il Quest in standby, WaitGetPoses faceva
    // attendere ogni frame per secondi e il gioco restava di fatto bloccato.
    {
        vr::session::Shared vrLock(true);
        if (!vrLock.ok()) return;  // OpenVR in riapertura: frame solo sul monitor
        vr::EDeviceActivityLevel level =
            vr::VRSystem() ? vr::VRSystem()->GetTrackedDeviceActivityLevel(vr::k_unTrackedDeviceIndex_Hmd)
                           : vr::k_EDeviceActivityLevel_Unknown;
        bool active = vr::session::HmdPresent();
        static int lastLevel = -99;
        if (level != lastLevel) {
            lastLevel = level;
            vr::log::Line("VR-OUTPUT: headset state " + std::to_string(level) +
                          (active ? " (active: sending to the headset)" : " (idle/standby: monitor only)"));
        }
        // In Scene si invia sempre (lo stato del visore con Link non e' affidabile);
        // solo prima del primo passaggio a Scene conta lo stato.
        (void)active;
    }
    if (!g_d3d11 && !InitD3D11()) {
        Disable("impossibile creare il device D3D11");
        return;
    }

    IDirect3DSurface9* backBuffer = nullptr;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer))) return;
    D3DSURFACE_DESC desc;
    backBuffer->GetDesc(&desc);
    static UINT boundsForW = 0, boundsForH = 0;
    if (desc.Width != boundsForW || desc.Height != boundsForH ||
        (!vr::control::g_fovAuto && g_boundsForFov != vr::control::g_fovOverride)) {
        if (UpdateFovAndBounds(desc.Width, desc.Height)) {
            boundsForW = desc.Width;
            boundsForH = desc.Height;
        }
    }

    static int frame = 0;
    ++frame;
    // Stereo: aggiorniamo solo l'occhio di questo frame (l'invio avviene dopo
    // il destro). Mono (nessuno stereo in corso, es. menu): entrambi gli
    // occhi dallo stesso frame, ciascuno con il suo ritaglio.
    bool cinema = vr::control::g_cinemaActive;
    // Schermo virtuale: tutto il back buffer (le eventuali bande nere del
    // 16:9 le toglie il thread di uscita, vedi DetectContentRows).
    RECT cinemaRect = {0, 0, static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height)};
    if (cinema || vr::control::g_stereoHalfIpd <= 0) {
        eye = -1;
    } else if (eye < 0 && GetTickCount64() - vr::control::g_lastStereoDrawMs < 200) {
        // Stereo attivo ma occhio non riconosciuto (raro): meglio saltare il
        // frame che mandare a entrambi gli occhi la stessa immagine.
        backBuffer->Release();
        return;
    }

    double t0 = QpcMs();
    float scale = vr::control::g_outputScale;
    UINT texW = static_cast<UINT>(g_cropW * scale + 0.5f), texH = static_cast<UINT>(g_cropH * scale + 0.5f);
    bool ok = EnsureResources(device, texW, texH);
    if (!ok) {
        backBuffer->Release();
        return;
    }
    // Sblocca le superfici che il thread di uscita ha finito di leggere.
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (int e = 0; e < 2; ++e) {
            for (int k : g_doneSlots[e]) {
                if (g_slotState[e][k] == kLocked && !g_slotGpu[e][k]) g_sysmem[e][k]->UnlockRect();
                g_slotState[e][k] = kFree;
            }
            g_doneSlots[e].clear();
        }
    }
    for (int e = 0; e < 2; ++e) {
        if (eye >= 0 && e != eye) continue;
        if (cinema && e == 1) continue;  // schermo virtuale: una sola immagine
        // 1) La lettura avviata al tick precedente e' pronta: la si blocca e
        //    la si consegna (sostituendo un'eventuale consegna non ancora presa).
        int g = g_gpuSlot[e];
        if (g >= 0 && g_slotGpu[e][g]) {
            // Percorso GPU: si aspetta (di solito gia' finito) lo StretchRect
            // del tick precedente, poi la texture condivisa va al thread di uscita.
            double tq = QpcMs();
            while (g_shQuery[e][g]->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE && QpcMs() - tq < 8.0)
                YieldProcessor();
            g_slotState[e][g] = kLocked;
            std::lock_guard<std::mutex> lock(g_mutex);
            int old = g_ready[e].slot;
            if (old >= 0) {
                if (!g_slotGpu[e][old]) g_sysmem[e][old]->UnlockRect();
                g_slotState[e][old] = kFree;
            }
            EyePacket pk;
            pk.slot = g;
            pk.w = g_slotW[e][g];
            pk.h = g_slotH[e][g];
            pk.gpuSrc = g_shTex11[e][g];
            g_ready[e] = pk;
            g_gpuSlot[e] = -1;
        } else if (g >= 0) {
            D3DLOCKED_RECT lr;
            if (SUCCEEDED(g_sysmem[e][g]->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
                g_slotState[e][g] = kLocked;
                std::lock_guard<std::mutex> lock(g_mutex);
                int old = g_ready[e].slot;
                if (old >= 0) {
                    if (!g_slotGpu[e][old]) g_sysmem[e][old]->UnlockRect();
                    g_slotState[e][old] = kFree;
                }
                g_ready[e] = {g, static_cast<const uint8_t*>(lr.pBits), static_cast<UINT>(lr.Pitch),
                              g_slotW[e][g], g_slotH[e][g], g_slotCinema[e][g]};
            } else {
                g_slotState[e][g] = kFree;
            }
            g_gpuSlot[e] = -1;
        }
        // 2) Nuova lettura asincrona del ritaglio di questo occhio.
        int s = -1;
        for (int k = 0; k < kSlots; ++k)
            if (g_slotState[e][k] == kFree) {
                s = k;
                break;
            }
        if (s < 0 || vr::control::g_outputTest == 1 || vr::control::g_outputTest == 2) continue;
        const RECT& src = cinema ? cinemaRect : g_eyeCrop[e];
        UINT sw = static_cast<UINT>((src.right - src.left) * scale + 0.5f);
        UINT sh = static_cast<UINT>((src.bottom - src.top) * scale + 0.5f);
        if (cinema) {
            // Tutto il back buffer nella superficie, mantenendo le proporzioni.
            float a = static_cast<float>(desc.Width) / static_cast<float>(desc.Height);
            sw = texW;
            sh = static_cast<UINT>(texW / a);
            if (sh > texH) {
                sh = texH;
                sw = static_cast<UINT>(texH * a);
            }
        }
        if (sw > texW) sw = texW;
        if (sh > texH) sh = texH;
        RECT dst = {0, 0, static_cast<LONG>(sw), static_cast<LONG>(sh)};
        bool gpu = g_gpuReady && !cinema && vr::control::g_gpuCopy;
        if (gpu) {
            // Percorso GPU: ritaglio direttamente nella texture condivisa.
            if (FAILED(device->StretchRect(backBuffer, &src, g_shSurf9[e][s], &dst,
                                           scale < 0.999f ? D3DTEXF_LINEAR : D3DTEXF_NONE))) {
                if (frame % 300 == 1) vr::log::Line("VR-OUTPUT: StretchRect into the shared surface failed.");
                continue;
            }
            g_shQuery[e][s]->Issue(D3DISSUE_END);
            g_slotState[e][s] = kGpu;
            g_slotGpu[e][s] = true;
            g_slotW[e][s] = sw;
            g_slotH[e][s] = sh;
            g_slotCinema[e][s] = false;
            g_gpuSlot[e] = s;
            continue;
        }
        if (FAILED(device->StretchRect(backBuffer, &src, g_sharedSurface, &dst,
                                       cinema || scale < 0.999f ? D3DTEXF_LINEAR : D3DTEXF_NONE)) ||
            FAILED(device->GetRenderTargetData(g_sharedSurface, g_sysmem[e][s]))) {
            if (frame % 300 == 1) vr::log::Line("VR-OUTPUT: StretchRect/GetRenderTargetData failed.");
            continue;
        }
        g_slotState[e][s] = kGpu;
        g_slotW[e][s] = sw;
        g_slotH[e][s] = sh;
        g_slotCinema[e][s] = cinema;
        g_slotGpu[e][s] = false;
        g_gpuSlot[e] = s;
    }
    backBuffer->Release();
    g_copyMsAccum += QpcMs() - t0;

    // Stereo: la coppia e' completa dopo l'occhio destro (mono: subito).
    if (eye == 0) return;
    {
        std::unique_lock<std::mutex> lock(g_mutex);
        bool both = g_ready[0].slot >= 0 && (g_ready[1].slot >= 0 || g_ready[0].cinema);
        if (both) g_frameReady = true;
        g_cv.notify_all();
        // Ritmo del gioco = ritmo del visore: si aspetta (al massimo 25 ms)
        // che il thread di uscita prenda la coppia appena consegnata, mentre
        // la GPU disegna gia' il frame seguente. Con outputtest 2 (nessuna
        // attesa del visore) il gioco corre libero.
        if (both && vr::control::g_outputTest < 2) {
            double tw = QpcMs();
            g_cv.wait_for(lock, std::chrono::milliseconds(25), [] { return !g_frameReady; });
            g_waitMs += QpcMs() - tw;
        }
    }
}

}  // namespace vr::output
