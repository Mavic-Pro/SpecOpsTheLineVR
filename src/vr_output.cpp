#include "vr_output.h"
#include "control.h"
#include "logger.h"
#include "vr_session.h"
#include "vr_toast.h"
#include "vr_menu.h"
#include "vr_wrist.h"
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

namespace vr::display {
HWND GameWindow();
}

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
bool g_disabled = false;  // errore dell'uscita VR: sospesa fino a g_disabledUntil (vedi Disable)
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
bool g_slotPoseValid[2][kSlots] = {};
vr::HmdMatrix34_t g_slotPose[2][kSlots] = {};
// Momento (ProfMs/QpcMs) in cui e' stata campionata la posa della testa del frame.
double g_slotSampleMs[2][kSlots] = {};
double g_renderSampleMs[2] = {};
// Ritardo misurato dal campionamento della testa ai fotoni (ms, media mobile;
// 0 = ancora nessuna misura): serve a "predict auto".
std::atomic<float> g_photonLatencyMs{0.0f};
// Posa con cui e' stato disegnato l'ultimo frame di ciascun occhio (thread di rendering).
bool g_renderPoseValid[2] = {};
uint32_t g_renderTick[2] = {};
uint32_t g_slotTick[2][kSlots] = {};
// Numero progressivo dei Present (thread di rendering): i due occhi di una
// coppia valida sono presentati uno dopo l'altro.
uint32_t g_presentSeq = 0;
uint32_t g_slotSeq[2][kSlots] = {};
// Attese sulle fence scadute (copia D3D9 StretchRect 8 ms, copia D3D11 10 ms):
// lo slot viene consegnato lo stesso; se capita, un frame puo' mescolare due
// tick (L2). Contate nella riga VR-OUTPUT.
std::atomic<unsigned> g_fenceTimeout9{0}, g_fenceTimeout11{0};
vr::HmdMatrix34_t g_renderPose[2] = {};
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
    uint32_t tick = 0;                  // tick stereo del frame (diagnostica)
    uint32_t seq = 0;                   // numero del Present che l'ha prodotto
    bool poseValid = false;             // posa del visore usata per disegnarlo
    vr::HmdMatrix34_t pose = {};
    double sampleMs = 0.0;              // campionamento della testa usata per disegnarlo
    vr::VRTextureBounds_t bounds = {0, 0, 1, 1};  // parte della texture con l'immagine
};
std::mutex g_mutex;
std::condition_variable g_cv;
EyePacket g_ready[2];            // ultimo frame bloccato per occhio, non ancora preso
int g_pairsDropped = 0;          // coppie in attesa scartate per un sinistro nuovo
// Ritaglio dello schermo virtuale tenuto stabile (thread di uscita).
UINT g_cropTop = 0, g_cropBottom = 0, g_cropCandTop = 0, g_cropCandBottom = 0;
int g_cropCandFrames = 0;
bool g_cropSession = false;
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
// Tangenti dei frustum degli occhi (GetProjectionRaw: sinistra, destra, alto,
// basso; alto negativo = in su), per la proiezione decentrata.
std::atomic<float> g_tan[2][4];
std::atomic<bool> g_tanValid{false};
// Display inclinati (Pimax e simili): rotazione di ogni occhio rispetto alla
// testa (GetEyeToHeadTransform), come assi avanti/destra/su dell'occhio nella
// testa di UE3 (componenti avanti, destra, su).
std::atomic<float> g_eyeAxes[2][9];
std::atomic<bool> g_eyeCanted[2];
std::atomic<float> g_hmdHz{0.0f};
std::atomic<float> g_sameTickWaitMs{0.0f};
int g_renderPercent[2] = {100, 100};
// Immagine disegnata con la proiezione decentrata (tutto il back buffer e'
// l'occhio): niente ritaglio. Per occhio, dal frame tag.
bool g_renderOffCenter[2] = {};
vr::VRTextureBounds_t g_slotBounds[2][kSlots] = {};
UINT g_cropW = 0, g_cropH = 0;
float g_boundsForFov = -1.0f;

std::string Hex(unsigned long v) {
    char b[16];
    std::snprintf(b, sizeof(b), "0x%08lX", v);
    return b;
}

// Errore dell'uscita VR: si riprova con attesa crescente (5, 10, 20, 40, 60 s)
// invece di spegnere l'uscita per tutta la partita (L22: un errore D3D11
// passeggero lasciava il visore nero fino al riavvio).
ULONGLONG g_disabledUntil = 0;
unsigned g_disableBackoffMs = 5000;
void Disable(const std::string& why) {
    g_disabledUntil = GetTickCount64() + g_disableBackoffMs;
    vr::log::Line("VR-OUTPUT disabled for " + std::to_string(g_disableBackoffMs / 1000) + " s: " + why);
    vr::toast::Notify("VR output paused for " + std::to_string(g_disableBackoffMs / 1000) + " s after an error");
    if (g_disableBackoffMs < 60000) g_disableBackoffMs *= 2;
    g_disabled = true;
}
bool OutputDisabled() {
    if (!g_disabled) return false;
    if (GetTickCount64() < g_disabledUntil) return true;
    g_disabled = false;
    vr::log::Line("VR-OUTPUT: retrying after the error.");
    return false;
}

bool UpdateFovAndBounds(UINT bbW, UINT bbH) {
    float aspect = static_cast<float>(bbW) / static_cast<float>(bbH);
    vr::session::Shared vrLock(true);
    vr::IVRSystem* sys = vrLock.ok() ? vr::VRSystem() : nullptr;
    if (!sys) return false;
    float l[2], r[2], t[2], b[2];
    float maxH = 0.0f, maxV = 0.0f;
    for (int e = 0; e < 2; ++e) {
        float rl, rr, rt, rb;
        sys->GetProjectionRaw(static_cast<vr::EVREye>(e), &rl, &rr, &rt, &rb);
        // Il verso di "top"/"bottom" di GetProjectionRaw e' ambiguo (in
        // ComposeProjection di OpenVR "bottom" e' il lato in alto): fino alla
        // 0.9.5 la mod li usava al contrario e su Quest 3 (55 gradi in basso,
        // 44 in alto) disegnava il frustum capovolto in verticale, immagine
        // deformata muovendo la testa (19/10, "fisheye" segnalato con Virtual
        // Desktop; sparisce riducendo il FOV inferiore in SteamVR). Le
        // tangenti si ricavano dalla matrice che compone SteamVR stesso (stile
        // OpenGL: y in su, avanti = -z): in NDC +-1 ai bordi.
        vr::HmdMatrix44_t pm = sys->GetProjectionMatrix(static_cast<vr::EVREye>(e), 1.0f, 1000.0f);
        float p00 = pm.m[0][0], p02 = pm.m[0][2], p11 = pm.m[1][1], p12 = pm.m[1][2];
        if (p00 > 0.05f && p11 > 0.05f && p00 < 100.0f && p11 < 100.0f && std::isfinite(p02) && std::isfinite(p12)) {
            l[e] = (p02 - 1.0f) / p00;
            r[e] = (p02 + 1.0f) / p00;
            t[e] = -(p12 + 1.0f) / p11;  // convenzione della mod: alto negativo = in su
            b[e] = -(p12 - 1.0f) / p11;
        } else {
            l[e] = rl; r[e] = rr; t[e] = rt; b[e] = rb;
        }
        if (vr::control::g_projFlip) std::swap(t[e], b[e]), t[e] = -t[e], b[e] = -b[e];
        if (std::fabs(t[e] - rt) > 0.01f || std::fabs(b[e] - rb) > 0.01f || std::fabs(l[e] - rl) > 0.01f) {
            char pb[200];
            std::snprintf(pb, sizeof(pb),
                          "VR-FOV: %s eye raw tangents L%.3f R%.3f T%.3f B%.3f; from the projection matrix: "
                          "up %.3f down %.3f", e ? "right" : "left", rl, rr, rt, rb, -t[e], b[e]);
            vr::log::Line(pb);
        }
        vr::HmdMatrix34_t eh = sys->GetEyeToHeadTransform(static_cast<vr::EVREye>(e));
        // Colonne OpenVR: x destra, y su, z indietro; UE3: (avanti, destra, su) = (-z, x, y).
        float ax[9] = {eh.m[2][2],  -eh.m[0][2], -eh.m[1][2],   // avanti = -colonna z
                       -eh.m[2][0], eh.m[0][0],  eh.m[1][0],    // destra = colonna x
                       -eh.m[2][1], eh.m[0][1],  eh.m[1][1]};   // su = colonna y
        for (int i = 0; i < 9; ++i) g_eyeAxes[e][i] = ax[i];
        bool canted = ax[0] < 0.99999f;  // piu' di ~0,25 gradi
        if (canted != g_eyeCanted[e]) {
            char cb[160];
            std::snprintf(cb, sizeof(cb), "VR-FOV: %s eye canted %.1f degrees (yaw %.1f) relative to the head.",
                          e ? "right" : "left", std::acos((std::min)(1.0f, ax[0])) * 57.2957795f,
                          std::atan2(ax[1], ax[0]) * 57.2957795f);
            vr::log::Line(cb);
        }
        g_eyeCanted[e] = canted;
        g_tan[e][0] = l[e];
        g_tan[e][1] = r[e];
        g_tan[e][2] = t[e];
        g_tan[e][3] = b[e];
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
    g_tanValid = true;
    g_hmdHz = sys->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_DisplayFrequency_Float);
    // Proiezione decentrata: ogni occhio riempie tutto il back buffer, quindi
    // le texture devono poterlo contenere.
    if (vr::control::g_offCenter) {
        g_cropW = std::max<UINT>(g_cropW, bbW);
        g_cropH = std::max<UINT>(g_cropH, bbH);
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
    // Fino a 5 s (prima 500 ms): un Submit bloccato da un intoppo di SteamVR
    // durante un Reset faceva liberare superfici ancora lette dal thread di
    // uscita (audit 20/10).
    if (!g_cv.wait_for(lock, std::chrono::milliseconds(5000), [] { return !g_workerBusy; }))
        vr::log::Line("VR-OUTPUT: output thread still busy after 5 s, releasing resources anyway.");
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
// Percorso GPU dello schermo virtuale (R-C5): il frame arriva nella texture
// condivisa come per gli occhi e viene copiato GPU -> GPU nella texture
// dell'overlay; le righe con contenuto (bande nere) si leggono dalla CPU da
// una copia di staging ogni 15 frame (prima tutto il frame, ~5,7 MB, passava
// dalla CPU a ogni tick: menu e filmati a meta' frequenza).
ID3D11Texture2D* g_cinemaStage = nullptr;
bool g_cinemaSbs = false;  // overlay in stereo fianco a fianco (cinema3d)
bool g_cinemaStagePending = false;
unsigned g_cinemaFrames = 0;

bool EnsureCinemaTextures(UINT w, UINT h) {
    if (!g_blackTex) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = td.Height = 16;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        // Grigio molto scuro invece del nero assoluto: lo schermo virtuale non
        // galleggia nel vuoto (audit 19/10).
        std::vector<uint32_t> px(16 * 16, 0xFF0D0D10u);
        D3D11_SUBRESOURCE_DATA init = {px.data(), 16 * 4, 0};
        if (FAILED(g_d3d11->CreateTexture2D(&td, &init, &g_blackTex))) return false;
    }
    if (!g_cinemaTex || g_cinemaTexW != w || g_cinemaTexH != h) {
        if (g_cinemaTex) g_cinemaTex->Release();
        g_cinemaTex = nullptr;
        if (g_cinemaStage) g_cinemaStage->Release();
        g_cinemaStage = nullptr;
        g_cinemaStagePending = false;
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w;
        td.Height = h;
        td.MipLevels = td.ArraySize = 1;
        // Stesso formato della texture condivisa (copia GPU -> GPU, R-C5); il
        // back buffer del gioco ha un alfa qualsiasi e l'overlay lo userebbe
        // come trasparenza (immagine scura, cielo nero): l'overlay ha il flag
        // IgnoreTextureAlpha (prima: formato X8 senza alfa).
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(g_d3d11->CreateTexture2D(&td, nullptr, &g_cinemaTex))) return false;
        g_cinemaTexW = w;
        g_cinemaTexH = h;
        D3D11_TEXTURE2D_DESC sd = td;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(g_d3d11->CreateTexture2D(&sd, nullptr, &g_cinemaStage))) g_cinemaStage = nullptr;
    }
    return true;
}

void DetectContentRows(const uint8_t* bits, UINT pitch, UINT w, UINT h, UINT* top, UINT* bottom);
// Righe con contenuto sul percorso GPU: copia di staging ogni 15 frame, letta
// al frame seguente (la Map non aspetta la GPU). Ritorna true con top/bottom nuovi.
bool DetectContentRowsGpu(UINT* top, UINT* bottom) {
    if (!g_cinemaStage) return false;
    ++g_cinemaFrames;
    if (g_cinemaStagePending) {
        g_cinemaStagePending = false;
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (SUCCEEDED(g_d3d11Ctx->Map(g_cinemaStage, 0, D3D11_MAP_READ, 0, &m))) {
            DetectContentRows(static_cast<const uint8_t*>(m.pData), m.RowPitch, g_cinemaTexW, g_cinemaTexH, top, bottom);
            g_d3d11Ctx->Unmap(g_cinemaStage, 0);
            return true;
        }
        return false;
    }
    if (g_cinemaFrames % 15 == 0) {
        g_d3d11Ctx->CopyResource(g_cinemaStage, g_cinemaTex);
        g_cinemaStagePending = true;
    }
    return false;
}

// Laser sullo schermo virtuale (R-U5): il raggio del controller dell'arma
// (-Z della posa, spazio standing) interseca l'overlay dello schermo; il
// punto UV diventa un pixel della fascia mostrata e quindi un punto della
// finestra del gioco: cursore di Windows li' (i menu UIScene del gioco
// seguono il cursore e mostrano il loro), grilletto = clic sinistro. Niente
// input focus di SteamVR sull'overlay: i tasti restano al gioco (oggi un
// overlay interattivo aveva "spento" i tasti, 21/10). Non con il menu del mod aperto.
void CinemaLaserTick(bool cinema) {
    static bool triggerWas = false, hitWas = false;
    static int logged = 0;
    if (!cinema || !g_cinemaShown || !vr::control::g_cinemaLaser || vr::menu::IsOpen() ||
        !vr::control::g_gunCtrlPoseValid || g_cinemaOverlay == vr::k_ulOverlayHandleInvalid) {
        hitWas = false;
        return;
    }
    vr::IVROverlay* ov = vr::VROverlay();
    HWND h = vr::display::GameWindow();
    if (!ov || !h) return;
    float m[12];
    for (int i = 0; i < 12; ++i) m[i] = vr::control::g_gunCtrlPose[i].load();
    vr::VROverlayIntersectionParams_t p = {};
    p.vSource = {m[3], m[7], m[11]};
    p.vDirection = {-m[2], -m[6], -m[10]};
    p.eOrigin = vr::TrackingUniverseStanding;
    vr::VROverlayIntersectionResults_t r = {};
    if (!ov->ComputeOverlayIntersection(g_cinemaOverlay, &p, &r)) {
        hitWas = false;
        return;
    }
    float u = r.vUVs.v[0], v = r.vUVs.v[1];
    if (!vr::control::g_cinemaLaserFlipY) v = 1.0f - v;  // OpenVR: UV con origine in basso a sinistra (da provare)
    if (!(u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f) || !g_cinemaTexW || !g_cinemaTexH) return;
    UINT top = g_cropTop, bottom = g_cropBottom;
    if (vr::control::g_cinemaLaserFullUv || bottom <= top || bottom > g_cinemaTexH) {
        top = 0;
        bottom = g_cinemaTexH;
    }
    float texX = u * g_cinemaTexW, texY = top + v * (bottom - top);
    RECT rc;
    if (!GetClientRect(h, &rc) || rc.right <= rc.left || rc.bottom <= rc.top) return;
    POINT pt = {static_cast<LONG>(texX / g_cinemaTexW * (rc.right - rc.left)),
                static_cast<LONG>(texY / g_cinemaTexH * (rc.bottom - rc.top))};
    ClientToScreen(h, &pt);
    SetCursorPos(pt.x, pt.y);
    bool trigger = vr::control::g_rightTriggerHeld;
    bool edge = trigger != triggerWas;
    if (edge) {
        INPUT in = {};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = trigger ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        SendInput(1, &in, sizeof(in));
    }
    triggerWas = trigger;
    if ((!hitWas || edge) && logged < 6) {
        ++logged;
        char b[200];
        std::snprintf(b, sizeof(b), "LASER: screen hit uv=(%.3f, %.3f) rows %u-%u -> client (%ld, %ld) of %ldx%ld%s", u,
                      r.vUVs.v[1], top, bottom, pt.x, pt.y, rc.right - rc.left, rc.bottom - rc.top,
                      edge ? (trigger ? " click down" : " click up") : "");
        vr::log::Line(b);
    }
    hitWas = true;
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
        ov->SetOverlayFlag(g_cinemaOverlay, vr::VROverlayFlags_IgnoreTextureAlpha, true);  // alfa del back buffer qualsiasi
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
        // Le texture D3D11 sono usate solo dal thread di uscita: si aspetta che
        // abbia finito il Submit in corso (prima non si controllava).
        std::unique_lock<std::mutex> lock(g_mutex);
        g_cv.wait_for(lock, std::chrono::milliseconds(5000), [] { return !g_workerBusy; });
        ReleaseEyeTextures();
    }
    HRESULT hr = device->CreateRenderTarget(w, h, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE,
                                            &g_sharedSurface, nullptr);
    if (FAILED(hr)) {
        Disable("CreateRenderTarget fallita hr=" + Hex(hr));
        return false;
    }
    // Le superfici in memoria di sistema (lettura dalla CPU) si creano solo
    // quando servono (schermo virtuale, "gpushare off"): con la copia via GPU
    // erano ~170 MB di spazio di indirizzi sprecato in un processo a 32 bit
    // (audit 20/10). Vedi EnsureSysmem.
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

// Dissolvenze (audit 19/10: nessuna; tagli, teletrasporti e cambi di schermo
// erano netti). Richieste da qualsiasi thread, eseguite qui sotto.
std::atomic<float> g_blinkRequest{0.0f};
std::atomic<bool> g_holdBlack{false};
std::atomic<bool> g_holdInstant{false};

void ApplyFades(vr::IVRCompositor* compositor) {
    static bool held = false;
    bool hold = g_holdBlack.load();
    float blink = g_blinkRequest.exchange(0.0f);
    if (hold && !held) {
        // Alla fine dello schermo virtuale il nero deve esserci dal primo frame
        // in 3D (R-C4); alla morte resta la dissolvenza breve.
        compositor->FadeToColor(g_holdInstant.load() ? 0.0f : 0.08f, 0.0f, 0.0f, 0.0f, 1.0f);
        held = true;
    } else if (!hold && held) {
        compositor->FadeToColor(0.25f, 0.0f, 0.0f, 0.0f, 0.0f);
        held = false;
    } else if (!held && blink > 0.0f) {
        compositor->FadeToColor(0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
        compositor->FadeToColor(blink, 0.0f, 0.0f, 0.0f, 0.0f);
    }
}

// Thread di uscita: aspetta una coppia di occhi pronta, attende il ritmo del
// visore, prende la coppia piu' recente, la copia nelle texture D3D11 e la
// invia. Anche le chiamate al compositor non legate al frame (finestra
// specchio, cattura) stanno qui, cosi' il contesto D3D11 ha un solo thread.
void WorkerLoop() {
    vr::log::Line("VR-OUTPUT: output thread started.");
    int submits = 0, posedSubmits = 0, mismatchedPairs = 0;
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
        if (!compositor || (g_disabled && GetTickCount64() < g_disabledUntil)) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_frameReady = false;  // niente compositor (Background): coppia scartata
            g_cv.notify_all();
            continue;
        }
        ApplyFades(compositor);
        {
            std::string mirrorPath;
            if (vr::control::TakeMirrorShotRequest(&mirrorPath)) SaveMirrorShot(compositor, mirrorPath);
            int mirror = vr::control::g_mirrorRequest.exchange(-1);
            if (mirror == 1) compositor->ShowMirrorWindow();
            if (mirror == 0) compositor->HideMirrorWindow();
        }
        double tw = QpcMs();
        // "waitbeforesubmit on" (predefinito dal 21/10): l'attesa del
        // compositor sta dopo la copia, subito prima dell'invio, cosi' il
        // frame parte al "running start" e non 4-8 ms dopo il vsync. Prima
        // (attesa -> copia -> invio) con Virtual Desktop l'invio arrivava
        // tardi e il compositor saltava un frame su due (45 su 90, 36 su 72)
        // con la GPU libera.
        bool waitLate = vr::control::g_waitBeforeSubmit;
        if (vr::control::g_outputTest < 2 && !waitLate) {
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
            bounds[0] = pk[0].slot >= 0 ? pk[0].bounds : g_eyeBounds[0];
            bounds[1] = pk[1].slot >= 0 ? pk[1].bounds : g_eyeBounds[1];
            // Riferimenti propri finche' questo giro non ha finito (copia e
            // Submit): il thread di rendering puo' liberare texture e
            // superfici condivise allo scadere dei 5 s di attesa (M7).
            for (int e = 0; e < 2; ++e) {
                if (tex[e]) tex[e]->AddRef();
                if (pk[e].gpuSrc) pk[e].gpuSrc->AddRef();
            }
        }
        struct HeldRefs {
            ID3D11Texture2D* t[4];
            ~HeldRefs() {
                for (ID3D11Texture2D* x : t)
                    if (x) x->Release();
            }
        } held = {{tex[0], tex[1], pk[0].gpuSrc, pk[1].gpuSrc}};
        g_cv.notify_all();  // il thread di rendering puo' preparare il frame seguente
        double tc = QpcMs();
        bool cinema = pk[0].cinema;
        UINT cinemaTop = 0, cinemaBottom = pk[0].h;
        if (cinema) {
            // Schermo virtuale: l'immagine 16:9 va nella texture dell'overlay.
            // In stereo (cinema3d) i due occhi stanno fianco a fianco nella
            // stessa texture (larghezza doppia, flag SideBySide dell'overlay).
            bool fed = false;
            UINT top = 0, bottom = pk[0].h;
            bool rowsKnown = false;
            bool sbs = vr::control::g_cinema3d && (pk[1].gpuSrc || pk[1].bits) && pk[1].w == pk[0].w && pk[1].h == pk[0].h;
            UINT texW = sbs ? pk[0].w * 2 : pk[0].w;
            if (pk[0].gpuSrc && EnsureCinemaTextures(texW, pk[0].h)) {
                D3D11_BOX box = {0, 0, 0, pk[0].w, pk[0].h, 1};
                g_d3d11Ctx->CopySubresourceRegion(g_cinemaTex, 0, 0, 0, 0, pk[0].gpuSrc, 0, &box);  // GPU -> GPU
                if (sbs && pk[1].gpuSrc) g_d3d11Ctx->CopySubresourceRegion(g_cinemaTex, 0, pk[0].w, 0, 0, pk[1].gpuSrc, 0, &box);
                fed = true;
                rowsKnown = DetectContentRowsGpu(&top, &bottom);
                if (!rowsKnown) {  // nessuna lettura nuova: ritaglio invariato
                    top = g_cropSession ? g_cropTop : 0;
                    bottom = g_cropSession ? g_cropBottom : pk[0].h;
                }
            } else if (pk[0].bits && EnsureCinemaTextures(texW, pk[0].h)) {
                D3D11_BOX box = {0, 0, 0, pk[0].w, pk[0].h, 1};
                g_d3d11Ctx->UpdateSubresource(g_cinemaTex, 0, &box, pk[0].bits, pk[0].pitch, 0);
                if (sbs && pk[1].bits) {
                    D3D11_BOX box2 = {pk[0].w, 0, 0, pk[0].w * 2, pk[0].h, 1};
                    g_d3d11Ctx->UpdateSubresource(g_cinemaTex, 0, &box2, pk[1].bits, pk[1].pitch, 0);
                }
                DetectContentRows(pk[0].bits, pk[0].pitch, pk[0].w, pk[0].h, &top, &bottom);
                fed = true;
            }
            if (fed && vr::VROverlay() && g_cinemaOverlay != vr::k_ulOverlayHandleInvalid && g_cinemaSbs != sbs) {
                g_cinemaSbs = sbs;
                vr::VROverlay()->SetOverlayFlag(g_cinemaOverlay, vr::VROverlayFlags_SideBySide_Parallel, sbs);
                vr::log::Line(sbs ? "CINEMA: stereo screen (side by side)." : "CINEMA: flat screen.");
            }
            if (fed) {
                // Il gioco disegna menu e filmati in 16:9 dentro il back buffer
                // quasi quadrato: le bande vere sono quelle del 16:9 centrato.
                // Scene scure davano fasce piu' strette (il contenuto veniva
                // stirato: "menu ingrandito in una finestra", 21/10): una fascia
                // sotto il 95% dell'altezza 16:9 viene portata al 16:9 centrato.
                // (Nessuna fascia 16:9 forzata: i menu Scaleform sono a tutta
                // finestra e la fascia li tagliava sopra e sotto, 21/10.)
                // Ritaglio stabile: cambia solo se il nuovo resta uguale per 30
                // frame (scene scure e dissolvenze lo facevano saltare a ogni
                // frame) o all'inizio di ogni passaggio allo schermo virtuale.
                // Percorso GPU: le righe si leggono ogni 15 frame, quindi il
                // candidato va contato solo sulle letture vere (3 uguali =
                // ~45 frame); prima i frame senza lettura azzeravano il
                // contatore e il ritaglio restava quello del primo frame
                // (menu del gioco tagliato, 21/10).
                bool gpuRows = pk[0].gpuSrc != nullptr;
                const int need = gpuRows ? 3 : 30;
                if (gpuRows && !rowsKnown && g_cropSession) {
                    // nessuna lettura nuova: ritaglio e candidato invariati
                } else if (!g_cropSession) {
                    g_cropSession = true;
                    g_cropTop = top;
                    g_cropBottom = bottom;
                    g_cropCandFrames = 0;
                } else if (top != g_cropTop || bottom != g_cropBottom) {
                    if (top == g_cropCandTop && bottom == g_cropCandBottom) {
                        if (++g_cropCandFrames >= need) {
                            g_cropTop = top;
                            g_cropBottom = bottom;
                            g_cropCandFrames = 0;
                        }
                    } else {
                        g_cropCandTop = top;
                        g_cropCandBottom = bottom;
                        g_cropCandFrames = 1;
                    }
                } else {
                    g_cropCandFrames = 0;
                }
                cinemaTop = g_cropTop;
                cinemaBottom = g_cropBottom;
                {
                    static UINT lastTop = 0xFFFFFFFF, lastBottom = 0xFFFFFFFF;
                    if (cinemaTop != lastTop || cinemaBottom != lastBottom) {
                        lastTop = cinemaTop;
                        lastBottom = cinemaBottom;
                        vr::log::Line("CINEMA: content rows " + std::to_string(cinemaTop) + "-" + std::to_string(cinemaBottom) + " of " +
                                      std::to_string(pk[0].h) + " (texture " + std::to_string(g_cinemaTexW) + "x" + std::to_string(g_cinemaTexH) +
                                      ", gpu " + (pk[0].gpuSrc ? "yes" : "no") + ", stage " + (g_cinemaStage ? "yes" : "no") + ")");
                    }
                }
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
        // Le CopyResource dalle superfici condivise sono solo accodate: lo slot
        // torna al thread di rendering (che ci riscrive con StretchRect) solo a
        // copia finita, altrimenti un frame poteva mescolare due tick.
        if (pk[0].gpuSrc || pk[1].gpuSrc) {  // anche la copia nello schermo virtuale
            static ID3D11Query* copyDone = nullptr;
            if (!copyDone) {
                D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
                g_d3d11->CreateQuery(&qd, &copyDone);
            }
            if (copyDone) {
                g_d3d11Ctx->End(copyDone);
                double tq = QpcMs();
                BOOL doneQ = FALSE;
                while (g_d3d11Ctx->GetData(copyDone, &doneQ, sizeof(doneQ), 0) != S_OK && QpcMs() - tq < 10.0)
                    YieldProcessor();
                if (g_d3d11Ctx->GetData(copyDone, &doneQ, sizeof(doneQ), 0) != S_OK) ++g_fenceTimeout11;  // L2: contati
            }
        }
        copyMs += QpcMs() - tc;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            for (int e = 0; e < 2; ++e)
                if (pk[e].slot >= 0) g_doneSlots[e].push_back(pk[e].slot);
            // g_workerBusy resta vero fino a dopo il Submit: le texture degli
            // occhi non vanno liberate (EnsureResources) mentre il compositor
            // le legge.
        }
        g_cv.notify_all();
        auto endBusy = [] {
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_workerBusy = false;
            }
            g_cv.notify_all();
        };
        // Nuova connessione a SteamVR (riavvio, cambio Background/Scene): gli
        // overlay appartengono alla connessione vecchia. Senza ricrearlo lo
        // schermo virtuale restava grigio fino al riavvio del gioco (audit 20/10).
        {
            static unsigned overlayGen = 0;
            unsigned gen = vr::session::Generation();
            if (gen != overlayGen) {
                overlayGen = gen;
                g_cinemaOverlay = vr::k_ulOverlayHandleInvalid;
                g_cinemaShown = false;
                g_cinemaSbs = false;  // il flag stereo va rimesso sull'overlay nuovo
            }
        }
        // Il menu del mod e' opaco: lo schermo virtuale resta visibile dietro
        // (nascondendolo, chi indossava il visore girato altrove vedeva solo
        // nero e chiudeva il gioco, 21/10).
        if (cinema && g_cinemaTex) {
            // Ricentramento durante lo schermo virtuale: lo schermo si rimette
            // davanti (prima restava dov'era stato aperto).
            if (!g_cinemaShown || vr::control::g_cinemaRecenter.exchange(false)) ShowCinemaOverlay();
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
            g_cropSession = false;
        }
        vr::toast::Tick();  // avvisi nel visore (R-U1)
        vr::wrist::Tick();  // HUD al polso (R-U3)
        CinemaLaserTick(cinema);  // laser sui menu del gioco (R-U5)
        vr::menu::Tick(cinema);  // menu del mod: eventi del puntatore a ogni frame (dal thread della sessione erano ogni 0,5 s)
        if (!tex[0] || !tex[1]) {
            endBusy();
            continue;
        }
        // I due occhi devono essere stati presentati uno dopo l'altro: all'inizio
        // del gameplay un occhio veniva riconosciuto come l'altro e la coppia
        // univa un'immagine nuova a una vecchia (fucile doppio, 18/10). Coppia
        // spaiata: niente invio, il compositor ripete il frame precedente.
        // (Il tick dell'anello non basta: stando fermi l'origine e' uguale in
        // piu' tick e coppie buone sembravano spaiate.)
        uint32_t seqGap = pk[0].seq > pk[1].seq ? pk[0].seq - pk[1].seq : pk[1].seq - pk[0].seq;
        // Sperimentale, spento di default ("pairCheck on"): non toglieva lo
        // sdoppiamento iniziale e aggiungeva scatti (prova col visore 18/10).
        if (vr::control::g_pairCheck && !cinema && pk[0].slot >= 0 && pk[1].slot >= 0 && seqGap > 2) {
            ++mismatchedPairs;
            endBusy();
            continue;
        }
        if (waitLate && vr::control::g_outputTest < 2) {
            double tw2 = QpcMs();
            vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
            compositor->WaitGetPoses(poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
            waitMs += QpcMs() - tw2;
        }
        double ts = QpcMs();
        vr::Texture_t texL = {cinema && g_blackTex ? g_blackTex : tex[0], vr::TextureType_DirectX, vr::ColorSpace_Gamma};
        vr::Texture_t texR = {cinema && g_blackTex ? g_blackTex : tex[1], vr::TextureType_DirectX, vr::ColorSpace_Gamma};
        vr::VRTextureBounds_t full = {0.0f, 0.0f, 1.0f, 1.0f};
        vr::EVRCompositorError errL, errR;
        if (!cinema && vr::control::g_submitWithPose && pk[0].poseValid && pk[1].poseValid) {
            // Con la posa usata per disegnare: il compositor riproietta da
            // quella alla posa reale al momento della visualizzazione.
            vr::VRTextureWithPose_t pl = {}, pr = {};
            static_cast<vr::Texture_t&>(pl) = texL;
            static_cast<vr::Texture_t&>(pr) = texR;
            pl.mDeviceToAbsoluteTracking = pk[0].pose;
            pr.mDeviceToAbsoluteTracking = pk[1].pose;
            errL = compositor->Submit(vr::Eye_Left, &pl, &bounds[0], vr::Submit_TextureWithPose);
            errR = compositor->Submit(vr::Eye_Right, &pr, &bounds[1], vr::Submit_TextureWithPose);
            ++posedSubmits;
            // Rete di sicurezza: se il compositor rifiuta l'invio con la posa
            // (errore diverso da "nessun focus"), si torna all'invio normale.
            auto bad = [](vr::EVRCompositorError e) {
                return e != vr::VRCompositorError_None && e != vr::VRCompositorError_DoNotHaveFocus;
            };
            if (bad(errL) || bad(errR)) {
                vr::control::g_submitWithPose = false;
                vr::log::Line("VR-OUTPUT: submit with render pose refused (L=" + std::to_string(static_cast<int>(errL)) +
                              " R=" + std::to_string(static_cast<int>(errR)) + "), back to plain submit.");
                errL = compositor->Submit(vr::Eye_Left, &texL, &bounds[0]);
                errR = compositor->Submit(vr::Eye_Right, &texR, &bounds[1]);
            }
        } else {
            errL = compositor->Submit(vr::Eye_Left, &texL, cinema ? &full : &bounds[0]);
            errR = compositor->Submit(vr::Eye_Right, &texR, cinema ? &full : &bounds[1]);
        }
        {
            double took = QpcMs() - ts;
            submitMs += took;
            // Un Submit oltre 100 ms (compositor bloccato, SteamVR che si
            // riavvia) finisce nel log, al massimo una volta ogni 5 s (R-R3).
            static double lastSlowLog = 0.0;
            if (took > 100.0 && QpcMs() - lastSlowLog > 5000.0) {
                lastSlowLog = QpcMs();
                vr::log::Line("VR-OUTPUT: Submit took " + std::to_string(static_cast<int>(took)) + " ms (L=" +
                              std::to_string(static_cast<int>(errL)) + " R=" + std::to_string(static_cast<int>(errR)) + ").");
            }
        }
        endBusy();
        ++submits;
        // Ritardo reale testa -> fotoni di questo frame: dal campionamento della
        // posa all'invio, piu' il tempo dall'invio ai fotoni secondo il runtime
        // (fino al prossimo vsync + vsync->fotoni; con Link comprende la
        // trasmissione). Media mobile per "predict auto": ogni PC e visore ha
        // il suo valore.
        if (!cinema && pk[0].sampleMs > 0.0) {
            vr::IVRSystem* sys = vr::VRSystem();
            static float hz = 0.0f, vsyncToPhotons = 0.0f;
            static int propsAge = 0;
            if (sys && (propsAge++ % 600 == 0 || hz < 30.0f)) {
                hz = sys->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_DisplayFrequency_Float);
                vsyncToPhotons = sys->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,
                                                                    vr::Prop_SecondsFromVsyncToPhotons_Float);
            }
            float sinceVsync = 0.0f;
            uint64_t counter = 0;
            if (sys && hz > 30.0f && sys->GetTimeSinceLastVsync(&sinceVsync, &counter)) {
                float frame = 1.0f / hz;
                sinceVsync = std::fmod(sinceVsync < 0.0f ? 0.0f : sinceVsync, frame);
                double total = (QpcMs() - pk[0].sampleMs) + 1000.0 * (frame - sinceVsync + vsyncToPhotons);
                if (total > 0.0 && total < 200.0) {
                    float old = g_photonLatencyMs.load();
                    g_photonLatencyMs = old <= 0.0f ? static_cast<float>(total) : old + (static_cast<float>(total) - old) * 0.02f;
                }
            }
        }
        if (submits == 1 || submits % 300 == 0 || (errL != vr::VRCompositorError_None && submits % 120 == 0)) {
            double now = QpcMs();
            double n = submits == 1 ? 1.0 : 300.0;
            char b[300];
            std::snprintf(b, sizeof(b),
                          "VR-OUTPUT: submit %d L=%d R=%d [D3D11, dedicated thread] %.1f submits/s; per submit ms: "
                          "copy=%.2f headset wait=%.2f Submit=%.2f; with render pose %d; mismatched pairs skipped %d; stale pairs "
                          "dropped %d; fence timeouts d3d9=%u d3d11=%u",
                          submits, static_cast<int>(errL), static_cast<int>(errR), 1000.0 * n / (now - windowStart),
                          copyMs / n, waitMs / n, submitMs / n, posedSubmits, mismatchedPairs, g_pairsDropped,
                          g_fenceTimeout9.load(), g_fenceTimeout11.load());
            vr::log::Line(b);
            // Visore a meta' frequenza (SSW di Virtual Desktop, ASW di Link):
            // 36 invii/s su 72 Hz con la GPU libera = riproiezione continua,
            // fisheye e nausea (prova 21/10). Avviso nel visore una volta.
            {
                static int halfWindows = 0;
                static bool warned = false;
                double rate = 1000.0 * n / (now - windowStart);
                float hz = g_hmdHz.load();
                if (submits > 1 && hz > 0.0f && rate > 10.0 && rate < 0.6 * hz) ++halfWindows;
                else halfWindows = 0;
                if (halfWindows >= 2 && !warned) {
                    warned = true;
                    char w[160];
                    std::snprintf(w, sizeof(w), "Headset at half rate (%.0f of %.0f fps): turn off SSW / ASW in Virtual Desktop or Link",
                                  rate, hz);
                    vr::toast::Notify(w);
                    vr::log::Line(std::string("VR-OUTPUT: ") + w);
                }
            }
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

bool OffCenterProjection(int eye, EyeProjection* out) {
    if (eye < 0 || eye > 1 || !g_tanValid) return false;
    float l = g_tan[eye][0], r = g_tan[eye][1];
    float up = -g_tan[eye][2], down = -g_tan[eye][3];  // in su positivo, come la vista di UE3
    if (!(r - l > 0.2f) || !(up - down > 0.2f)) return false;
    out->m00 = 2.0f / (r - l);
    out->m20 = -(r + l) / (r - l);
    out->m11 = 2.0f / (up - down);
    out->m21 = -(up + down) / (up - down);
    return true;
}

float HmdRefreshHz() { return g_hmdHz; }

float PhotonLatencyMs() { return g_photonLatencyMs; }

void SetSameTickWait(float ms) { g_sameTickWaitMs = ms; }

void SetRenderScreenPercent(int eye, int percent) {
    if (eye >= 0 && eye <= 1) g_renderPercent[eye] = percent < 10 ? 100 : (percent > 100 ? 100 : percent);
}

bool EyeCant(int eye, float* fwd, float* right, float* up) {
    float sim = vr::control::g_simCantDeg;
    if (sim != 0.0f && eye >= 0 && eye <= 1) {
        // Prova senza Pimax: ogni occhio ruotato verso l'esterno di "sim" gradi.
        float a = (eye == 0 ? 1.0f : -1.0f) * sim / 57.2957795f;  // sinistro verso sinistra
        float s = std::sin(a), c = std::cos(a);
        fwd[0] = c; fwd[1] = -s; fwd[2] = 0.0f;
        right[0] = s; right[1] = c; right[2] = 0.0f;
        up[0] = 0.0f; up[1] = 0.0f; up[2] = 1.0f;
        return true;
    }
    if (eye < 0 || eye > 1 || !g_eyeCanted[eye]) return false;
    for (int i = 0; i < 3; ++i) {
        fwd[i] = g_eyeAxes[eye][i];
        right[i] = g_eyeAxes[eye][3 + i];
        up[i] = g_eyeAxes[eye][6 + i];
    }
    return true;
}

void SetRenderPose(int eye, const vr::HmdMatrix34_t* pose, uint32_t tick, bool offCenter, double sampleMs) {
    if (eye < 0 || eye > 1) return;
    g_renderSampleMs[eye] = sampleMs;
    g_renderOffCenter[eye] = offCenter;
    g_renderTick[eye] = tick;
    g_renderPoseValid[eye] = pose != nullptr;
    if (pose) g_renderPose[eye] = *pose;
}

RECT EyeCropRect(int eye) {
    RECT r = g_eyeCrop[eye == 1 ? 1 : 0];
    if (r.right <= r.left) {
        // Senza visore (nessun ritaglio calcolato): tutto il back buffer.
        r = {0, 0, static_cast<LONG>(g_width ? g_width : 0), static_cast<LONG>(g_height ? g_height : 0)};
    }
    return r;
}

// Con g_mutex preso. Un occhio sinistro nuovo mentre la coppia precedente
// e' ancora in attesa del thread di uscita: la coppia vecchia si scarta tutta.
// Prima si sostituiva solo il sinistro e il thread di uscita univa il
// sinistro del tick N al destro del tick N-1 (immagini doppie per un frame
// con attese del compositor oltre 25 ms, audit 19/10).
void DropPendingPairForNewLeft(int e) {
    if (e != 0 || !g_frameReady) return;
    int old = g_ready[1].slot;
    if (old >= 0) {
        if (!g_slotGpu[1][old]) g_sysmem[1][old]->UnlockRect();
        g_slotState[1][old] = kFree;
    }
    g_ready[1] = EyePacket{};
    g_frameReady = false;
    ++g_pairsDropped;
}

// Consegna al thread di uscita la texture condivisa dello slot (copia GPU finita).
void DeliverGpuSlot(int e, int g) {
    g_slotState[e][g] = kLocked;
    std::lock_guard<std::mutex> lock(g_mutex);
    DropPendingPairForNewLeft(e);
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
    // Schermo virtuale sul percorso GPU (R-C5): senza questo flag la coppia
    // non risultava mai completa (serve il destro) e al menu nessun frame
    // arrivava al compositor: SteamVR restava su "Prossimo" (21/10, VD).
    pk.cinema = g_slotCinema[e][g];
    pk.poseValid = g_slotPoseValid[e][g];
    pk.tick = g_slotTick[e][g];
    pk.seq = g_slotSeq[e][g];
    pk.pose = g_slotPose[e][g];
    pk.sampleMs = g_slotSampleMs[e][g];
    pk.bounds = g_slotBounds[e][g];
    g_ready[e] = pk;
    g_gpuSlot[e] = -1;
}

int g_sameTickOk = 0, g_sameTickMissed = 0;

void OnPresent(IDirect3DDevice9* device, int eye) {
    if (vr::control::g_exiting) return;
    if (OutputDisabled() || !vr::control::g_vrOutput) return;
    ++g_presentSeq;
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
    static bool boundsForFlip = false;
    if (boundsForFlip != vr::control::g_projFlip) {
        boundsForFlip = vr::control::g_projFlip;
        boundsForW = 0;  // ricalcolo
    }
    if (vr::control::g_vrProjectionDirty.exchange(false)) boundsForW = 0;  // IPD/visore/sessione cambiati
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
    // Niente Draw stereo da 1,5 s fuori dallo schermo virtuale (caricamenti
    // bloccanti, filmati di caricamento, menu): l'immagine va sullo schermo
    // virtuale invece che piatta a entrambi gli occhi (audit 19/10); al
    // ritorno del 3D le immagini in attesa si scartano. Va deciso prima del
    // controllo di ritorno qui sotto: deciderlo dopo faceva sembrare ogni frame
    // un "ritorno" e scartava le immagini in coda a ogni frame (20/10, test
    // con Virtual Desktop).
    if (!cinema && vr::control::g_stereoHalfIpd > 0 && eye < 0 &&
        GetTickCount64() - vr::control::g_lastStereoDrawMs >= 1500)
        cinema = true;
    // Schermo virtuale: tutto il back buffer (le eventuali bande nere del
    // 16:9 le toglie il thread di uscita, vedi DetectContentRows).
    RECT cinemaRect = {0, 0, static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height)};
    // Ritorno dallo schermo virtuale: durante la cinematica l'occhio destro non
    // viene letto, quindi resterebbe pronta un'immagine destra di prima della
    // cinematica e la prima coppia univa un sinistro nuovo a un destro vecchio
    // (fucile doppio all'inizio del gameplay, 18/10). Si scarta tutto cio' che
    // e' in attesa e si riparte da una coppia nuova.
    static bool wasCinema = false;
    if (wasCinema && !cinema) {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (int e = 0; e < 2; ++e) {
            int old = g_ready[e].slot;
            if (old >= 0) {
                if (!g_slotGpu[e][old]) g_sysmem[e][old]->UnlockRect();
                g_slotState[e][old] = kFree;
            }
            g_ready[e] = EyePacket{};
            if (g_gpuSlot[e] >= 0) {
                g_slotState[e][g_gpuSlot[e]] = kFree;
                g_gpuSlot[e] = -1;
            }
        }
        g_frameReady = false;
        vr::log::Line("VR-OUTPUT: back from the cinema screen, pending eye images dropped.");
    }
    wasCinema = cinema;
    // Schermo virtuale in stereo (cinema3d): i due disegni arrivano con il
    // loro occhio nell'etichetta e vanno in due slot; altrimenti una sola
    // immagine (occhio -1).
    bool cinema3d = cinema && vr::control::g_cinema3d && vr::control::g_cinemaActive && eye >= 0;
    if (cinema3d) {
        // tenuto l'occhio dell'etichetta
    } else if (cinema || vr::control::g_stereoHalfIpd <= 0) {
        eye = -1;
    } else if (eye < 0) {
        // Stereo attivo ma occhio non riconosciuto, o pausa breve del gioco
        // (salvataggio del checkpoint): si salta il frame e il visore ripete
        // l'ultima coppia. Con lo schermo virtuale gia' dopo 200 ms il
        // salvataggio mostrava per un attimo il back buffer piatto, che nel
        // visore sembrava il mondo sdoppiato (19/10).
        backBuffer->Release();
        return;
    }

    double t0 = QpcMs();
    float scale = vr::control::g_outputScale;
    UINT texW = static_cast<UINT>(g_cropW * scale + 0.5f), texH = static_cast<UINT>(g_cropH * scale + 0.5f);
    // Proiezione non ancora calcolata (SteamVR non pronto al primo frame):
    // si aspetta. Prima EnsureResources(0, 0) falliva e l'uscita verso il
    // visore restava spenta per tutta la sessione (audit 20/10).
    if (texW == 0 || texH == 0) {
        backBuffer->Release();
        return;
    }
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
        if (cinema && e == 1 && !cinema3d) continue;  // schermo virtuale piatto: una sola immagine
        // 1) La lettura avviata al tick precedente e' pronta: la si blocca e
        //    la si consegna (sostituendo un'eventuale consegna non ancora presa).
        int g = g_gpuSlot[e];
        if (g >= 0 && g_slotGpu[e][g]) {
            // Percorso GPU: si aspetta (di solito gia' finito) lo StretchRect
            // del tick precedente, poi la texture condivisa va al thread di uscita.
            double tq = QpcMs();
            while (g_shQuery[e][g]->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE && QpcMs() - tq < 8.0)
                YieldProcessor();
            if (g_shQuery[e][g]->GetData(nullptr, 0, 0) == S_FALSE) ++g_fenceTimeout9;  // consegnato lo stesso (L2: contati)
            DeliverGpuSlot(e, g);
        } else if (g >= 0) {
            D3DLOCKED_RECT lr;
            if (SUCCEEDED(g_sysmem[e][g]->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
                g_slotState[e][g] = kLocked;
                std::lock_guard<std::mutex> lock(g_mutex);
                DropPendingPairForNewLeft(e);
                int old = g_ready[e].slot;
                if (old >= 0) {
                    if (!g_slotGpu[e][old]) g_sysmem[e][old]->UnlockRect();
                    g_slotState[e][old] = kFree;
                }
                g_ready[e] = {g, static_cast<const uint8_t*>(lr.pBits), static_cast<UINT>(lr.Pitch),
                              g_slotW[e][g], g_slotH[e][g], g_slotCinema[e][g]};
                g_ready[e].poseValid = g_slotPoseValid[e][g];
                g_ready[e].tick = g_slotTick[e][g];
                g_ready[e].seq = g_slotSeq[e][g];
                g_ready[e].pose = g_slotPose[e][g];
                g_ready[e].sampleMs = g_slotSampleMs[e][g];
                g_ready[e].bounds = g_slotBounds[e][g];
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
        RECT fullRect = {0, 0, static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height)};
        RECT src = cinema ? cinemaRect : (g_renderOffCenter[e] ? fullRect : g_eyeCrop[e]);
        if (!cinema && g_renderPercent[e] < 100) {
            // Scena piu' piccola al centro del back buffer (ScreenPercentage).
            float k = g_renderPercent[e] / 100.0f, cx = desc.Width * 0.5f, cy = desc.Height * 0.5f;
            src = {static_cast<LONG>(cx + (src.left - cx) * k + 0.5f), static_cast<LONG>(cy + (src.top - cy) * k + 0.5f),
                   static_cast<LONG>(cx + (src.right - cx) * k + 0.5f), static_cast<LONG>(cy + (src.bottom - cy) * k + 0.5f)};
        }
        UINT sw = static_cast<UINT>((src.right - src.left) * scale + 0.5f);
        UINT sh = static_cast<UINT>((src.bottom - src.top) * scale + 0.5f);
        if (cinema) {
            // Tutto il back buffer nella superficie, mantenendo le proporzioni,
            // al massimo 1600 pixel di larghezza: la lettura dalla CPU di tutto
            // il back buffer (~28 MB a frame) faceva scattare menu e filmati
            // (audit 19/10).
            float a = static_cast<float>(desc.Width) / static_cast<float>(desc.Height);
            sw = (std::min)(texW, 1600u);
            sh = static_cast<UINT>(sw / a);
            if (sh > texH) {
                sh = texH;
                sw = static_cast<UINT>(texH * a);
            }
        }
        if (sw > texW) sw = texW;
        if (sh > texH) sh = texH;
        RECT dst = {0, 0, static_cast<LONG>(sw), static_cast<LONG>(sh)};
        vr::VRTextureBounds_t texBounds = {0.0f, 0.0f, float(sw) / texW, float(sh) / texH};
        bool gpu = g_gpuReady && vr::control::g_gpuCopy;  // anche lo schermo virtuale (R-C5)
        if (gpu) {
            // Percorso GPU: ritaglio direttamente nella texture condivisa.
            if (FAILED(device->StretchRect(backBuffer, &src, g_shSurf9[e][s], &dst,
                                           cinema || scale < 0.999f ? D3DTEXF_LINEAR : D3DTEXF_NONE))) {
                if (frame % 300 == 1) vr::log::Line("VR-OUTPUT: StretchRect into the shared surface failed.");
                continue;
            }
            g_shQuery[e][s]->Issue(D3DISSUE_END);
            g_slotState[e][s] = kGpu;
            g_slotGpu[e][s] = true;
            g_slotW[e][s] = sw;
            g_slotH[e][s] = sh;
            g_slotCinema[e][s] = cinema;
            g_slotPoseValid[e][s] = g_renderPoseValid[e];
            g_slotBounds[e][s] = texBounds;
            g_slotPose[e][s] = g_renderPose[e];
            g_slotSampleMs[e][s] = g_renderSampleMs[e];
            g_slotTick[e][s] = g_renderTick[e];
            g_slotSeq[e][s] = g_presentSeq;
            g_gpuSlot[e] = s;
            continue;
        }
        if (!g_sysmem[e][s] &&
            FAILED(device->CreateOffscreenPlainSurface(g_width, g_height, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                                                       &g_sysmem[e][s], nullptr))) {
            g_sysmem[e][s] = nullptr;
            if (frame % 300 == 1) vr::log::Line("VR-OUTPUT: CreateOffscreenPlainSurface failed.");
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
        g_slotPoseValid[e][s] = g_renderPoseValid[e];
        g_slotBounds[e][s] = texBounds;
        g_slotPose[e][s] = g_renderPose[e];
        g_slotSampleMs[e][s] = g_renderSampleMs[e];
        g_slotTick[e][s] = g_renderTick[e];
        g_slotSeq[e][s] = g_presentSeq;
        g_slotGpu[e][s] = false;
        g_gpuSlot[e] = s;
    }
    backBuffer->Release();
    g_copyMsAccum += QpcMs() - t0;

    // Invio nello stesso tick (bassa latenza): dopo l'occhio destro si aspetta
    // un poco che la GPU finisca entrambe le copie e si consegna subito la
    // coppia di questo tick, invece di quella del tick precedente (~un frame,
    // 14 ms a 72 Hz, in meno su mani e fucile). Solo se la GPU ha margine
    // (g_sameTickWaitMs > 0, deciso da hooks.cpp); la coppia va consegnata
    // intera o per niente: con un occhio solo si unirebbero due tick diversi.
    float waitBudget = g_sameTickWaitMs;
    // Solo se i due slot sono dello stesso tick (L3: l'occhio sinistro del
    // tick prima con il destro di questo univa due tick).
    // L3 (21/10): il vincolo "stessi tick" era sbagliato in pratica (i tick
    // dei due slot non coincidono mai: la riga "same-tick submit" non e'
    // piu' apparsa e con Virtual Desktop si andava a meta' frequenza);
    // tolto, si contano solo le coppie con tick diversi nel log.
    static unsigned tickMismatch = 0;
    if (eye == 1 && waitBudget > 0.0f && g_gpuSlot[0] >= 0 && g_gpuSlot[1] >= 0 && g_slotGpu[0][g_gpuSlot[0]] &&
        g_slotGpu[1][g_gpuSlot[1]] && g_slotTick[0][g_gpuSlot[0]] != g_slotTick[1][g_gpuSlot[1]])
        ++tickMismatch;
    if (eye == 1 && waitBudget > 0.0f && g_gpuSlot[0] >= 0 && g_gpuSlot[1] >= 0 && g_slotGpu[0][g_gpuSlot[0]] &&
        g_slotGpu[1][g_gpuSlot[1]]) {
        double tq = QpcMs();
        bool done = false;
        while (true) {
            done = g_shQuery[0][g_gpuSlot[0]]->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_OK &&
                   g_shQuery[1][g_gpuSlot[1]]->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_OK;
            if (done || QpcMs() - tq > waitBudget) break;
            YieldProcessor();
        }
        if (done) {
            DeliverGpuSlot(0, g_gpuSlot[0]);
            DeliverGpuSlot(1, g_gpuSlot[1]);
            ++g_sameTickOk;
        } else {
            ++g_sameTickMissed;
        }
        if ((g_sameTickOk + g_sameTickMissed) % 600 == 1)
            vr::log::Line("VR-OUTPUT: same-tick submit " + std::to_string(g_sameTickOk) + " ok, " +
                          std::to_string(g_sameTickMissed) + " left for the next tick, " + std::to_string(tickMismatch) +
                          " pairs with different slot ticks.");
    }
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

void Blink(float seconds) {
    if (seconds > g_blinkRequest.load()) g_blinkRequest = seconds;
}
void HoldBlack(bool hold, bool instant) {
    if (hold) g_holdInstant = instant;
    g_holdBlack = hold;
}

}  // namespace vr::output
