#include "hooks.h"
#include "logger.h"
#include "control.h"
#include "screenshot.h"
#include "vr_output.h"
#include "vr_session.h"
#include "script_call.h"
#include "display_spoof.h"
#include "haptics_ext.h"
#include "vr_input.h"

#include <tlhelp32.h>
#include <intrin.h>
#include <MinHook.h>
#include <openvr.h>
#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace vr::hooks {

namespace {

// Indici nella vtable di IDirect3D9 / IDirect3DDevice9 (layout COM standard,
// gli indici 0-2 sono IUnknown::QueryInterface/AddRef/Release).
constexpr int kIDirect3D9_CreateDevice = 16;
constexpr int kIDirect3DDevice9_Reset = 16;
constexpr int kIDirect3DDevice9_Present = 17;
constexpr int kIDirect3DDevice9_BeginScene = 41;
constexpr int kIDirect3DDevice9_EndScene = 42;
constexpr int kIDirect3DDevice9_SetTransform = 44;
constexpr int kIDirect3DDevice9_SetVertexShaderConstantF = 94;
constexpr int kIDirect3DDevice9_CreateTexture = 23;
constexpr int kIDirect3DDevice9_CreateVolumeTexture = 24;
constexpr int kIDirect3DDevice9_CreateCubeTexture = 25;
constexpr int kIDirect3DDevice9_CreateVertexBuffer = 26;
constexpr int kIDirect3DDevice9_CreateIndexBuffer = 27;

bool g_deviceHooksInstalled = false;
std::atomic<uint64_t> g_frameCount{0};

// --- Rilevamento euristico della matrice camera tra le costanti dei vertex
// shader (fase 2). Un registro "candidato camera" e' impostato molte volte
// nello stesso frame (una per ogni draw call che lo usa) ma sempre con lo
// STESSO valore, perche' e' condiviso da tutti gli oggetti della scena; una
// matrice per-oggetto (world) invece cambia ad ogni chiamata. Vedi README
// per i dettagli del metodo.
constexpr int kMaxVSConstRegisters = 256;
constexpr int kMinCallsForCandidate = 10;
constexpr int kLogBudget = 150;

struct RegisterTracker {
    bool seenThisFrame = false;
    bool consistentThisFrame = true;
    int callsThisFrame = 0;
    float frameValue[16] = {};
    float lastLoggedValue[16] = {};
    bool everLogged = false;
};

RegisterTracker g_vsConstTrackers[kMaxVSConstRegisters];
int g_maxRegisterSeen = -1;
std::atomic<int> g_logBudgetRemaining{kLogBudget};
std::atomic<bool> g_budgetExhaustedLogged{false};

std::string FormatMatrix16(const float* v) {
    char buf[400];
    std::snprintf(buf, sizeof(buf),
        "[%.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f]",
        v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7],
        v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15]);
    return std::string(buf);
}

void AnalyzeVsConstCandidate(UINT StartRegister, const float* pConstantData) {
    if (StartRegister >= kMaxVSConstRegisters) return;
    RegisterTracker& t = g_vsConstTrackers[StartRegister];

    if (!t.seenThisFrame) {
        t.seenThisFrame = true;
        t.consistentThisFrame = true;
        t.callsThisFrame = 1;
        std::memcpy(t.frameValue, pConstantData, sizeof(float) * 16);
    } else {
        t.callsThisFrame++;
        if (std::memcmp(t.frameValue, pConstantData, sizeof(float) * 16) != 0) {
            t.consistentThisFrame = false;
        }
    }
    if (static_cast<int>(StartRegister) > g_maxRegisterSeen) {
        g_maxRegisterSeen = static_cast<int>(StartRegister);
    }
}

void EndFrameVsConstAnalysis() {
    for (int reg = 0; reg <= g_maxRegisterSeen; ++reg) {
        RegisterTracker& t = g_vsConstTrackers[reg];
        if (t.seenThisFrame && t.consistentThisFrame && t.callsThisFrame >= kMinCallsForCandidate) {
            bool changed = !t.everLogged ||
                std::memcmp(t.frameValue, t.lastLoggedValue, sizeof(float) * 16) != 0;
            if (changed) {
                if (g_logBudgetRemaining.fetch_sub(1) > 0) {
                    vr::log::Line(
                        "CAMERA CANDIDATE reg=c" + std::to_string(reg) +
                        " calls_per_frame=" + std::to_string(t.callsThisFrame) +
                        " value=" + FormatMatrix16(t.frameValue));
                    std::memcpy(t.lastLoggedValue, t.frameValue, sizeof(float) * 16);
                    t.everLogged = true;
                } else if (!g_budgetExhaustedLogged.exchange(true)) {
                    vr::log::Line("Candidate log budget exhausted (" + std::to_string(kLogBudget) +
                                  "): no more lines to keep the file small, data capture continues silently.");
                }
            }
        }
        t.seenThisFrame = false;
        t.consistentThisFrame = true;
        t.callsThisFrame = 0;
    }
}

// Seconda euristica, indipendente dalla ripetizione per frame: le matrici di
// PROIEZIONE prospettica sono tipicamente molto sparse (quasi tutti zeri)
// con un elemento esattamente 1.0 o -1.0 (il termine che porta la profondita'
// in w per la divisione prospettica). Questo pattern e' riconoscibile anche
// se il registro viene impostato una sola volta per frame (al contrario
// dell'euristica precedente, basata sulla ripetizione).
bool g_projSignatureLogged[kMaxVSConstRegisters] = {};
std::atomic<int> g_projLogBudgetRemaining{60};

bool IsZero(float v) { return v == 0.0f; }
bool IsOne(float v) { return std::fabs(std::fabs(v) - 1.0f) < 1e-5f; }

// Firma esatta di una matrice di proiezione prospettica D3D (convenzione
// D3DXMatrixPerspectiveFovLH, riga 2 porta la profondita' in w):
//   [ xScale  0       0        0 ]
//   [ 0       yScale  0        0 ]
//   [ 0       0       A        1 ]
//   [ 0       0       B        0 ]
// Controlliamo anche la trasposta, nel caso lo shader usi la convenzione
// column-major (mul(matrice, vettore)).
bool MatchesExactProjection(const float* d) {
    bool rowMajor =
        IsZero(d[2]) && IsZero(d[3]) && IsZero(d[6]) && IsZero(d[7]) &&
        IsZero(d[8]) && IsZero(d[9]) && IsZero(d[12]) && IsZero(d[13]) &&
        IsOne(d[11]) && IsZero(d[15]);
    bool colMajor =
        IsZero(d[1]) && IsZero(d[2]) && IsZero(d[3]) && IsZero(d[4]) &&
        IsZero(d[6]) && IsZero(d[7]) && IsZero(d[8]) && IsZero(d[9]) &&
        IsZero(d[12]) && IsZero(d[13]) &&
        IsOne(d[14]) && IsZero(d[15]);
    return rowMajor || colMajor;
}

void AnalyzeProjectionSignature(UINT StartRegister, const float* data) {
    if (StartRegister >= kMaxVSConstRegisters || g_projSignatureLogged[StartRegister]) return;

    if (MatchesExactProjection(data)) {
        if (g_projLogBudgetRemaining.fetch_sub(1) > 0) {
            vr::log::Line(
                "PROJECTION CANDIDATE (exact signature) reg=c" + std::to_string(StartRegister) +
                " value=" + FormatMatrix16(data));
        }
        g_projSignatureLogged[StartRegister] = true;
    }
}

typedef HRESULT(WINAPI* CreateDevice_t)(
    IDirect3D9* self, UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
    DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters,
    IDirect3DDevice9** ppReturnedDeviceInterface);

typedef HRESULT(WINAPI* Present_t)(
    IDirect3DDevice9* self, const RECT* pSourceRect, const RECT* pDestRect,
    HWND hDestWindowOverride, const RGNDATA* pDirtyRegion);

typedef HRESULT(WINAPI* Reset_t)(IDirect3DDevice9* self, D3DPRESENT_PARAMETERS* params);
typedef HRESULT(WINAPI* BeginScene_t)(IDirect3DDevice9* self);
typedef HRESULT(WINAPI* EndScene_t)(IDirect3DDevice9* self);

typedef HRESULT(WINAPI* SetTransform_t)(
    IDirect3DDevice9* self, D3DTRANSFORMSTATETYPE State, const D3DMATRIX* pMatrix);

typedef HRESULT(WINAPI* SetVertexShaderConstantF_t)(
    IDirect3DDevice9* self, UINT StartRegister, const float* pConstantData, UINT Vector4fCount);

CreateDevice_t g_CreateDevice_Original = nullptr;
Present_t g_Present_Original = nullptr;
Reset_t g_Reset_Original = nullptr;
BeginScene_t g_BeginScene_Original = nullptr;
EndScene_t g_EndScene_Original = nullptr;
SetTransform_t g_SetTransform_Original = nullptr;
SetVertexShaderConstantF_t g_SetVertexShaderConstantF_Original = nullptr;

void UpdateHeadTracking();
void ExecutionAssist(void* pc, void* pawn);
float HitAngleFromHead(void* pawn);
void InstallAudioListenerHook();
void RotatorToAxes(int32_t pitchU, int32_t yawU, int32_t rollU, float* fwd, float* right, float* up);
bool TrySafeReadUint32(void* addr, uint32_t* outValue);
extern std::atomic<bool> g_onTurret;
extern std::atomic<int32_t> g_turretYawUnits;
bool TurretAimActive();
bool SafeCopy(void* dst, const void* src, size_t n);
// Giocatore bloccato da una scena (bCinematicMode), aggiornato dal thread di gioco.
std::atomic<bool> g_gameLockedScene{false};

void* VTableEntry(void* obj, int index) {
    void** vtable = *reinterpret_cast<void***>(obj);
    return vtable[index];
}

void GpuQueriesRelease();

HRESULT WINAPI Hooked_Reset(IDirect3DDevice9* self, D3DPRESENT_PARAMETERS* params) {
    vr::output::OnBeforeReset();
    GpuQueriesRelease();
    if (params) params->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    HRESULT hr = g_Reset_Original(self, params);
    vr::log::Line("Device reset, hr=" + std::to_string(hr));
    if (SUCCEEDED(hr) && params)
        vr::display::SetWantedClientSize(static_cast<int>(params->BackBufferWidth), static_cast<int>(params->BackBufferHeight));
    return hr;
}

// --- Watchdog di diagnostica ----------------------------------------------
//
// Se il thread di rendering non arriva a Present per alcuni secondi, campiona
// il suo contesto (in-process, nativo a 32 bit: niente debugger) e logga EIP
// e gli indirizzi di ritorno sullo stack con il modulo di appartenenza.
std::atomic<DWORD> g_renderThreadId{0};
std::atomic<ULONGLONG> g_lastPresentMs{0};
std::atomic<ULONGLONG> g_lastBeginSceneMs{0};

std::string DescribeAddress(uint32_t a) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(a)), &m) || !m) {
        return "";
    }
    char path[MAX_PATH] = {};
    GetModuleFileNameA(m, path, MAX_PATH);
    std::string n(path);
    n = n.substr(n.find_last_of("\\/") + 1);
    char b[32];
    std::snprintf(b, sizeof(b), "+0x%X", a - static_cast<uint32_t>(reinterpret_cast<uintptr_t>(m)));
    return n + b;
}

DWORD WINAPI WatchdogThread(LPVOID) {
    int reports = 0;
    for (;;) {
        Sleep(1000);
        ULONGLONG now = GetTickCount64();
        ULONGLONG lastPresent = g_lastPresentMs, lastBegin = g_lastBeginSceneMs;
        DWORD tid = g_renderThreadId;
        if (!tid || !lastBegin || reports >= 6) continue;
        bool stalled = now - (lastPresent ? lastPresent : lastBegin) > 4000;
        if (!stalled) continue;
        HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, tid);
        if (!th) continue;
        CONTEXT ctx = {};
        ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        uint32_t stack[512] = {};
        bool got = false;
        if (SuspendThread(th) != static_cast<DWORD>(-1)) {
            if (GetThreadContext(th, &ctx)) {
                got = true;
                for (int i = 0; i < 512; ++i) {
                    uint32_t v = 0;
                    if (!TrySafeReadUint32(reinterpret_cast<uint32_t*>(ctx.Esp) + i, &v)) break;
                    stack[i] = v;
                }
            }
            ResumeThread(th);
        }
        CloseHandle(th);
        if (!got) continue;
        ++reports;
        std::string line = "WATCHDOG: render thread stalled for " + std::to_string((now - (lastPresent ? lastPresent : lastBegin)) / 1000) +
                           "s, EIP=" + DescribeAddress(ctx.Eip) + " stack:";
        int found = 0;
        for (int i = 0; i < 512 && found < 16; ++i) {
            std::string d = DescribeAddress(stack[i]);
            if (!d.empty() && d.find("SpecOpsTheLine.exe") == std::string::npos) {
                line += " " + d;
                ++found;
            }
        }
        vr::log::Line(line);
    }
}

HRESULT WINAPI Hooked_BeginScene(IDirect3DDevice9* self) {
    g_renderThreadId = GetCurrentThreadId();
    g_lastBeginSceneMs = GetTickCount64();
    static bool watchdogStarted = false;
    if (!watchdogStarted) {
        watchdogStarted = true;
        HANDLE t = CreateThread(nullptr, 0, WatchdogThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    // Iniettiamo lo yaw qui, non in EndScene: il gioco ricalcola la
    // rotazione dall'input del mouse a inizio frame (tick), quindi una
    // scrittura in EndScene verrebbe sovrascritta dal tick successivo
    // prima di essere mai renderizzata. BeginScene arriva dopo il tick di
    // questo frame e prima che il disegno usi la rotazione: e' l'ultimo
    // punto utile per vincere.
    // Dal 19/10 (audit) la posa si legge una volta per tick sul thread di
    // gioco, all'inizio del Draw (ViewportDrawThunk): qui ne venivano 2-3 per
    // tick sul thread di rendering, che scrivevano anche la rotazione del
    // PlayerController mentre il gioco la aggiornava. "headsample render"
    // rimette il comportamento vecchio.
    if (!vr::control::g_headSampleOnDraw) UpdateHeadTracking();
    return g_BeginScene_Original(self);
}

// --- Motion blur e profondita' di campo (nausea in VR) ---------------------
//
// Gli ini del gioco sono cifrati e il menu Grafica non ha queste voci: si
// scrive direttamente in GSystemSettings, oggetto globale a indirizzo fisso
// (vtable a +0, FSystemSettingsData da +4). Offset dalla tabella nome->campo
// di FSystemSettingsData::LoadFromIni (0x00F06090): MotionBlur +0x30,
// DepthOfField +0x38. Verificato con screenshot il 16/10: immagine nitida
// ruotando, ma sparisce anche parte del bloom/foschia (in UE3 DOF e bloom
// sono lo stesso passaggio). Riapplicato periodicamente perche' il gioco
// rilegge le impostazioni (es. cambiando le opzioni grafiche).
constexpr uintptr_t kGSystemSettings = 0x014D77B0;
constexpr uintptr_t kGSystemSettingsVTable = 0x0113A00C;
constexpr int kSettingMotionBlur = 4 + 0x30;
constexpr int kSettingDepthOfField = 4 + 0x38;

void ApplyPostFxSettings() {
    static int frame = 0;
    if (++frame % 60 != 0) return;
    uint32_t vtable = 0;
    if (!TrySafeReadUint32(reinterpret_cast<void*>(kGSystemSettings), &vtable) ||
        vtable != kGSystemSettingsVTable) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            vr::log::Line("POSTFX: GSystemSettings not recognised, leaving it alone.");
        }
        return;
    }
    int32_t value = vr::control::g_disablePostFx ? 0 : 1;
    auto* motionBlur = reinterpret_cast<int32_t*>(kGSystemSettings + kSettingMotionBlur);
    auto* depthOfField = reinterpret_cast<int32_t*>(kGSystemSettings + kSettingDepthOfField);
    if (*motionBlur != value || *depthOfField != value) {
        *motionBlur = value;
        *depthOfField = value;
        vr::log::Line(std::string("POSTFX: motion blur and depth of field ") + (value ? "on" : "off"));
    }
}

HRESULT WINAPI Hooked_EndScene(IDirect3DDevice9* self) {
    uint64_t count = ++g_frameCount;
    if (count == 1) {
        vr::log::Line("First EndScene intercepted: hook pipeline active.");
    } else if (count % 300 == 0) {
        vr::log::Line("EndScene: " + std::to_string(count) + " frames rendered so far.");
    }
    if (vr::control::g_vsDiag) EndFrameVsConstAnalysis();
    vr::display::KeepSubclass();
    vr::control::Poll();
    ApplyPostFxSettings();
    return g_EndScene_Original(self);
}

// Diagnostica sfarfallio: occhio di ogni Present (-1 = sconosciuto) e
// sequenze anomale (stesso occhio due volte di fila) in stereo sincrono.
std::atomic<uint32_t> g_presentByEye[3];
std::atomic<uint32_t> g_presentRepeats{0};

// Etichetta di ogni frame disegnato (Fase 2, 18/10): il thread di gioco la
// accoda prima di ogni FViewport::Draw che presenta, il thread di rendering la
// toglie al Present corrispondente (i comandi di rendering restano in ordine).
// Porta occhio, tick e posa del visore usata per QUEL frame: il riconoscimento
// tramite l'origine della vista (c5) resta come controllo incrociato, ma stando
// fermi l'origine e' uguale in molti tick e non dice quale tick e' (0.9.1).
// --- Proiezione decentrata per occhio -----------------------------------------
//
// Gli occhi del visore hanno frustum asimmetrici (Quest 3: 1,376 verso
// l'esterno e 0,839 verso il naso, 1,428 in su e 0,966 in giu'). Prima il
// gioco disegnava un'immagine simmetrica che li conteneva entrambi e se ne
// ritagliava la parte di ciascun occhio: 1932x2087 pixel usati su 2400x2500
// disegnati, un terzo del lavoro della GPU buttato. Ora la matrice di
// proiezione del gioco (FPerspectiveMatrix in ULocalPlayer::CalcSceneView)
// viene sostituita, durante il disegno di ogni occhio, con quella decentrata
// dell'occhio: tutto il back buffer e' l'occhio ("offcenter on|off").
// Thread di gioco: decisione per il Draw in corso. Thread di rendering:
// modalita' dell'immagine presentata (dal frame tag) per mirino, HUD, vignetta.
std::atomic<bool> g_drawOffCenter{false};
vr::output::EyeProjection g_drawProj = {};
std::atomic<bool> g_presentOffCenter{false};
// Risoluzione dinamica: ScreenPercentage attuale (thread di gioco) e quella
// dell'immagine presentata (thread di rendering).
std::atomic<int> g_screenPercent{100};
std::atomic<int> g_presentPercent{100};
UINT g_testVpMax = 0;  // "dynres test" (thread di rendering)
int g_testFrames = 0, g_testBad = 0;

// --- Tempo della GPU --------------------------------------------------------
//
// Query D3D9 di timestamp: inizio al primo disegno dopo un Present, fine al
// Present; lette qualche frame dopo senza aspettare. Tempo di GPU per tick =
// occhio sinistro + destro (prima non c'era alcuna misura della GPU).
struct GpuQuerySet {
    IDirect3DQuery9* disjoint = nullptr;
    IDirect3DQuery9* freq = nullptr;
    IDirect3DQuery9* t0 = nullptr;
    IDirect3DQuery9* t1 = nullptr;
    int state = 0;  // 0 libero, 1 iniziato, 2 in attesa dei risultati
    int eye = -1;
};
constexpr int kGpuQuerySets = 8;
GpuQuerySet g_gq[kGpuQuerySets];
int g_gqWrite = 0;
bool g_gqNeedStart = true;
bool g_gqFailed = false;
double g_gpuEye0Ms = 0.0;
// Statistiche per la riga GPU (somme dal thread di rendering).
std::mutex g_gpuStatsMutex;
double g_gpuSum = 0.0, g_gpuMax = 0.0;
int g_gpuCount = 0;
std::atomic<float> g_gpuTickMsEma{0.0f};

void GpuQueriesRelease() {
    for (GpuQuerySet& q : g_gq) {
        for (IDirect3DQuery9** p : {&q.disjoint, &q.freq, &q.t0, &q.t1})
            if (*p) {
                (*p)->Release();
                *p = nullptr;
            }
        q.state = 0;
    }
    g_gqNeedStart = true;
}

void GpuFrameStart(IDirect3DDevice9* d) {
    g_gqNeedStart = false;
    if (g_gqFailed) return;
    GpuQuerySet& q = g_gq[g_gqWrite];
    if (q.state != 0) return;  // risultati non ancora letti: frame saltato
    if (!q.t0) {
        if (FAILED(d->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &q.disjoint)) ||
            FAILED(d->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &q.freq)) ||
            FAILED(d->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &q.t0)) || FAILED(d->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &q.t1))) {
            g_gqFailed = true;
            vr::log::Line("GPU: timestamp queries not supported, no GPU timing.");
            return;
        }
    }
    q.disjoint->Issue(D3DISSUE_BEGIN);
    q.t0->Issue(D3DISSUE_END);
    q.state = 1;
}

void GpuFrameEnd(int eye) {
    GpuQuerySet& q = g_gq[g_gqWrite];
    if (q.state == 1) {
        q.t1->Issue(D3DISSUE_END);
        q.freq->Issue(D3DISSUE_END);
        q.disjoint->Issue(D3DISSUE_END);
        q.state = 2;
        q.eye = eye;
        g_gqWrite = (g_gqWrite + 1) % kGpuQuerySets;
    }
    g_gqNeedStart = true;
    // Risultati pronti (in ordine): niente attese.
    for (int k = 0; k < kGpuQuerySets; ++k) {
        GpuQuerySet& r = g_gq[(g_gqWrite + k) % kGpuQuerySets];
        if (r.state != 2) continue;
        BOOL disjoint = TRUE;
        UINT64 f = 0, a = 0, b = 0;
        if (r.disjoint->GetData(&disjoint, sizeof(disjoint), 0) != S_OK || r.freq->GetData(&f, sizeof(f), 0) != S_OK ||
            r.t0->GetData(&a, sizeof(a), 0) != S_OK || r.t1->GetData(&b, sizeof(b), 0) != S_OK)
            break;
        r.state = 0;
        if (disjoint || !f || b < a) continue;
        double ms = 1000.0 * static_cast<double>(b - a) / static_cast<double>(f);
        if (r.eye == 0) {
            g_gpuEye0Ms = ms;
        } else if (r.eye == 1) {
            double tick = g_gpuEye0Ms + ms;
            float ema = g_gpuTickMsEma;
            g_gpuTickMsEma = ema <= 0.0f ? static_cast<float>(tick) : ema + 0.1f * (static_cast<float>(tick) - ema);
            std::lock_guard<std::mutex> lock(g_gpuStatsMutex);
            g_gpuSum += tick;
            g_gpuMax = (std::max)(g_gpuMax, tick);
            ++g_gpuCount;
        } else {
            std::lock_guard<std::mutex> lock(g_gpuStatsMutex);  // mono (menu, schermo virtuale)
            g_gpuSum += ms;
            g_gpuMax = (std::max)(g_gpuMax, ms);
            ++g_gpuCount;
        }
    }
}

// NDC dell'occhio per una direzione nella vista (tangenti x a destra, y in
// su): proiezione decentrata se l'immagine lo e', altrimenti simmetrica.
void TanToNdc(int eye, bool offCenter, float tx, float ty, float thSym, float tvSym, float* nx, float* ny) {
    vr::output::EyeProjection pj;
    if (offCenter && vr::output::OffCenterProjection(eye, &pj)) {
        *nx = tx * pj.m00 + pj.m20;
        *ny = ty * pj.m11 + pj.m21;
    } else {
        *nx = tx / thSym;
        *ny = ty / tvSym;
    }
}

struct FrameTag {
    double pushMs = 0.0;  // momento del Draw (latenza fino al Present)
    int eye = -1;
    uint32_t tick = 0;
    bool offCenter = false;  // disegnato con la proiezione decentrata dell'occhio
    int screenPercent = 100;  // ScreenPercentage del disegno (risoluzione dinamica)
    bool poseValid = false;
    vr::HmdMatrix34_t pose = {};
    double sampleMs = 0.0;  // campionamento della testa (ProfMs)
    // Mirino e riquadro del bersaglio calcolati per questo disegno (scritti a
    // fine UpdateAimMarker, prima del Present di questo occhio).
    bool markerSet = false, markerVis = false, targetVis = false;
    float markerNdc[2] = {0, 0}, targetNdc[2] = {0, 0};
};
std::mutex g_frameTagMutex;
std::deque<FrameTag> g_frameTags;
std::atomic<uint32_t> g_frameTagDisagree{0}, g_frameTagResync{0}, g_frameTagEmpty{0}, g_frameTagStale{0};

double ProfMs();
std::mutex g_tagLatencyMutex;
double g_tagLatencySum = 0.0, g_tagLatencyMax = 0.0;
int g_tagLatencyCount = 0;

void PushFrameTag(const FrameTag& t0) {
    FrameTag t = t0;
    t.pushMs = ProfMs();
    std::lock_guard<std::mutex> lock(g_frameTagMutex);
    if (g_frameTags.size() >= 16) g_frameTags.pop_front();
    g_frameTags.push_back(t);
}

// Etichetta del frame che il thread di rendering sta disegnando adesso (la
// prima in coda, con lo stesso scarto delle vecchie di PopFrameTag): serve
// a chi agisce durante il disegno, prima del Present (matrice dell'HUD).
bool PeekRenderingTag(FrameTag* out) {
    std::lock_guard<std::mutex> lock(g_frameTagMutex);
    if (g_frameTags.empty()) return false;
    size_t idx = g_frameTags.size() > 4 ? ((g_frameTags.size() - 3) & ~static_cast<size_t>(1)) : 0;
    *out = g_frameTags[idx];
    return true;
}

// Etichetta del Present in corso; c5Eye = occhio riconosciuto dall'origine
// (-1 se nessuno). Se non coincidono e l'etichetta seguente coincide, la coda
// era sfasata (un Present senza Draw o un Draw senza Present): si riallinea.
bool PopFrameTag(int c5Eye, FrameTag* out) {
    std::lock_guard<std::mutex> lock(g_frameTagMutex);
    if (g_frameTags.empty()) {
        g_frameTagEmpty.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // Il thread di rendering e' indietro al massimo di un tick (due Draw per
    // tick): piu' di 4 etichette = Draw rimasti senza Present (caricamenti).
    // Un numero pari di etichette in piu' non fa mai scattare il controllo
    // dell'occhio qui sotto e la coda restava sfasata per sempre: ogni
    // immagine partiva con la posa di ~7 tick prima (98 ms, bande nere ai
    // bordi muovendo la testa; 19/10 dopo un "open" da console). Si scartano
    // le coppie piu' vecchie, mantenendo l'alternanza degli occhi.
    if (g_frameTags.size() > 4) {
        size_t drop = (g_frameTags.size() - 3) & ~static_cast<size_t>(1);
        for (size_t i = 0; i < drop; ++i) g_frameTags.pop_front();
        g_frameTagStale.fetch_add(static_cast<uint32_t>(drop), std::memory_order_relaxed);
    }
    // Etichette mono (schermo virtuale, eye -1) seguite da etichette stereo:
    // avanzi di un filmato, si saltano, altrimenti ogni immagine prendeva
    // l'etichetta del disegno precedente (occhi scambiati dopo i filmati, 19/10).
    while (g_frameTags.size() > 1 && g_frameTags.front().eye < 0 && g_frameTags[1].eye >= 0) {
        g_frameTags.pop_front();
        g_frameTagStale.fetch_add(1, std::memory_order_relaxed);
    }
    *out = g_frameTags.front();
    g_frameTags.pop_front();
    if (c5Eye >= 0 && out->eye >= 0 && out->eye != c5Eye) {
        g_frameTagDisagree.fetch_add(1, std::memory_order_relaxed);
        if (!g_frameTags.empty() && g_frameTags.front().eye == c5Eye) {
            *out = g_frameTags.front();
            g_frameTags.pop_front();
            g_frameTagResync.fetch_add(1, std::memory_order_relaxed);
        } else {
            out->eye = c5Eye;
            out->poseValid = false;  // tick incerto: niente posa
        }
    }
    return true;
}

// Coerenza tra gli occhi: disegni e triangoli di ogni immagine (thread di
// rendering). Con la stessa scena i due occhi disegnano quasi le stesse cose;
// una differenza grande = visibilita' diversa tra gli occhi (sfarfallio).
uint32_t g_frameDraws = 0, g_frameTris = 0;
struct EyeDrawStats {
    uint64_t pairs = 0, drawsL = 0, drawsR = 0, mismatch2 = 0;
    double triDiff = 0.0;
    uint64_t jitter[2] = {0, 0};  // somma di |disegni - disegni del frame prima| per occhio
};
std::mutex g_eyeDrawMutex;
EyeDrawStats g_eyeDraws;

void RecordEyeDraws(int eye) {
    static uint32_t lDraws = 0, lTris = 0;
    static bool haveL = false;
    static uint32_t prevDraws[2] = {0, 0};
    if (eye == 0 || eye == 1) {
        uint32_t d = g_frameDraws > prevDraws[eye] ? g_frameDraws - prevDraws[eye] : prevDraws[eye] - g_frameDraws;
        prevDraws[eye] = g_frameDraws;
        std::lock_guard<std::mutex> lock(g_eyeDrawMutex);
        g_eyeDraws.jitter[eye] += d;
    }
    if (eye == 0) {
        lDraws = g_frameDraws;
        lTris = g_frameTris;
        haveL = true;
    } else if (eye == 1 && haveL) {
        haveL = false;
        uint32_t mx = (std::max)(lTris, g_frameTris);
        double diff = mx ? std::fabs(static_cast<double>(lTris) - g_frameTris) / mx : 0.0;
        std::lock_guard<std::mutex> lock(g_eyeDrawMutex);
        ++g_eyeDraws.pairs;
        g_eyeDraws.drawsL += lDraws;
        g_eyeDraws.drawsR += g_frameDraws;
        g_eyeDraws.triDiff += diff;
        if (diff > 0.02) ++g_eyeDraws.mismatch2;
    }
    g_frameDraws = g_frameTris = 0;
}

void RecordPresentEye(int eye) {
    RecordEyeDraws(eye);
    static int last = -2;
    g_presentByEye[eye + 1].fetch_add(1, std::memory_order_relaxed);
    if (eye >= 0 && eye == last) g_presentRepeats.fetch_add(1, std::memory_order_relaxed);
    last = eye;
}

std::atomic<UINT> g_backBufferW{0}, g_backBufferH{0};
std::atomic<uint32_t> g_headBlocked{0};
// Unita' di cui la vista arretra per un muro davanti al corpo (vedi
// GetPlayerViewPointThunk); il corpo in prima persona arretra con lei.
std::atomic<float> g_wallPull{0.0f};
// Telemetria STATS: passaggi allo schermo virtuale e salto massimo della vista
// (occhio sinistro) da un tick all'altro.
std::atomic<uint32_t> g_cinemaSwitches{0};
float g_maxViewJump = 0.0f;
void LogStats();
// Indicatore di mira: NDC per occhio (thread di gioco -> Present).
std::atomic<float> g_markerNdc[2][2];
std::atomic<bool> g_markerVisible[2];
std::atomic<bool> g_markerFrameValid{false};
void DrawAimMarker(IDirect3DDevice9* dev, int eye);
// Bersaglio del comando alla squadra (tasto squadra tenuto): posizione nel
// mondo (thread di gioco) e NDC per occhio, disegnato come riquadro arancione.
float g_squadTargetLoc[3] = {0, 0, 0};
std::atomic<unsigned long long> g_squadTargetMs{0};
std::atomic<float> g_targetNdc[2][2];
std::atomic<bool> g_targetVisible[2];
// Mirino del frame in presentazione (thread di rendering), dall'etichetta del
// frame. Prima si leggevano i valori globali, che il thread di gioco poteva
// avere gia' riscritto per il tick seguente: un occhio mostrava il mirino di
// un altro frame e muovendosi lo si vedeva doppio (19/10).
struct PresentMarker {
    bool vis = false, targetVis = false;
    float ndc[2] = {0, 0}, targetNdc[2] = {0, 0};
};
PresentMarker g_presentMarker;
std::atomic<uint32_t> g_markerFromTag{0}, g_markerFromGlobals{0};

double ProfMs();
extern double g_profOutput, g_profPresent;

void PresentMarkerFromGlobals(int eye) {
    g_markerFromGlobals.fetch_add(1, std::memory_order_relaxed);
    // Frame senza occhio (schermo virtuale, caricamenti): niente mirino ne'
    // riquadro del bersaglio. Prima restavano quelli dell'ultimo frame 3D,
    // disegnati sopra filmati e caricamenti (audit 20/10).
    if (eye < 0 || vr::control::g_cinemaActive) {
        g_presentMarker.vis = false;
        g_presentMarker.targetVis = false;
        return;
    }
    int e = eye == 1 ? 1 : 0;
    g_presentMarker.vis = g_markerVisible[e];
    g_presentMarker.targetVis = g_targetVisible[e];
    for (int i = 0; i < 2; ++i) {
        g_presentMarker.ndc[i] = g_markerNdc[e][i];
        g_presentMarker.targetNdc[i] = g_targetNdc[e][i];
    }
}

// --- Finestra specchio ("mirrorwin on") ----------------------------------------
//
// Con Link lo specchio di SteamVR (ShowMirrorWindow, "Legacy Mirror") resta
// vuoto (19/10). La finestra del gioco e' piu' grande dello schermo: per
// registrare, il Present dell'occhio destro va in una finestra a parte
// (hDestWindowOverride, scalata dal driver), sempre in primo piano e che non
// prende il focus (il gioco va in pausa quando lo perde).
std::atomic<HWND> g_mirrorHwnd{nullptr};

LRESULT CALLBACK MirrorWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (m == WM_CLOSE) {
        DestroyWindow(h);
        return 0;
    }
    if (m == WM_DESTROY) {
        g_mirrorHwnd = nullptr;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void MirrorWindowThread() {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = MirrorWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SpecOpsVRMirror";
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    RegisterClassW(&wc);
    // Proporzioni del back buffer di un occhio (quasi quadrato), alto quanto lo schermo.
    UINT bw = g_backBufferW ? g_backBufferW.load() : 2528, bh = g_backBufferH ? g_backBufferH.load() : 2776;
    RECT r = {0, 0, static_cast<LONG>(940 * bw / bh), 940};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_VISIBLE;
    AdjustWindowRect(&r, style, FALSE);
    HWND h = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE, wc.lpszClassName, L"SpecOpsVR mirror", style, 0, 0,
                             r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (!h) {
        vr::log::Line("MIRRORWIN: window creation failed.");
        return;
    }
    ShowWindow(h, SW_SHOWNOACTIVATE);
    g_mirrorHwnd = h;
    vr::log::Line("MIRRORWIN: mirror window opened (right eye).");
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    vr::log::Line("MIRRORWIN: mirror window closed.");
}

void UpdateMirrorWindow() {
    bool want = vr::control::g_mirrorWin;
    HWND h = g_mirrorHwnd;
    static bool starting = false;
    if (want && !h && !starting) {
        starting = true;
        std::thread(MirrorWindowThread).detach();
    }
    if (h) starting = false;
    if (!want && h) PostMessageW(h, WM_CLOSE, 0, 0);
}

HRESULT WINAPI Hooked_Present(
    IDirect3DDevice9* self, const RECT* pSourceRect, const RECT* pDestRect,
    HWND hDestWindowOverride, const RGNDATA* pDirtyRegion) {
    g_lastPresentMs = GetTickCount64();
    int eye = vr::control::g_presentEye.exchange(-1);
    {
        FrameTag tag;
        if (PopFrameTag(eye, &tag)) {
            if (tag.eye >= 0) eye = tag.eye;
            if (tag.pushMs > 0.0 && tag.eye >= 0) {
                // Dal Draw sul thread di gioco al Present sul thread di rendering.
                double lat = ProfMs() - tag.pushMs;
                std::lock_guard<std::mutex> lock(g_tagLatencyMutex);
                g_tagLatencySum += lat;
                g_tagLatencyMax = (std::max)(g_tagLatencyMax, lat);
                ++g_tagLatencyCount;
            }
            if (eye >= 0) vr::output::SetRenderPose(eye, tag.poseValid ? &tag.pose : nullptr, tag.tick, tag.offCenter,
                                                 tag.poseValid ? tag.sampleMs : 0.0);
            g_presentOffCenter = tag.offCenter;
            g_presentPercent = tag.screenPercent;
            if (tag.markerSet) {
                g_markerFromTag.fetch_add(1, std::memory_order_relaxed);
                g_presentMarker.vis = tag.markerVis;
                g_presentMarker.targetVis = tag.targetVis;
                for (int i = 0; i < 2; ++i) {
                    g_presentMarker.ndc[i] = tag.markerNdc[i];
                    g_presentMarker.targetNdc[i] = tag.targetNdc[i];
                }
            } else {
                PresentMarkerFromGlobals(eye);
            }
            if (eye >= 0) vr::output::SetRenderScreenPercent(eye, tag.screenPercent);
        } else if (eye >= 0) {
            PresentMarkerFromGlobals(eye);
            vr::output::SetRenderPose(eye, nullptr, 0, false);
            vr::output::SetRenderScreenPercent(eye, g_screenPercent);
            g_presentOffCenter = false;
            g_presentPercent = g_screenPercent.load();
        }
    }
    RecordPresentEye(eye);
    DrawAimMarker(self, eye);
    if (vr::control::g_gpuTimer) GpuFrameEnd(eye);
    if (vr::control::g_dynResTestFrames > 0 && eye >= 0) {
        vr::control::g_dynResTestFrames.fetch_sub(1);
        UINT expect = static_cast<UINT>(g_backBufferW * (g_presentPercent / 100.0f) + 0.5f);
        bool ok = g_testVpMax + 2 >= expect && g_testVpMax <= expect + 2;
        ++g_testFrames;
        if (!ok) {
            ++g_testBad;
            if (g_testBad <= 10)
                vr::log::Line("DYNRES TEST: frame with viewport " + std::to_string(g_testVpMax) + " but tag " +
                              std::to_string(g_presentPercent.load()) + "% (expected " + std::to_string(expect) + ")");
        }
        if (vr::control::g_dynResTestFrames == 0)
            vr::log::Line("DYNRES TEST: " + std::to_string(g_testFrames) + " frames, " + std::to_string(g_testBad) +
                          " with the wrong crop.");
    }
    g_testVpMax = 0;
    std::string shotPath;
    if (vr::control::TakeScreenshotRequest(&shotPath)) {
        vr::screenshot::SaveBackBuffer(self, shotPath);
    }
    double tOut = ProfMs();
    vr::output::OnPresent(self, eye);
    g_profOutput += ProfMs() - tOut;
    // Stereo sincrono: il frame dell'occhio sinistro va solo al visore; sul
    // monitor presentiamo solo il destro (altrimenti due Present per tick).
    if (eye == 0 && vr::control::g_stereoHalfIpd > 0 && vr::control::g_syncStereo) return S_OK;
    if (eye == 1 && vr::control::g_syncStereo) {
        static uint32_t n = 0;
        if (++n % static_cast<uint32_t>(vr::control::g_monitorEvery.load()) != 0) return S_OK;
    }
    double tPres = ProfMs();
    UpdateMirrorWindow();
    HWND mirror = vr::control::g_mirrorWin ? g_mirrorHwnd.load() : nullptr;
    HRESULT hr = mirror ? g_Present_Original(self, nullptr, nullptr, mirror, nullptr)
                        : g_Present_Original(self, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
    g_profPresent += ProfMs() - tPres;
    return hr;
}


// Disegna l'indicatore di mira sul back buffer (prima della copia per il
// visore) con dei Clear su rettangoli: un mirino chiaro con bordo scuro.
// Vignettatura sfumata: un anello ellittico di triangoli con trasparenza per
// vertice (trasparente all'interno, nero verso il bordo) disegnato sopra
// l'immagine dell'occhio; la GPU interpola la sfumatura, nessuno shader
// (pipeline fissa, stato del gioco salvato e ripristinato con uno state
// block). L'intensita' segue il movimento con una rampa, senza scatti.
struct VignetteVertex {
    float x, y, z, rhw;
    DWORD color;
};

// Zoom del cannocchiale in corso: l'HUD (maschera del mirino) resta a tutta vista.
std::atomic<bool> g_scopeActive{false};

// Cannocchiale: la maschera del gioco copre un'area 16:9 centrata; sopra e
// sotto (back buffer quasi quadrato) si vedrebbe la scena ingrandita senza
// maschera: si anneriscono le due strisce.
// Dal 19/10 (audit) il cannocchiale e' una lente: fuori da un cerchio al
// centro dell'occhio (raggio 0,42 della mezza altezza del ritaglio, ~25 gradi)
// tutto nero, come guardando nel cannocchiale; la vista ingrandita a tutto
// campo, girando con la testa, dava nausea. Il cerchio e' fatto di strisce
// orizzontali annerite fuori dalla corda (128 rettangoli in un Clear).
void DrawScopeBars(IDirect3DDevice9* dev, IDirect3DSurface9* bb, int eye = 0) {
    if (!g_scopeActive) return;
    D3DSURFACE_DESC d;
    bb->GetDesc(&d);
    RECT crop = vr::output::EyeCropRect(eye == 1 ? 1 : 0);
    if (crop.right <= crop.left || crop.bottom <= crop.top)
        crop = {0, 0, static_cast<LONG>(d.Width), static_cast<LONG>(d.Height)};
    float pk = g_presentPercent / 100.0f, mx = d.Width * 0.5f, my = d.Height * 0.5f;
    // Col cannocchiale la proiezione e' simmetrica: il davanti dell'occhio
    // (tangente 0, dove stanno l'immagine ingrandita e il reticolo) e' il
    // centro del back buffer, non il centro del ritaglio dell'occhio. Prima il
    // cerchio stava ~15 gradi verso l'esterno in ciascun occhio e ~13 in basso:
    // i due cerchi non si fondevano (audit 20/10). La risoluzione dinamica
    // restringe attorno al centro, che quindi non si sposta.
    float cx = mx, cy = my;
    float rad = 0.42f * 0.5f * (crop.bottom - crop.top) * pk;
    const int kStrips = 62;
    D3DRECT r[2 * kStrips + 4];
    int n = 0;
    LONG top = static_cast<LONG>(cy - rad), bottom = static_cast<LONG>(cy + rad);
    r[n++] = {0, 0, static_cast<LONG>(d.Width), (std::max)(0L, top)};
    r[n++] = {0, (std::min)(static_cast<LONG>(d.Height), bottom), static_cast<LONG>(d.Width), static_cast<LONG>(d.Height)};
    for (int i = 0; i < kStrips; ++i) {
        float y0 = cy - rad + 2.0f * rad * i / kStrips, y1 = cy - rad + 2.0f * rad * (i + 1) / kStrips;
        float ym = 0.5f * (y0 + y1) - cy;
        float half = std::sqrt((std::max)(0.0f, rad * rad - ym * ym));
        LONG a = static_cast<LONG>(y0), b = static_cast<LONG>(y1) + 1;
        r[n++] = {0, a, static_cast<LONG>(cx - half), b};
        r[n++] = {static_cast<LONG>(cx + half), a, static_cast<LONG>(d.Width), b};
    }
    IDirect3DSurface9* oldRt = nullptr;
    dev->GetRenderTarget(0, &oldRt);
    D3DVIEWPORT9 oldVp;
    dev->GetViewport(&oldVp);
    DWORD scissor = FALSE;
    dev->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissor);
    dev->SetRenderTarget(0, bb);
    D3DVIEWPORT9 vp = {0, 0, d.Width, d.Height, 0.0f, 1.0f};
    dev->SetViewport(&vp);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->Clear(n, r, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, scissor);
    dev->SetViewport(&oldVp);
    if (oldRt) {
        dev->SetRenderTarget(0, oldRt);
        oldRt->Release();
    }
}

void DrawVignette(IDirect3DDevice9* dev, IDirect3DSurface9* bb, int eye) {
    DrawScopeBars(dev, bb, eye);
    float strength = vr::control::g_vignette;
    if (strength <= 0.0f) return;
    // Livello aggiornato una volta per coppia (occhio sinistro o mono) e in
    // base al tempo: prima ogni occhio lo cambiava e i due bordi differivano;
    // conta anche il movimento non fatto dal giocatore (veicoli, rotazioni del
    // gioco), non solo lo stick (audit 19/10).
    static float level = 0.0f;
    if (eye != 1) {
        static double lastT = 0.0;
        double now = ProfMs() * 0.001;
        float dt = lastT > 0.0 ? static_cast<float>(now - lastT) : 0.0f;
        lastT = now;
        float m = (std::max)(vr::control::g_motionAmount.load(), vr::control::g_forcedMotion.load());
        float target = m * strength;
        if (dt > 0.0f && dt < 0.5f) level += (target - level) * (1.0f - std::exp(-dt / 0.1f));
    }
    if (level < 0.01f) return;
    D3DSURFACE_DESC d;
    bb->GetDesc(&d);
    RECT crop = vr::output::EyeCropRect(eye == 1 ? 1 : 0);
    vr::output::EyeProjection pj;
    bool oc = g_presentOffCenter && vr::output::OffCenterProjection(eye == 1 ? 1 : 0, &pj);
    if (oc || crop.right <= crop.left || crop.bottom <= crop.top)
        crop = {0, 0, static_cast<LONG>(d.Width), static_cast<LONG>(d.Height)};
    // Davanti dell'occhio: centro del back buffer con la proiezione simmetrica
    // (non il centro del ritaglio, che e' spostato verso l'esterno; audit 20/10).
    float cx = 0.5f * d.Width, cy = 0.5f * d.Height;
    float hx = 0.5f * (crop.right - crop.left), hy = 0.5f * (crop.bottom - crop.top);
    if (oc) {  // centro dell'occhio (davanti) nell'immagine decentrata
        cx = (0.5f + 0.5f * pj.m20) * d.Width;
        cy = (0.5f - 0.5f * pj.m21) * d.Height;
    }
    {  // scena ristretta al centro (risoluzione dinamica)
        float pk = g_presentPercent / 100.0f, mx = d.Width * 0.5f, my = d.Height * 0.5f;
        cx = mx + (cx - mx) * pk;
        cy = my + (cy - my) * pk;
        hx *= pk;
        hy *= pk;
    }
    // Raggi relativi alla mezza larghezza/altezza dell'occhio: libero fino a
    // r0, nero da r1 in poi; con intensita' massima r0 = 0,55, r1 = 0,9.
    float r0 = 1.45f - 0.9f * level, r1 = 1.75f - 0.85f * level;
    float r2 = std::fmax(r1 + 0.05f, 1.45f);  // oltre gli angoli (sqrt 2)
    constexpr int kSeg = 64;
    VignetteVertex ring[2][(kSeg + 1) * 2];
    for (int i = 0; i <= kSeg; ++i) {
        float t = 6.28318530718f * i / kSeg;
        float c = std::cos(t), sn = std::sin(t);
        auto vtx = [&](float r, DWORD alpha) {
            return VignetteVertex{cx + c * r * hx, cy + sn * r * hy, 0.0f, 1.0f, D3DCOLOR_ARGB(alpha, 0, 0, 0)};
        };
        ring[0][2 * i] = vtx(r0, 0);
        ring[0][2 * i + 1] = vtx(r1, 255);
        ring[1][2 * i] = vtx(r1, 255);
        ring[1][2 * i + 1] = vtx(r2, 255);
    }
    IDirect3DStateBlock9* saved = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &saved)) || !saved) return;
    IDirect3DSurface9* oldRt = nullptr;
    dev->GetRenderTarget(0, &oldRt);
    dev->SetRenderTarget(0, bb);
    D3DVIEWPORT9 vp = {0, 0, d.Width, d.Height, 0.0f, 1.0f};
    dev->SetViewport(&vp);
    dev->SetScissorRect(&crop);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    dev->SetTexture(0, nullptr);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, kSeg * 2, ring[0], sizeof(VignetteVertex));
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, kSeg * 2, ring[1], sizeof(VignetteVertex));
    saved->Apply();
    saved->Release();
    if (oldRt) {
        dev->SetRenderTarget(0, oldRt);
        oldRt->Release();
    }
}

// Riquadro arancione (angoli) sul bersaglio dell'ordine alla squadra.
void DrawTargetBox(IDirect3DDevice9* dev, IDirect3DSurface9* bb, const D3DSURFACE_DESC& d, int e) {
    (void)e;
    if (!g_presentMarker.targetVis) return;
    float nx = g_presentMarker.targetNdc[0], ny = g_presentMarker.targetNdc[1];
    if (std::fabs(nx) > 1.0f || std::fabs(ny) > 1.0f) return;
    float pk = g_presentPercent / 100.0f;
    LONG cx = static_cast<LONG>((nx * 0.5f * pk + 0.5f) * d.Width);
    LONG cy = static_cast<LONG>((0.5f - ny * 0.5f * pk) * d.Height);
    IDirect3DSurface9* oldRt = nullptr;
    dev->GetRenderTarget(0, &oldRt);
    D3DVIEWPORT9 oldVp;
    dev->GetViewport(&oldVp);
    DWORD scissor = FALSE;
    dev->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissor);
    dev->SetRenderTarget(0, bb);
    D3DVIEWPORT9 vp = {0, 0, d.Width, d.Height, 0.0f, 1.0f};
    dev->SetViewport(&vp);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    const LONG s = 34, l = 14, t = 4;  // mezzo lato, lunghezza degli angoli, spessore
    D3DRECT r[8] = {
        {cx - s, cy - s, cx - s + l, cy - s + t}, {cx - s, cy - s, cx - s + t, cy - s + l},
        {cx + s - l, cy - s, cx + s, cy - s + t}, {cx + s - t, cy - s, cx + s, cy - s + l},
        {cx - s, cy + s - t, cx - s + l, cy + s}, {cx - s, cy + s - l, cx - s + t, cy + s},
        {cx + s - l, cy + s - t, cx + s, cy + s}, {cx + s - t, cy + s - l, cx + s, cy + s},
    };
    dev->Clear(8, r, D3DCLEAR_TARGET, D3DCOLOR_XRGB(255, 150, 30), 1.0f, 0);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, scissor);
    dev->SetViewport(&oldVp);
    if (oldRt) {
        dev->SetRenderTarget(0, oldRt);
        oldRt->Release();
    }
}

void DrawAimMarker(IDirect3DDevice9* dev, int eye) {
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    D3DSURFACE_DESC d;
    bb->GetDesc(&d);
    g_backBufferW = d.Width;
    g_backBufferH = d.Height;
    DrawVignette(dev, bb, eye);
    int e = (eye == 1) ? 1 : 0;
    DrawTargetBox(dev, bb, d, e);
    if (!g_presentMarker.vis) {
        bb->Release();
        return;
    }
    float nx = g_presentMarker.ndc[0], ny = g_presentMarker.ndc[1];
    if (std::fabs(nx) > 1.0f || std::fabs(ny) > 1.0f) {
        bb->Release();
        return;
    }
    float pk = g_presentPercent / 100.0f;  // scena ristretta al centro (risoluzione dinamica)
    LONG cx = static_cast<LONG>((nx * 0.5f * pk + 0.5f) * d.Width);
    LONG cy = static_cast<LONG>((0.5f - ny * 0.5f * pk) * d.Height);

    IDirect3DSurface9* oldRt = nullptr;
    dev->GetRenderTarget(0, &oldRt);
    D3DVIEWPORT9 oldVp;
    dev->GetViewport(&oldVp);
    DWORD scissor = FALSE;
    dev->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissor);
    dev->SetRenderTarget(0, bb);
    D3DVIEWPORT9 vp = {0, 0, d.Width, d.Height, 0.0f, 1.0f};
    dev->SetViewport(&vp);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);

    const LONG gap = 6, len = 16, th = 3, b = 2;
    auto bars = [&](LONG grow, D3DCOLOR color) {
        D3DRECT r[5] = {
            {cx - th / 2 - grow - 1, cy - th / 2 - grow - 1, cx + th / 2 + grow + 1, cy + th / 2 + grow + 1},       // punto
            {cx - gap - len - grow, cy - th / 2 - grow, cx - gap + grow, cy + th / 2 + 1 + grow},                   // sinistra
            {cx + gap - grow, cy - th / 2 - grow, cx + gap + len + grow, cy + th / 2 + 1 + grow},                   // destra
            {cx - th / 2 - grow, cy - gap - len - grow, cx + th / 2 + 1 + grow, cy - gap + grow},                   // su
            {cx - th / 2 - grow, cy + gap - grow, cx + th / 2 + 1 + grow, cy + gap + len + grow},                   // giu'
        };
        dev->Clear(5, r, D3DCLEAR_TARGET, color, 1.0f, 0);
    };
    bars(b, D3DCOLOR_XRGB(0, 0, 0));
    bars(0, D3DCOLOR_XRGB(90, 255, 120));

    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, scissor);
    dev->SetViewport(&oldVp);
    if (oldRt) {
        dev->SetRenderTarget(0, oldRt);
        oldRt->Release();
    }
    bb->Release();
}

HRESULT WINAPI Hooked_SetTransform(
    IDirect3DDevice9* self, D3DTRANSFORMSTATETYPE State, const D3DMATRIX* pMatrix) {
    static std::atomic<int> logged{0};
    if (State == D3DTS_VIEW || State == D3DTS_PROJECTION) {
        if (logged.fetch_add(1) < 20) {
            vr::log::Line(
                "SetTransform state=" + std::to_string(static_cast<int>(State)) +
                " _41=" + std::to_string(pMatrix->_41) +
                " _42=" + std::to_string(pMatrix->_42) +
                " _43=" + std::to_string(pMatrix->_43));
        }
    }
    return g_SetTransform_Original(self, State, pMatrix);
}

// Diagnostica stereo: quali registri VS ricevono la posizione della camera
// (o il suo opposto, es. PreViewTranslation)? Serve per capire, sul thread
// di rendering, da quale posizione (= quale occhio) e' stato calcolato il
// frame. g_camLocForVs e' copiata dal thread di gioco.
float g_camLocForVs[3] = {};
std::atomic<bool> g_camLocValid{false};
std::atomic<uint32_t> g_vsRegMatchPlus[8];   // bitmask 256 registri, +loc
std::atomic<uint32_t> g_vsRegMatchMinus[8];  // bitmask 256 registri, -loc

// Origini generate per ciascun occhio dal thread di gioco (ApplyEyeOffset),
// confrontate sul thread di rendering con c5 (= -origine della vista) per
// sapere a quale occhio appartiene il frame.
struct EyeOrigin {
    float x, y, z;
    int eye;
    bool poseValid;           // posa del visore del tick che ha generato l'origine
    vr::HmdMatrix34_t pose;
    uint32_t tick;            // tick stereo (g_stereoDraws) che ha generato l'origine
    bool valid;               // false: casella mai scritta o annullata
};
extern std::atomic<uint32_t> g_stereoDraws;
// Posa del visore usata dal tick di gioco in corso (thread di gioco).
vr::HmdMatrix34_t g_tickHmdPose = {};
bool g_tickHmdPoseValid = false;
constexpr int kEyeRingSize = 32;
EyeOrigin g_eyeRing[kEyeRingSize];
std::atomic<uint32_t> g_eyeRingPos{0};
std::atomic<uint32_t> g_eyeMatches[2];
std::atomic<uint32_t> g_eyeMisses{0};
constexpr UINT kViewOriginRegister = 5;

// Annulla le origini registrate (all'ingresso e all'uscita dallo schermo
// virtuale: le voci vecchie non devono riconoscere i frame nuovi).
void InvalidateEyeRing() {
    for (auto& e : g_eyeRing) e.valid = false;
}

// c0-c3 del passaggio che ha dato l'occhio (la scena principale): per misurare
// il FOV effettivo del rendering (l'ultimo c0-c3 del frame era dell'interfaccia,
// 90x58,7 gradi anche col visore).
float g_vsViewProj[16] = {};
float g_mainViewProj[16] = {};

void IdentifyEyeFromViewOrigin(const float* v) {
    float x = -v[0], y = -v[1], z = -v[2];
    // Origine (0,0,0): passaggi senza vista (post-processo, HUD). Prima
    // combaciava con le caselle vuote dell'anello (tutte "occhio sinistro in
    // 0,0,0") e all'inizio del gameplay i frame destri diventavano sinistri:
    // 282 sinistri e 0 destri, fucile doppio (18/10).
    if (std::fabs(x) < 1.0f && std::fabs(y) < 1.0f && std::fabs(z) < 1.0f) return;
    // Vale il primo riconoscimento del frame (la scena principale); i passaggi
    // successivi non lo cambiano. vr::control::g_presentEye torna a -1 a ogni Present.
    if (vr::control::g_presentEye.load() >= 0) return;
    for (int i = 0; i < kEyeRingSize; ++i) {
        const EyeOrigin& e = g_eyeRing[i];
        if (!e.valid) continue;
        if (std::fabs(e.x - x) < 0.01f && std::fabs(e.y - y) < 0.01f && std::fabs(e.z - z) < 0.01f) {
            vr::control::g_presentEye = e.eye;
            std::memcpy(g_mainViewProj, g_vsViewProj, sizeof(g_mainViewProj));
            g_eyeMatches[e.eye & 1].fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    g_eyeMisses.fetch_add(1, std::memory_order_relaxed);
}

// Ultima matrice c0-c3 (view-projection della vista, senza traslazione):
// g_vsViewProj e g_mainViewProj sono dichiarate sopra.

void ScanVsConstantsForCameraLocation(UINT start, const float* data, UINT count) {
    if (start == 0 && count >= 4) std::memcpy(g_vsViewProj, data, sizeof(g_vsViewProj));
    if (vr::control::g_stereoHalfIpd > 0 && start <= kViewOriginRegister && kViewOriginRegister < start + count) {
        IdentifyEyeFromViewOrigin(data + (kViewOriginRegister - start) * 4);
    }
    // Il resto e' solo diagnostica (registri che contengono la camera).
    if (!vr::control::g_vsDiag || !g_camLocValid) return;
    const float* c = g_camLocForVs;
    for (UINT i = 0; i < count && start + i < 256; ++i) {
        const float* v = data + i * 4;
        float dp = std::fabs(v[0] - c[0]) + std::fabs(v[1] - c[1]) + std::fabs(v[2] - c[2]);
        float dm = std::fabs(v[0] + c[0]) + std::fabs(v[1] + c[1]) + std::fabs(v[2] + c[2]);
        UINT r = start + i;
        if (dp < 30.0f) g_vsRegMatchPlus[r / 32].fetch_or(1u << (r % 32), std::memory_order_relaxed);
        if (dm < 30.0f) g_vsRegMatchMinus[r / 32].fetch_or(1u << (r % 32), std::memory_order_relaxed);
    }
}

std::string RegMaskToString(std::atomic<uint32_t>* mask) {
    std::string out;
    for (int r = 0; r < 256; ++r) {
        if (mask[r / 32].load() & (1u << (r % 32))) out += " c" + std::to_string(r);
    }
    return out.empty() ? " (nessuno)" : out;
}

std::atomic<int> g_hudDiagFrames{0};
int g_hudDiagId = 0;
bool g_canvasMatrixActive = false;

// Diagnostica HUD: chiamate di disegno sul back buffer durante un dumpcam.
typedef HRESULT(WINAPI* DrawPrimitive_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
typedef HRESULT(WINAPI* DrawIndexedPrimitive_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
typedef HRESULT(WINAPI* DrawPrimitiveUP_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
typedef HRESULT(WINAPI* DrawIndexedPrimitiveUP_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*,
                                                  D3DFORMAT, const void*, UINT);
DrawPrimitive_t g_DrawPrimitive_Original = nullptr;
DrawIndexedPrimitive_t g_DrawIndexedPrimitive_Original = nullptr;
DrawPrimitiveUP_t g_DrawPrimitiveUP_Original = nullptr;
DrawIndexedPrimitiveUP_t g_DrawIndexedPrimitiveUP_Original = nullptr;

// Anello degli ultimi disegni: alla copia finale sul back buffer (unico
// disegno su quel render target) stampa i 40 precedenti, cioe' l'HUD.
struct DrawRec {
    char kind[6];
    UINT prims, stride, rtW, rtH, vpX, vpY, vpW, vpH;
    void* vs;
    void* tex;
    float v0[4];
    float c0[8];
    float regs[16][4];
};
DrawRec g_drawRing[64];
int g_drawRingPos = 0;
float g_lastC0[8] = {};
float g_vsShadow[16][4] = {};

void LogHudDraw(IDirect3DDevice9* dev, const char* kind, UINT prims, const void* verts, UINT stride) {
    if (g_hudDiagFrames <= 0) return;
    IDirect3DSurface9* rt = nullptr;
    D3DSURFACE_DESC d = {};
    if (FAILED(dev->GetRenderTarget(0, &rt)) || !rt) return;
    rt->GetDesc(&d);
    rt->Release();
    if (d.Height == 2500) {
        static int lastDumpId = -1;
        if (lastDumpId == g_hudDiagId) return;
        bool hasHud = false;
        for (int i = 0; i < 40; ++i) hasHud |= g_drawRing[(g_drawRingPos - 40 + i + 64) % 64].v0[0] > 1000.0f;
        if (!hasHud) return;  // frame senza HUD: aspetta il prossimo
        lastDumpId = g_hudDiagId;
        for (int i = 0; i < 40; ++i) {
            const DrawRec& r = g_drawRing[(g_drawRingPos - 40 + i + 64) % 64];
            char b[400];
            std::snprintf(b, sizeof(b),
                "DRAWSEQ %02d %s prims=%u stride=%u RT=%ux%u vp=%u,%u %ux%u vs=%p tex=%p v0=(%.1f %.1f %.2f %.2f) c0=(%.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f)",
                i, r.kind, r.prims, r.stride, r.rtW, r.rtH, r.vpX, r.vpY, r.vpW, r.vpH, r.vs, r.tex, r.v0[0], r.v0[1], r.v0[2], r.v0[3],
                r.c0[0], r.c0[1], r.c0[2], r.c0[3], r.c0[4], r.c0[5], r.c0[6], r.c0[7]);
            vr::log::Line(b);
            if (r.v0[0] > 1000.0f) {
                for (int k = 0; k < 16; ++k) {
                    char rb[128];
                    std::snprintf(rb, sizeof(rb), "DRAWSEQ    c%d = %.5f %.5f %.5f %.5f", k, r.regs[k][0], r.regs[k][1], r.regs[k][2], r.regs[k][3]);
                    vr::log::Line(rb);
                }
            }
        }
        return;
    }
    DrawRec& r = g_drawRing[g_drawRingPos++ % 64];
    std::snprintf(r.kind, sizeof(r.kind), "%s", kind);
    r.prims = prims; r.stride = stride; r.rtW = d.Width; r.rtH = d.Height;
    D3DVIEWPORT9 vp = {};
    dev->GetViewport(&vp);
    r.vpX = vp.X; r.vpY = vp.Y; r.vpW = vp.Width; r.vpH = vp.Height;
    IDirect3DVertexShader9* vs = nullptr;
    dev->GetVertexShader(&vs);
    r.vs = vs;
    if (vs) vs->Release();
    IDirect3DBaseTexture9* tex = nullptr;
    dev->GetTexture(0, &tex);
    r.tex = tex;
    if (tex) tex->Release();
    std::memset(r.v0, 0, sizeof(r.v0));
    if (verts && stride >= 16) std::memcpy(r.v0, verts, sizeof(r.v0));
    std::memcpy(r.c0, g_lastC0, sizeof(r.c0));
    std::memcpy(r.regs, g_vsShadow, sizeof(r.regs));
}

HRESULT WINAPI Hooked_DrawPrimitive(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT start, UINT count) {
    LogHudDraw(d, "DP", count, nullptr, 0);
    return g_DrawPrimitive_Original(d, t, start, count);
}
HRESULT WINAPI Hooked_DrawIndexedPrimitive(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, INT bv, UINT mi, UINT nv, UINT si, UINT pc) {
    LogHudDraw(d, "DIP", pc, nullptr, 0);
    if (g_gqNeedStart && vr::control::g_gpuTimer) GpuFrameStart(d);
    if (vr::control::g_dynResTestFrames > 0) {
        // Larghezza massima del viewport sui render target grandi come il back buffer.
        D3DVIEWPORT9 vp;
        IDirect3DSurface9* rt = nullptr;
        if (SUCCEEDED(d->GetViewport(&vp)) && SUCCEEDED(d->GetRenderTarget(0, &rt)) && rt) {
            D3DSURFACE_DESC rd;
            rt->GetDesc(&rd);
            rt->Release();
            if (rd.Width == g_backBufferW && vp.Width > g_testVpMax) g_testVpMax = vp.Width;
        }
    }
    ++g_frameDraws;
    g_frameTris += pc;
    return g_DrawIndexedPrimitive_Original(d, t, bv, mi, nv, si, pc);
}
// Matrice del Canvas impostata per ultima: quella del gioco e quella
// ridotta/spostata per l'HUD in VR (AdjustCanvasMatrix).
struct CanvasMatrixState {
    bool adjusted = false;
    UINT reg = 0;
    float orig[16], adj[16];
    float full[16];  // originale ristretta come la scena (risoluzione dinamica)
};
CanvasMatrixState g_canvasState;

// Disegno del Canvas che copre quasi tutto lo schermo (sangue ai bordi quando
// si e' feriti, lampi, dissolvenze): con la matrice originale, a tutta vista.
// Ridotto come l'HUD diventava un riquadro coi bordi visibili davanti agli
// occhi (playtest 19/10).
bool IsFullScreenCanvasDraw(UINT nv, const void* v, UINT stride) {
    if (!g_canvasState.adjusted || !v || stride < 8 || nv < 3 || nv > 4096 || !g_backBufferW || !g_backBufferH) return false;
    float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
    const char* p = static_cast<const char*>(v);
    for (UINT i = 0; i < nv; ++i) {
        const float* xy = reinterpret_cast<const float*>(p + i * stride);
        minX = (std::min)(minX, xy[0]);
        maxX = (std::max)(maxX, xy[0]);
        minY = (std::min)(minY, xy[1]);
        maxY = (std::max)(maxY, xy[1]);
    }
    return maxX - minX > 0.85f * g_backBufferW && maxY - minY > 0.85f * g_backBufferH;
}

template <typename Draw>
HRESULT DrawCanvasMaybeFullScreen(IDirect3DDevice9* d, UINT nv, const void* v, UINT stride, Draw draw) {
    if (!IsFullScreenCanvasDraw(nv, v, stride)) return draw();
    // Matrice originale ma ristretta come la scena: con la risoluzione dinamica
    // al 70% la scena occupa il 70% centrale, e il sangue ai bordi disegnato a
    // tutto viewport finiva fuori dall'immagine inviata (audit 20/10).
    g_SetVertexShaderConstantF_Original(d, g_canvasState.reg, g_canvasState.full, 4);
    HRESULT hr = draw();
    g_SetVertexShaderConstantF_Original(d, g_canvasState.reg, g_canvasState.adj, 4);
    return hr;
}

HRESULT WINAPI Hooked_DrawPrimitiveUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT pc, const void* v, UINT stride) {
    LogHudDraw(d, "DPUP", pc, v, stride);
    UINT nv = t == D3DPT_TRIANGLELIST ? pc * 3 : (t == D3DPT_TRIANGLESTRIP || t == D3DPT_TRIANGLEFAN ? pc + 2 : 0);
    return DrawCanvasMaybeFullScreen(d, nv, v, stride, [&] { return g_DrawPrimitiveUP_Original(d, t, pc, v, stride); });
}
HRESULT WINAPI Hooked_DrawIndexedPrimitiveUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT mi, UINT nv, UINT pc, const void* idx,
                                             D3DFORMAT f, const void* v, UINT stride) {
    LogHudDraw(d, "DIPUP", pc, v, stride);
    return DrawCanvasMaybeFullScreen(d, mi + nv, v, stride,
                                     [&] { return g_DrawIndexedPrimitiveUP_Original(d, t, mi, nv, pc, idx, f, v, stride); });
}

// --- HUD in VR ------------------------------------------------------------
//
// L'HUD e' disegnato dal Canvas di UE3 con una matrice riconoscibile in
// c0-c3 (16/10): righe (1,0,0,0) (0,-aspect,0,0) (0,0,~1,1) (eps,eps,-~10,eps),
// con i vertici in pixel traslati di c5 e w = z = meta' larghezza: e' una
// prospettiva che rimappa i pixel dello schermo. Scalando x/y (prime due
// colonne di ogni riga) l'HUD si stringe verso il centro, cosi' gli elementi
// ancorati ai bordi (munizioni a destra) restano visibili in entrambi gli
// occhi; uno spostamento opposto per occhio lo porta a distanza finita.
bool IsCanvasMatrix(const float* m) {
    // Righe (1,0,0,0) (0,-aspect,0,0) (0,0,~1,1); la quarta contiene la
    // traslazione in pixel (-W/2, H/2, z, W/2) e non va controllata.
    return std::fabs(m[0] - 1.0f) < 1e-3f && std::fabs(m[1]) < 1e-3f && std::fabs(m[2]) < 1e-3f &&
           std::fabs(m[3]) < 1e-3f && std::fabs(m[4]) < 1e-3f && m[5] < -0.1f && m[5] > -3.0f &&
           std::fabs(m[6]) < 1e-3f && std::fabs(m[7]) < 1e-3f && std::fabs(m[8]) < 1e-3f &&
           std::fabs(m[9]) < 1e-3f && std::fabs(m[11] - 1.0f) < 1e-3f;
}

bool AdjustCanvasMatrix(UINT start, const float* data, UINT count, float* out) {
    // Il Canvas carica la matrice in c0-c3 o in c6-c9 a seconda dello shader.
    if ((start != 0 && start != 6) || count != 4 || !IsCanvasMatrix(data)) return false;
    if (vr::control::g_cinemaActive) return false;  // schermo virtuale: HUD/sottotitoli come sul monitor
    if (g_scopeActive) {
        // Cannocchiale: maschera a tutta vista (niente riduzione dell'HUD). Il
        // gioco la centra su un'area 16:9 (centro a y = W*9/32, misurato), il
        // back buffer e' quasi quadrato: la si sposta giu' al centro della vista.
        UINT w = g_backBufferW, h = g_backBufferH;
        if (!w || !h) return false;
        float hc = w * 9.0f / 16.0f;
        if (hc >= h) return false;
        std::memcpy(out, data, sizeof(float) * 16);
        float shiftNdc = -(1.0f - hc / h);  // y_ndc += shift  <=>  colonna y += shift * colonna w
        for (int i = 0; i < 4; ++i) out[4 * i + 1] += shiftNdc * out[4 * i + 3];
        return true;
    }
    // Frame in disegno: proiezione decentrata e percentuale della risoluzione
    // dinamica di questo disegno (prima si usavano quelle del Present
    // precedente). Con la scena ristretta al centro (ScreenPercentage) l'HUD,
    // disegnato su tutto il viewport, va ristretto allo stesso modo: prima
    // con la risoluzione al 65% risultava ingrandito, tagliato ai bordi e
    // diviso tra gli occhi (audit 19/10).
    FrameTag cur;
    bool haveTag = PeekRenderingTag(&cur);
    bool offCenter = haveTag ? cur.offCenter : g_presentOffCenter.load();
    float pk = (haveTag ? cur.screenPercent : g_presentPercent.load()) / 100.0f;
    if (!(pk > 0.2f && pk <= 1.0f)) pk = 1.0f;
    float scale = vr::control::g_hudScale, depth = vr::control::g_hudDepth;
    if (scale == 1.0f && depth <= 0.0f && pk >= 1.0f) return false;
    std::memcpy(out, data, sizeof(float) * 16);
    for (int i = 0; i < 4; ++i) {
        out[4 * i + 0] *= scale;
        out[4 * i + 1] *= scale;
    }
    auto applyPercent = [&] {
        for (int i = 0; i < 4; ++i) {
            out[4 * i + 0] *= pk;
            out[4 * i + 1] *= pk;
        }
    };
    int eye = vr::control::g_presentEye;  // occhio del Draw in corso (thread di rendering)
    if (haveTag && cur.eye >= 0) eye = cur.eye;
    float half = vr::control::g_stereoHalfIpd;
    float th = std::tan(vr::control::g_fovOverride * 0.5f / 57.2957795f);
    vr::output::EyeProjection pj;
    if (eye >= 0 && th > 0.0f && offCenter && vr::output::OffCenterProjection(eye, &pj)) {
        // Immagine decentrata: l'HUD (disposto come nella vista simmetrica,
        // tangente = NDC * th) va riportato nella proiezione dell'occhio, piu'
        // la parallasse della distanza apparente.
        float tv = th * (g_backBufferW && g_backBufferH ? float(g_backBufferH) / g_backBufferW : 1.0f);
        float par = (depth > 0.0f && half > 0.0f) ? (eye == 0 ? 1.0f : -1.0f) * half / depth : 0.0f;
        float parY = 0.0f;
        float cf[3], cr[3], cu[3];
        if (vr::output::EyeCant(eye, cf, cr, cu) && cf[0] > 0.1f) {
            // Occhio inclinato: "davanti alla testa" non e' il centro dell'occhio.
            par += cr[0] / cf[0];
            parY = cu[0] / cf[0];
        }
        for (int i = 0; i < 4; ++i) {
            out[4 * i + 0] = out[4 * i + 0] * th * pj.m00 + (par * pj.m00 + pj.m20) * out[4 * i + 3];
            out[4 * i + 1] = out[4 * i + 1] * tv * pj.m11 + (parY * pj.m11 + pj.m21) * out[4 * i + 3];
        }
        applyPercent();
        return true;
    }
    if (depth > 0.0f && half > 0.0f && eye >= 0 && th > 0.0f) {
        // x_ndc += shift  <=>  colonna x += shift * colonna w.
        float shiftNdc = (eye == 0 ? 1.0f : -1.0f) * (half / depth) / th;
        for (int i = 0; i < 4; ++i) out[4 * i + 0] += shiftNdc * out[4 * i + 3];
    }
    applyPercent();
    return true;
}

HRESULT WINAPI Hooked_SetVertexShaderConstantF(
    IDirect3DDevice9* self, UINT StartRegister, const float* pConstantData, UINT Vector4fCount) {
    // Riconoscimento dell'occhio dall'origine della vista (c5): sempre, serve
    // a riallineare le etichette dei frame. La diagnostica dei registri dentro
    // la funzione solo con "vsdiag on". (19/10: spegnere tutto aveva tolto il
    // riallineamento e dopo le scene scriptate gli occhi restavano scambiati.)
    ScanVsConstantsForCameraLocation(StartRegister, pConstantData, Vector4fCount);
    if (StartRegister == 0 && Vector4fCount >= 4) g_canvasMatrixActive = IsCanvasMatrix(pConstantData);
    if (StartRegister == 0 && Vector4fCount >= 2) std::memcpy(g_lastC0, pConstantData, sizeof(g_lastC0));
    for (UINT i = 0; i < Vector4fCount && StartRegister + i < 16; ++i) std::memcpy(g_vsShadow[StartRegister + i], pConstantData + 4 * i, 16);
    float hud[16];
    if (AdjustCanvasMatrix(StartRegister, pConstantData, Vector4fCount, hud)) {
        g_canvasState.adjusted = true;
        g_canvasState.reg = StartRegister;
        std::memcpy(g_canvasState.orig, pConstantData, sizeof(g_canvasState.orig));
        std::memcpy(g_canvasState.adj, hud, sizeof(g_canvasState.adj));
        {
            FrameTag cur;
            float pk = (PeekRenderingTag(&cur) ? cur.screenPercent : g_presentPercent.load()) / 100.0f;
            if (!(pk > 0.2f && pk <= 1.0f) || g_scopeActive) pk = 1.0f;
            std::memcpy(g_canvasState.full, pConstantData, sizeof(g_canvasState.full));
            for (int i = 0; i < 4; ++i) {
                g_canvasState.full[4 * i + 0] *= pk;
                g_canvasState.full[4 * i + 1] *= pk;
            }
        }
        return g_SetVertexShaderConstantF_Original(self, StartRegister, hud, Vector4fCount);
    }
    if (StartRegister <= g_canvasState.reg + 3 && StartRegister + Vector4fCount > g_canvasState.reg)
        g_canvasState.adjusted = false;  // matrice sovrascritta da altro
    // Ci interessano solo le chiamate che caricano esattamente una matrice
    // 4x4 (4 vettori); gli array di matrici (es. bone per lo skinning) non
    // sono candidati camera e vengono ignorati.
    if (Vector4fCount == 4 && vr::control::g_vsDiag) {
        AnalyzeVsConstCandidate(StartRegister, pConstantData);
        AnalyzeProjectionSignature(StartRegister, pConstantData);
    }
    return g_SetVertexShaderConstantF_Original(self, StartRegister, pConstantData, Vector4fCount);
}

// --- Fase 3: tracking della testa del Quest 3 -------------------------
//
// Indirizzo FISSO (nessuna scansione di memoria, vale per sempre su questo
// exe perche' non c'e' ASLR) della funzione nativa UnrealScript
// APawn::execGetPawnViewLocation, trovato nella tabella di registrazione
// delle funzioni native di UE3 (vedi README, sezione Fase 2). E' una
// funzione membro C++ (thiscall): il puntatore al Pawn arriva in ECX.
constexpr uintptr_t kExecGetPawnViewLocationAddr = 0x00BBD730;
constexpr uintptr_t kExecGetViewTargetAddr = 0x0057E900;  // APlayerController::execGetViewTarget
// Offset confermati empiricamente nella struct APawn (vedi README). NOTA:
// +0x674 si e' rivelato essere la rotazione del CORPO (per l'animazione),
// NON la camera -- vedi sotto per il vero oggetto camera.
constexpr int kOffsetLocationX = 0x54;  // 3 float consecutivi X,Y,Z
constexpr int kOffsetYaw = 0x674;       // int32, unita' Rotator UE3 (65536=360°)

// --- Vera camera, trovata da una cheat table pubblica (freecam) per questo
// gioco (Jim2Point0/One3rd/Erika T., via gist di FransBouma, autore di
// 3Dmigoto). Tecnica: un punto di codice FISSO (0x00400000+0x3ECAB2) e'
// un epilogo condiviso "fstp dword ptr [ecx]; ret 0014" chiamato per
// scrivere MOLTE proprieta' float/vettore diverse in tutto il gioco (verosimilmente
// il setter generico usato dalla VM di UnrealScript per le proprieta'); lo
// filtriamo controllando un valore-firma a [ecx+0x64] (il float 0.05, bit
// pattern 0x3D4CCCCD) per riconoscere specificamente l'istanza camera.
constexpr uintptr_t kCameraHookAddr = 0x007ECAB2;
constexpr int kCameraSignatureOffset = 0x64;
constexpr uint32_t kCameraSignatureValue = 0x3D4CCCCD;  // float 0.05f
constexpr int kCameraOffsetX = 0x10;
constexpr int kCameraOffsetY = 0x14;
constexpr int kCameraOffsetZ = 0x18;
// Ordine FRotator di UE3 (Pitch, Yaw, Roll), verificato il 16/10 campionando
// la memoria dal vivo: +0x20 oscilla di migliaia di unita' girando col mouse,
// +0x1C guardando su/giu', +0x24 piccole oscillazioni (dondolio camera). La
// cheat table li chiamava "Y/X Rotation" invertiti rispetto a questo.
constexpr int kCameraOffsetPitch = 0x1C;
constexpr int kCameraOffsetYaw = 0x20;
constexpr int kCameraOffsetRoll = 0x24;

// Scritto dal thread di GIOCO (hook del setter), letto dal thread di
// RENDERING (BeginScene): atomici. g_cameraSeenSeq avanza ad ogni passaggio
// dell'hook sull'istanza camera: se smette di avanzare l'oggetto e'
// probabilmente stato liberato (cambio camera/checkpoint) e non va toccato.
std::atomic<void*> g_cameraPtr{nullptr};
std::atomic<uint32_t> g_cameraSeenSeq{0};
std::atomic<DWORD> g_gameThreadId{0};

typedef void(__thiscall* CameraSetter_t)(void* self);
CameraSetter_t g_CameraSetter_Original = nullptr;

// Hook estremamente minimale e deliberatamente SENZA log/allocazioni: questa
// funzione e' l'epilogo di un setter che lascia un valore sullo stack FPU
// (x87, ST0) da scrivere con fstp. Qualsiasi operazione in virgola mobile
// nostra qui rischierebbe di corrompere quello stato. Facciamo solo un
// confronto a interi e una scrittura di puntatore, poi passiamo subito
// all'originale (il trampoline di MinHook) che esegue il vero fstp/ret.
// Lettura protetta (SEH): questo codice condiviso viene raggiunto da
// moltissimi punti diversi del motore, non e' garantito che 'self' sia
// sempre un vero puntatore ad oggetto leggibile a quell'offset. Una
// dereferenziazione alla cieca ha causato un crash reale in un test
// precedente. Funzione dedicata, senza oggetti C++ nello scope, cosi'
// __try/__except e' sicuro da usare.
bool TrySafeReadUint32(void* addr, uint32_t* outValue) {
    __try {
        *outValue = *reinterpret_cast<uint32_t*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool TrySafeWriteInt32(void* addr, int32_t value) {
    __try {
        *reinterpret_cast<int32_t*>(addr) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool HasCameraSignature(void* p) {
    uint32_t signature = 0;
    return p != nullptr &&
           TrySafeReadUint32(reinterpret_cast<char*>(p) + kCameraSignatureOffset, &signature) &&
           signature == kCameraSignatureValue;
}

// Chiamata dallo stub naked qui sotto. Solo aritmetica intera: ST0 contiene
// ancora il float che l'fstp originale deve scrivere, non va toccato.
extern "C" void __cdecl CameraHookCheck(void* dest) {
    if (HasCameraSignature(dest)) {
        g_cameraPtr = dest;
        vr::control::g_playerController = static_cast<char*>(dest) - 0x77C;
        g_cameraSeenSeq.fetch_add(1, std::memory_order_relaxed);
        g_gameThreadId = GetCurrentThreadId();
    }
}

// 0x007ECAB2 e' a META' funzione (setter thiscall a 0x007ECAA0, ECX qui e'
// gia' stato ricaricato con l'argomento destinazione [esp+4]):
//   fstp dword ptr [ecx]
//   ret 0x14
// Il detour quindi NON puo' essere una funzione C++ che fa 'call' del
// trampoline: il 'ret 0x14' tornerebbe dentro al detour togliendo 0x14 byte
// di stack, e il nostro ret salterebbe a spazzatura (crash con EIP=0 del
// 15/10 21:19). Stub naked: salva i registri, controlla la firma, poi
// 'jmp' (non call) al trampoline con lo stack identico all'originale.
__declspec(naked) void CameraHookStub() {
    __asm {
        pushad
        pushfd
        push ecx
        call CameraHookCheck
        add esp, 4
        popfd
        popad
        jmp dword ptr [g_CameraSetter_Original]
    }
}

void* GetCameraHookThunkAddress() {
    return reinterpret_cast<void*>(&CameraHookStub);
}

// --- Diagnostica sicura: osservatore di scrittura su una pagina di memoria
// via Vectored Exception Handler, interamente in-process. Niente debugger
// esterno, niente traduzione WOW64 (siamo gia' codice nativo a 32 bit
// dentro al gioco): molto piu' sicuro di tools/hwbp.py (che ha causato 4
// crash del gioco e non viene piu' usato).
constexpr SIZE_T kPageSize = 0x1000;
void* g_watchPageBase = nullptr;
void* g_watchExactLo = nullptr;  // range esatto di interesse: solo qui
void* g_watchExactHi = nullptr;  // logghiamo e consumiamo budget
std::atomic<bool> g_watchArmed{false};
std::atomic<bool> g_watchPendingRearm{false};
std::atomic<int> g_watchHitBudget{40};

LONG WINAPI PageWatchHandler(PEXCEPTION_POINTERS info) {
    DWORD code = info->ExceptionRecord->ExceptionCode;
    PCONTEXT ctx = info->ContextRecord;

    if (code == EXCEPTION_ACCESS_VIOLATION && g_watchArmed) {
        void* faultAddr = reinterpret_cast<void*>(info->ExceptionRecord->ExceptionInformation[1]);
        if (faultAddr >= g_watchPageBase &&
            faultAddr < reinterpret_cast<char*>(g_watchPageBase) + kPageSize) {
            bool inExactRange = faultAddr >= g_watchExactLo && faultAddr < g_watchExactHi;
            if (inExactRange) {
                if (g_watchHitBudget.fetch_sub(1) > 0) {
                    char buf[256];
                    std::snprintf(buf, sizeof(buf),
                        "PAGE-WATCH write to 0x%p  EIP=0x%08X EAX=0x%08X EBX=0x%08X ECX=0x%08X "
                        "EDX=0x%08X ESI=0x%08X EDI=0x%08X",
                        faultAddr, ctx->Eip, ctx->Eax, ctx->Ebx, ctx->Ecx, ctx->Edx, ctx->Esi, ctx->Edi);
                    vr::log::Line(buf);
                } else if (g_watchArmed.exchange(false)) {
                    vr::log::Line("PAGE-WATCH: budget exhausted, protection removed.");
                    DWORD old;
                    VirtualProtect(g_watchPageBase, kPageSize, PAGE_READWRITE, &old);
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            }
            // Lasciamo passare la scrittura (torniamo scrivibili), ma
            // vogliamo tornare a intercettare la PROSSIMA scrittura: single
            // step su questa istruzione, poi ri-proteggiamo nel prossimo
            // trap (EXCEPTION_SINGLE_STEP qui sotto). Scritture fuori dal
            // range esatto non vengono loggate (per non sprecare budget su
            // rumore) ma la pagina resta comunque protetta per il resto.
            DWORD old;
            VirtualProtect(g_watchPageBase, kPageSize, PAGE_READWRITE, &old);
            ctx->EFlags |= 0x100;  // trap flag: un solo passo
            g_watchPendingRearm = true;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (code == EXCEPTION_SINGLE_STEP && g_watchPendingRearm.exchange(false)) {
        if (g_watchArmed) {
            DWORD old;
            VirtualProtect(g_watchPageBase, kPageSize, PAGE_READONLY, &old);
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void ArmPageWatch(void* targetAddr, size_t exactRangeBytes = 4) {
    static std::atomic<bool> handlerRegistered{false};
    if (!handlerRegistered.exchange(true)) {
        AddVectoredExceptionHandler(1, PageWatchHandler);
    }
    uintptr_t pageBase = reinterpret_cast<uintptr_t>(targetAddr) & ~(kPageSize - 1);
    g_watchPageBase = reinterpret_cast<void*>(pageBase);
    g_watchExactLo = targetAddr;
    g_watchExactHi = reinterpret_cast<char*>(targetAddr) + exactRangeBytes;
    g_watchHitBudget = 40;
    DWORD old;
    if (VirtualProtect(g_watchPageBase, kPageSize, PAGE_READONLY, &old)) {
        g_watchArmed = true;
        vr::log::Line("PAGE-WATCH armed on page 0x" +
            [](void* p){ char b[32]; std::snprintf(b, sizeof(b), "%p", p); return std::string(b); }(g_watchPageBase));
    } else {
        vr::log::Line("ERROR: VirtualProtect failed for PAGE-WATCH.");
    }
}

void* g_pawnPtr = nullptr;
void* g_playerControllerPtr = nullptr;
std::atomic<bool> g_openVrReady{false};

typedef void(__thiscall* ExecGetPawnViewLocation_t)(void* self, void* stack, void* result);
ExecGetPawnViewLocation_t g_ExecGetPawnViewLocation_Original = nullptr;
typedef void(__thiscall* ExecGetViewTarget_t)(void* self, void* stack, void* result);
ExecGetViewTarget_t g_ExecGetViewTarget_Original = nullptr;

// MSVC non ammette __thiscall su una funzione libera: per intercettare una
// chiamata thiscall (il Pawn arriva in ECX) serve una funzione MEMBRO, dove
// 'this' viene passato in ECX automaticamente. Classe fittizia, usata solo
// come contenitore: il puntatore a funzione membro, su MSVC x86 non
// virtuale, e' letteralmente l'indirizzo del codice, convertibile a void*.
class PawnHookThunk {
public:
    void Detour(void* stack, void* result) {
        void* self = this;
        if (g_pawnPtr != self) {
            g_pawnPtr = self;
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%p", self);
            vr::log::Line(std::string("Pawn pointer captured: 0x") + buf);
            // PAGE-WATCH disattivato: la sua logica di single-step/re-arm
            // non e' thread-safe e con un motore a 90+ thread come questo
            // ha causato un crash/blocco durante il caricamento. Non
            // riattivare senza aggiungere una vera sincronizzazione
            // (es. una CRITICAL_SECTION attorno a tutto lo stato condiviso).
            // if (!g_watchArmed) {
            //     ArmPageWatch(reinterpret_cast<char*>(self) + kOffsetLocationX, 12);
            // }
        }
        g_ExecGetPawnViewLocation_Original(self, stack, result);
    }
};

void* GetPawnHookThunkAddress() {
    union {
        void (PawnHookThunk::*mfp)(void*, void*);
        void* fp;
    } converter;
    converter.mfp = &PawnHookThunk::Detour;
    return converter.fp;
}

class ViewTargetHookThunk {
public:
    void Detour(void* stack, void* result) {
        void* self = this;
        if (g_playerControllerPtr != self) {
            g_playerControllerPtr = self;
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%p", self);
            vr::log::Line(std::string("PlayerController pointer captured: 0x") + buf);
        }
        g_ExecGetViewTarget_Original(self, stack, result);
    }
};

void* GetViewTargetHookThunkAddress() {
    union {
        void (ViewTargetHookThunk::*mfp)(void*, void*);
        void* fp;
    } converter;
    converter.mfp = &ViewTargetHookThunk::Detour;
    return converter.fp;
}

bool InitOpenVR() {
    // Diagnostica: con il file "specopsvr_novr" nella cartella del gioco la
    // mod non si registra presso SteamVR (gioco solo su monitor).
    {
        char path[MAX_PATH] = {};
        HMODULE self = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCSTR>(&InitOpenVR), &self);
        GetModuleFileNameA(self, path, MAX_PATH);
        std::string flag(path);
        flag = flag.substr(0, flag.find_last_of("\\/")) + "\\specopsvr_novr";
        if (GetFileAttributesA(flag.c_str()) != INVALID_FILE_ATTRIBUTES) {
            vr::log::Line("OpenVR NOT initialised (specopsvr_novr file present).");
            return false;
        }
    }
    // Background all'avvio, Scene solo con il visore indossato (vr_session.h).
    return vr::session::Init();
}

// Estrae lo yaw (rotazione attorno all'asse verticale Y) dalla matrice di
// posa OpenVR (convenzione Y-up, mano destra). Il segno/offset esatto
// potrebbe richiedere una calibrazione empirica la prima volta che si
// prova nel visore.
float YawFromOpenVrPose(const vr::HmdMatrix34_t& m) {
    float forwardX = -m.m[0][2];
    float forwardZ = -m.m[2][2];
    // Guardando quasi dritto in basso (o in alto) la proiezione orizzontale
    // dell'avanti e' corta e rumorosa: si aggiunge quella dell'asse "su" della
    // testa, che in quel caso punta avanti (in basso) o indietro (in alto).
    // Prima lo yaw saltava e poteva far girare il corpo (audit 19/10).
    float forwardY = -m.m[1][2];
    float k = forwardY < 0.0f ? -forwardY : forwardY;
    float sgn = forwardY < 0.0f ? 1.0f : -1.0f;
    forwardX += sgn * k * m.m[0][1];
    forwardZ += sgn * k * m.m[2][1];
    return std::atan2(forwardX, -forwardZ);
}

// Estrae anche il pitch (rotazione attorno all'asse X/laterale) dalla
// stessa matrice di posa, stessa convenzione di YawFromOpenVrPose.
float PitchFromOpenVrPose(const vr::HmdMatrix34_t& m) {
    float forwardY = -m.m[1][2];
    if (forwardY > 1.0f) forwardY = 1.0f;
    if (forwardY < -1.0f) forwardY = -1.0f;
    return std::asin(forwardY);
}

bool g_rotReferenceCaptured = false;
float g_hmdYawReference = 0.0f;
float g_hmdPitchReference = 0.0f;
int32_t g_gameYawReference = 0;
int32_t g_gamePitchReference = 0;
void* g_lastLoggedCameraPtr = nullptr;

// --- Diagnostica: quale istruzione del gioco scrive yaw/pitch della camera?
//
// Il gioco riscrive la rotazione ad ogni tick sul thread di gioco, quindi la
// nostra scrittura da BeginScene (thread di rendering) viene persa. Per
// sapere dove agganciarci serve l'EIP dello scrittore. Breakpoint hardware
// sui registri DR0/DR1, impostati DALL'INTERNO del processo (siamo codice
// nativo a 32 bit: niente debugger esterno, niente Wow64*ThreadContext come
// in tools/hwbp.py). Il VEH registra solo EIP/registri in array lock-free;
// i log li scrive il thread di diagnostica, mai il VEH.
// Mentre e' attivo il mod NON scrive sulla camera, per vedere solo il gioco.
// Disattivato: il dato (scrittore = memcpy) e' stato ottenuto, e la sessione
// successiva e' crashata nel thread di gioco poco dopo il disarmo.
constexpr bool kDiagnoseCameraWriters = false;
constexpr int kMaxWriterSites = 16;
constexpr int kHwbpMaxHitsPerThread = 20000;
constexpr DWORD kHwbpPhaseMs = 20000;

struct WriterSite {
    std::atomic<uint32_t> eip;
    std::atomic<uint32_t> hits;
    std::atomic<uint32_t> regMask;  // bit0 = yaw (DR0), bit1 = pitch (DR1)
    uint32_t ecx, esi, edi, ebp, esp0, threadId;
};
WriterSite g_writerSites[kMaxWriterSites];
std::atomic<uint32_t> g_hwbpTotalHits{0};
std::atomic<bool> g_hwbpStarted{false};

LONG WINAPI CameraWriteBpHandler(PEXCEPTION_POINTERS info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    PCONTEXT ctx = info->ContextRecord;
    DWORD which = ctx->Dr6 & 0x3;
    if (which == 0) return EXCEPTION_CONTINUE_SEARCH;

    // Breakpoint di dati = trap: EIP punta GIA' all'istruzione successiva
    // alla scrittura.
    uint32_t eip = ctx->Eip;
    for (int i = 0; i < kMaxWriterSites; ++i) {
        WriterSite& s = g_writerSites[i];
        uint32_t cur = s.eip.load();
        if (cur == 0) {
            uint32_t expected = 0;
            if (s.eip.compare_exchange_strong(expected, eip)) {
                s.ecx = ctx->Ecx; s.esi = ctx->Esi; s.edi = ctx->Edi;
                s.ebp = ctx->Ebp; s.threadId = GetCurrentThreadId();
                uint32_t ret = 0;
                TrySafeReadUint32(reinterpret_cast<void*>(ctx->Esp), &ret);
                s.esp0 = ret;
                cur = eip;
            } else {
                cur = expected;
            }
        }
        if (cur == eip) {
            s.hits.fetch_add(1);
            s.regMask.fetch_or(which);
            break;
        }
    }
    ctx->Dr6 = 0;
    if (g_hwbpTotalHits.fetch_add(1) > kHwbpMaxHitsPerThread) {
        ctx->Dr7 = 0;  // circuito di sicurezza: disarma questo thread
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

bool SetThreadHwbp(DWORD tid, uintptr_t yawAddr, uintptr_t pitchAddr, bool arm) {
    HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, tid);
    if (!th) return false;
    bool ok = false;
    if (SuspendThread(th) != static_cast<DWORD>(-1)) {
        CONTEXT ctx = {};
        ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(th, &ctx)) {
            if (arm) {
                ctx.Dr0 = yawAddr;
                ctx.Dr1 = pitchAddr;
                // L0|L1, RW=01 (scrittura), LEN=11 (4 byte) per entrambi.
                ctx.Dr7 = 0x1 | 0x4 | (1u << 16) | (3u << 18) | (1u << 20) | (3u << 22);
            } else {
                ctx.Dr0 = ctx.Dr1 = 0;
                ctx.Dr7 = 0;
            }
            ctx.Dr6 = 0;
            ok = SetThreadContext(th, &ctx) != FALSE;
        }
        ResumeThread(th);
    }
    CloseHandle(th);
    return ok;
}

void LogWriterSites(const char* phase) {
    vr::log::Line(std::string("HWBP results (") + phase + "), total hits=" +
                  std::to_string(g_hwbpTotalHits.load()));
    for (int i = 0; i < kMaxWriterSites; ++i) {
        WriterSite& s = g_writerSites[i];
        if (s.eip == 0) break;
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "HWBP writer #%d EIP(after)=0x%08X hits=%u reg=%s%s tid=%u ECX=0x%08X ESI=0x%08X EDI=0x%08X EBP=0x%08X [esp]=0x%08X",
            i, s.eip.load(), s.hits.load(),
            (s.regMask & 1) ? "yaw " : "", (s.regMask & 2) ? "pitch" : "",
            s.threadId, s.ecx, s.esi, s.edi, s.ebp, s.esp0);
        vr::log::Line(buf);
    }
}

DWORD WINAPI CameraWriteBpThread(LPVOID param) {
    uintptr_t camera = reinterpret_cast<uintptr_t>(param);
    uintptr_t yawAddr = camera + kCameraOffsetYaw;
    uintptr_t pitchAddr = camera + kCameraOffsetPitch;
    AddVectoredExceptionHandler(1, CameraWriteBpHandler);

    // Fase 1: solo il thread di gioco (quello che chiama il setter della
    // cheat table). Raggio d'azione minimo.
    DWORD gameTid = g_gameThreadId;
    bool armed = gameTid && SetThreadHwbp(gameTid, yawAddr, pitchAddr, true);
    vr::log::Line("HWBP phase 1: game thread tid=" + std::to_string(gameTid) +
                  (armed ? " armed" : " NOT armed") + ", waiting " +
                  std::to_string(kHwbpPhaseMs / 1000) + "s (move the mouse and walk!).");
    Sleep(kHwbpPhaseMs);
    if (armed) SetThreadHwbp(gameTid, 0, 0, false);
    LogWriterSites("fase 1, thread di gioco");
    if (g_writerSites[0].eip != 0) return 0;

    // Fase 2: nessuno scrittore sul thread di gioco -> tutti i thread del
    // processo tranne questo.
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    DWORD tids[512];
    int count = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te = {sizeof(te)};
        for (BOOL more = Thread32First(snap, &te); more && count < 512; more = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID == pid && te.th32ThreadID != self) tids[count++] = te.th32ThreadID;
        }
        CloseHandle(snap);
    }
    int armedCount = 0;
    for (int i = 0; i < count; ++i) armedCount += SetThreadHwbp(tids[i], yawAddr, pitchAddr, true);
    vr::log::Line("HWBP phase 2: armed " + std::to_string(armedCount) + "/" + std::to_string(count) +
                  " threads, waiting " + std::to_string(kHwbpPhaseMs / 1000) + "s.");
    Sleep(kHwbpPhaseMs);
    for (int i = 0; i < count; ++i) SetThreadHwbp(tids[i], 0, 0, false);
    LogWriterSites("fase 2, tutti i thread");
    return 0;
}

// --- Applicazione della rotazione della testa sul thread di GIOCO ---------
//
// Il gioco ricalcola la POV in una variabile locale e la copia nella camera
// (PlayerController+0x77C) con memcpy, una volta per frame: e' l'unico
// scrittore di yaw/pitch (trovato col breakpoint hardware del 16/10, vedi
// README). Tutto il codice dell'exe chiama memcpy attraverso un unico thunk
// (0x0102A200: jmp [0x01109438]), quindi sostituiamo quella voce della IAT:
// subito dopo la copia del gioco, se la destinazione copre la rotazione
// della camera, aggiungiamo il delta della testa. Siamo sul thread di gioco
// e subito dopo la scrittura del gioco, quindi niente piu' sovrascritture.
// Nessuna patch di codice: solo un puntatore nella tabella di import.
constexpr uintptr_t kIatMemcpy = 0x01109438;
constexpr uintptr_t kIatMemmove = 0x0110943C;

typedef void*(__cdecl* Memcpy_t)(void* dst, const void* src, size_t n);
Memcpy_t g_memcpyReal = nullptr;
Memcpy_t g_memmoveReal = nullptr;

// Delta calcolati dal thread di rendering (UpdateHeadTracking), applicati
// dal thread di gioco (memcpy). Unita' Rotator UE3 (65536 = 360 gradi).
std::atomic<int32_t> g_deltaYaw{0};
std::atomic<int32_t> g_deltaRoll{0};
// "aim hand": direzione del controller destro rispetto al corpo (unita'
// Rotator) e rotazione del corpo scritta dal gioco prima della nostra
// sostituzione (serve a ricostruire la vista = corpo + testa).
std::atomic<int32_t> g_aimDeltaYaw{0};
std::atomic<int32_t> g_aimDeltaPitch{0};
// Mira dalla canna: rotazione assoluta (dagli occhi del corpo verso il punto
// in cui punta la canna del fucile visibile), calcolata in UpdateAimMarker.
std::atomic<bool> g_muzzleAimValid{false};
std::atomic<int32_t> g_muzzleAimYaw{0}, g_muzzleAimPitch{0};
float g_muzzleTarget[3];  // punto in cui arriva la canna (thread di gioco)
std::atomic<bool> g_aimValid{false};
std::atomic<int32_t> g_bodyYaw{0};
std::atomic<int32_t> g_bodyPitch{0};
// Spostamento della testa in unita' Unreal nel sistema di riferimento del
// corpo al momento del ricentramento: [0] avanti, [1] destra, [2] su.
std::atomic<float> g_headOffset[3];
// Controller destro: posizione rispetto alla testa al ricentramento (unita'
// del gioco: avanti, destra, su, nel riferimento del corpo) e rollio.
std::atomic<float> g_handOffset[3];
std::atomic<int32_t> g_handRoll{0};
std::atomic<bool> g_handValid{false};
std::atomic<bool> g_twoHandActive{false};  // per DUMPCAM
std::atomic<bool> g_leftOnGun{false};      // mano sinistra agganciata all'arma (UpdateArms)
std::atomic<bool> g_leftDriven{false};     // mano sinistra guidata dalla mod (KeepGoodBones)
// Controller sinistro: posizione come g_handOffset, direzione relativa come
// g_aimDelta* (yaw rispetto al riferimento, pitch assoluto) e rollio.
std::atomic<float> g_leftOffset[3];
std::atomic<int32_t> g_leftYaw{0}, g_leftPitch{0}, g_leftRoll{0};
std::atomic<bool> g_leftValid{false};
int32_t RadiansToUnreal(float rad);

// Mira a due mani: si attiva solo quando la mano sinistra e' sulla canna,
// cioe' vicina (8 cm) alla linea di mira del controller destro, tra 12 e 70 cm
// davanti a lui; resta attiva finche' la sinistra non si allontana oltre 14 cm
// dalla linea (isteresi). Mentre e' attiva la mira va dalla destra alla
// sinistra, come imbracciando un fucile. (17/10: la versione precedente, con un
// cono di 40 gradi, scattava sempre e il fucile puntava verso la mano sinistra.)
void ApplyTwoHandAim(const float* right, const float* left) {
    static bool wasActive = false;
    // Passaggio graduale tra una e due mani (~100 ms): prima l'arma scattava
    // di colpo dalla direzione del polso a quella tra le mani.
    static float blend = 0.0f;
    static float twoYaw = 0.0f, twoPitch = 0.0f;
    static double lastT = 0.0;
    double now = ProfMs() * 0.001;
    float dt = lastT > 0.0 ? static_cast<float>(now - lastT) : 0.0f;
    lastT = now;
    if (dt > 0.2f) dt = 0.2f;
    auto applyBlend = [&](bool target) {
        blend += (target ? 1.0f : -1.0f) * dt / 0.1f;
        blend = blend < 0.0f ? 0.0f : (blend > 1.0f ? 1.0f : blend);
        if (blend <= 0.0f) return;
        float ry = static_cast<float>(g_aimDeltaYaw.load()) * (6.28318530718f / 65536.0f);
        float rp = static_cast<float>(g_aimDeltaPitch.load()) * (6.28318530718f / 65536.0f);
        float dy = std::remainder(twoYaw - ry, 6.28318530718f);
        g_aimDeltaYaw = RadiansToUnreal(ry + dy * blend);
        g_aimDeltaPitch = RadiansToUnreal(rp + (twoPitch - rp) * blend);
    };
    g_twoHandActive = false;
    vr::control::g_twoHandEngaged = false;
    if (!vr::control::g_twoHand) {
        wasActive = false;
        applyBlend(false);
        return;
    }
    float scale = vr::control::g_posScale;
    if (scale <= 0.0f) return;
    float d[3] = {(left[0] - right[0]) / scale, (left[1] - right[1]) / scale, (left[2] - right[2]) / scale};
    // Direzione del controller destro (prima della correzione a due mani).
    float yawR = static_cast<float>(g_aimDeltaYaw.load()) * (6.28318530718f / 65536.0f);
    float pitchR = static_cast<float>(g_aimDeltaPitch.load()) * (6.28318530718f / 65536.0f);
    float af[3] = {std::cos(pitchR) * std::cos(yawR), std::cos(pitchR) * std::sin(yawR), std::sin(pitchR)};
    float along = d[0] * af[0] + d[1] * af[1] + d[2] * af[2];
    float px = d[0] - along * af[0], py = d[1] - along * af[1], pz = d[2] - along * af[2];
    float off = std::sqrt(px * px + py * py + pz * pz);
    float limit = wasActive ? 0.14f : 0.08f;
    if (along < 0.12f || along > 0.70f || off > limit) {
        wasActive = false;
        applyBlend(false);
        return;
    }
    float horiz = std::sqrt(d[0] * d[0] + d[1] * d[1]);
    float lineYaw = std::atan2(d[1], d[0]), linePitch = std::atan2(d[2], horiz);
    // All'aggancio si ricorda lo scarto tra la direzione della mano destra
    // (con aimpitch del calcio) e la linea tra le mani, e lo si mantiene:
    // l'arma non salta verso la sinistra (fino a ~15 gradi con la tolleranza
    // di 8 cm) e la calibrazione del calcio resta valida (audit 19/10).
    static float offYaw = 0.0f, offPitch = 0.0f;
    if (!wasActive) {
        offYaw = std::remainder(yawR - lineYaw, 6.28318530718f);
        offPitch = pitchR - linePitch;
        vr::log::Line("AIM: two-handed grip.");
    }
    wasActive = true;
    twoYaw = lineYaw + offYaw;
    twoPitch = linePitch + offPitch;
    applyBlend(true);
    g_twoHandActive = true;
    vr::control::g_twoHandEngaged = true;
}
std::atomic<int32_t> g_deltaPitch{0};
std::atomic<uint32_t> g_overridesApplied{0};

std::atomic<int> g_lastCopyOffset{0};
// Call stack grezzo della copia POV: indirizzi nel .text dell'exe trovati
// sullo stack al momento della chiamata (solo diagnostica, catturato una volta).
constexpr int kCopyStackDepth = 12;
uint32_t g_copyStack[kCopyStackDepth] = {};
std::atomic<bool> g_copyStackCaptured{false};

void CaptureCopyStack(void* returnAddress, void** stackTop) {
    if (g_copyStackCaptured.exchange(true)) return;
    int n = 0;
    g_copyStack[n++] = reinterpret_cast<uint32_t>(returnAddress);
    for (int i = 0; i < 512 && n < kCopyStackDepth; ++i) {
        uint32_t v = 0;
        if (!TrySafeReadUint32(stackTop + i, &v)) break;
        if (v >= 0x00401000 && v < 0x01109000) g_copyStack[n++] = v;
    }
}
std::atomic<int> g_lastCopySize{0};

std::atomic<uint32_t> g_locCopies{0};

bool TrySafeReadFloat3(const void* addr, float* out) {
    __try {
        const float* f = static_cast<const float*>(addr);
        out[0] = f[0];
        out[1] = f[1];
        out[2] = f[2];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Posizione "degli occhi" del Pawn del giocatore, da APawn::GetPawnViewLocation
// (virtuale a vtable +0x404, FVector* (thiscall)(FVector* out), chiamata da
// execGetPawnViewLocation @ 0x00BBD730). Aggiornata nel Draw (thread di
// gioco, fuori dalla VM di UnrealScript), usata da ApplyFirstPerson.
float g_pawnViewLoc[3] = {};
float g_pawnLocAtView[3] = {};
std::atomic<bool> g_pawnViewLocValid{false};

typedef float*(__thiscall* GetPawnViewLocation_t)(void* pawn, float* out);

bool CallGetPawnViewLocation(void* pawn, float* out) {
    __try {
        void** vtable = *reinterpret_cast<void***>(pawn);
        auto fn = reinterpret_cast<GetPawnViewLocation_t>(vtable[0x404 / 4]);
        float tmp[3];
        float* r = fn(pawn, tmp);
        out[0] = r[0];
        out[1] = r[1];
        out[2] = r[2];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Pawn del giocatore se e' anche il ViewTarget (= gameplay, non cinematica).
void* PlayerPawnIfViewTarget(char* cam) {
    uint32_t pawnPtr = 0, viewTarget = 0;
    if (!TrySafeReadUint32(cam - 0x77C + 0x1F8, &pawnPtr) || !pawnPtr) return nullptr;
    if (!TrySafeReadUint32(cam - 0x77C + 0x3B4, &viewTarget) || viewTarget != pawnPtr) return nullptr;
    return reinterpret_cast<void*>(pawnPtr);
}

void UpdatePawnViewLocation(char* cam) {
    void* pawn = PlayerPawnIfViewTarget(cam);
    float v[3], p[3];
    if (pawn && CallGetPawnViewLocation(pawn, v) &&
        TrySafeReadFloat3(static_cast<char*>(pawn) + kOffsetLocationX, p)) {
        std::memcpy(g_pawnViewLoc, v, sizeof(v));
        std::memcpy(g_pawnLocAtView, p, sizeof(p));
        g_pawnViewLocValid = true;
    } else {
        g_pawnViewLocValid = false;
    }
}

// --- Piano di clipping vicino ----------------------------------------------
//
// Globale GNearClippingPlane @ 0x0141578C (10 unita' nel gioco; usato da
// CalcSceneView a 0x0075CB55 per la proiezione). In prima persona, mirando,
// Walker si sporge in avanti e la sua testa finisce davanti alla camera:
// alzando il piano a ~18-22 unita' la testa viene tagliata e restano visibili
// fucile e mani (verificato con screenshot il 16/10; gli impatti cadono sul
// mirino anche con la testa girata). Rischio: muri molto vicini (coperture).
// Solo in prima persona durante il gameplay; altrimenti valore originale.
constexpr uintptr_t kNearClipPlaneAddr = 0x0141578C;

void ApplyNearPlane(char* cam) {
    static float original = -1.0f;
    float* nearPlane = reinterpret_cast<float*>(kNearClipPlaneAddr);
    if (original < 0.0f) {
        float v = *nearPlane;
        if (!(v > 0.5f && v < 100.0f)) return;  // valore inatteso: non tocchiamo
        original = v;
    }
    bool fp = vr::control::g_firstPerson && HasCameraSignature(cam) && PlayerPawnIfViewTarget(cam);
    *nearPlane = fp ? vr::control::g_fpNearPlane.load() : original;
}

// Prima persona (esperimento): sostituisce la posizione appena copiata nella
// camera con quella del Pawn (+0x54, confermata in Fase 2) + altezza occhi,
// spostata in avanti lungo lo yaw della camera.
bool PawnOnFirmGround(void* pawn);

void ApplyFirstPerson(char* cam) {
    if (!vr::control::g_firstPerson) return;
    // Pawn del giocatore: PlayerController.Pawn (+0x1F8); la camera e' a
    // PC+0x77C. (g_pawnPtr dall'hook di execGetPawnViewLocation e' spesso un
    // compagno di squadra o un nemico.)
    uint32_t pawnPtr = 0, viewTarget = 0;
    if (!TrySafeReadUint32(cam - 0x77C + 0x1F8, &pawnPtr) || !pawnPtr) return;
    // Cinematiche: il ViewTarget del PC (+0x3B4) e' un attore camera invece
    // del Pawn (verificato il 16/10): lasciamo la camera del gioco.
    if (!TrySafeReadUint32(cam - 0x77C + 0x3B4, &viewTarget) || viewTarget != pawnPtr) return;
    void* pawn = reinterpret_cast<void*>(pawnPtr);
    float p[3];
    if (!pawn || !TrySafeReadFloat3(static_cast<char*>(pawn) + kOffsetLocationX, p)) return;
    float* loc = reinterpret_cast<float*>(cam + 0x10);
    // Avanti lungo lo yaw del CORPO (Pawn.Rotation +0x64). La rotazione nella
    // camera, al momento di questa copia, poteva essere ancora quella
    // dell'arma messa dalla mod (mira con la canna): mirando di lato in
    // copertura gli occhi saltavano di ~19 unita' da un frame all'altro
    // (18/10, "le coperture fanno vibrare la camera").
    int32_t pawnYaw = 0;
    float yaw = TrySafeReadUint32(static_cast<char*>(pawn) + 0x64, reinterpret_cast<uint32_t*>(&pawnYaw))
                    ? pawnYaw * (6.28318530718f / 65536.0f)
                    : *reinterpret_cast<int32_t*>(cam + kCameraOffsetYaw) * (6.28318530718f / 65536.0f);
    // Su un mezzo (elicottero, camion, torretta a bordo) niente spostamento in
    // avanti: con 15 unita' davanti al Pawn la vista finiva fuori dalla porta
    // dell'elicottero (19/10, nel visore, anche nella 0.9.6).
    float fwd = PawnOnFirmGround(pawn) ? vr::control::g_fpForward.load() : 0.0f;
    loc[0] = p[0] + fwd * std::cos(yaw);
    loc[1] = p[1] + fwd * std::sin(yaw);
    // "height auto": occhi all'altezza della testa vera (in piedi, misurata da
    // SteamVR sul pavimento) sopra i piedi di Walker; prima sempre 40 unita'
    // sopra il centro del Pawn, ~20 cm sopra gli occhi di Walker e uguale per
    // giocatori alti e bassi (audit 19/10). Seduti: altezza fissa (fp).
    // La camera logica del gioco (visibilita' degli ordini alla squadra,
    // bersagli) resta all'altezza fissa fp: con "height auto" piu' bassa il
    // cecchino dietro il parapetto non risultava visibile e l'ordine veniva
    // rifiutato (19/10, nel visore). La differenza la aggiunge solo la vista
    // (GetPlayerViewPoint, ViewHeightExtra).
    loc[2] = p[2] + vr::control::g_fpEyeHeight;
}

// Thread di gioco, subito dopo che la VM di UnrealScript ha copiato la
// posizione nella camera: sposta la vista di +-meta' IPD lungo il vettore
// "right" della camera, alternando l'occhio ad ogni frame.
// Occhio del Draw in corso (stereo sincrono, thread di gioco), -1 = fuori.
std::atomic<int> g_drawEye{-1};

// Posa della testa pubblicata in blocco da UpdateHeadTracking (thread di
// rendering) e letta in blocco dal thread di gioco: rotazioni, spostamento e
// matrice del visore dello stesso campionamento.
struct PublishedHead {
    bool valid = false;
    int32_t yaw = 0, pitch = 0, roll = 0;
    float offset[3] = {0, 0, 0};
    vr::HmdMatrix34_t pose = {};
    double sampleMs = 0.0;
};
std::mutex g_publishedHeadMutex;
PublishedHead g_publishedHead;

void PublishHeadPose(const vr::HmdMatrix34_t* m) {
    std::lock_guard<std::mutex> lock(g_publishedHeadMutex);
    g_publishedHead.valid = true;
    g_publishedHead.yaw = g_deltaYaw.load();
    g_publishedHead.pitch = g_deltaPitch.load();
    g_publishedHead.roll = g_deltaRoll.load();
    for (int i = 0; i < 3; ++i) g_publishedHead.offset[i] = g_headOffset[i].load();
    g_publishedHead.pose = *m;
    g_publishedHead.sampleMs = ProfMs();
}

// Testa congelata per il tick (thread di gioco, prima dei due Draw): la posa
// viene aggiornata sul thread di rendering (BeginScene) e poteva cambiare tra
// l'occhio sinistro e il destro dello stesso tick.
struct HeadSnapshot {
    bool valid = false;
    int32_t yaw = 0, pitch = 0, roll = 0;
    float offset[3] = {0, 0, 0};
    bool poseValid = false;
    vr::HmdMatrix34_t pose = {};
    double sampleMs = 0.0;
};
HeadSnapshot g_headSnap;
int32_t HeadYaw() { return g_headSnap.valid ? g_headSnap.yaw : g_deltaYaw.load(std::memory_order_relaxed); }
int32_t HeadPitch() { return g_headSnap.valid ? g_headSnap.pitch : g_deltaPitch.load(std::memory_order_relaxed); }
int32_t HeadRoll() { return g_headSnap.valid ? g_headSnap.roll : g_deltaRoll.load(std::memory_order_relaxed); }
void HeadOffset(float* out) {
    for (int i = 0; i < 3; ++i) out[i] = g_headSnap.valid ? g_headSnap.offset[i] : g_headOffset[i].load();
}

void RecordEyeOrigin(const float* loc, int eye) {
    uint32_t slot = g_eyeRingPos.fetch_add(1) % kEyeRingSize;
    g_eyeRing[slot] = {loc[0], loc[1], loc[2], eye, g_tickHmdPoseValid, g_tickHmdPose,
                       g_stereoDraws.load(std::memory_order_relaxed), true};
}

// Sposta loc (gia' nella camera) di +-meta' IPD lungo il vettore "right".
// Altezza della vista rispetto alla camera logica del gioco: "height auto"
// (testa vera) meno l'altezza fissa fp.
float ViewHeightExtra() {
    if (vr::control::g_heightAuto && !vr::control::g_seated && vr::control::g_eyeHeightAuto >= 0.0f)
        return vr::control::g_eyeHeightAuto - vr::control::g_fpEyeHeight;
    return 0.0f;
}

// Walker vivo (Pawn.Health +0x304 > 0). Morto il corpo diventa un ragdoll
// fisico: spostare il Pawn, la mesh o le ossa a quel punto ha preceduto un
// crash del thread di rendering e di PhysX alla morte (19/10, nel visore).
// Walker a piedi su terreno fermo (non su un mezzo): Physics == PHYS_Walking
// (+0x94 = 1), nessun veicolo guidato (+0x3B8), non alla torretta, e l'attore
// su cui poggia (Base, +0xAC) non si muove. Prima si controllava solo che la
// Base fosse il livello, ma nei livelli quasi tutti i pavimenti sono
// StaticMeshActor: a piedi risultava spesso "su un mezzo" (19/10). Sui mezzi
// la vista e il corpo restano dove li mette il gioco (elicottero: fuori dalla
// porta, dal tetto, sopra o sotto la cabina).
extern std::atomic<bool> g_onTurret;
bool PawnOnFirmGround(void* pawn) {
    if (!pawn) return false;
    char* p = static_cast<char*>(pawn);
    uint32_t phys = 0, base = 0, veh = 0;
    if (g_onTurret) return false;
    if (!TrySafeReadUint32(p + 0x94, &phys) || (phys & 0xFF) != 1) return false;
    if (TrySafeReadUint32(p + 0x3B8, &veh) && veh) return false;
    if (!TrySafeReadUint32(p + 0xAC, &base)) return false;
    // Base in movimento nell'ultimo secondo = mezzo.
    static uint32_t lastBase = 0;
    static float lastLoc[3] = {0, 0, 0};
    static ULONGLONG lastMove = 0;
    float loc[3];
    ULONGLONG now = GetTickCount64();
    if (base && SafeCopy(loc, reinterpret_cast<char*>(base) + 0x54, 12)) {
        if (base == lastBase) {
            float d = std::fabs(loc[0] - lastLoc[0]) + std::fabs(loc[1] - lastLoc[1]) + std::fabs(loc[2] - lastLoc[2]);
            if (d > 0.5f) lastMove = now;
        } else {
            lastMove = 0;
        }
        lastBase = base;
        std::memcpy(lastLoc, loc, sizeof(loc));
    } else {
        lastBase = 0;
        lastMove = 0;
    }
    return !(lastMove && now - lastMove < 1000);
}

bool PawnAlive(void* pawn) {
    uint32_t hp = 0;
    return pawn && TrySafeReadUint32(static_cast<char*>(pawn) + 0x304, &hp) && static_cast<int32_t>(hp) > 0;
}

// Ultimo spostamento d'occhio applicato (mondo), per togliere/rimettere
// l'occhio intorno allo smorzamento dell'altezza e alla collisione della testa.
float g_eyeOffsetApplied[2][3] = {};

void OffsetLocationForEye(char* cam, int eye, float half) {
    float* loc = reinterpret_cast<float*>(cam + 0x10);
    // UE3: X avanti, Y destra, Z su; yaw in unita' Rotator. Gli occhi stanno
    // sull'asse della TESTA: se la rotazione della testa non e' nella camera
    // (aim body) la aggiungiamo qui.
    int32_t yawUnits = *reinterpret_cast<int32_t*>(cam + kCameraOffsetYaw);
    int32_t pitchUnits = *reinterpret_cast<int32_t*>(cam + kCameraOffsetPitch);
    if (vr::control::g_aimHand && vr::control::g_aimFollowsBody && g_aimValid) {
        yawUnits = g_bodyYaw.load();
        pitchUnits = g_bodyPitch.load();
    }
    if (vr::control::g_aimFollowsBody) {
        yawUnits += HeadYaw();
        pitchUnits += HeadPitch();
    }
    const float k = 6.28318530718f / 65536.0f;
    float sy = std::sin(yawUnits * k), cy = std::cos(yawUnits * k);
    float sp = std::sin(pitchUnits * k), cp = std::cos(pitchUnits * k);
    float sr = std::sin(HeadRoll() * k), cr = std::cos(HeadRoll() * k);
    float sign = eye == 0 ? -1.0f : 1.0f;
    // Asse Y (destra) di FRotationMatrix(pitch, yaw, roll) di UE3: con testa
    // inclinata e sguardo in alto/basso gli occhi restano sull'asse giusto.
    float off[3] = {sign * half * (sr * sp * cy - cr * sy), sign * half * (sr * sp * sy + cr * cy), sign * half * (-sr * cp)};
    for (int i = 0; i < 3; ++i) loc[i] += off[i];
    if (eye == 0 || eye == 1) std::memcpy(g_eyeOffsetApplied[eye], off, sizeof(off));
    RecordEyeOrigin(loc, eye);
}

void ApplyEyeOffset(char* cam) {
    g_locCopies.fetch_add(1, std::memory_order_relaxed);
    float half = vr::control::g_stereoHalfIpd;
    if (half <= 0) return;
    if (vr::control::g_syncStereo) {
        // Stereo sincrono: lo spostamento si applica solo dentro il Draw
        // (se il gioco ricopia la posizione durante il disegno la
        // risistemiamo); durante il tick la camera resta al centro.
        int drawEye = g_drawEye;
        if (drawEye >= 0) OffsetLocationForEye(cam, drawEye, half);
        return;
    }
    // La posizione viene copiata DUE volte per tick (verificato il 16/10: con
    // l'alternanza per copia vinceva sempre l'occhio destro). Cambiamo occhio
    // solo all'inizio di un nuovo tick, riconosciuto dal tempo: le copie dello
    // stesso tick distano microsecondi, tick diversi ~10+ ms.
    static int eye = 0;  // 0 = sinistro, 1 = destro
    static LARGE_INTEGER lastCopy = {};
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    if ((now.QuadPart - lastCopy.QuadPart) * 1000 > freq.QuadPart * 3) eye ^= 1;
    lastCopy = now;
    OffsetLocationForEye(cam, eye, half);
}

// --- Stereo sincrono: FViewport::Draw chiamato due volte per tick ---------
//
// 0x008F6A80 = FViewport::Draw(UBOOL bShouldPresent) (thiscall, ret 4):
// contiene i costruttori di BeginDrawingCommand ed EndDrawingCommand
// (trovati dalle loro stringhe DescribeCommand -> vtable -> costruttore).
// Lo chiamiamo una volta per occhio con la camera spostata; ogni chiamata
// produce un Present sul thread di rendering (vedi Hooked_Present).
constexpr uintptr_t kViewportDrawAddr = 0x008F6A80;
typedef void(__thiscall* ViewportDraw_t)(void* self, int bShouldPresent);
ViewportDraw_t g_ViewportDraw_Original = nullptr;
std::atomic<uint32_t> g_stereoDraws{0};
// Profilo (ms accumulati): Draw occhio 0, Draw occhio 1, tick intero (tra due
// Draw consecutivi = logica di gioco + attese).
double g_profDraw[2] = {};
double g_profTick = 0.0;
double g_profOutput = 0.0;   // vr::output::OnPresent (copia + attesa + invio)
double g_profPresent = 0.0;  // Present originale del gioco (monitor)
uint32_t g_profCount = 0;
double ProfMs() {
    static LARGE_INTEGER f = {};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return 1000.0 * t.QuadPart / f.QuadPart;
}

void UpdateFirstPersonBody(void* pawn);
void UpdateSquadCommandAim(char* cam, void* pawn);
void VisibilityCheck(void* pawn);
// Ultima vista dell'occhio sinistro (posizione e rotazione, per VisibilityCheck).
float g_lastViewLoc[3] = {0, 0, 0};
int32_t g_lastViewRot[3] = {0, 0, 0};
float g_lastViewBase[3] = {0, 0, 0};
void UpdateProjectileLog();
void RedirectNewProjectiles(void* pawn);
void RedirectThrownGrenade(void* pawn);

// Una riga per i test automatici (tools/regress.py), ogni 300 tick stereo:
// differenze dall'ultima riga. ripetuti = Present consecutivi dello stesso
// occhio (immagini doppie), sconosciuti = frame senza occhio riconosciuto.
void LogStats() {
    static uint32_t last[13] = {};
    uint32_t now[13] = {g_presentByEye[0].load(), g_presentByEye[1].load(), g_presentByEye[2].load(),
                        g_presentRepeats.load(), g_eyeMisses.load(), g_headBlocked.load(), g_cinemaSwitches.load(),
                        g_frameTagDisagree.load(), g_frameTagResync.load(), g_frameTagEmpty.load(),
                        g_frameTagStale.load(), g_markerFromTag.load(), g_markerFromGlobals.load()};
    // FOV della scena principale (colonne di c0-c3: x/w, y/w) contro quello chiesto.
    const float* mv = g_mainViewProj;
    auto n3 = [](float a, float b2, float c) { return std::sqrt(a * a + b2 * b2 + c * c); };
    float cw = n3(mv[3], mv[7], mv[11]);
    float hfov = cw > 0 ? 2.0f * std::atan(cw / n3(mv[0], mv[4], mv[8])) * 57.2957795f : 0.0f;
    float vfov = cw > 0 ? 2.0f * std::atan(cw / n3(mv[1], mv[5], mv[9])) * 57.2957795f : 0.0f;
    float want = vr::control::g_fovOverride > 0 ? vr::control::g_fovOverride.load() : 0.0f;
    char b[520];
    snprintf(b, sizeof(b),
             "STATS: present L=%u R=%u unknown=%u repeated=%u | eye misses=%u | head blocked=%u | cinema switches=%u | "
             "max view jump=%.1f | cinema=%d | frame tags disagree=%u resync=%u empty=%u | view fov %.1fx%.1f (wanted %.1f) | stale tags %u | marker from tag %u, from globals %u",
             now[1] - last[1], now[2] - last[2], now[0] - last[0], now[3] - last[3], now[4] - last[4], now[5] - last[5],
             now[6] - last[6], g_maxViewJump, (int)vr::control::g_cinemaActive.load(), now[7] - last[7],
             now[8] - last[8], now[9] - last[9], hfov, vfov, want, now[10] - last[10], now[11] - last[11], now[12] - last[12]);
    vr::log::Line(b);
    for (int i = 0; i < 13; ++i) last[i] = now[i];
    g_maxViewJump = 0.0f;
    {
        double sum, mx;
        int n;
        {
            std::lock_guard<std::mutex> lock(g_gpuStatsMutex);
            sum = g_gpuSum;
            mx = g_gpuMax;
            n = g_gpuCount;
            g_gpuSum = g_gpuMax = 0.0;
            g_gpuCount = 0;
        }
        float hz = vr::output::HmdRefreshHz();
        double ls = 0.0, lm = 0.0;
        int ln = 0;
        {
            std::lock_guard<std::mutex> lock(g_tagLatencyMutex);
            ls = g_tagLatencySum;
            lm = g_tagLatencyMax;
            ln = g_tagLatencyCount;
            g_tagLatencySum = g_tagLatencyMax = 0.0;
            g_tagLatencyCount = 0;
        }
        std::snprintf(b, sizeof(b),
                      "GPU: %.2f ms per tick (max %.2f, %d ticks) | headset frame %.2f ms | scene %d%% | draw to present "
                      "%.2f ms (max %.2f)",
                      n ? sum / n : 0.0, mx, n, hz > 0 ? 1000.0f / hz : 0.0f, g_screenPercent.load(),
                      ln ? ls / ln : 0.0, lm);
        vr::log::Line(b);
    }
    EyeDrawStats e;
    {
        std::lock_guard<std::mutex> lock(g_eyeDrawMutex);
        e = g_eyeDraws;
        g_eyeDraws = EyeDrawStats();
    }
    if (e.pairs) {
        std::snprintf(b, sizeof(b), "EYEDRAWS: pairs=%llu draws L=%.0f R=%.0f | triangles differ %.2f%% | pairs over 2%%=%llu | "
                      "draw jitter L=%.2f R=%.2f",
                      e.pairs, double(e.drawsL) / e.pairs, double(e.drawsR) / e.pairs, 100.0 * e.triDiff / e.pairs,
                      e.mismatch2, double(e.jitter[0]) / e.pairs, double(e.jitter[1]) / e.pairs);
        vr::log::Line(b);
    }
}

// --- Stato della vista per occhio -------------------------------------------
//
// ULocalPlayer.ViewState (+0x88, FSceneViewStateInterface*) contiene lo
// storico dell'occlusione (query dei frame precedenti), le dissolvenze dei LOD
// e le matrici del frame precedente. Disegnando due occhi per tick con lo
// stesso stato, i risultati di un occhio decidono cosa disegna l'altro:
// oggetti che appaiono e spariscono tra i due occhi (sfarfallio) e nemici
// "occlusi" per un attimo sporgendosi da un riparo. L'occhio destro usa un
// secondo stato creato con AllocateViewState (0x00C4F370: new FSceneViewState,
// 0x2F0 byte, lo stesso che usa il motore). Se il motore ricrea il suo (reset
// del device, cambio di risoluzione) si ricrea anche il nostro.
constexpr uintptr_t kAllocateViewStateAddr = 0x00C4F370;
constexpr uint32_t kOffPcPlayer = 0x388, kOffLpViewState = 0x88;
typedef void*(__cdecl* AllocateViewState_t)();

void* CallAllocateViewState() {
    __try {
        return reinterpret_cast<AllocateViewState_t>(kAllocateViewStateAddr)();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Indirizzo di LocalPlayer.ViewState del giocatore (nullptr se non valido).
uint32_t* ViewStateSlot(char* cam) {
    uint32_t lp = 0, vs = 0;
    if (!HasCameraSignature(cam) || !TrySafeReadUint32(cam - 0x77C + kOffPcPlayer, &lp) || !lp) return nullptr;
    if (!TrySafeReadUint32(reinterpret_cast<char*>(lp) + kOffLpViewState, &vs) || !vs) return nullptr;
    return reinterpret_cast<uint32_t*>(lp + kOffLpViewState);
}

// Stato da usare per l'occhio destro (0 = condiviso, come il gioco).
uint32_t RightEyeViewState(uint32_t engineState) {
    static uint32_t owner = 0, mine = 0;
    static bool failed = false;
    if (!vr::control::g_perEyeViewState || failed) return 0;
    if (engineState != owner) {
        owner = engineState;
        mine = reinterpret_cast<uint32_t>(CallAllocateViewState());
        if (!mine) failed = true;
        vr::log::Line(mine ? "STEREO: separate view state for the right eye (occlusion, LOD fades)."
                           : "STEREO: AllocateViewState failed, both eyes share the view state.");
    }
    return mine;
}

// --- Risoluzione dinamica ----------------------------------------------------
//
// Obiettivo: tenere il tempo di GPU per tick sotto ~85% del frame del visore
// (13,9 ms a 72 Hz) invece di cadere a meta' frequenza (riproiezione). Il
// lavoro di pixel scala col quadrato della percentuale: passo proporzionale
// sqrt(obiettivo / misurato), al massimo -10 / +5 per volta, ogni 0,7 s, a
// multipli di 5. Fuori dal gioco in 3D (schermo virtuale, menu) sempre 100:
// lo schermo virtuale mostra il back buffer intero.
void ApplyVrPreset(char* cam) {
    static bool applied = false;
    if (applied || !vr::control::g_vrPreset || !HasCameraSignature(cam)) return;
    applied = true;
    // Misure del 18/10 (tempo GPU per tick, A/B): SSAO -15%, distorsione
    // filtrata -17%; ombre, anisotropia, bloom, LOD sotto il 5%: restano.
    for (const char* c : {"scale set AmbientOcclusion false", "scale set FilteredDistortion false"})
        vr::script::ExecConsole(cam - 0x77C, c);
    // OneFrameThreadLag (il thread di gioco un frame avanti a quello di
    // rendering) resta come nel gioco: spento toglie ~10 ms di latenza ma mette
    // in serie gioco e rendering; nel combattimento del capitolo 1 (18/10, nel
    // visore) 48-53 fps contro 75-86 acceso. "threadlag off" per provarlo.
    if (!vr::control::g_threadLag) vr::script::ExecConsole(cam - 0x77C, "scale set OneFrameThreadLag false");
    vr::log::Line(std::string("VRPRESET: screen-space ambient occlusion and filtered distortion off") +
                  (vr::control::g_threadLag ? "." : ", no one-frame thread lag."));
}

// Risoluzione automatica: con la proiezione decentrata il back buffer e'
// l'occhio, quindi la sua dimensione giusta e' quella che SteamVR consiglia
// per un occhio (GetRecommendedRenderTargetSize, gia' moltiplicata per il
// cursore della risoluzione di SteamVR). Prima la finestra era fissa
// (ResX/ResY a mano): su visori ad alta risoluzione (Dream Air, Pimax) troppo
// piccola o, se impostata grande, troppo pesante. Controllo ogni 5 s; il
// cambio (setres, con reset del device) solo se la differenza supera il 4%.
// Massimo 4096 (desktop finto di display_spoof).
void UpdateAutoResolution(char* cam) {
    static ULONGLONG last = 0;
    if (!vr::control::g_autoResolution || !vr::control::g_offCenter || GetTickCount64() - last < 5000) return;
    last = GetTickCount64();
    uint32_t w = 0, h = 0;
    {
        vr::session::Shared vrLock(true);
        if (!vrLock.ok() || !vr::session::IsScene() || !vr::VRSystem()) return;
        vr::VRSystem()->GetRecommendedRenderTargetSize(&w, &h);
    }
    if (w < 640 || h < 640) return;
    w = (std::min)(w, 4096u) & ~7u;
    h = (std::min)(h, 4096u) & ~7u;
    UINT bw = g_backBufferW, bh = g_backBufferH;
    if (!bw || !bh) return;
    if (std::abs(static_cast<int>(w) - static_cast<int>(bw)) <= static_cast<int>(bw) / 25 &&
        std::abs(static_cast<int>(h) - static_cast<int>(bh)) <= static_cast<int>(bh) / 25)
        return;
    static uint32_t lastW = 0, lastH = 0;
    if (w == lastW && h == lastH) return;  // gia' chiesta: il gioco puo' averla limitata
    lastW = w;
    lastH = h;
    char cmd[64];
    std::snprintf(cmd, sizeof(cmd), "setres %ux%uw", w, h);
    vr::log::Line("AUTORES: SteamVR recommends " + std::to_string(w) + "x" + std::to_string(h) + " per eye, game is " +
                  std::to_string(bw) + "x" + std::to_string(bh) + ": " + cmd);
    vr::script::ExecConsole(cam - 0x77C, cmd);
}

void UpdateDynamicResolution(char* cam, bool mono) {
    static ULONGLONG last = 0;
    if (!mono) ApplyVrPreset(cam);
    if (!mono && HasCameraSignature(cam)) UpdateAutoResolution(cam);
    int cur = g_screenPercent;
    int want = cur;
    float hz = vr::output::HmdRefreshHz();
    float fakeBudget = vr::control::g_dynResBudgetMs;
    if (fakeBudget > 0.0f) hz = 1000.0f / fakeBudget;
    bool active = vr::control::g_dynRes && !mono && hz > 30.0f && (vr::session::IsScene() || fakeBudget > 0.0f) &&
                  HasCameraSignature(cam);
    if (vr::control::g_dynResTestFrames > 0 && HasCameraSignature(cam) && !mono) {
        static ULONGLONG flip = 0;
        if (GetTickCount64() - flip > 500) {
            flip = GetTickCount64();
            want = cur == 100 ? 70 : 100;
        }
    } else if (!active) {
        want = 100;
    } else if (GetTickCount64() - last > 700) {
        last = GetTickCount64();
        float budget = 1000.0f / hz, gpu = g_gpuTickMsEma;
        if (gpu > 0.0f) {
            float target = 0.85f * budget;
            if (gpu > 0.92f * budget || (gpu < 0.70f * budget && cur < 100)) {
                float next = cur * std::sqrt(target / gpu);
                next = (std::max)(next, cur - 10.0f);
                next = (std::min)(next, cur + 5.0f);
                want = static_cast<int>(next / 5.0f + 0.5f) * 5;
                want = (std::max)(want, vr::control::g_dynResMin.load());
                want = (std::min)(want, 100);
            }
        }
    }
    {
        // Invio nello stesso tick: solo con la GPU sotto meta' del frame del
        // visore; si aspetta al massimo un quarto di frame.
        float gpu = g_gpuTickMsEma, frame = hz > 30.0f ? 1000.0f / hz : 0.0f;
        bool ok = vr::control::g_lowLatency && frame > 0.0f && gpu > 0.0f && gpu < 0.5f * frame;
        vr::output::SetSameTickWait(ok ? 0.25f * frame : 0.0f);
    }
    if (want == cur || !HasCameraSignature(cam)) return;
    vr::script::ExecConsole(cam - 0x77C, "scale set ScreenPercentage " + std::to_string(want));
    g_screenPercent = want;
    char b[160];
    std::snprintf(b, sizeof(b), "DYNRES: scene %d%% -> %d%% (GPU %.1f ms per tick, headset frame %.1f ms)", cur, want,
                  g_gpuTickMsEma.load(), hz > 0 ? 1000.0f / hz : 0.0f);
    vr::log::Line(b);
}

class ViewportDrawThunk {
public:
    void Detour(int bShouldPresent) {
        void* self = this;
        if (vr::control::g_headSampleOnDraw) UpdateHeadTracking();
        char* cam = static_cast<char*>(g_cameraPtr.load());
        vr::script::Tick(HasCameraSignature(cam) ? cam - 0x77C : nullptr,
                         HasCameraSignature(cam) ? PlayerPawnIfViewTarget(cam) : nullptr);
        UpdateFirstPersonBody(vr::control::g_fpBody && HasCameraSignature(cam) ? PlayerPawnIfViewTarget(cam) : nullptr);
        UpdateProjectileLog();
        RedirectNewProjectiles(HasCameraSignature(cam) ? PlayerPawnIfViewTarget(cam) : nullptr);
        RedirectThrownGrenade(HasCameraSignature(cam) ? PlayerPawnIfViewTarget(cam) : nullptr);
        UpdateSquadCommandAim(cam, HasCameraSignature(cam) ? PlayerPawnIfViewTarget(cam) : nullptr);
        VisibilityCheck(HasCameraSignature(cam) ? PlayerPawnIfViewTarget(cam) : nullptr);
        // Mirino del gioco: al centro dello schermo segue la testa e non l'arma;
        // con la mira dalla canna lo sostituisce il mirino verde.
        // YHUD.m_hideCrosshair (+0x480, bit 0x40) sull'HUD del PlayerController (+0x3EC).
        {
            static bool weHid = false;
            bool hide = HasCameraSignature(cam) && vr::control::g_aimHand && g_muzzleAimValid &&
                        !vr::control::g_gameCrosshair;
            uint32_t hud = 0, flags = 0;
            if ((hide || weHid) && HasCameraSignature(cam) && TrySafeReadUint32(cam - 0x77C + 0x3EC, &hud) && hud &&
                TrySafeReadUint32(reinterpret_cast<char*>(hud) + 0x480, &flags)) {
                uint32_t want = hide ? (flags | 0x40u) : (flags & ~0x40u);
                if (want != flags) SafeCopy(reinterpret_cast<char*>(hud) + 0x480, &want, 4);
                weHid = hide;
            }
        }
        {
            // Danno subito (giubbotto bHaptics / ProTube): salute di Walker in calo.
            static void* lastPawn = nullptr;
            static int32_t lastHealth = 0;
            void* pw = HasCameraSignature(cam) ? PlayerPawnIfViewTarget(cam) : nullptr;
            int32_t hp = 0;
            if (pw && TrySafeReadUint32(static_cast<char*>(pw) + 0x304, reinterpret_cast<uint32_t*>(&hp))) {
                if (pw == lastPawn && hp < lastHealth && hp >= 0) vr::haptics::Damage(lastHealth - hp, HitAngleFromHead(pw));
                lastPawn = pw;
                lastHealth = hp;
            }
        }
        if (vr::control::g_firstPerson && HasCameraSignature(cam)) UpdatePawnViewLocation(cam);
        ApplyNearPlane(cam);
        float half = vr::control::g_stereoHalfIpd;
        // Schermo virtuale: tutto cio' che non e' gameplay (la camera non
        // segue Walker) va mono sullo schermo, senza testa ne' 6DOF.
        // Anche le scene "nel motore" con la camera di Walker ma il giocatore
        // bloccato: PlayerController.bCinematicMode (+0x39C, bit 0x8000).
        bool cinematicMode = false;
        if (HasCameraSignature(cam)) {
            uint32_t pcFlags = 0;
            cinematicMode = TrySafeReadUint32(cam - 0x77C + 0x39C, &pcFlags) && (pcFlags & 0x8000);
        }
        bool noWalker = !(HasCameraSignature(cam) && PlayerPawnIfViewTarget(cam));
        // Modalita' cinematica con la camera ancora su Walker (camminate guidate,
        // porte, scene scriptate): resta in 3D, salvo un video a schermo intero
        // (YPlayerController.IsPlayingFullscreenMovie, controllato ogni 10 tick).
        bool movie = false;
        if (cinematicMode && !noWalker && vr::control::g_cinemaWalker3D) {
            static int n = 0;
            static bool lastMovie = true;
            if (++n % 10 == 1) {
                struct {
                    uint32_t ret;
                } mp = {0};
                lastMovie = !vr::script::CallFunction(cam - 0x77C, "IsPlayingFullscreenMovie", &mp) || mp.ret != 0;
            }
            movie = lastMovie;
        }
        bool lockedScene = cinematicMode && (noWalker || !vr::control::g_cinemaWalker3D || movie);
        g_gameLockedScene = cinematicMode;
        vr::control::g_gameLocked = cinematicMode;
        {
            // WorldInfo.Pauser (+0x46C) != None: menu di pausa (niente gesti).
            uint32_t wi = 0, pauser = 0;
            bool paused = HasCameraSignature(cam) && TrySafeReadUint32(cam - 0x77C + 0xEC, &wi) && wi &&
                          TrySafeReadUint32(reinterpret_cast<char*>(wi) + 0x46C, &pauser) && pauser;
            vr::control::g_gamePaused = paused;
        }
        bool wanted = vr::control::g_cinemaEnabled && (vr::control::g_cinemaForce || lockedScene || noWalker);
        // Camera del gioco lontana da Walker (morte, inquadrature): nero subito,
        // non 0,6 s di camera del gioco in 3D con la testa sopra; se torna su
        // Walker l'immagine riappare, se diventa schermo virtuale anche
        // (audit 19/10).
        // Solo alla morte (Walker senza vita): le inquadrature brevi delle scene
        // scriptate davano lampi neri continui (19/10).
        {
            uint32_t pawnPtr = 0;
            bool dead = HasCameraSignature(cam) && TrySafeReadUint32(cam - 0x77C + 0x1F8, &pawnPtr) && pawnPtr &&
                        !PawnAlive(reinterpret_cast<void*>(pawnPtr));
            vr::output::HoldBlack(vr::control::g_fades && vr::control::g_cinemaEnabled && noWalker && dead &&
                                  !vr::control::g_cinemaActive && !vr::control::g_cinemaForce);
        }
        // Isteresi: momenti brevi di modalita' cinematica durante il gioco
        // (porte, eventi scriptati) restano in VR. Passare allo schermo
        // virtuale per mezzo secondo e tornare indietro, nel visore, e' un
        // lampo (segnalazione 18/10: "flash of light ... during gameplay").
        static ULONGLONG wantedSince = 0, unwantedSince = 0;
        ULONGLONG nowMs = GetTickCount64();
        bool cinema = vr::control::g_cinemaActive.load();
        if (wanted) {
            unwantedSince = 0;
            if (!wantedSince) wantedSince = nowMs;
            // Camera lontana da Walker per meno di 0,6 s (inquadrature brevi da 0,1-0,5 s
            // nei log): resta in VR, niente lampo dello schermo virtuale.
            ULONGLONG need = vr::control::g_cinemaForce ? 0 : 600;
            if (nowMs - wantedSince >= need) cinema = true;
        } else {
            wantedSince = 0;
            if (!unwantedSince) unwantedSince = nowMs;
            if (nowMs - unwantedSince >= 150) cinema = false;
        }
        if (cinema != vr::control::g_cinemaActive.load()) {
            InvalidateEyeRing();
            // Coda delle etichette svuotata al cambio: durante i filmati il
            // gioco disegna senza presentare e restavano etichette vecchie
            // (1912 scartate in un filmato, 19/10). Un'etichetta in piu' rimasta
            // in coda (meno di 5: mai scartata) faceva prendere a ogni immagine
            // l'etichetta del disegno prima, occhi scambiati: mondo doppio
            // dopo i filmati.
            {
                std::lock_guard<std::mutex> lock(g_frameTagMutex);
                g_frameTags.clear();
            }
            g_cinemaSwitches.fetch_add(1, std::memory_order_relaxed);
            vr::log::Line(cinema ? "CINEMA: virtual screen (outside gameplay)" : "CINEMA: end, back to VR");
            vr::control::g_cinemaActive = cinema;
            if (vr::control::g_fades) vr::output::Blink(0.3f);
        }
        UpdateDynamicResolution(cam, cinema || half <= 0 || !vr::control::g_syncStereo);
        if (cinema || half <= 0 || !vr::control::g_syncStereo || !HasCameraSignature(cam)) {
            if (bShouldPresent) PushFrameTag(FrameTag{});
            g_ViewportDraw_Original(self, bShouldPresent);
            return;
        }
        float* loc = reinterpret_cast<float*>(cam + 0x10);
        float base[3] = {loc[0], loc[1], loc[2]};
        {
            std::lock_guard<std::mutex> lock(g_publishedHeadMutex);
            if (g_publishedHead.valid && vr::control::g_mode == vr::control::kModeHmd) {
                g_headSnap.yaw = g_publishedHead.yaw;
                g_headSnap.pitch = g_publishedHead.pitch;
                g_headSnap.roll = g_publishedHead.roll;
                for (int i = 0; i < 3; ++i) g_headSnap.offset[i] = g_publishedHead.offset[i];
                g_headSnap.pose = g_publishedHead.pose;
                g_headSnap.sampleMs = g_publishedHead.sampleMs;
                g_headSnap.poseValid = true;
            } else {
                g_headSnap.yaw = g_deltaYaw.load();
                g_headSnap.pitch = g_deltaPitch.load();
                g_headSnap.roll = g_deltaRoll.load();
                for (int i = 0; i < 3; ++i) g_headSnap.offset[i] = g_headOffset[i].load();
                g_headSnap.poseValid = false;
                g_headSnap.sampleMs = 0.0;
            }
        }
        g_headSnap.valid = true;
        uint32_t* vsSlot = ViewStateSlot(cam);
        uint32_t vsEngine = vsSlot ? *vsSlot : 0;
        uint32_t vsRight = vsSlot ? RightEyeViewState(vsEngine) : 0;
        for (int eye = 0; eye < 2; ++eye) {
            loc[0] = base[0];
            loc[1] = base[1];
            loc[2] = base[2];
            g_drawEye = eye;
            OffsetLocationForEye(cam, eye, half);
            double t0 = ProfMs();
            g_drawOffCenter = vr::control::g_offCenter && !g_scopeActive && vr::output::OffCenterProjection(eye, &g_drawProj);
            if (bShouldPresent) {
                FrameTag tag;
                tag.eye = eye;
                tag.offCenter = g_drawOffCenter;
                tag.screenPercent = g_screenPercent;
                tag.tick = g_stereoDraws.load(std::memory_order_relaxed);
                tag.poseValid = g_headSnap.poseValid;
                tag.pose = g_headSnap.pose;
                tag.sampleMs = g_headSnap.sampleMs;
                PushFrameTag(tag);
            }
            if (eye == 1 && vsRight) *vsSlot = vsRight;
            g_ViewportDraw_Original(self, bShouldPresent);
            if (eye == 1 && vsRight) *vsSlot = vsEngine;
            g_profDraw[eye] += ProfMs() - t0;
        }
        {
            static double last = 0.0;
            // Scatti: tick oltre 20 ms (a 72-90 Hz = almeno un frame perso) e il
            // piu' lungo, che la media dei 300 tick nasconde.
            static int longTicks = 0;
            static double maxTick = 0.0;
            double now = ProfMs();
            // Oltre 1 s senza disegni stereo = schermo virtuale o caricamento,
            // non un tick: escluso (prima gonfiava la media dopo ogni cinematica).
            if (last > 0.0 && now - last < 1000.0) {
                double dt = now - last;
                g_profTick += dt;
                if (dt > 20.0) ++longTicks;
                if (dt > maxTick) maxTick = dt;
            }
            last = now;
            if (++g_profCount >= 300) {
                char b[300];
                std::snprintf(b, sizeof(b), "PROFILE: tick %.2f ms (%.1f fps stereo), Draw left eye %.2f ms, right %.2f ms, "
                              "VR output %.2f ms, monitor Present %.2f ms, rest %.2f ms",
                              g_profTick / g_profCount, 1000.0 * g_profCount / g_profTick, g_profDraw[0] / g_profCount,
                              g_profDraw[1] / g_profCount, g_profOutput / g_profCount, g_profPresent / g_profCount,
                              (g_profTick - g_profDraw[0] - g_profDraw[1]) / g_profCount);
                g_profOutput = g_profPresent = 0.0;
                vr::log::Line(b);
                std::snprintf(b, sizeof(b), "HITCHES: ticks over 20 ms=%d, longest %.1f ms", longTicks, maxTick);
                vr::log::Line(b);
                longTicks = 0;
                maxTick = 0.0;
                LogStats();
                g_profDraw[0] = g_profDraw[1] = g_profTick = 0.0;
                g_profCount = 0;
            }
        }
        g_drawEye = -1;
        g_drawOffCenter = false;
        g_headSnap.valid = false;
        if (HasCameraSignature(cam)) {
            loc[0] = base[0];
            loc[1] = base[1];
            loc[2] = base[2];
        }
        g_stereoDraws.fetch_add(1, std::memory_order_relaxed);
        vr::control::g_lastStereoDrawMs = GetTickCount64();
        // Di nuovo dopo il disegno: la scelta del gioco (centro dello schermo)
        // gira durante il frame e al rilascio del tasto deve valere la nostra.
        UpdateSquadCommandAim(cam, PlayerPawnIfViewTarget(cam));
    }
};

// --- FOV: APlayerController::GetFOVAngle() -------------------------------
//
// 0x006F1EA0: wrapper C++ dell'evento UnrealScript GetFOVAngle (thiscall,
// nessun argomento, float restituito in ST0). Lo usa ULocalPlayer::
// CalcSceneView (0x0075C460) per costruire la proiezione: FOV * PI/360 ->
// FPerspectiveMatrix. Le copie del FOV in memoria (PlayerController+0x3C0..,
// camera+0x7C0) NON hanno effetto (verificato il 16/10 misurando c0-c3).
constexpr uintptr_t kGetFovAngleAddr = 0x006F1EA0;
typedef float(__thiscall* GetFovAngle_t)(void* self);
GetFovAngle_t g_GetFovAngle_Original = nullptr;

constexpr uintptr_t kOffPawnCurrentWeaponFov = 0x1594;  // YGamePawn.m_currentWeapon


// FPerspectiveMatrix(HalfFOV, Width, Height, MinZ) (0x009EAE80, thiscall, il
// costruttore restituisce this). Si cambia solo la matrice costruita da
// ULocalPlayer::CalcSceneView (ritorno a 0x0075CAC5 o 0x0075CBBA) durante il
// Draw di un occhio: M[0][0], M[1][1] (scala) e M[2][0], M[2][1] (centro),
// riga z della convenzione a vettori riga di UE3.
constexpr uintptr_t kPerspectiveMatrixAddr = 0x009EAE80;
typedef void*(__thiscall* PerspectiveMatrix_t)(void* self, float halfFov, float w, float h, float minZ);
PerspectiveMatrix_t g_PerspectiveMatrix_Original = nullptr;

class PerspectiveThunk {
public:
    void* Detour(float halfFov, float w, float h, float minZ) {
        void* r = g_PerspectiveMatrix_Original(this, halfFov, w, h, minZ);
        uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
        if (g_drawOffCenter && (ret == 0x0075CAC5 || ret == 0x0075CBBA)) {
            float* m = static_cast<float*>(r);
            static int logged = 0;
            if (logged < 2) {
                ++logged;
                char b[320];
                std::snprintf(b, sizeof(b),
                              "OFFCENTER: game projection [%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | "
                              "%.3f %.3f %.3f %.3f] -> eye m00=%.3f m11=%.3f m20=%.3f m21=%.3f",
                              m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13],
                              m[14], m[15], g_drawProj.m00, g_drawProj.m11, g_drawProj.m20, g_drawProj.m21);
                vr::log::Line(b);
            }
            m[0] = g_drawProj.m00;
            m[5] = g_drawProj.m11;
            m[8] = g_drawProj.m20;
            m[9] = g_drawProj.m21;
        }
        return r;
    }
};

void* GetPerspectiveThunkAddress() {
    union {
        void* (PerspectiveThunk::*mfp)(float, float, float, float);
        void* fp;
    } converter;
    converter.mfp = &PerspectiveThunk::Detour;
    return converter.fp;
}

class FovThunk {
public:
    float Detour() {
        float fov = g_GetFovAngle_Original(this);
        g_scopeActive = false;
        if (vr::control::g_cinemaActive) return fov;  // schermo virtuale: inquadratura del gioco
        // Cannocchiale: arma con m_scopeClass (+0xB38) e zoom del gioco in
        // corso (FOV 16-20 gradi): si lascia lo zoom, altrimenti il mirino
        // telescopico sarebbe inutile in VR.
        if (vr::control::g_scopeZoom && fov > 1.0f && fov < 40.0f) {
            uint32_t pawn = 0, weapon = 0, scope = 0;
            char* pc = reinterpret_cast<char*>(this);
            if (TrySafeReadUint32(pc + 0x1F8, &pawn) && pawn &&
                TrySafeReadUint32(reinterpret_cast<char*>(pawn) + kOffPawnCurrentWeaponFov, &weapon) && weapon &&
                TrySafeReadUint32(reinterpret_cast<char*>(weapon) + 0xB38, &scope) && scope) {
                static bool logged = false;
                if (!logged) {
                    logged = true;
                    vr::log::Line("ZOOM: scope, game FOV " + std::to_string(fov));
                }
                g_scopeActive = true;
                return fov;
            }
        }
        float forced = vr::control::g_fovOverride;
        return forced > 0.0f ? forced : fov;
    }
};

void* GetFovThunkAddress() {
    union {
        float (FovThunk::*mfp)();
        void* fp;
    } converter;
    converter.mfp = &FovThunk::Detour;
    return converter.fp;
}

void* GetViewportDrawThunkAddress() {
    union {
        void (ViewportDrawThunk::*mfp)(int);
        void* fp;
    } converter;
    converter.mfp = &ViewportDrawThunk::Detour;
    return converter.fp;
}

inline bool MaybeApplyCameraOverride(void* dst, size_t n) {
    char* cam = static_cast<char*>(g_cameraPtr.load(std::memory_order_relaxed));
    if (!cam || vr::control::g_cinemaActive.load(std::memory_order_relaxed) ||
        vr::control::g_exiting.load(std::memory_order_relaxed))
        return false;
    char* d = static_cast<char*>(dst);
    bool coversLocation = d <= cam + 0x10 && d + n >= cam + 0x1C;
    if (coversLocation && HasCameraSignature(cam)) {
        ApplyFirstPerson(cam);
        ApplyEyeOffset(cam);
    }
    if (d > cam + kCameraOffsetPitch || d + n < cam + kCameraOffsetYaw + 4) return false;
    // La memoria della rotazione e' valida (appena scritta da memcpy), ma il
    // puntatore potrebbe essere vecchio e quella memoria ora appartenere a
    // un altro oggetto: ricontrolliamo la firma prima di toccarla.
    if (!HasCameraSignature(cam)) return false;
    // Rotazione del corpo come l'ha scritta il gioco (mouse/stick).
    int32_t* camYaw = reinterpret_cast<int32_t*>(cam + kCameraOffsetYaw);
    int32_t* camPitch = reinterpret_cast<int32_t*>(cam + kCameraOffsetPitch);
    // Corpo VR = rotazione del PlayerController (+0x60 Pitch, +0x64 Yaw), non
    // quella della camera del gioco: la camera aggiunge scosse (esplosioni,
    // colpi, rinculo dell'arma) e animazioni di camera che nel visore
    // ruotavano tutta la vista (audit 19/10: raffica = vista su di 10 gradi).
    // Il beccheggio del corpo vale 0 senza "stickpitch on": l'inclinazione
    // della vista e' solo quella della testa (anche per mani, occhi e mirino).
    {
        int32_t pcRot[2] = {0, 0};
        bool pcOk = SafeCopy(pcRot, cam - 0x77C + 0x60, 8);
        int32_t yaw = pcOk ? pcRot[1] : *camYaw;
        int32_t pitch = pcOk ? pcRot[0] : *camPitch;
        if (pitch > 32768) pitch -= 65536;
        g_bodyYaw = yaw;
        g_bodyPitch = vr::control::g_stickPitch ? pitch : 0;
        static ULONGLONG lastDiag = 0;
        int32_t dYaw = static_cast<int16_t>(static_cast<uint16_t>(*camYaw - yaw));
        if (pcOk && std::abs(dYaw) > 182 && GetTickCount64() - lastDiag > 2000) {
            lastDiag = GetTickCount64();
            char b[160];
            snprintf(b, sizeof(b), "BODY: game camera yaw differs from the controller by %.1f deg (shake/animation filtered)",
                     dYaw * (360.0f / 65536.0f));
            vr::log::Line(b);
        }
    }
    // "aim hand": la camera usata dalla logica di gioco (mira) punta dove
    // punta il controller destro; la vista viene ricostruita in
    // GetPlayerViewPoint come corpo + testa.
    if (vr::control::g_aimHand && vr::control::g_aimFollowsBody && g_aimValid && !TurretAimActive()) {
        if (g_muzzleAimValid) {
            // La canna del fucile visibile decide la mira (colpi e mirino
            // dove punta la canna, con una o due mani).
            *camYaw = g_muzzleAimYaw.load(std::memory_order_relaxed);
            *camPitch = g_muzzleAimPitch.load(std::memory_order_relaxed);
        } else {
            *camYaw += g_aimDeltaYaw.load(std::memory_order_relaxed);
            *camPitch += g_aimDeltaPitch.load(std::memory_order_relaxed);
        }
    }
    // "aim body" (predefinito): la testa NON entra nella camera usata dalla
    // logica di gioco (mira), viene aggiunta solo attorno a CalcSceneView.
    if (!vr::control::g_aimFollowsBody) {
        int32_t dy = g_deltaYaw.load(std::memory_order_relaxed);
        int32_t dp = g_deltaPitch.load(std::memory_order_relaxed);
        *reinterpret_cast<int32_t*>(cam + kCameraOffsetYaw) += dy;
        *reinterpret_cast<int32_t*>(cam + kCameraOffsetPitch) += dp;
    }
    g_overridesApplied.fetch_add(1, std::memory_order_relaxed);
    g_lastCopyOffset = static_cast<int>(d - cam);
    g_lastCopySize = static_cast<int>(n);
    const float* loc = reinterpret_cast<const float*>(cam + 0x10);
    g_camLocForVs[0] = loc[0];
    g_camLocForVs[1] = loc[1];
    g_camLocForVs[2] = loc[2];
    g_camLocValid = true;
    return true;
}

void* __cdecl Hooked_memcpy(void* dst, const void* src, size_t n) {
    void* r = g_memcpyReal(dst, src, n);
    if (MaybeApplyCameraOverride(dst, n)) {
        CaptureCopyStack(_ReturnAddress(), reinterpret_cast<void**>(_AddressOfReturnAddress()));
    }
    return r;
}

void* __cdecl Hooked_memmove(void* dst, const void* src, size_t n) {
    void* r = g_memmoveReal(dst, src, n);
    if (MaybeApplyCameraOverride(dst, n)) {
        CaptureCopyStack(_ReturnAddress(), reinterpret_cast<void**>(_AddressOfReturnAddress()));
    }
    return r;
}

// Sostituisce una voce della IAT solo se contiene davvero la funzione
// attesa (protezione contro un exe diverso da quello analizzato).
bool PatchIat(uintptr_t slot, const char* name, void* replacement, Memcpy_t* outOriginal) {
    HMODULE crt = GetModuleHandleA("MSVCR90.dll");
    void* expected = crt ? reinterpret_cast<void*>(GetProcAddress(crt, name)) : nullptr;
    void** entry = reinterpret_cast<void**>(slot);
    if (!expected || *entry != expected) {
        vr::log::Line(std::string("IAT: entry ") + name + " unexpected, hook NOT installed.");
        return false;
    }
    DWORD old;
    if (!VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *outOriginal = reinterpret_cast<Memcpy_t>(expected);
    InterlockedExchange(reinterpret_cast<volatile LONG*>(entry), reinterpret_cast<LONG>(replacement));
    VirtualProtect(entry, sizeof(void*), old, &old);
    vr::log::Line(std::string("IAT: hook on ") + name + " installed.");
    return true;
}

// --- Audio: ascoltatore 3D di FMOD ---------------------------------------------
//
// Il gioco importa FMOD::EventSystem::set3DListenerAttributes (fmod_event.dll,
// IAT 0x01109878): __stdcall con this sullo stack. Diagnostica (20/10): ogni
// 2 s posizione/avanti/alto passati dal gioco, con la vista disegnata (testa)
// e la camera logica (canna), per capire cosa segue l'audio.
struct FmodVec {
    float x, y, z;
};
typedef int(__stdcall* SetListener_t)(void* self, int listener, const FmodVec* pos, const FmodVec* vel,
                                      const FmodVec* fwd, const FmodVec* up);
SetListener_t g_setListenerReal = nullptr;
constexpr uintptr_t kIatFmodSetListener = 0x01109878;

int __stdcall Hooked_SetListener(void* self, int listener, const FmodVec* pos, const FmodVec* vel, const FmodVec* fwd,
                                 const FmodVec* up) {
    if (vr::control::g_audioLog > 0 && pos && fwd && up) {
        static ULONGLONG last = 0;
        if (GetTickCount64() - last > 2000) {
            last = GetTickCount64();
            vr::control::g_audioLog.fetch_sub(1);
            float vf[3], vr_[3], vu[3];
            RotatorToAxes(g_lastViewRot[0], g_lastViewRot[1], 0, vf, vr_, vu);
            float cf[3] = {0, 0, 0}, cr[3], cu[3], cl[3] = {0, 0, 0};
            char* cam = static_cast<char*>(g_cameraPtr.load());
            if (cam && HasCameraSignature(cam)) {
                int32_t* rot = reinterpret_cast<int32_t*>(cam + kCameraOffsetPitch);
                RotatorToAxes(rot[0], rot[1], 0, cf, cr, cu);
                std::memcpy(cl, cam + 0x10, sizeof(cl));
            }
            char b[600];
            snprintf(b, sizeof(b),
                     "AUDIO: listener %d pos (%.1f %.1f %.1f) fwd (%.2f %.2f %.2f) up (%.2f %.2f %.2f) | view loc (%.1f %.1f "
                     "%.1f) fwd (%.2f %.2f %.2f) | camera loc (%.1f %.1f %.1f) fwd (%.2f %.2f %.2f)",
                     listener, pos->x, pos->y, pos->z, fwd->x, fwd->y, fwd->z, up->x, up->y, up->z, g_lastViewLoc[0],
                     g_lastViewLoc[1], g_lastViewLoc[2], vf[0], vf[1], vf[2], cl[0], cl[1], cl[2], cf[0], cf[1], cf[2]);
            vr::log::Line(b);
        }
    }
    return g_setListenerReal(self, listener, pos, vel, fwd, up);
}

void InstallAudioListenerHook() {
    HMODULE ev = GetModuleHandleA("fmod_event.dll");
    void* expected =
        ev ? reinterpret_cast<void*>(GetProcAddress(
                 ev, "?set3DListenerAttributes@EventSystem@FMOD@@QAG?AW4FMOD_RESULT@@HPBUFMOD_VECTOR@@000@Z"))
           : nullptr;
    void** entry = reinterpret_cast<void**>(kIatFmodSetListener);
    if (!expected || *entry != expected) {
        vr::log::Line("AUDIO: FMOD listener entry unexpected, audio hook NOT installed.");
        return;
    }
    DWORD old;
    if (!VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &old)) return;
    g_setListenerReal = reinterpret_cast<SetListener_t>(expected);
    InterlockedExchange(reinterpret_cast<volatile LONG*>(entry), reinterpret_cast<LONG>(&Hooked_SetListener));
    VirtualProtect(entry, sizeof(void*), old, &old);
    vr::log::Line("AUDIO: hook on the FMOD 3D listener installed.");
}

void DumpCamera(void* camera) {
    if (!HasCameraSignature(camera)) {
        vr::log::Line("DUMPCAM: no valid camera.");
        return;
    }
    const char* c = static_cast<const char*>(camera);
    const float* loc = reinterpret_cast<const float*>(c + 0x10);
    const int32_t* rot = reinterpret_cast<const int32_t*>(c + kCameraOffsetPitch);
    char buf[256];
    std::snprintf(buf, sizeof(buf),
        "DUMPCAM cam=0x%p loc=(%.1f, %.1f, %.1f) pitch=%d yaw=%d roll=%d deltaYaw=%d deltaPitch=%d override=%u",
        camera, loc[0], loc[1], loc[2], rot[0], rot[1], rot[2],
        g_deltaYaw.load(), g_deltaPitch.load(), g_overridesApplied.load());
    vr::log::Line(buf);
    std::snprintf(buf, sizeof(buf), "DUMPCAM POV copy: dst offset=%+d size=%d",
                  g_lastCopyOffset.load(), g_lastCopySize.load());
    vr::log::Line(buf);
    std::string floats = "DUMPCAM float cam+0x00..0x5C:";
    for (int off = 0; off < 0x60; off += 4) {
        char b[32];
        std::snprintf(b, sizeof(b), " %02X=%.3f", off, *reinterpret_cast<const float*>(c + off));
        floats += b;
    }
    vr::log::Line(floats);
    std::string stack = "DUMPCAM POV copy stack:";
    for (int i = 0; i < kCopyStackDepth && g_copyStack[i]; ++i) {
        char b[16];
        std::snprintf(b, sizeof(b), " %08X", g_copyStack[i]);
        stack += b;
    }
    vr::log::Line(stack);
    vr::log::Line("DUMPCAM stereo: location copies=" + std::to_string(g_locCopies.load()) +
                  " eye recognised L=" + std::to_string(g_eyeMatches[0].load()) +
                  " R=" + std::to_string(g_eyeMatches[1].load()) +
                  " unrecognised=" + std::to_string(g_eyeMisses.load()) +
                  " draw stereo=" + std::to_string(g_stereoDraws.load()) +
                  " | Present: sconosciuto=" + std::to_string(g_presentByEye[0].load()) +
                  " L=" + std::to_string(g_presentByEye[1].load()) +
                  " R=" + std::to_string(g_presentByEye[2].load()) +
                  " ripetuti=" + std::to_string(g_presentRepeats.load()));
    {
        // In D3D (vettore riga) ViewProj = View * Proj: la norma della parte
        // di rotazione della colonna 0 vale xScale, quella della colonna 3
        // vale 1. Se lo shader usa la trasposta valgono le righe: calcoliamo
        // entrambe le ipotesi e logghiamo il FOV orizzontale corrispondente.
        const float* m = g_vsViewProj;
        auto norm3 = [](float a, float b, float c) { return std::sqrt(a * a + b * b + c * c); };
        float colX = norm3(m[0], m[4], m[8]), colY = norm3(m[1], m[5], m[9]), colW = norm3(m[3], m[7], m[11]);
        float rowX = norm3(m[0], m[1], m[2]), rowY = norm3(m[4], m[5], m[6]), rowW = norm3(m[12], m[13], m[14]);
        auto fovDeg = [](float scale) { return scale > 0 ? 2.0f * std::atan(1.0f / scale) * 57.2957795f : 0.0f; };
        char b[256];
        std::snprintf(b, sizeof(b),
            "DUMPCAM FOV from c0-c3: columns hfov=%.2f vfov=%.2f (w=%.3f) | rows hfov=%.2f vfov=%.2f (w=%.3f)",
            fovDeg(colW > 0 ? colX / colW : 0), fovDeg(colW > 0 ? colY / colW : 0), colW,
            fovDeg(rowW > 0 ? rowX / rowW : 0), fovDeg(rowW > 0 ? rowY / rowW : 0), rowW);
        vr::log::Line(b);
    }
    {
        // Stato per il collaudo dei livelli: chi possiede la camera, arma, cinema.
        char* c = static_cast<char*>(camera);
        bool sig = HasCameraSignature(camera);
        uint32_t pawn = 0, vt = 0, weapon = 0, pcFlags = 0;
        if (sig) {
            TrySafeReadUint32(c - 0x77C + 0x39C, &pcFlags);
            TrySafeReadUint32(c - 0x77C + 0x1F8, &pawn);
            TrySafeReadUint32(c - 0x77C + 0x3B4, &vt);
            if (pawn) TrySafeReadUint32(reinterpret_cast<char*>(pawn) + 0x1594, &weapon);
        }
        vr::log::Line(std::string("STATE gameplay=") + (sig && PlayerPawnIfViewTarget(c) ? "1" : "0") +
                      " cinema=" + (vr::control::g_cinemaActive ? "1" : "0") +
                      " cinematic=" + ((pcFlags & 0x8000) ? "1" : "0") +
                      " pawn=" + vr::script::ClassName(reinterpret_cast<void*>(pawn)) +
                      " viewtarget=" + vr::script::ClassName(reinterpret_cast<void*>(vt)) +
                      " arma=" + vr::script::ClassName(reinterpret_cast<void*>(weapon)));
    }
    {
        char b[260];
        std::snprintf(b, sizeof(b),
            "DUMPCAM marker: hand=%d body=%d aimValid=%d markerOn=%d vis=(%d,%d) ndcL=(%.3f,%.3f) ndcR=(%.3f,%.3f) aimDelta=(%d,%d) body=(%d,%d) headBlocked=%u twohand=%d",
            (int)vr::control::g_aimHand.load(), (int)vr::control::g_aimFollowsBody.load(), (int)g_aimValid.load(),
            (int)vr::control::g_aimMarker.load(), (int)g_markerVisible[0].load(), (int)g_markerVisible[1].load(),
            g_markerNdc[0][0].load(), g_markerNdc[0][1].load(), g_markerNdc[1][0].load(), g_markerNdc[1][1].load(),
            g_aimDeltaYaw.load(), g_aimDeltaPitch.load(), g_bodyYaw.load(), g_bodyPitch.load(), g_headBlocked.load(),
            (int)g_twoHandActive.load());
        vr::log::Line(b);
    }
    if (g_pawnViewLocValid) {
        char b[200];
        std::snprintf(b, sizeof(b), "DUMPCAM PawnViewLocation=(%.1f, %.1f, %.1f) - Location = (%.1f, %.1f, %.1f)",
                      g_pawnViewLoc[0], g_pawnViewLoc[1], g_pawnViewLoc[2],
                      g_pawnViewLoc[0] - g_pawnLocAtView[0], g_pawnViewLoc[1] - g_pawnLocAtView[1],
                      g_pawnViewLoc[2] - g_pawnLocAtView[2]);
        vr::log::Line(b);
    }
    vr::log::Line("DUMPCAM VS registers = +loc:" + RegMaskToString(g_vsRegMatchPlus) +
                  " | = -loc:" + RegMaskToString(g_vsRegMatchMinus));
}

int32_t RadiansToUnreal(float rad) {
    constexpr float kTwoPi = 6.28318530718f;
    return static_cast<int32_t>((rad / kTwoPi) * 65536.0f);
}

// Thread di rendering, una volta per frame: calcola SOLO i delta (nessuna
// scrittura sulla camera da qui, vedi MaybeApplyCameraOverride).
// Secondi da ora a quando il frame che si sta preparando arriva sul display:
// fino al prossimo vsync, piu' vsync->fotoni, piu' predictFrames frame di
// pipeline (thread di rendering, lettura del frame, thread di uscita).
float PredictedSecondsToPhotons(vr::IVRSystem* sys) {
    if (vr::control::g_predictFrames < 0.0f && !vr::control::g_predictAuto) return 0.0f;  // "predict off" (predefinito)
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
    float s = frame - sinceVsync + vsyncToPhotons + frame * vr::control::g_predictFrames;
    if (vr::control::g_predictAuto) {
        // Ritardo reale misurato dall'uscita (campionamento -> fotoni), diverso
        // per ogni PC, visore e collegamento; finche' non c'e', un frame in piu'.
        float measured = vr::output::PhotonLatencyMs();
        s = measured > 0.0f ? measured * 0.001f : frame - sinceVsync + vsyncToPhotons + frame;
    }
    return s < 0.0f ? 0.0f : (s > 0.1f ? 0.1f : s);
}

// AActor::Rotation.Yaw del PlayerController (+0x64): ruota il corpo.
bool AddControllerYaw(void* pc, int32_t delta) {
    __try {
        *reinterpret_cast<int32_t*>(static_cast<char*>(pc) + 0x64) += delta;
        vr::control::g_ownYawUnits.fetch_add(delta);
        // La vista di questo tick e' gia' costruita sul corpo letto prima: il
        // riferimento della testa si sposta adesso, quindi anche il corpo
        // (altrimenti un tick con la vista girata di delta).
        g_bodyYaw.fetch_add(delta);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void UpdateHeadTracking() {
    g_tickHmdPoseValid = false;
    if (vr::control::g_exiting) {
        g_deltaYaw = 0;
        g_deltaPitch = 0;
        g_deltaRoll = 0;
        for (auto& v : g_headOffset) v = 0.0f;
        return;
    }
    static int frame = 0;
    bool shouldLog = (++frame % 120 == 0);

    void* camera = g_cameraPtr.load();
    if (camera != g_lastLoggedCameraPtr) {
        g_lastLoggedCameraPtr = camera;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%p", camera);
        vr::log::Line(std::string("Real camera pointer captured/changed: 0x") + buf);
    }
    if (g_hudDiagFrames > 0) --g_hudDiagFrames;
    if (vr::control::g_dumpCameraRequested.exchange(false)) {
        DumpCamera(camera);
        g_hudDiagFrames = 12;
        ++g_hudDiagId;
    }

    int mode = vr::control::g_mode;
    if (mode == vr::control::kModeOff) {
        g_deltaYaw = 0;
        g_deltaPitch = 0;
        g_deltaRoll = 0;
        for (auto& v : g_headOffset) v = 0.0f;
        return;
    }

    if (mode == vr::control::kModeSynthetic) {
        constexpr float kTwoPi = 6.28318530718f;
        constexpr float kDegToRad = 0.01745329252f;
        float t = static_cast<float>(GetTickCount64() % 3600000) / 1000.0f;
        float period = vr::control::g_synthPeriodSec;
        float s = period > 0.0f ? std::sin(kTwoPi * t / period) : 1.0f;  // 0 = spostamento fisso
        g_deltaYaw = RadiansToUnreal(s * vr::control::g_synthYawAmpDeg * kDegToRad);
        g_deltaPitch = RadiansToUnreal(s * vr::control::g_synthPitchAmpDeg * kDegToRad);
        g_deltaRoll = 0;
        for (int i = 0; i < 3; ++i) g_headOffset[i] = s * vr::control::g_synthPosCm[i] * 0.01f * vr::control::g_posScale;
        g_aimDeltaYaw = RadiansToUnreal(vr::control::g_synthAimYawDeg * kDegToRad);
        g_aimDeltaPitch = RadiansToUnreal(vr::control::g_synthAimPitchDeg * kDegToRad);
        g_aimValid = true;
        for (int i = 0; i < 3; ++i)
            g_handOffset[i] = vr::control::g_synthHandCm[i] * 0.01f * vr::control::g_posScale;
        g_handRoll = 0;
        g_handValid = true;
        {
            float right[3], left[3];
            bool haveLeft = false;
            for (int i = 0; i < 3; ++i) {
                right[i] = g_handOffset[i];
                left[i] = vr::control::g_synthLeftCm[i] * 0.01f * vr::control::g_posScale;
                if (vr::control::g_synthLeftCm[i] != 0.0f) haveLeft = true;
            }
            for (int i = 0; i < 3; ++i) g_leftOffset[i] = left[i];
            g_leftYaw = 0;
            g_leftPitch = 0;
            g_leftRoll = 0;
            g_leftValid = haveLeft;
            if (haveLeft) ApplyTwoHandAim(right, left);
            else g_twoHandActive = false;
        }
        {
            // Aiuto alle esecuzioni anche con le pose finte (test senza visore).
            uint32_t ctrlPawn = 0;
            void* pc = g_playerControllerPtr;
            if (pc && TrySafeReadUint32(static_cast<char*>(pc) + 0x1F8, &ctrlPawn) && ctrlPawn)
                ExecutionAssist(pc, reinterpret_cast<void*>(ctrlPawn));
        }
        if (shouldLog) DumpCamera(camera);
        return;
    }

    // kModeHmd
    vr::session::Shared vrLock(true);  // durante un cambio di tipo: questo tick senza testa
    vr::IVRSystem* const vrsys = vrLock.ok() ? vr::VRSystem() : nullptr;
    if (!vr::session::Ready() || !vrsys) {
        static ULONGLONG lastNotReady = 0;
        if (GetTickCount64() - lastNotReady > 60000) {
            lastNotReady = GetTickCount64();
            vr::log::Line("UpdateHeadTracking: OpenVR not ready (logged once a minute).");
        }
        return;
    }
    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
    float predict = PredictedSecondsToPhotons(vrsys);
    vrsys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, predict, poses, vr::k_unMaxTrackedDeviceCount);
    vr::input::CorrectControllerPoses(vrsys, poses);
    const vr::TrackedDevicePose_t& hmdPose = poses[vr::k_unTrackedDeviceIndex_Hmd];
    // Controller predetti a parte (handpredict, ms): la testa la corregge la
    // riproiezione del visore, le mani no, quindi il fucile mostrava tutta la
    // latenza del sistema ("weapons felt stiff", 18/10).
    vr::TrackedDevicePose_t handPoses[vr::k_unMaxTrackedDeviceCount];
    float handSec = vr::control::g_handPredictAuto ? predict : vr::control::g_handPredictMs * 0.001f;
    // Con la testa predetta e handpredict 0 le mani vanno lette senza
    // predizione: prima prendevano quella della testa e il fucile si
    // sdoppiava (19/10, predict 1).
    if (handSec > 0.0f || predict > 0.0f) {
        vrsys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, handSec, handPoses, vr::k_unMaxTrackedDeviceCount);
        vr::input::CorrectControllerPoses(vrsys, handPoses);
    } else {
        std::memcpy(handPoses, poses, sizeof(handPoses));
    }
    if (!hmdPose.bPoseIsValid) {
        if (shouldLog) vr::log::Line("UpdateHeadTracking: invalid HMD pose.");
        return;
    }
    const vr::HmdMatrix34_t& m = hmdPose.mDeviceToAbsoluteTracking;
    g_tickHmdPose = m;
    g_tickHmdPoseValid = true;
    if (vr::control::g_ipdFromHmd && vr::control::g_stereoHalfIpd > 0.0f) {
        static ULONGLONG lastIpd = 0;
        if (GetTickCount64() - lastIpd > 2000) {
            lastIpd = GetTickCount64();
            float halfM = std::fabs(vrsys->GetEyeToHeadTransform(vr::Eye_Right).m[0][3]);
            if (halfM > 0.025f && halfM < 0.040f) {
                float half = halfM * vr::control::g_posScale;
                if (std::fabs(half - vr::control::g_stereoHalfIpd) > 0.01f) {
                    vr::control::g_stereoHalfIpd = half;
                    vr::log::Line("STEREO: headset IPD " + std::to_string(halfM * 2000.0f) + " mm -> half " +
                                  std::to_string(half) + " units");
                }
            }
        }
    }
    if (shouldLog) vr::log::Line("UpdateHeadTracking: poses predicted " + std::to_string(predict * 1000.0f) + " ms ahead.");
    float hmdYaw = YawFromOpenVrPose(m);
    float hmdPitch = PitchFromOpenVrPose(m);
    // OpenVR: x destra, y su, -z avanti; posizione in metri nella colonna 3.
    float pos[3] = {m.m[0][3], m.m[1][3], m.m[2][3]};
    static float posRef[3] = {};
    if (vr::control::g_recenterRequested.exchange(false)) {
        // Solo direzione e posizione orizzontale: l'orizzonte resta dritto
        // (17/10: un ricentramento fatto guardando in basso inclinava tutta la
        // vista di 50 gradi) e l'altezza di riferimento e' quella in piedi.
        g_hmdYawReference = hmdYaw;
        g_hmdPitchReference = 0.0f;
        for (int i = 0; i < 3; ++i) posRef[i] = pos[i];
        vr::control::g_cinemaRecenter = true;
        vr::log::Line("Recentered: hmdYaw=" + std::to_string(hmdYaw) + " hmdPitch=" + std::to_string(hmdPitch) +
                      " pos=(" + std::to_string(pos[0]) + ", " + std::to_string(pos[1]) + ", " + std::to_string(pos[2]) + ")");
    }
    // Rotazione con lo stick (continua o a scatti) attorno alla testa: il corpo
    // gira attorno al centro di Walker e gli occhi, fp unita' davanti piu' lo
    // spostamento della testa, giravano con lui (fino a 1 m di scivolata dopo
    // un passo dal punto di ricentramento, audit 19/10). Si sposta il
    // riferimento della posizione perche' gli occhi restino dove sono.
    {
        int32_t d = vr::control::g_stickYawUnits.exchange(0);
        float scale = vr::control::g_posScale;
        if (d && vr::control::g_sixDof && scale > 0.0f) {
            float a = d * (6.28318530718f / 65536.0f);
            float fwd = vr::control::g_fpForward.load();
            float vf = fwd + g_headOffset[0].load(), vrr = g_headOffset[1].load();
            float nf = vf * std::cos(a) + vrr * std::sin(a), nr = -vf * std::sin(a) + vrr * std::cos(a);
            float df = (nf - fwd) - g_headOffset[0].load(), dr = nr - g_headOffset[1].load();
            float sy = std::sin(g_hmdYawReference), cy = std::cos(g_hmdYawReference);
            posRef[0] -= (df * sy + dr * cy) / scale;
            posRef[2] -= (-df * cy + dr * sy) / scale;
        }
    }
    // Altezza degli occhi per "height auto": testa in piedi (riferimento, metri
    // sul pavimento) meno il centro del Pawn in piedi (45 unita' sopra i
    // piedi), entro 25..55 unita'.
    if (posRef[1] > 0.5f) {
        float h = posRef[1] * vr::control::g_posScale - 45.0f;
        vr::control::g_eyeHeightAuto = h < 25.0f ? 25.0f : (h > 55.0f ? 55.0f : h);
    }
    // Altezza di riferimento = altezza della testa in piedi (la massima vista
    // dall'ultimo ricentramento): abbassarsi davvero abbassa la vista, ma un
    // ricentramento fatto da chinati non solleva tutto quando ci si alza.
    // Picchi del tracciamento (oltre 10 cm in un campione) non alzano il
    // riferimento; vicino all'altezza in piedi (entro 15 cm) il riferimento
    // scende di 1 cm/s, cosi' dopo essersi alzati in punta di piedi la vista
    // non resta piu' bassa fino al ricentramento. Accucciati non scende.
    {
        static float lastY = 0.0f;
        static double lastRefT = 0.0;
        double nowR = ProfMs() * 0.001;
        float dtR = lastRefT > 0.0 ? static_cast<float>(nowR - lastRefT) : 0.0f;
        lastRefT = nowR;
        bool spike = lastY != 0.0f && pos[1] - lastY > 0.10f;
        lastY = pos[1];
        if (pos[1] > posRef[1] && !spike) posRef[1] = pos[1];
        else if (pos[1] < posRef[1] && posRef[1] - pos[1] < 0.15f && dtR > 0.0f && dtR < 0.5f)
            posRef[1] = (std::max)(pos[1], posRef[1] - 0.01f * dtR);
    }
    // Il corpo segue la testa: l'eccesso oltre la soglia passa dalla testa al
    // corpo (rotazione del PlayerController) e il riferimento si sposta della
    // stessa quantita', cosi' la vista non cambia.
    float turnThr = vr::control::g_turnWithHeadDeg * 0.01745329252f;
    void* pcForTurn = g_playerControllerPtr;
    // In copertura (YGamePawn.m_coverFaceState +0x498 != 0) il gioco orienta
    // Walker verso il riparo: niente rotazione del corpo con la testa.
    bool inCover = false, specialMove = false;
    {
        char* camT = static_cast<char*>(g_cameraPtr.load());
        void* pawnT = HasCameraSignature(camT) ? PlayerPawnIfViewTarget(camT) : nullptr;
        uint32_t cover = 0, special = 0;
        inCover = pawnT && TrySafeReadUint32(static_cast<char*>(pawnT) + 0x498, &cover) && (cover & 0xFF) != 0;
        // YGamePawn.m_specialMove (+0x491): 19 entrando in copertura (la
        // rotazione verso il riparo avviene qui, m_coverFaceState e' ancora 0),
        // 20 uscendo; anche scavalcamenti e corpo a corpo.
        specialMove = pawnT && TrySafeReadUint32(static_cast<char*>(pawnT) + 0x490, &special) &&
                      ((special >> 8) & 0xFF) != 0;
        vr::control::g_inCover = inCover || specialMove;
    }
    // Rotazione del corpo decisa dal gioco (entrando in copertura Walker viene
    // girato di ~50 gradi verso il riparo in pochi frame): la si toglie dalla
    // vista spostando il riferimento della testa, cosi' il mondo non gira
    // attorno al giocatore. Esclusi lo stick (pochi gradi per frame) e le
    // rotazioni fatte dalla mod (g_ownYawUnits).
    {
        static int32_t lastBodyYaw = 0;
        static bool haveBody = false;
        int32_t bodyYawNow = g_bodyYaw.load();
        int32_t own = vr::control::g_ownYawUnits.exchange(0);
        int32_t turretAdd = g_turretYawUnits.exchange(0);
        if (haveBody && turretAdd != 0) {
            // Solo la parte davvero eseguita (la torretta puo' essere al limite).
            int32_t actual = static_cast<int16_t>(bodyYawNow - lastBodyYaw - (own - turretAdd));
            int32_t comp = 0;
            if ((actual > 0) == (turretAdd > 0)) comp = std::abs(actual) < std::abs(turretAdd) ? actual : turretAdd;
            g_hmdYawReference += vr::control::g_yawSign * comp * (6.28318530718f / 65536.0f);
            lastBodyYaw += comp;
            own -= turretAdd;
        }
        // Dopo una cinematica il gioco rimette Walker nella sua direzione
        // (anche 90 gradi): quella rotazione e' voluta, non si compensa.
        static ULONGLONG cinemaSeen = 0;
        if (vr::control::g_cinemaActive || g_gameLockedScene) cinemaSeen = GetTickCount64();
        bool afterCinema = GetTickCount64() - cinemaSeen < 2000;
        // Solo entrando, stando o appena usciti dalla copertura. Prima valeva per
        // ogni rotazione del gioco: dopo un salto al checkpoint, una ripartenza o
        // una rotazione scriptata il corpo restava girato rispetto alla vista e
        // lo stick in avanti spingeva Walker di lato, contro le pareti (18/10,
        // "after it ended I was stuck in the fuselage").
        // La rotazione puo' arrivare un attimo prima che la mossa speciale
        // risulti: resta in sospeso 0,4 s e si applica se nel frattempo
        // compare la copertura, altrimenti si scarta.
        static ULONGLONG coverSeen = 0, pendingT = 0;
        static int pendingD = 0;
        ULONGLONG nowC = GetTickCount64();
        if (inCover || specialMove) coverSeen = nowC;
        bool nearCover = coverSeen && nowC - coverSeen < 1500;
        auto compensate = [&](int d) {
            g_hmdYawReference += vr::control::g_yawSign * d * (6.28318530718f / 65536.0f);
            static ULONGLONG lastLog = 0;
            if (nowC - lastLog > 500) {
                lastLog = nowC;
                vr::log::Line("COVER: body turned " + std::to_string(d * 360 / 65536) + " degrees by the game" +
                              (inCover ? " (cover)" : "") + ", view kept still");
            }
        };
        if (pendingD && nowC - pendingT > 400) pendingD = 0;
        if (pendingD && nearCover) {
            compensate(pendingD);
            pendingD = 0;
        }
        bool gameStickTurn = vr::control::g_stickTurning && vr::control::g_turnSpeedDeg <= 0.0f &&
                             vr::control::g_snapTurnDeg <= 0.0f;
        if (haveBody && !vr::control::g_cinemaActive && vr::control::g_coverSnapCompensate &&
            !gameStickTurn && !TurretAimActive() && !g_gameLockedScene && !afterCinema) {
            int16_t d = static_cast<int16_t>(bodyYawNow - lastBodyYaw - own);
            // Vicino alla copertura si toglie ogni rotazione non nostra sopra
            // mezzo grado per tick: con 7 gradi una rotazione graduale del
            // gioco (sotto 630 gradi/s a 90 Hz) sfuggiva e il mondo girava
            // (audit 19/10). Lo stick ora gira tramite la mod (turnspeed,
            // g_ownYawUnits), quindi non viene scambiato per il gioco.
            const int kMin = (nearCover ? 65536 / 720 : 7 * 65536 / 360), kMax = 75 * 65536 / 360;
            if (std::abs(d) > kMin && std::abs(d) < kMax) {
                if (nearCover) {
                    compensate(d);
                } else {
                    pendingD += d;
                    pendingT = nowC;
                }
            }
        }
        lastBodyYaw = bodyYawNow;
        haveBody = true;
    }
    // Sporgersi dal riparo col corpo: alzarsi di 12 cm o spostarsi di 18 cm
    // rispetto a dove si era entrando in copertura = LT (esce a mirare);
    // rientro sotto 7 / 11 cm.
    {
        static bool wasCover = false, peeking = false;
        static float entry[3] = {0, 0, 0};
        if (inCover && !wasCover) {
            for (int i = 0; i < 3; ++i) entry[i] = pos[i];
            // Entrati in copertura in piedi (tasto A): il riferimento e'
            // l'altezza da accovacciati (25 cm sotto quella in piedi), se no
            // alzarsi non faceva mai uscire a mirare (audit 20/10).
            if (posRef[1] > 0.5f && entry[1] > posRef[1] - 0.25f) entry[1] = posRef[1] - 0.25f;
            peeking = false;
        }
        if (inCover && vr::control::g_coverPeekEnabled) {
            float up = pos[1] - entry[1];
            // Solo di lato rispetto al corpo (destra del riferimento): chinarsi
            // in avanti (guardare la cintura nella ricarica, guardare giu' dal
            // riparo) faceva uscire Walker sotto il fuoco (audit 19/10).
            float sy = std::sin(g_hmdYawReference), cy = std::cos(g_hmdYawReference);
            float lat = std::fabs((pos[0] - entry[0]) * cy + (pos[2] - entry[2]) * sy);
            if (!peeking && (up > 0.12f || lat > 0.18f)) {
                peeking = true;
                vr::log::Line("COVER: leaning out (head up " + std::to_string(up) + " m, sideways " + std::to_string(lat) + " m)");
            } else if (peeking && up < 0.07f && lat < 0.11f) {
                peeking = false;
            }
        } else {
            peeking = false;
        }
        vr::control::g_coverPeek = peeking;
        wasCover = inCover;
    }
    {
        // Pawn controllato: Controller.Pawn (+0x1F8).
        uint32_t ctrlPawn = 0;
        if (pcForTurn && TrySafeReadUint32(static_cast<char*>(pcForTurn) + 0x1F8, &ctrlPawn) && ctrlPawn)
            ExecutionAssist(pcForTurn, reinterpret_cast<void*>(ctrlPawn));
    }
    // Non sulle torrette: li' il PlayerController segue il controller (sotto) e
    // girare la testa oltre la soglia lo tirava verso lo sguardo, con la
    // torretta che tremava e restava indietro (audit 20/10).
    if (turnThr > 0.0f && pcForTurn && !vr::control::g_cinemaActive && !inCover && !g_gameLockedScene &&
        !TurretAimActive()) {
        float ex = std::remainder(hmdYaw - g_hmdYawReference, 6.28318530718f);
        static double lastTurnT = 0.0;
        double nowTurn = ProfMs() * 0.001;
        float dtTurn = lastTurnT > 0.0 ? static_cast<float>(nowTurn - lastTurnT) : 0.0f;
        lastTurnT = nowTurn;
        if (std::fabs(ex) > turnThr) {
            float shift = ex - std::copysign(turnThr, ex);
            // Al massimo 180 gradi/s: uscendo dalla copertura o da una scena
            // con la testa girata il corpo scattava tutto in un frame.
            float maxStep = 3.14159265f * (dtTurn > 0.0f && dtTurn < 0.1f ? dtTurn : 0.011f);
            if (std::fabs(shift) > maxStep) shift = std::copysign(maxStep, shift);
            if (AddControllerYaw(pcForTurn, vr::control::g_yawSign * RadiansToUnreal(shift)))
                g_hmdYawReference += shift;
        }
    }
    g_deltaYaw = vr::control::g_yawSign * RadiansToUnreal(std::remainder(hmdYaw - g_hmdYawReference, 6.28318530718f));
    vr::control::g_headYawUnits = g_deltaYaw.load();
    g_deltaPitch = vr::control::g_pitchSign * RadiansToUnreal(hmdPitch - g_hmdPitchReference);
    if (vr::control::g_sixDof) {
        // Rollio: inclinazione del vettore "right" del visore (colonna 0).
        float rightY = m.m[1][0];
        if (rightY > 1.0f) rightY = 1.0f;
        if (rightY < -1.0f) rightY = -1.0f;
        // atan2(right.y, up.y): esatto anche guardando in alto/basso (asin di
        // right.y vale solo a beccheggio 0: a 45 gradi in basso sbagliava ~6 gradi).
        g_deltaRoll = vr::control::g_rollSign * RadiansToUnreal(std::atan2(-m.m[1][0], m.m[1][1]));
        // Spostamento nel riferimento della direzione al ricentramento.
        float dx = pos[0] - posRef[0], dy = pos[1] - posRef[1], dz = pos[2] - posRef[2];
        float sy = std::sin(g_hmdYawReference), cy = std::cos(g_hmdYawReference);
        float forward = dx * sy - dz * cy;  // avanti di riferimento = (sin, 0, -cos)
        float right = dx * cy + dz * sy;    // destra di riferimento = (cos, 0, sin)
        float scale = vr::control::g_posScale;
        g_headOffset[0] = forward * scale;
        g_headOffset[1] = right * scale;
        g_headOffset[2] = dy * scale;
        // Room-scale: oltre 15 cm di passo il Pawn si sposta (Actor.Move, con
        // le collisioni) fino a 5 cm dalla testa e il riferimento della
        // posizione si sposta di quanto si e' mosso davvero: la vista non
        // cambia, ma collisioni, bersaglio dei nemici e copertura seguono il
        // giocatore. Prima oltre 1,2 m il corpo restava indietro (audit 19/10).
        float horiz = std::sqrt(forward * forward + right * right);
        if (vr::control::g_roomScale && horiz > 0.15f && !vr::control::g_cinemaActive && !g_gameLockedScene &&
            PawnAlive(PlayerPawnIfViewTarget(static_cast<char*>(g_cameraPtr.load()))) &&
            PawnOnFirmGround(PlayerPawnIfViewTarget(static_cast<char*>(g_cameraPtr.load()))) && !g_onTurret &&
            !vr::control::g_inCover && scale > 0.0f) {
            char* camR = static_cast<char*>(g_cameraPtr.load());
            void* pawnR = HasCameraSignature(camR) ? PlayerPawnIfViewTarget(camR) : nullptr;
            float before[3];
            if (pawnR && SafeCopy(before, static_cast<char*>(pawnR) + 0x54, 12)) {
                float k = (horiz - 0.05f) / horiz;  // si lasciano 5 cm
                float mf = forward * k * scale, mr = right * k * scale;
                float b = g_bodyYaw.load() * (6.28318530718f / 65536.0f);
                struct {
                    float delta[3];
                    uint32_t ret;
                } mv = {{mf * std::cos(b) - mr * std::sin(b), mf * std::sin(b) + mr * std::cos(b), 0.0f}, 0};
                float after[3];
                if (vr::script::CallFunction(pawnR, "Move", &mv) && SafeCopy(after, static_cast<char*>(pawnR) + 0x54, 12)) {
                    float wx = after[0] - before[0], wy = after[1] - before[1];
                    // Mondo -> riferimento del corpo (avanti, destra), poi -> metri.
                    float mfw = (wx * std::cos(b) + wy * std::sin(b)) / scale;
                    float mrw = (-wx * std::sin(b) + wy * std::cos(b)) / scale;
                    if (mfw * mfw + mrw * mrw > 1e-6f) {
                        // offset = M * (pos - posRef): togliere (mfw, mrw) dall'offset.
                        posRef[0] += mfw * sy + mrw * cy;
                        posRef[2] += -mfw * cy + mrw * sy;
                        float ndx = pos[0] - posRef[0], ndz = pos[2] - posRef[2];
                        g_headOffset[0] = (ndx * sy - ndz * cy) * scale;
                        g_headOffset[1] = (ndx * cy + ndz * sy) * scale;
                    }
                }
            }
        }
    } else {
        g_deltaRoll = 0;
        for (auto& v : g_headOffset) v = 0.0f;
    }
    PublishHeadPose(&m);
    // Controller destro: direzione di puntamento (-Z del controller), yaw
    // relativo alla direzione di ricentramento, pitch rispetto all'orizzonte.
    {
        vr::TrackedDeviceIndex_t ri = vrsys->GetTrackedDeviceIndexForControllerRole(vr::control::GunRole());
        if (ri != vr::k_unTrackedDeviceIndexInvalid && ri < vr::k_unMaxTrackedDeviceCount && handPoses[ri].bPoseIsValid) {
            const vr::HmdMatrix34_t& c = handPoses[ri].mDeviceToAbsoluteTracking;
            if (vr::control::g_stockCalibRequest.exchange(false)) {
                // Calcio in posizione di mira, bersaglio lontano guardato al
                // centro della vista: la direzione vera di mira e' quella della
                // testa. (La linea occhio-controller non va: sul calcio la mano
                // sta all'impugnatura, ~25 cm sotto l'occhio; dava -67 gradi.)
                float d[3] = {c.m[0][3] - pos[0], c.m[1][3] - pos[1], c.m[2][3] - pos[2]};
                float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                if (dist > 0.15f) {
                    float linePitch = hmdPitch * 57.2957795f;
                    float ctrlPitch = PitchFromOpenVrPose(c) * 57.2957795f;
                    float offset = std::round((linePitch - ctrlPitch) * 2.0f) / 2.0f;
                    // Esito diverso al tatto: 150 ms = calibrato, 500 ms = rifiutata
                    // (prima la stessa vibrazione anche quando falliva; audit 20/10).
                    bool rejected = offset < -45.0f || offset > 45.0f;
                    if (rejected) {
                        vr::log::Line("STOCK: calibration out of range (" + std::to_string(offset) +
                                      " deg), ignored: look at the target along the stock and try again.");
                        offset = vr::control::g_aimPitchOffsetDeg;
                    }
                    vr::control::g_aimPitchOffsetDeg = offset;
                    char b[200];
                    snprintf(b, sizeof(b), "STOCK: calibrated, head %.1f deg, controller %.1f deg -> aimpitch %.1f",
                             linePitch, ctrlPitch, offset);
                    vr::log::Line(b);
                    char val[32];
                    snprintf(val, sizeof(val), "%.1f", offset);
                    vr::control::SaveIniValue("aimpitch", val);
                    vr::TrackedDeviceIndex_t ri2 = vrsys->GetTrackedDeviceIndexForControllerRole(vr::control::GunRole());
                    (void)ri2;
                    vr::input::PulseHand(1, rejected ? 500 : 150);
                } else {
                    vr::log::Line("STOCK: calibration ignored, controller too close to the headset.");
                    vr::input::PulseHand(1, 500);
                }
            }
            // aimpitch (calcio) come rotazione attorno all'asse destro del
            // controller, poi yaw/beccheggio/rollio dalla matrice ruotata.
            // Prima si sommava al beccheggio assoluto: col fucile inclinato la
            // canna prendeva un errore di lato di aimpitch*sin(rollio) (7,5
            // gradi a 30 gradi di rollio con -15, audit 19/10).
            vr::HmdMatrix34_t ca = c;
            {
                float a = vr::control::g_aimPitchOffsetDeg * 0.01745329252f;
                float co = std::cos(a), si = std::sin(a);
                for (int k = 0; k < 3; ++k) {
                    float y = c.m[k][1], z = c.m[k][2];
                    ca.m[k][1] = co * y + si * z;
                    ca.m[k][2] = co * z - si * y;
                }
            }
            float cYaw = YawFromOpenVrPose(ca);
            float cPitch = PitchFromOpenVrPose(ca);
            g_aimDeltaYaw = vr::control::g_yawSign * RadiansToUnreal(cYaw - g_hmdYawReference);
            g_aimDeltaPitch = vr::control::g_pitchSign * RadiansToUnreal(cPitch);
            g_aimValid = true;
            // Posizione del controller rispetto alla testa al ricentramento,
            // nello stesso riferimento dello spostamento 6DOF della testa.
            float dx = c.m[0][3] - posRef[0], dy = c.m[1][3] - posRef[1], dz = c.m[2][3] - posRef[2];
            float sy = std::sin(g_hmdYawReference), cy = std::cos(g_hmdYawReference);
            float scale = vr::control::g_posScale;
            g_handOffset[0] = (dx * sy - dz * cy) * scale;
            g_handOffset[1] = (dx * cy + dz * sy) * scale;
            g_handOffset[2] = dy * scale;
            float rightY = c.m[1][0];
            if (rightY > 1.0f) rightY = 1.0f;
            if (rightY < -1.0f) rightY = -1.0f;
            g_handRoll = vr::control::g_rollSign * RadiansToUnreal(std::atan2(-ca.m[1][0], ca.m[1][1]));
            g_handValid = true;
            // Controller sinistro, stesso riferimento: mira a due mani.
            vr::TrackedDeviceIndex_t li =
                vrsys->GetTrackedDeviceIndexForControllerRole(vr::control::SupportRole());
            g_twoHandActive = false;
            if (li != vr::k_unTrackedDeviceIndexInvalid && li < vr::k_unMaxTrackedDeviceCount && handPoses[li].bPoseIsValid) {
                const vr::HmdMatrix34_t& lm = handPoses[li].mDeviceToAbsoluteTracking;
                float lx = lm.m[0][3] - posRef[0], ly = lm.m[1][3] - posRef[1], lz = lm.m[2][3] - posRef[2];
                float left[3] = {(lx * sy - lz * cy) * scale, (lx * cy + lz * sy) * scale, ly * scale};
                float right[3] = {g_handOffset[0], g_handOffset[1], g_handOffset[2]};
                for (int i = 0; i < 3; ++i) g_leftOffset[i] = left[i];
                g_leftYaw = vr::control::g_yawSign * RadiansToUnreal(YawFromOpenVrPose(lm) - g_hmdYawReference);
                vr::control::g_leftYawUnits = g_leftYaw.load();
                vr::control::g_leftYawValid = true;
                g_leftPitch = vr::control::g_pitchSign *
                              RadiansToUnreal(PitchFromOpenVrPose(lm) + vr::control::g_aimPitchOffsetDeg * 0.01745329252f);
                float lRightY = lm.m[1][0];
                if (lRightY > 1.0f) lRightY = 1.0f;
                if (lRightY < -1.0f) lRightY = -1.0f;
                g_leftRoll = vr::control::g_rollSign * RadiansToUnreal(std::atan2(-lm.m[1][0], lm.m[1][1]));
                g_leftValid = true;
                ApplyTwoHandAim(right, left);
            } else {
                g_leftValid = false;
            }
        } else {
            g_aimValid = false;
            g_handValid = false;
        }
    }
    // Torretta: il PlayerController (e con lui la torretta) ruota verso il
    // controller destro; la vista non gira (compensazione sopra) e il
    // beccheggio del corpo non entra nella vista. Prima la torretta seguiva lo
    // stick e solo in parte il controller ("had to aim my headset", 18/10).
    // Scesi dalla torretta: il beccheggio scritto sul PlayerController restava
    // (lo stick su/giu' e' escluso), e razzi, lanciagranate e il controllo
    // dell'esecuzione partivano inclinati (audit 20/10).
    {
        static bool wasTurret = false;
        bool turret = TurretAimActive();
        if (wasTurret && !turret && g_playerControllerPtr && !vr::control::g_stickPitch) {
            int32_t zero = 0;
            SafeCopy(static_cast<char*>(g_playerControllerPtr) + 0x60, &zero, 4);
            vr::log::Line("TURRET: left, PlayerController pitch reset to 0.");
        }
        wasTurret = turret;
    }
    if (TurretAimActive() && g_aimValid && g_playerControllerPtr) {
        char* pc = static_cast<char*>(g_playerControllerPtr);
        int32_t dy = static_cast<int32_t>(g_aimDeltaYaw.load() * 0.35f);
        if (dy != 0 && AddControllerYaw(pc, dy)) g_turretYawUnits.fetch_add(dy);
        uint32_t pitchNow = 0;
        if (TrySafeReadUint32(pc + 0x60, &pitchNow)) {
            int16_t err = static_cast<int16_t>(g_aimDeltaPitch.load() - static_cast<int32_t>(pitchNow));
            int32_t newPitch = static_cast<int32_t>(pitchNow) + static_cast<int32_t>(err * 0.35f);
            SafeCopy(pc + 0x60, &newPitch, 4);
        }
    }
    if (shouldLog) DumpCamera(camera);
}

// Patch di codice con verifica dei byte originali (rifiuta se l'exe e'
// diverso da quello analizzato).
bool PatchCode(uintptr_t addr, const uint8_t* expected, const uint8_t* replacement, size_t len, const char* what) {
    uint8_t* p = reinterpret_cast<uint8_t*>(addr);
    if (std::memcmp(p, expected, len) != 0) {
        vr::log::Line(std::string("PATCH ") + what + ": unexpected bytes, not applied.");
        return false;
    }
    DWORD old;
    if (!VirtualProtect(p, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(p, replacement, len);
    VirtualProtect(p, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, len);
    vr::log::Line(std::string("PATCH ") + what + ": applied.");
    return true;
}

// --- Bande nere: ULocalPlayer::CalcSceneView (0x0075C460, thiscall, 5 arg,
// ret 0x14) calcola il rettangolo della vista come LocalPlayer.Origin
// (+0x68/+0x6C) e Size (+0x70/+0x74) per la dimensione del viewport. Lo
// script del gioco li imposta per forzare il 16:9: con la finestra quasi
// quadrata per il visore restavano bande nere sopra e sotto. Prima di ogni
// chiamata li riportiamo a tutto schermo. (Il ramo bConstrainAspectRatio a
// 0x0075CA71 NON e' la causa: provato a disattivarlo, nessun effetto.)
constexpr uintptr_t kCalcSceneViewAddr = 0x0075C460;
typedef void*(__thiscall* CalcSceneView_t)(void* self, void* a, void* b, void* c, void* d, void* e);
CalcSceneView_t g_CalcSceneView_Original = nullptr;
void* g_localPlayer = nullptr;

// APlayerController::GetPlayerViewPoint(out Location, out Rotation) @
// 0x00AB83F0: wrapper C++ dell'evento UnrealScript (thiscall, ret 8),
// chiamato da CalcSceneView a 0x0075C602 per ottenere la vista.
constexpr uintptr_t kGetPlayerViewPointAddr = 0x00AB83F0;
typedef void(__thiscall* GetPlayerViewPoint_t)(void* self, float* outLoc, int32_t* outRot);
GetPlayerViewPoint_t g_GetPlayerViewPoint_Original = nullptr;
bool g_inCalcSceneView = false;  // solo thread di gioco

// --- Trace di collisione del motore -----------------------------------------
//
// UWorld::SingleLineCheck @ 0x009BDE30 (thiscall su GWorld = [0x014DF734],
// ret 0x1C): (FCheckResult& Hit, AActor* Source, const FVector& End,
// const FVector& Start, DWORD TraceFlags, const FVector& Extent, light).
// Usato cosi' da AActor::execFastTrace (0x0083CA90) con flag 0x2286. Nel
// risultato: Actor a +4, Location a +8 (il tempo si ricava dalla posizione).
constexpr uintptr_t kSingleLineCheckAddr = 0x009BDE30;
constexpr uintptr_t kGWorldAddr = 0x014DF734;
// + TRACE_Pawns (0x1): la testa non entra piu' nei compagni di squadra e il
// mirino si ferma sul nemico invece che sul muro dietro di lui (playtest
// 19/10). Il Pawn di Walker e' la sorgente, quindi escluso.
constexpr DWORD kTraceFlags = 0x2286 | 0x1 | 0x10;  // + TRACE_Others: attori mossi (cabina dell'elicottero)
typedef int(__thiscall* SingleLineCheck_t)(void* world, void* hit, void* source, const float* end, const float* start,
                                           DWORD flags, const float* extent, void* light);

// Ritorna true se la linea start->end colpisce qualcosa; hitTime in [0,1].
bool TraceLine(void* source, const float* start, const float* end, float* hitTime) {
    __try {
        void* world = *reinterpret_cast<void**>(kGWorldAddr);
        if (!world) return false;
        alignas(16) unsigned char hit[0x100] = {};
        const float extent[3] = {0.0f, 0.0f, 0.0f};
        reinterpret_cast<SingleLineCheck_t>(kSingleLineCheckAddr)(world, hit, source, end, start, kTraceFlags, extent, nullptr);
        if (*reinterpret_cast<void**>(hit + 4) == nullptr) return false;
        // Tempo ricavato dalla posizione d'impatto (+8): l'offset del campo
        // Time non e' certo, la posizione si'.
        const float* loc = reinterpret_cast<const float*>(hit + 8);
        float sx = end[0] - start[0], sy = end[1] - start[1], sz = end[2] - start[2];
        float hx = loc[0] - start[0], hy = loc[1] - start[1], hz = loc[2] - start[2];
        float seg2 = sx * sx + sy * sy + sz * sz;
        float t = seg2 > 0.0f ? (hx * sx + hy * sy + hz * sz) / seg2 : 0.0f;
        *hitTime = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void RotatorToAxes(int32_t pitchU, int32_t yawU, int32_t rollU, float* fwd, float* right, float* up) {
    const float k = 6.28318530718f / 65536.0f;
    float cp = std::cos(pitchU * k), sp = std::sin(pitchU * k);
    float cy = std::cos(yawU * k), sy = std::sin(yawU * k);
    float cr = std::cos(rollU * k), sr = std::sin(rollU * k);
    // Convenzione FRotationMatrix di UE3.
    fwd[0] = cp * cy; fwd[1] = cp * sy; fwd[2] = sp;
    right[0] = sr * sp * cy - cr * sy; right[1] = sr * sp * sy + cr * cy; right[2] = -sr * cp;
    up[0] = -(cr * sp * cy + sr * sy); up[1] = cy * sr - cr * sp * sy; up[2] = cr * cp;
}


// Calcola dove cade il raggio del controller (trace del motore lungo la mira
// del gioco) e lo proietta nella vista dell'occhio corrente.
void UpdateAimMarker(const float* aimOrigin, const float* eyeLoc, const int32_t* viewRot);

// --- Oggetti del motore ancora vivi ------------------------------------------
//
// La mod tiene puntatori a oggetti del gioco (scheletro, controlli IK, Pawn,
// componente dell'arma) per rimettere i valori originali. Quando un livello si
// scarica (cambio livello, morte) quegli oggetti vengono distrutti: scriverci
// corrompe la memoria e fa crashare il gioco (17/10, al caricamento di un
// livello). Prima di ogni scrittura: l'oggetto deve essere ancora in GObjects
// al suo indice (UObject +0x04) con la stessa classe (+0x34).
constexpr uintptr_t kGObjectsAddr = 0x014D41EC;
uint32_t ObjClass(void* obj) {
    uint32_t c = 0;
    if (obj) TrySafeReadUint32(static_cast<char*>(obj) + 0x34, &c);
    return c;
}
bool StillAlive(void* obj, uint32_t cls) {
    if (!obj || !cls) return false;
    uint32_t idx = 0, data = 0, num = 0, slot = 0;
    if (!TrySafeReadUint32(static_cast<char*>(obj) + 0x04, &idx) ||
        !TrySafeReadUint32(reinterpret_cast<void*>(kGObjectsAddr), &data) ||
        !TrySafeReadUint32(reinterpret_cast<void*>(kGObjectsAddr + 4), &num) || idx >= num ||
        !TrySafeReadUint32(reinterpret_cast<void*>(data + 4 * idx), &slot))
        return false;
    return slot == reinterpret_cast<uint32_t>(obj) && ObjClass(obj) == cls;
}


// --- Diagnostica dei proiettili (projlog) ---------------------------------------
//
// Razzi e granate (RPG, Milkor) non passano da CalcWeaponFire: per N secondi
// ogni frame si cercano in GObjects gli oggetti di classe Projectile e si
// registrano posizione e velocita' alla nascita e l'ultima posizione prima che
// spariscano (esplosione), da confrontare con l'asse della canna e il mirino.
constexpr uintptr_t kOffActorVelocity = 0x140;
bool SafeCopy(void* dst, const void* src, size_t n);
struct ProjTrack {
    void* obj;
    uint32_t cls;
    float start[3], vel[3], last[3];
    bool seen;
};
std::vector<ProjTrack> g_proj;
ULONGLONG g_projUntil = 0;
void* g_projClass = nullptr;

bool IsProjectile(void* obj) {
    uint32_t c = ObjClass(obj);
    for (int i = 0; c && i < 10; ++i) {
        if (reinterpret_cast<void*>(c) == g_projClass) return true;
        if (!TrySafeReadUint32(reinterpret_cast<char*>(c) + 0x3C, &c)) return false;
    }
    return false;
}

void LogProjectile(const ProjTrack& t, const char* what) {
    char b[400];
    snprintf(b, sizeof(b), "PROJECTILE %s: %s start=(%.1f, %.1f, %.1f) velocity=(%.1f, %.1f, %.1f) last=(%.1f, %.1f, %.1f)",
             what, vr::script::ObjectName(reinterpret_cast<void*>(t.cls)).c_str(), t.start[0], t.start[1], t.start[2],
             t.vel[0], t.vel[1], t.vel[2], t.last[0], t.last[1], t.last[2]);
    vr::log::Line(b);
}

void UpdateProjectileLog() {
    int secs = vr::control::g_projLogSeconds.exchange(0);
    ULONGLONG now = GetTickCount64();
    if (secs > 0) {
        g_projUntil = now + secs * 1000ULL;
        if (!g_projClass) {
            uint32_t data = 0, num = 0;
            TrySafeReadUint32(reinterpret_cast<void*>(kGObjectsAddr), &data);
            TrySafeReadUint32(reinterpret_cast<void*>(kGObjectsAddr + 4), &num);
            for (uint32_t i = 0; i < num && !g_projClass; ++i) {
                uint32_t o = 0;
                if (TrySafeReadUint32(reinterpret_cast<void*>(data + 4 * i), &o) && o &&
                    vr::script::ObjectName(reinterpret_cast<void*>(o)) == "Projectile" &&
                    vr::script::ClassName(reinterpret_cast<void*>(o)) == "Class")
                    g_projClass = reinterpret_cast<void*>(o);
            }
        }
        // Proiettili gia' presenti: non vanno registrati come nuovi.
        g_proj.clear();
        vr::log::Line(std::string("PROJECTILE: watching for ") + std::to_string(secs) + " s");
    }
    if (!g_projUntil || !g_projClass) return;
    for (auto& t : g_proj) t.seen = false;
    uint32_t data = 0, num = 0;
    TrySafeReadUint32(reinterpret_cast<void*>(kGObjectsAddr), &data);
    TrySafeReadUint32(reinterpret_cast<void*>(kGObjectsAddr + 4), &num);
    bool first = secs > 0;
    for (uint32_t i = 0; i < num; ++i) {
        uint32_t o = 0;
        if (!TrySafeReadUint32(reinterpret_cast<void*>(data + 4 * i), &o) || !o) continue;
        void* obj = reinterpret_cast<void*>(o);
        if (!IsProjectile(obj)) continue;
        ProjTrack* t = nullptr;
        for (auto& x : g_proj)
            if (x.obj == obj) t = &x;
        if (!t) {
            ProjTrack n = {obj, ObjClass(obj), {}, {}, {}, true};
            SafeCopy(n.start, static_cast<char*>(obj) + 0x54, 12);
            SafeCopy(n.vel, static_cast<char*>(obj) + kOffActorVelocity, 12);
            std::memcpy(n.last, n.start, 12);
            g_proj.push_back(n);
            if (!first) LogProjectile(n, "spawned");
            continue;
        }
        t->seen = true;
        SafeCopy(t->last, static_cast<char*>(obj) + 0x54, 12);
    }
    if (first) return;  // all'avvio si prendono solo quelli gia' in volo, senza registrarli
    for (auto it = g_proj.begin(); it != g_proj.end();) {
        if (!it->seen) {
            LogProjectile(*it, "gone");
            it = g_proj.erase(it);
        } else {
            ++it;
        }
    }
    if (now > g_projUntil) {
        for (auto& t : g_proj) LogProjectile(t, "still flying");
        g_proj.clear();
        g_projUntil = 0;
        vr::log::Line("PROJECTILE: watch ended");
    }
}

constexpr uintptr_t kOffCompLocalToWorld = 0x70;  // PrimitiveComponent.LocalToWorld (righe X, Y, Z, T)

// --- Arma nella mano ---------------------------------------------------------
//
// Il modello dell'arma (YWeapon.m_weaponMeshComp, SkeletalMeshComponent) e'
// agganciato a una presa della mano di Walker. Attivando AbsoluteTranslation
// e AbsoluteRotation del componente e scrivendo Translation/Rotation in
// coordinate del mondo, il motore lo disegna dove sta il controller destro
// (ombre ed effetti agganciati all'arma compresi). Offset ricavati il 16/10
// dalle proprieta' UnrealScript (tools/ue3.py props <classe>):
constexpr uintptr_t kOffPawnCurrentWeapon = 0x1594;  // YGamePawn.m_currentWeapon
constexpr uintptr_t kOffWeaponMeshComp = 0x2E0;      // YWeapon.m_weaponMeshComp
constexpr uintptr_t kOffCompFlags = 0x14C;           // PrimitiveComponent, campo di bit
constexpr uint32_t kAbsTranslation = 0x80000, kAbsRotation = 0x100000;
constexpr uintptr_t kOffCompTranslation = 0x1C0;  // FVector
constexpr uintptr_t kOffCompRotation = 0x1CC;     // FRotator
void* g_gunComp = nullptr;                        // componente modificato (thread di gioco)
uint32_t g_gunCompCls = 0;
float g_gunEyeBase[3];                            // posizione del corpo dell'occhio sinistro
bool g_gunEyeBaseValid = false;

bool SafeSetGunPose(void* comp, bool enable, const float* pos, const int32_t* rot) {
    __try {
        char* c = static_cast<char*>(comp);
        uint32_t& flags = *reinterpret_cast<uint32_t*>(c + kOffCompFlags);
        if (enable) {
            std::memcpy(c + kOffCompTranslation, pos, 12);
            std::memcpy(c + kOffCompRotation, rot, 12);
            flags |= kAbsTranslation | kAbsRotation;
        } else {
            flags &= ~(kAbsTranslation | kAbsRotation);
            std::memset(c + kOffCompTranslation, 0, 24);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Classe dell'arma corrente e se e' una postazione fissa (YWeapon_Stationary:
// mitragliatrici fisse, armi di Humvee ed elicottero, mortaio), calcolate una
// volta per arma.
struct WeaponInfo {
    void* weapon = nullptr;
    std::string cls;
    bool stationary = false;
    bool launcher = false;  // YWeapon_Launcher: RPG, Milkor (proiettili veri)
};
WeaponInfo g_weaponInfo;

const WeaponInfo& CurrentWeaponInfo(void* weapon) {
    if (weapon != g_weaponInfo.weapon) {
        g_weaponInfo.weapon = weapon;
        g_weaponInfo.cls = weapon ? vr::script::ClassName(weapon) : std::string();
        g_weaponInfo.stationary = weapon && vr::script::IsA(weapon, "YWeapon_Stationary");
        g_weaponInfo.launcher = weapon && vr::script::IsA(weapon, "YWeapon_Launcher");
        // L'RPG spara solo mirando (LT): col controller LT si tiene da solo
        // mentre si preme il grilletto (audit 19/10).
        vr::control::g_weaponNeedsAim = g_weaponInfo.cls.find("RPG") != std::string::npos;
        // Caricatore per tipo d'arma (rispetto a magwell, tarato sui fucili):
        // pistole e Uzi nell'impugnatura sotto la mano, P90 sopra la canna,
        // fucili a tamburo/pompa piu' avanti.
        const std::string& c = g_weaponInfo.cls;
        float mf = 0.0f, md = 0.0f;
        if (c.find("DesertEagle") != std::string::npos || c.find("M9A1") != std::string::npos ||
            c.find("Python") != std::string::npos || c.find("MicroUzi") != std::string::npos ||
            c.find("Pistol") != std::string::npos) {
            mf = -4.0f;
            md = 2.0f;
        } else if (c.find("P90") != std::string::npos) {
            mf = 0.0f;
            md = -16.0f;
        } else if (c.find("AA12") != std::string::npos || c.find("Shotgun") != std::string::npos ||
                   c.find("Benelli") != std::string::npos) {
            mf = 8.0f;
        }
        vr::control::g_magwellClassCm[0] = mf;
        vr::control::g_magwellClassCm[1] = md;
        if (weapon)
            vr::log::Line("WEAPON: " + g_weaponInfo.cls + (g_weaponInfo.stationary ? " (mounted weapon: left in place)" : ""));
    }
    return g_weaponInfo;
}

void* CurrentGunComponent(void* pawn) {
    uint32_t weapon = 0, comp = 0;
    if (!pawn || !TrySafeReadUint32(static_cast<char*>(pawn) + kOffPawnCurrentWeapon, &weapon) || !weapon) return nullptr;
    if (CurrentWeaponInfo(reinterpret_cast<void*>(weapon)).stationary) return nullptr;
    if (!TrySafeReadUint32(reinterpret_cast<char*>(weapon) + kOffWeaponMeshComp, &comp)) return nullptr;
    return reinterpret_cast<void*>(comp);
}

void ReleaseGun() {
    if (g_gunComp && StillAlive(g_gunComp, g_gunCompCls)) SafeSetGunPose(g_gunComp, false, nullptr, nullptr);
    g_gunComp = nullptr;
}

// Chiamata per ogni occhio con la posizione del corpo (senza testa) e la
// direzione del corpo. In stereo le due posizioni differiscono per lo
// spostamento dell'occhio: si usa il punto medio, applicato col secondo occhio.
std::atomic<bool> g_armsActive{false};

void UpdateGunInHand(const float* base, int32_t bodyYaw) {
    if (g_armsActive) {
        ReleaseGun();
        return;
    }
    char* cam = static_cast<char*>(g_cameraPtr.load());
    bool hand = vr::control::g_aimHand && vr::control::g_aimFollowsBody && g_aimValid && g_handValid;
    void* pawn = HasCameraSignature(cam) ? PlayerPawnIfViewTarget(cam) : nullptr;
    // Solo in prima persona: in terza persona la mano sarebbe davanti alla camera.
    bool fp = vr::control::g_firstPerson;
    void* comp = (vr::control::g_gunInHand && hand && fp && pawn) ? CurrentGunComponent(pawn) : nullptr;
    if (comp != g_gunComp) ReleaseGun();
    if (!comp) return;
    float center[3] = {base[0], base[1], base[2]};
    int eye = g_drawEye;
    if (eye == 0) {
        std::memcpy(g_gunEyeBase, base, sizeof(g_gunEyeBase));
        g_gunEyeBaseValid = true;
        return;
    }
    if (eye == 1 && g_gunEyeBaseValid) {
        for (int i = 0; i < 3; ++i) center[i] = 0.5f * (base[i] + g_gunEyeBase[i]);
    }
    g_gunEyeBaseValid = false;
    // Rotazione = mira (corpo + controller) con il rollio del controller.
    int32_t rot[3] = {g_bodyPitch.load() + g_aimDeltaPitch.load(), g_bodyYaw.load() + g_aimDeltaYaw.load(),
                      g_handRoll.load()};
    float b = bodyYaw * (6.28318530718f / 65536.0f);
    float f = g_handOffset[0], r = g_handOffset[1], u = g_handOffset[2];
    float pos[3] = {center[0] + f * std::cos(b) - r * std::sin(b), center[1] + f * std::sin(b) + r * std::cos(b),
                    center[2] + u};
    // Offset dell'impugnatura nel riferimento del controller.
    float gf[3], gr[3], gu[3];
    RotatorToAxes(rot[0], rot[1], rot[2], gf, gr, gu);
    float k = 0.01f * vr::control::g_posScale;
    float cls[3] = {0, 0, 0};
    vr::control::GunOffsetForClass(g_weaponInfo.cls, cls);
    float of = (vr::control::g_gunOffsetCm[0] + cls[0]) * k, orr = (vr::control::g_gunOffsetCm[1] + cls[1]) * k,
          ou = (vr::control::g_gunOffsetCm[2] + cls[2]) * k;
    for (int i = 0; i < 3; ++i) pos[i] += gf[i] * of + gr[i] * orr + gu[i] * ou;
    if (SafeSetGunPose(comp, true, pos, rot)) {
        g_gunComp = comp;
        g_gunCompCls = ObjClass(comp);
    } else {
        g_gunComp = nullptr;
    }
}


// --- Braccia di Walker guidate dai controller -------------------------------
//
// L'AnimTree di Walker ha gia' i controlli IK delle mani (SkelControlLimb
// "IK_RightHand"/"IK_LeftHand": IK a due ossa spalla-gomito-polso) e della
// rotazione del polso (SkelControlSingleBone "IKRot_RightHand"/"IKRot_LeftHand"),
// usati dal gioco per l'impugnatura delle armi. Ogni frame la mod ci scrive la
// posa dei controller nello spazio dell'attore (Pawn), cosi' lo spostamento
// del corpo tra un frame e l'altro non introduce ritardo; il motore li applica
// all'aggiornamento dello scheletro e l'arma, attaccata alla mano destra, segue.
// Offset dalle proprieta' UnrealScript (tools/ue3.py props).
constexpr uintptr_t kOffPawnMesh = 0x3A8;             // Pawn.Mesh
constexpr uintptr_t kOffActorLocation = 0x54, kOffActorRotation = 0x60, kOffActorDrawScale = 0x6C;
constexpr uintptr_t kOffCtrlStrength = 0x68, kOffCtrlFlags = 0x78, kOffCtrlStrengthTarget = 0x7C,
                    kOffCtrlBlendTimeToGo = 0x80;     // SkelControlBase
constexpr uint32_t kCtrlByAnimMetadata = 0x8;
// bSetStrengthFromAnimNode: forza ricalcolata dal peso di nodi dell'animazione.
// Tolto ai controlli usati dalla mod (difensivo: le mani devono valere sempre).
constexpr uint32_t kCtrlStrengthFromAnimNode = 0x2;
// bIgnoreWhenNotRendered: controllo saltato se la mesh non risulta disegnata
// nel frame prima. Tolto anche questo (difensivo). Il fucile sdoppiato
// camminando non dipendeva da questi flag: vedi KeepGoodBones.
constexpr uint32_t kCtrlIgnoreWhenNotRendered = 0x20;
constexpr uintptr_t kOffCtrlBlendOutTime = 0x70;
constexpr uintptr_t kOffLimbEffector = 0xBC, kOffLimbEffectorSpace = 0xC8, kOffLimbJointSpace = 0xC9,
                    kOffLimbJoint = 0xD4, kOffLimbFlags = 0xE8, kOffLimbStretchLimits = 0xEC;  // SkelControlLimb
constexpr uint32_t kLimbAllowStretching = 0x10;
constexpr uintptr_t kOffSingleFlags = 0xBC, kOffSingleRotSpace = 0xCD, kOffSingleRot = 0xD8;  // SkelControlSingleBone
constexpr uint32_t kSingleApplyTranslation = 0x1, kSingleApplyRotation = 0x2, kSingleAddRotation = 0x8;
constexpr uint8_t kBcsActorSpace = 1;
constexpr size_t kCtrlSaveFrom = 0x68, kCtrlSaveLen = 0x90;  // campi modificati, ripristinati al rilascio
// SkeletalMeshComponent.SpaceBases / LocalAtoms: TArray di FBoneAtom (32 byte:
// quaternione, traslazione, scala), uno per osso.
constexpr uintptr_t kOffMeshSpaceBases = 0x274, kOffMeshLocalAtoms = 0x280;
constexpr size_t kBoneAtomSize = 32;

struct ArmRig {
    void* mesh = nullptr;
    uint32_t meshCls = 0;
    // limb destra, limb sinistra, rot destra, rot sinistra, e gli IK sulle ossa
    // ausiliarie (IKBone_*), che il gioco usa per alcune armi (RPG in spalla):
    // applicati dopo i nostri riporterebbero la mano sull'animazione, quindi
    // restano a forza 0 finche' le braccia seguono i controller.
    static constexpr int kCount = 6;
    void* ctrl[kCount] = {};
    uint32_t ctrlCls[kCount] = {};
    uint8_t saved[kCount][kCtrlSaveLen];
};
ArmRig g_rig;
// Assi della mano espressi nel riferimento dell'osso dell'arma (Weapon_Right /
// Weapon_Left, figli della mano: relazione fissa). L'arma e' agganciata a
// quell'osso: orientando la mano cosi', l'arma ha l'orientamento del controller.
float g_handInWeapon[2][3][3];
bool g_handInWeaponValid[2] = {false, false};
float g_armEyeBase[3];
bool g_armEyeBaseValid = false;

bool SafeCopy(void* dst, const void* src, size_t n) {
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* FindSkelControl(void* mesh, const std::string& name) {
    struct {
        int32_t name[2];
        void* ret;
    } parms = {{vr::script::NameIndex(name), 0}, nullptr};
    if (parms.name[0] < 0 || !vr::script::CallFunction(mesh, "FindSkelControl", &parms)) return nullptr;
    return parms.ret;
}

void ReleaseArms() {
    for (int i = 0; i < ArmRig::kCount; ++i) {
        if (g_rig.ctrl[i] && StillAlive(g_rig.ctrl[i], g_rig.ctrlCls[i]))
            SafeCopy(static_cast<char*>(g_rig.ctrl[i]) + kCtrlSaveFrom, g_rig.saved[i], kCtrlSaveLen);
        g_rig.ctrl[i] = nullptr;
    }
    g_rig.mesh = nullptr;
    g_handInWeaponValid[0] = g_handInWeaponValid[1] = false;
    g_armsActive = false;
    g_leftDriven = false;
}

bool AcquireArms(void* mesh) {
    ReleaseArms();
    std::string names;
    for (int i = 0; i < ArmRig::kCount; ++i) {
        std::string n = i < 4 ? vr::control::ArmControlName(i) : (i == 4 ? "IKBone_RightHand" : "IKBone_LeftHand");
        void* c = FindSkelControl(mesh, n);
        names += " " + n + (c ? "=ok" : "=missing");
        if (!c || !SafeCopy(g_rig.saved[i], static_cast<char*>(c) + kCtrlSaveFrom, kCtrlSaveLen)) {
            g_rig.ctrl[i] = nullptr;
            continue;
        }
        g_rig.ctrl[i] = c;
        g_rig.ctrlCls[i] = ObjClass(c);
    }
    g_rig.mesh = mesh;
    g_rig.meshCls = ObjClass(mesh);
    vr::log::Line("ARMS: Walker's IK controls:" + names);
    return g_rig.ctrl[0] != nullptr;
}

// Assi (colonne avanti, destra, su) <-> FRotator, convenzione di UE3.
void AxesToRotator(const float* f, const float* r, const float* u, int32_t* rot) {
    const float k = 65536.0f / 6.28318530718f;
    float pitch = std::atan2(f[2], std::sqrt(f[0] * f[0] + f[1] * f[1]));
    float yaw = std::atan2(f[1], f[0]);
    float sf[3], sr[3], su[3];
    RotatorToAxes(static_cast<int32_t>(pitch * k), static_cast<int32_t>(yaw * k), 0, sf, sr, su);
    float roll = std::atan2(u[0] * sr[0] + u[1] * sr[1] + u[2] * sr[2], r[0] * sr[0] + r[1] * sr[1] + r[2] * sr[2]);
    rot[0] = static_cast<int32_t>(pitch * k);
    rot[1] = static_cast<int32_t>(yaw * k);
    rot[2] = static_cast<int32_t>(roll * k);
}

// Vettore del mondo -> spazio dell'attore (inversa di rotazione e scala).
void WorldToActor(const float* v, const float* af, const float* ar, const float* au, float scale, float* out) {
    out[0] = (v[0] * af[0] + v[1] * af[1] + v[2] * af[2]) / scale;
    out[1] = (v[0] * ar[0] + v[1] * ar[1] + v[2] * ar[2]) / scale;
    out[2] = (v[0] * au[0] + v[1] * au[1] + v[2] * au[2]) / scale;
}

bool WriteHandControls(void* limb, void* rotc, const float* eff, const float* joint, const int32_t* lrot) {
    __try {
        char* l = static_cast<char*>(limb);
        *reinterpret_cast<float*>(l + kOffCtrlStrength) = 1.0f;
        *reinterpret_cast<float*>(l + kOffCtrlStrengthTarget) = 1.0f;
        *reinterpret_cast<float*>(l + kOffCtrlBlendTimeToGo) = 0.0f;
        *reinterpret_cast<uint32_t*>(l + kOffCtrlFlags) &= ~(kCtrlByAnimMetadata | kCtrlStrengthFromAnimNode | kCtrlIgnoreWhenNotRendered);
        // Uno spegnimento dallo script del gioco (SetSkelControlActive) sfuma
        // in BlendOutTime: lunghissimo, fino al nostro aggiornamento non cambia nulla.
        *reinterpret_cast<float*>(l + kOffCtrlBlendOutTime) = 1000.0f;
        std::memcpy(l + kOffLimbEffector, eff, 12);
        l[kOffLimbEffectorSpace] = kBcsActorSpace;
        // Braccio un po' allungabile (fino al 25%): con le mani tese in avanti
        // il braccio di Walker (~24 unita') non arriverebbe ai controller.
        *reinterpret_cast<uint32_t*>(l + kOffLimbFlags) |= kLimbAllowStretching;
        const float stretch[2] = {1.0f, vr::control::g_armStretch};
        std::memcpy(l + kOffLimbStretchLimits, stretch, 8);
        std::memcpy(l + kOffLimbJoint, joint, 12);
        l[kOffLimbJointSpace] = kBcsActorSpace;
        if (rotc) {
            char* r = static_cast<char*>(rotc);
            *reinterpret_cast<float*>(r + kOffCtrlStrength) = 1.0f;
            *reinterpret_cast<float*>(r + kOffCtrlStrengthTarget) = 1.0f;
            *reinterpret_cast<float*>(r + kOffCtrlBlendTimeToGo) = 0.0f;
            *reinterpret_cast<uint32_t*>(r + kOffCtrlFlags) &= ~(kCtrlByAnimMetadata | kCtrlStrengthFromAnimNode | kCtrlIgnoreWhenNotRendered);
            *reinterpret_cast<float*>(r + kOffCtrlBlendOutTime) = 1000.0f;
            uint32_t& sf = *reinterpret_cast<uint32_t*>(r + kOffSingleFlags);
            sf = (sf | kSingleApplyRotation) & ~(kSingleAddRotation | kSingleApplyTranslation);
            r[kOffSingleRotSpace] = kBcsActorSpace;
            std::memcpy(r + kOffSingleRot, lrot, 12);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void GunKickNow(float* pitchDeg, float* back);
// Quanto la volata e' dentro un ostacolo (unita', lungo la linea spalla ->
// volata, da MuzzleStart): l'arma disegnata arretra di tanto (HandTarget).
std::atomic<float> g_muzzlePenetration{0.0f};
std::atomic<float> g_gunPushApplied{0.0f};  // arretramento gia' applicato (per misurare senza)

// Mano (0 destra, 1 sinistra): posa voluta nel mondo dal controller.
// axes: righe X, Y, Z dell'osso della mano, come BoneAxes.
void HandTarget(int side, const float* center, int32_t bodyYaw, const float* off, int32_t pitch, int32_t yaw, int32_t roll,
                float* pos, float axes[3][3]) {
    float b = bodyYaw * (6.28318530718f / 65536.0f);
    pos[0] = center[0] + off[0] * std::cos(b) - off[1] * std::sin(b);
    pos[1] = center[1] + off[0] * std::sin(b) + off[1] * std::cos(b);
    pos[2] = center[2] + off[2];
    // Assi del controller, poi rotazione della mano rispetto al controller
    // (specchiata per la sinistra) e posizione del polso.
    float kickDeg = 0.0f, kickBack = 0.0f;
    if (side == 0) GunKickNow(&kickDeg, &kickBack);
    // Arma contro un muro: arretra lungo la canna quanto la volata e' dentro
    // l'ostacolo (al massimo 20 unita', in ~60 ms), invece di attraversarlo
    // come nei giochi VR nativi (audit 19/10: il fucile entrava nella fusoliera).
    if (side == 0) {
        static float push = 0.0f;
        static double lastPushT = 0.0;
        double nowP = ProfMs();
        float dtP = lastPushT > 0.0 ? static_cast<float>((nowP - lastPushT) * 0.001) : 0.0f;
        lastPushT = nowP;
        float want = (std::min)(g_muzzlePenetration.load(), 20.0f);
        push += (want - push) * (dtP > 0.0f ? (std::min)(1.0f, dtP / 0.06f) : 1.0f);
        g_gunPushApplied = push;
        kickBack += push;
    }
    pitch += static_cast<int32_t>(kickDeg * (65536.0f / 360.0f));
    float cf[3], cr[3], cu[3];
    RotatorToAxes(pitch, yaw, roll, cf, cr, cu);
    for (int i = 0; i < 3; ++i) pos[i] -= cf[i] * kickBack;
    float mirror = side == 0 ? 1.0f : -1.0f;
    float k = 0.01f * vr::control::g_posScale;
    float wo[3] = {vr::control::g_armOffsetCm[0] * k, vr::control::g_armOffsetCm[1] * k * mirror,
                   vr::control::g_armOffsetCm[2] * k};
    for (int i = 0; i < 3; ++i) pos[i] += cf[i] * wo[0] + cr[i] * wo[1] + cu[i] * wo[2];
    const float d2u = 65536.0f / 360.0f;
    float of[3], orr[3], ou[3];
    RotatorToAxes(static_cast<int32_t>(vr::control::g_armRotDeg[0] * d2u),
                  static_cast<int32_t>(vr::control::g_armRotDeg[1] * d2u * mirror),
                  static_cast<int32_t>(vr::control::g_armRotDeg[2] * d2u * mirror), of, orr, ou);
    // Riferimento dell'arma = controller ruotato di armrot.
    float wf[3], wr[3], wu[3];
    for (int i = 0; i < 3; ++i) {
        wf[i] = cf[i] * of[0] + cr[i] * of[1] + cu[i] * of[2];
        wr[i] = cf[i] * orr[0] + cr[i] * orr[1] + cu[i] * orr[2];
        wu[i] = cf[i] * ou[0] + cr[i] * ou[1] + cu[i] * ou[2];
    }
    // Mano = riferimento dell'arma composto con la relazione mano/arma.
    float* hf = axes[0];
    float* hr = axes[1];
    float* hu = axes[2];
    if (g_handInWeaponValid[side]) {
        const float(*m)[3] = g_handInWeapon[side];
        for (int i = 0; i < 3; ++i) {
            hf[i] = wf[i] * m[0][0] + wr[i] * m[0][1] + wu[i] * m[0][2];
            hr[i] = wf[i] * m[1][0] + wr[i] * m[1][1] + wu[i] * m[1][2];
            hu[i] = wf[i] * m[2][0] + wr[i] * m[2][1] + wu[i] * m[2][2];
        }
    } else {
        std::memcpy(hf, wf, 12);
        std::memcpy(hr, wr, 12);
        std::memcpy(hu, wu, 12);
    }
}

// Posa della mano nel mondo -> controlli IK (spazio dell'attore).
void WriteHand(int side, const float* pos, const float axes[3][3], const float* actorLoc, const float* af,
               const float* ar, const float* au, float scale, bool debug) {
    void* limb = g_rig.ctrl[side];
    void* rotc = g_rig.ctrl[2 + side];
    if (!limb) return;
    float mirror = side == 0 ? 1.0f : -1.0f;
    const float* hf = axes[0];
    const float* hr = axes[1];
    const float* hu = axes[2];
    // Spazio dell'attore.
    float rel[3] = {pos[0] - actorLoc[0], pos[1] - actorLoc[1], pos[2] - actorLoc[2]};
    float eff[3], lf[3], lr[3], lu[3];
    WorldToActor(rel, af, ar, au, scale, eff);
    WorldToActor(hf, af, ar, au, 1.0f, lf);
    WorldToActor(hr, af, ar, au, 1.0f, lr);
    WorldToActor(hu, af, ar, au, 1.0f, lu);
    int32_t lrot[3];
    AxesToRotator(lf, lr, lu, lrot);
    // Gomito: in basso, verso l'esterno e un po' indietro rispetto alla mano.
    float joint[3] = {eff[0] - 30.0f, eff[1] + 45.0f * mirror, eff[2] - 60.0f};
    if (!WriteHandControls(limb, rotc, eff, joint, lrot)) {
        ReleaseArms();
        return;
    }
    if (debug) {
        char b3[240];
        snprintf(b3, sizeof(b3), "ARMS %s: wanted hand axes X=(%.2f %.2f %.2f) Y=(%.2f %.2f %.2f) Z=(%.2f %.2f %.2f)",
                 side == 0 ? "right" : "left", hf[0], hf[1], hf[2], hr[0], hr[1], hr[2], hu[0], hu[1], hu[2]);
        vr::log::Line(b3);
        char buf[320];
        snprintf(buf, sizeof(buf),
                 "ARMS %s: wanted hand world=(%.1f, %.1f, %.1f) actor=(%.1f, %.1f, %.1f) actor rot=(%d, %d, %d)",
                 side == 0 ? "right" : "left", pos[0], pos[1], pos[2], eff[0], eff[1], eff[2], lrot[0], lrot[1],
                 lrot[2]);
        vr::log::Line(buf);
    }
}

// Presa della mano sinistra sull'arma, come la fa il gioco: nell'animazione le
// ossa IK_Hand_R / IK_Hand_L stanno dove il gioco mette le mani sull'arma (con
// "arms off" coincidono con Hand_Right / Hand_Left) e i controlli IKBone_*
// portano le mani li'. La mod muove le ossa Hand_*, non le IK_*: la posa di
// IK_Hand_L rispetto a IK_Hand_R (dalle SpaceBases dell'ultimo aggiornamento)
// e' quindi la presa sull'astina rispetto alla mano destra, per l'arma in mano
// e con le dita dell'animazione chiuse sull'arma.
bool SupportGripRelation(void* mesh, float pos[3], float rot[3][3]) {
    static void* cachedMesh = nullptr;
    static int idxR = -1, idxL = -1;
    if (mesh != cachedMesh) {
        cachedMesh = mesh;
        idxR = idxL = -1;
        for (int i = 0; i < 256 && (idxR < 0 || idxL < 0); ++i) {
            struct {
                int32_t index;
                int32_t ret[2];
            } parms = {i, {0, 0}};
            if (!vr::script::CallFunction(mesh, "GetBoneName", &parms) || parms.ret[0] <= 0) break;
            std::string n = vr::script::NameFromIndex(parms.ret[0]);
            if (n == "IK_Hand_R") idxR = i;
            if (n == "IK_Hand_L") idxL = i;
        }
        vr::log::Line("ARMS: grip bones IK_Hand_R=" + std::to_string(idxR) + " IK_Hand_L=" + std::to_string(idxL));
    }
    if (idxR < 0 || idxL < 0) return false;
    uint32_t data = 0, num = 0;
    char* m = static_cast<char*>(mesh);
    if (!TrySafeReadUint32(m + kOffMeshSpaceBases, &data) || !TrySafeReadUint32(m + kOffMeshSpaceBases + 4, &num) ||
        !data || num <= static_cast<uint32_t>(idxR > idxL ? idxR : idxL) || num > 1024)
        return false;
    float atom[2][8];
    if (!SafeCopy(atom[0], reinterpret_cast<char*>(data) + idxR * kBoneAtomSize, 32) ||
        !SafeCopy(atom[1], reinterpret_cast<char*>(data) + idxL * kBoneAtomSize, 32))
        return false;
    float ax[2][3][3];
    for (int k = 0; k < 2; ++k) {
        float x = atom[k][0], y = atom[k][1], z = atom[k][2], w = atom[k][3];
        float n = x * x + y * y + z * z + w * w;
        if (!(n > 0.5f && n < 1.5f)) return false;
        ax[k][0][0] = 1 - 2 * (y * y + z * z); ax[k][0][1] = 2 * (x * y + w * z); ax[k][0][2] = 2 * (x * z - w * y);
        ax[k][1][0] = 2 * (x * y - w * z); ax[k][1][1] = 1 - 2 * (x * x + z * z); ax[k][1][2] = 2 * (y * z + w * x);
        ax[k][2][0] = 2 * (x * z + w * y); ax[k][2][1] = 2 * (y * z - w * x); ax[k][2][2] = 1 - 2 * (x * x + y * y);
    }
    float d[3] = {atom[1][4] - atom[0][4], atom[1][5] - atom[0][5], atom[1][6] - atom[0][6]};
    for (int r = 0; r < 3; ++r) {
        pos[r] = d[0] * ax[0][r][0] + d[1] * ax[0][r][1] + d[2] * ax[0][r][2];
        for (int c = 0; c < 3; ++c)
            rot[r][c] = ax[1][r][0] * ax[0][c][0] + ax[1][r][1] * ax[0][c][1] + ax[1][r][2] * ax[0][c][2];
    }
    // Pistole e Micro Uzi (a una mano: IK_Hand_L a ~30 unita') e animazioni
    // senza la sinistra sull'arma (corsa, lancio): nessuna presa. Fucili,
    // fucili a pompa, P90, RPG: 9-23 unita'.
    return pos[0] * pos[0] + pos[1] * pos[1] + pos[2] * pos[2] < 26.0f * 26.0f;
}

// Spalla (osso Arm_Left / Arm_Right) nel mondo e portata massima del braccio
// (lunghezza dell'animazione per armstretch). Lunghezze
// di braccio e avambraccio dalle LocalAtoms; l'allungamento dell'IK le scrive
// li' (braccio teso: 18,1 + 16,4 invece di 14,5 + 13,1), quindi si tiene la
// minima vista sui due bracci, che e' quella dell'animazione.
bool ArmReach(void* mesh, int side, float shoulder[3], float* reach) {
    static void* cachedMesh = nullptr;
    static int idx[2][2] = {{-1, -1}, {-1, -1}};  // [sinistro, destro][gomito, mano]
    static float minLen = 0.0f;
    if (mesh != cachedMesh) {
        cachedMesh = mesh;
        minLen = 0.0f;
        static const char* kNames[2][2] = {{"Ellbow_Left", "Hand_Left"}, {"Ellbow_Right", "Hand_Right"}};
        for (auto& a : idx) a[0] = a[1] = -1;
        for (int i = 0; i < 256; ++i) {
            struct {
                int32_t index;
                int32_t ret[2];
            } parms = {i, {0, 0}};
            if (!vr::script::CallFunction(mesh, "GetBoneName", &parms) || parms.ret[0] <= 0) break;
            std::string n = vr::script::NameFromIndex(parms.ret[0]);
            for (int a = 0; a < 2; ++a)
                for (int b = 0; b < 2; ++b)
                    if (n == kNames[a][b] && idx[a][b] < 0) idx[a][b] = i;
        }
    }
    uint32_t data = 0, num = 0;
    char* m = static_cast<char*>(mesh);
    if (!TrySafeReadUint32(m + kOffMeshLocalAtoms, &data) || !TrySafeReadUint32(m + kOffMeshLocalAtoms + 4, &num) ||
        !data || num > 1024)
        return false;
    for (const auto& a : idx) {
        if (a[0] < 0 || a[1] < 0 || num <= static_cast<uint32_t>(a[0] > a[1] ? a[0] : a[1])) continue;
        float te[3], th[3];
        if (!SafeCopy(te, reinterpret_cast<char*>(data) + a[0] * kBoneAtomSize + 16, 12) ||
            !SafeCopy(th, reinterpret_cast<char*>(data) + a[1] * kBoneAtomSize + 16, 12))
            continue;
        float len = std::sqrt(te[0] * te[0] + te[1] * te[1] + te[2] * te[2]) +
                    std::sqrt(th[0] * th[0] + th[1] * th[1] + th[2] * th[2]);
        if (len > 10.0f && len < 80.0f && (minLen == 0.0f || len < minLen)) minLen = len;
    }
    if (minLen == 0.0f) return false;
    struct {
        int32_t name[2];
        int32_t space;
        float ret[3];
    } hp = {{vr::script::NameIndex(side == 0 ? "Arm_Right" : "Arm_Left"), 0}, 0, {0, 0, 0}};
    if (hp.name[0] < 0 || !vr::script::CallFunction(mesh, "GetBoneLocation", &hp)) return false;
    std::memcpy(shoulder, hp.ret, 12);
    *reach = minLen * vr::control::g_armStretch;
    return true;
}

// Assi (righe X, Y, Z, convenzione di BoneAxes) <-> quaternione (x, y, z, w).
void AxesToQuat(const float m[3][3], float q[4]) {
    float tr = m[0][0] + m[1][1] + m[2][2];
    if (tr > 0.0f) {
        float w = std::sqrt(1.0f + tr) * 0.5f, k = 0.25f / w;
        q[0] = (m[1][2] - m[2][1]) * k; q[1] = (m[2][0] - m[0][2]) * k; q[2] = (m[0][1] - m[1][0]) * k; q[3] = w;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        float x = std::sqrt(1.0f + m[0][0] - m[1][1] - m[2][2]) * 0.5f, k = 0.25f / x;
        q[0] = x; q[1] = (m[0][1] + m[1][0]) * k; q[2] = (m[0][2] + m[2][0]) * k; q[3] = (m[1][2] - m[2][1]) * k;
    } else if (m[1][1] > m[2][2]) {
        float y = std::sqrt(1.0f + m[1][1] - m[0][0] - m[2][2]) * 0.5f, k = 0.25f / y;
        q[0] = (m[0][1] + m[1][0]) * k; q[1] = y; q[2] = (m[1][2] + m[2][1]) * k; q[3] = (m[2][0] - m[0][2]) * k;
    } else {
        float z = std::sqrt(1.0f + m[2][2] - m[0][0] - m[1][1]) * 0.5f, k = 0.25f / z;
        q[0] = (m[0][2] + m[2][0]) * k; q[1] = (m[1][2] + m[2][1]) * k; q[2] = z; q[3] = (m[0][1] - m[1][0]) * k;
    }
}
void QuatToAxes(const float q[4], float m[3][3]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float n = std::sqrt(x * x + y * y + z * z + w * w);
    if (n > 1e-6f) { x /= n; y /= n; z /= n; w /= n; }
    m[0][0] = 1 - 2 * (y * y + z * z); m[0][1] = 2 * (x * y + w * z); m[0][2] = 2 * (x * z - w * y);
    m[1][0] = 2 * (x * y - w * z); m[1][1] = 1 - 2 * (x * x + z * z); m[1][2] = 2 * (y * z + w * x);
    m[2][0] = 2 * (x * z + w * y); m[2][1] = 2 * (y * z - w * x); m[2][2] = 1 - 2 * (x * x + y * y);
}
// Interpolazione sferica tra due orientamenti (assi), t in [0,1].
void SlerpAxes(const float a[3][3], const float b[3][3], float t, float out[3][3]) {
    float qa[4], qb[4], q[4];
    AxesToQuat(a, qa);
    AxesToQuat(b, qb);
    float d = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3];
    if (d < 0.0f) {
        for (float& v : qb) v = -v;
        d = -d;
    }
    float wa = 1.0f - t, wb = t;
    if (d < 0.9995f) {
        float th = std::acos(d), sn = std::sin(th);
        wa = std::sin((1.0f - t) * th) / sn;
        wb = std::sin(t * th) / sn;
    }
    for (int i = 0; i < 4; ++i) q[i] = qa[i] * wa + qb[i] * wb;
    QuatToAxes(q, out);
}

// Assi (righe X, Y, Z) di un osso nel mondo da GetBoneQuaternion.
bool BoneAxes(void* mesh, const char* bone, float axes[3][3]) {
    struct {
        int32_t name[2];
        int32_t space;
        int32_t pad;
        float q[4];
    } parms = {{vr::script::NameIndex(bone), 0}, 0, 0, {0, 0, 0, 1}};
    if (parms.name[0] < 0 || !vr::script::CallFunction(mesh, "GetBoneQuaternion", &parms)) return false;
    float x = parms.q[0], y = parms.q[1], z = parms.q[2], w = parms.q[3];
    float n = x * x + y * y + z * z + w * w;
    if (n < 0.5f || n > 1.5f) return false;
    axes[0][0] = 1 - 2 * (y * y + z * z); axes[0][1] = 2 * (x * y + w * z); axes[0][2] = 2 * (x * z - w * y);
    axes[1][0] = 2 * (x * y - w * z); axes[1][1] = 1 - 2 * (x * x + z * z); axes[1][2] = 2 * (y * z + w * x);
    axes[2][0] = 2 * (x * z + w * y); axes[2][1] = 2 * (y * z - w * x); axes[2][2] = 1 - 2 * (x * x + y * y);
    return true;
}

// Riferimento dell'arma: per la mano destra il modello dell'arma in mano
// (LocalToWorld del componente: righe X, Y, Z), che puo' essere agganciato con
// una rotazione propria (17/10: l'RPG, portato in spalla, puntava 50 gradi in
// basso usando l'osso Weapon_Right); altrimenti l'osso Weapon_Right/Left.
bool WeaponAxes(void* mesh, int side, void* gunComp, float w[3][3]) {
    if (side == 0 && gunComp) {
        float m[16];
        if (SafeCopy(m, static_cast<char*>(gunComp) + kOffCompLocalToWorld, sizeof(m))) {
            bool ok = true;
            for (int r = 0; r < 3 && ok; ++r) {
                float len = std::sqrt(m[4 * r] * m[4 * r] + m[4 * r + 1] * m[4 * r + 1] + m[4 * r + 2] * m[4 * r + 2]);
                if (!(len > 0.5f && len < 2.0f)) ok = false;
                for (int c = 0; c < 3 && ok; ++c) w[r][c] = m[4 * r + c] / len;
            }
            if (ok) return true;
        }
    }
    return BoneAxes(mesh, side == 0 ? "Weapon_Right" : "Weapon_Left", w);
}

void MeasureHandInWeapon(void* mesh, int side, void* gunComp) {
    float h[3][3], w[3][3];
    const char* hand = side == 0 ? "Hand_Right" : "Hand_Left";
    const char* weap = side == 0 ? (gunComp ? "weapon mesh" : "Weapon_Right") : "Weapon_Left";
    if (!BoneAxes(mesh, hand, h) || !WeaponAxes(mesh, side, gunComp, w)) return;
    float v[3][3];
    for (int a = 0; a < 3; ++a)
        for (int b = 0; b < 3; ++b) v[a][b] = h[a][0] * w[b][0] + h[a][1] * w[b][1] + h[a][2] * w[b][2];
    auto diff = [](const float x[3][3], const float y[3][3]) {
        float d = 0.0f;
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b) d += std::fabs(x[a][b] - y[a][b]);
        return d;
    };
    // Una misura molto diversa (animazione di cambio arma, mira del gioco)
    // vale solo se la seguente la conferma; le piccole correzioni subito.
    static float cand[2][3][3];
    static bool haveCand[2] = {false, false};
    float change = g_handInWeaponValid[side] ? diff(v, g_handInWeapon[side]) : 99.0f;
    if (g_handInWeaponValid[side] && change > 0.15f) {
        if (!haveCand[side] || diff(v, cand[side]) > 0.1f) {
            std::memcpy(cand[side], v, sizeof(v));
            haveCand[side] = true;
            return;
        }
    }
    haveCand[side] = false;
    std::memcpy(g_handInWeapon[side], v, sizeof(v));
    g_handInWeaponValid[side] = true;
    if (change < 0.05f && !vr::control::g_armDebugFrames.load()) return;  // nel log solo i cambiamenti
    char buf[200];
    snprintf(buf, sizeof(buf), "ARMS: %s in the frame of %s: X=(%.2f %.2f %.2f) Y=(%.2f %.2f %.2f) Z=(%.2f %.2f %.2f)",
             hand, weap, g_handInWeapon[side][0][0], g_handInWeapon[side][0][1], g_handInWeapon[side][0][2],
             g_handInWeapon[side][1][0], g_handInWeapon[side][1][1], g_handInWeapon[side][1][2],
             g_handInWeapon[side][2][0], g_handInWeapon[side][2][1], g_handInWeapon[side][2][2]);
    vr::log::Line(buf);
}

void LogBones(void* mesh) {
    std::string line;
    for (int i = 0; i < 200; ++i) {
        struct {
            int32_t index;
            int32_t ret[2];
        } parms = {i, {0, 0}};
        if (!vr::script::CallFunction(mesh, "GetBoneName", &parms) || parms.ret[0] <= 0) break;
        line += " " + std::to_string(i) + ":" + vr::script::NameFromIndex(parms.ret[0]);
        if (line.size() > 400) {
            vr::log::Line("BONES:" + line);
            line.clear();
        }
    }
    vr::log::Line("BONES:" + line);
}

// Scala del mondo: altezza dello scheletro (osso piu' alto - piu' basso) e
// lunghezza delle braccia, da confrontare con un adulto vero (~1,8 m).
void LogBodySize(void* mesh) {
    float zmin = 1e9f, zmax = -1e9f;
    std::string top, bottom;
    for (int i = 0; i < 200; ++i) {
        struct {
            int32_t index;
            int32_t ret[2];
        } bn = {i, {0, 0}};
        if (!vr::script::CallFunction(mesh, "GetBoneName", &bn) || bn.ret[0] <= 0) break;
        struct {
            int32_t name[2];
            int32_t space;
            float ret[3];
        } bl = {{bn.ret[0], bn.ret[1]}, 0, {0, 0, 0}};
        if (!vr::script::CallFunction(mesh, "GetBoneLocation", &bl)) continue;
        std::string nm = vr::script::NameFromIndex(bn.ret[0]);
        if (bl.ret[2] > zmax) {
            zmax = bl.ret[2];
            top = nm;
        }
        if (bl.ret[2] < zmin) {
            zmin = bl.ret[2];
            bottom = nm;
        }
    }
    char b[200];
    snprintf(b, sizeof(b), "BODYSIZE: skeleton height %.1f units (%s %.1f .. %s %.1f)", zmax - zmin, top.c_str(), zmax,
             bottom.c_str(), zmin);
    vr::log::Line(b);
}

void LogBoneLocation(void* mesh, const char* bone) {
    struct {
        int32_t name[2];
        int32_t space;
        float ret[3];
    } parms = {{vr::script::NameIndex(bone), 0}, 0, {0, 0, 0}};
    if (parms.name[0] < 0) return;
    if (vr::script::CallFunction(mesh, "GetBoneLocation", &parms)) {
        char buf[160];
        snprintf(buf, sizeof(buf), "ARMS: bone %s in the world=(%.1f, %.1f, %.1f)", bone, parms.ret[0], parms.ret[1],
                 parms.ret[2]);
        vr::log::Line(buf);
    }
}

// --- Corpo di Walker in prima persona ---------------------------------------
//
// YGamePawn rende Walker trasparente quando la camera entra in un cilindro
// attorno a lui (m_viewTargetCamInvisibilityCylinder*, raggio/altezza: 30/40
// in piedi, 28/40 accucciato): in terza persona evita che copra la visuale, in
// prima persona la camera ci sta sempre dentro e mani e arma diventano
// semitrasparenti. In prima persona i cilindri vanno a zero e si nasconde
// solo la testa (HideBoneByName "head", con le ossa del viso che ne dipendono);
// fuori (cinematiche, terza persona) si rimette tutto com'era.
constexpr uintptr_t kOffPawnInvisCylinders = 0x1C08;  // 4 FVector2D: non-viewtarget/viewtarget, in piedi/accucciato
// La mesh si sposta anche un po' indietro (bodyback, unita' del gioco): la
// camera sta davanti al collo e guardando in basso si vedono petto e gambe.
constexpr uintptr_t kOffMeshTranslationX = 0x1C0;  // PrimitiveComponent.Translation.X
void* g_fpPawn = nullptr;
void* g_fpHeadMesh = nullptr;
uint32_t g_fpPawnCls = 0, g_fpHeadMeshCls = 0;
float g_fpSavedCylinders[8];
float g_fpSavedMeshX = 0.0f, g_fpSavedMeshY = 0.0f, g_fpSavedMeshZ = 0.0f;

bool SetCylinders(void* pawn, const float* v) { return SafeCopy(static_cast<char*>(pawn) + kOffPawnInvisCylinders, v, 32); }

// Testa e collo nascosti (il collo comprende la testa): con la sola testa,
// accucciati in copertura e guardando in basso si vedeva il moncone del collo
// con la nuca insanguinata (playtest 19/10, Gorge02). Spalle e braccia sono
// figlie della colonna, restano visibili.
// Abbassamento del Pawn dovuto all'accucciata del gioco (unita'): capsula di
// collisione (Pawn.CylinderComponent +0x3AC, CollisionHeight +0x200) rispetto
// all'altezza in piedi (la massima vista per questo Pawn).
float GameCrouchDrop(void* pawn) {
    static float standingH = 0.0f;
    static void* standingPawn = nullptr;
    uint32_t cyl = 0;
    float h = 0.0f;
    if (!pawn || !TrySafeReadUint32(static_cast<char*>(pawn) + 0x3AC, &cyl) || !cyl ||
        !SafeCopy(&h, reinterpret_cast<char*>(cyl) + 0x200, 4) || !(h > 5.0f && h < 200.0f))
        return 0.0f;
    if (pawn != standingPawn) {
        standingPawn = pawn;
        standingH = h;
    }
    if (h > standingH) standingH = h;
    return standingH - h;
}

void SetHeadHidden(void* mesh, bool hide) {
    for (const char* bone : {"Neck", "head"}) {
        struct {
            int32_t name[2];
            uint8_t physBodyOption;
            uint8_t pad[3];
        } parms = {{vr::script::NameIndex(bone), 0}, 0, {0, 0, 0}};
        if (parms.name[0] < 0) continue;
        vr::script::CallFunction(mesh, hide ? "HideBoneByName" : "UnHideBoneByName", &parms);
    }
}

void UpdateFirstPersonBody(void* pawn) {
    bool want = pawn && vr::control::g_firstPerson && !vr::control::g_cinemaActive && PawnAlive(pawn);
    uint32_t mesh = 0;
    if (want && !TrySafeReadUint32(static_cast<char*>(pawn) + kOffPawnMesh, &mesh)) want = false;
    if (g_fpPawn && (!want || pawn != g_fpPawn)) {
        if (StillAlive(g_fpPawn, g_fpPawnCls)) SetCylinders(g_fpPawn, g_fpSavedCylinders);
        if (g_fpHeadMesh && StillAlive(g_fpHeadMesh, g_fpHeadMeshCls)) {
            SetHeadHidden(g_fpHeadMesh, false);
            float xyz[3] = {g_fpSavedMeshX, g_fpSavedMeshY, g_fpSavedMeshZ};
            SafeCopy(static_cast<char*>(g_fpHeadMesh) + kOffMeshTranslationX, xyz, 12);
        }
        g_fpPawn = nullptr;
        g_fpHeadMesh = nullptr;
    }
    if (!want) return;
    if (!g_fpPawn) {
        if (!SafeCopy(g_fpSavedCylinders, static_cast<char*>(pawn) + kOffPawnInvisCylinders, 32)) return;
        g_fpPawn = pawn;
        g_fpPawnCls = ObjClass(pawn);
        vr::log::Line("BODY: first person, Walker opaque and head hidden.");
    }
    static const float zero[8] = {};
    SetCylinders(pawn, zero);  // ogni frame: lo script potrebbe rimetterli
    if (g_fpHeadMesh && !StillAlive(g_fpHeadMesh, g_fpHeadMeshCls)) g_fpHeadMesh = nullptr;
    if (mesh && reinterpret_cast<void*>(mesh) != g_fpHeadMesh) {
        if (g_fpHeadMesh) {
            float xyz[3] = {g_fpSavedMeshX, g_fpSavedMeshY, g_fpSavedMeshZ};
            SafeCopy(static_cast<char*>(g_fpHeadMesh) + kOffMeshTranslationX, xyz, 12);
        }
        SafeCopy(&g_fpSavedMeshX, reinterpret_cast<char*>(mesh) + kOffMeshTranslationX, 4);
        SafeCopy(&g_fpSavedMeshY, reinterpret_cast<char*>(mesh) + kOffMeshTranslationX + 4, 4);
        SafeCopy(&g_fpSavedMeshZ, reinterpret_cast<char*>(mesh) + kOffMeshTranslationX + 8, 4);
        SetHeadHidden(reinterpret_cast<void*>(mesh), true);
        g_fpHeadMesh = reinterpret_cast<void*>(mesh);
        g_fpHeadMeshCls = ObjClass(g_fpHeadMesh);
    }
    if (g_fpHeadMesh) {
        // Il corpo segue gli spostamenti orizzontali della testa (6DOF), cosi'
        // spalle e braccia restano sotto il giocatore quando si sporge o fa un
        // passo; massimo 60 unita'.
        float f = g_headOffset[0], r = g_headOffset[1];
        float len = std::sqrt(f * f + r * r);
        if (len > 60.0f) {
            f *= 60.0f / len;
            r *= 60.0f / len;
        }
        int32_t actorYaw = 0;
        SafeCopy(&actorYaw, static_cast<char*>(pawn) + kOffActorRotation + 4, 4);
        float a = (g_bodyYaw.load() - actorYaw) * (6.28318530718f / 65536.0f);
        // Indietro di bodyback (negativo = avanti: la camera e' fp forward
        // unita' davanti a Walker e con le spalle 18 unita' dietro gli occhi
        // le braccia non arrivavano ai controller, 19/10) e di altre
        // bodybacklook unita' guardando in basso oltre 30 gradi (fino a 60),
        // cosi' si vedono petto e gambe e non l'interno del colletto.
        float pitchDeg = static_cast<float>(g_bodyPitch.load() + g_deltaPitch.load()) * (360.0f / 65536.0f);
        if (pitchDeg > 180.0f) pitchDeg -= 360.0f;
        float look = (-pitchDeg - 30.0f) / 30.0f;
        look = look < 0.0f ? 0.0f : (look > 1.0f ? 1.0f : look);
        // Il corpo si allontana dalla direzione dello sguardo (non solo
        // indietro rispetto al corpo): guardando di lato o alle spalle gli
        // occhi, davanti a Walker, vedevano l'interno del colletto con la testa
        // nascosta ("corpo decapitato", 19/10). Oltre 60 gradi di testa girata
        // si allontana di altre bodybacklook unita', come guardando in basso.
        float headRel = a + g_deltaYaw.load() * (6.28318530718f / 65536.0f);
        float turned = (std::fabs(std::remainder(g_deltaYaw.load() * (6.28318530718f / 65536.0f), 6.28318530718f)) *
                            57.2957795f - 60.0f) / 60.0f;
        turned = turned < 0.0f ? 0.0f : (turned > 1.0f ? 1.0f : turned);
        // bodyback si applica lungo il corpo, l'extra lontano dallo sguardo:
        // con bodyback negativo (corpo avanti, sotto la testa del giocatore,
        // perche' le braccia arrivino ai controller) girando la testa il corpo
        // non va verso lo sguardo.
        float away = vr::control::g_bodyBackLook * (std::max)(look, turned);
        float back = vr::control::g_bodyBack;
        // Vista tirata indietro dal muro davanti (wall pull): il corpo, portato
        // avanti da bodyback negativo, torna indietro con lei fino alla posa
        // del gioco, altrimenti gli occhi finivano dentro le spalle e il petto
        // nel riparo (audit 19/10).
        if (back < 0.0f) back += (std::min)(g_wallPull.load(), -back);
        float bx = -back * std::cos(a) - away * std::cos(headRel), by = -back * std::sin(a) - away * std::sin(headRel);
        float xy[2] = {g_fpSavedMeshX + bx + f * std::cos(a) - r * std::sin(a),
                       g_fpSavedMeshY + by + f * std::sin(a) + r * std::cos(a)};
        if (!vr::control::g_bodyFollowsHead) {
            xy[0] = g_fpSavedMeshX + bx;
            xy[1] = g_fpSavedMeshY + by;
        }
        // Su un mezzo il corpo resta dove lo mette il gioco (niente corpo che
        // segue la testa ne' spostamento avanti/indietro): sull'elicottero
        // finiva fuori dalla porta.
        bool onMount = !PawnOnFirmGround(pawn);
        if (onMount) {
            xy[0] = g_fpSavedMeshX;
            xy[1] = g_fpSavedMeshY;
        }
        SafeCopy(static_cast<char*>(g_fpHeadMesh) + kOffMeshTranslationX, xy, 8);
        // In altezza il busto segue la testa vera: con "crouchview real" e
        // Walker accucciato dal gioco sale della stessa differenza della vista
        // (altrimenti spalle e braccia restavano 20 unita' sotto e l'arma
        // fluttuava senza mano, playtest 19/10); accucciandosi davvero scende
        // con la testa (prima gli occhi finivano dentro il petto, audit 19/10).
        float z = g_fpSavedMeshZ;
        // Guardando in basso (oltre 30 gradi) il rialzo si toglie: sotto di se'
        // si vede il corpo accucciato a terra, non l'interno del colletto.
        if (!onMount) {
            // Seduti non ci si puo' accovacciare davvero: vista come nel gioco (20/10).
            if (vr::control::g_crouchViewReal && !vr::control::g_seated) z += GameCrouchDrop(pawn) * (1.0f - look);
            float headDrop = g_headOffset[2].load();
            if (headDrop < 0.0f) z += headDrop;
        }
        SafeCopy(static_cast<char*>(g_fpHeadMesh) + kOffMeshTranslationX + 8, &z, 4);
    }
}

// Aiuto alla mira del pad: "friction" (rallenta la mira vicino ai nemici) e
// "aim snap-on" (aggancia la camera al bersaglio quando si mira). Col
// controller spostano la mira da soli: spenti finche' si mira con la mano.
constexpr uintptr_t kOffWeaponAssistFlags = 0x260;  // YWeapon: friction zoom/non zoom, snap-on
constexpr uint32_t kWeaponAssistBits = 0x1000 | 0x2000 | 0x20000;
constexpr uintptr_t kOffPcAimSnapByUser = 0x734;    // YPlayerController.m_aimSnapOnActiveByUser
bool GunBarrel(void* pawn, float* origin, float* axis);

// Rinculo del gioco: YWeapon.m_recoilDistortionRange[3] (+0x7E0, 3 x 8 byte)
// fa salire e deviare la mira (e con lei la vista e l'arma: raffica = 10 gradi
// in su, 2,4 di lato, audit 19/10). Con la mira dalla mano il rinculo lo da'
// il contraccolpo visivo dell'arma (GunKick), quindi qui si azzera.
constexpr uintptr_t kOffWeaponRecoilRange = 0x7E0;
void DisableGameRecoil(void* weapon) {
    if (!weapon) return;
    static const uint8_t zero[24] = {};
    SafeCopy(static_cast<char*>(weapon) + kOffWeaponRecoilRange, zero, sizeof(zero));
}

// Contraccolpo visivo dell'arma a ogni colpo: la canna sale e l'arma arretra,
// poi torna in ~80 ms. Solo sull'arma disegnata (la vista resta ferma); i colpi
// vanno comunque lungo la canna.
std::atomic<double> g_gunKickMs{0.0};
std::atomic<float> g_gunKickStrength{1.0f};
void GunKick(const std::string& cls) {
    static const char* kHeavy[] = {"Barrett", "Steyr", "Shotgun", "Benelli", "AA12", "SawedOff", "RPG", "Milkor",
                                   "Python", "DesertEagle", "Mk47"};
    float k = 1.0f;
    for (const char* h : kHeavy)
        if (cls.find(h) != std::string::npos) k = 2.2f;
    g_gunKickStrength = k;
    g_gunKickMs = ProfMs();
}
// Gradi di salita della canna e unita' di arretramento in questo istante.
void GunKickNow(float* pitchDeg, float* back) {
    double dt = ProfMs() - g_gunKickMs.load();
    if (dt < 0.0 || dt > 300.0) {
        *pitchDeg = *back = 0.0f;
        return;
    }
    // Sale in 15 ms, scende con costante 60 ms.
    float a = dt < 15.0 ? static_cast<float>(dt / 15.0) : static_cast<float>(std::exp(-(dt - 15.0) / 60.0));
    float k = g_gunKickStrength.load();
    *pitchDeg = 2.5f * k * a;
    *back = 0.8f * k * a;
}

void DisableAimAssist(char* cam, void* weapon) {
    if (g_armsActive) DisableGameRecoil(weapon);
    if (!vr::control::g_noAimAssist) return;
    __try {
        if (weapon) *reinterpret_cast<uint32_t*>(static_cast<char*>(weapon) + kOffWeaponAssistFlags) &= ~kWeaponAssistBits;
        if (HasCameraSignature(cam)) *(cam - 0x77C + kOffPcAimSnapByUser) = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}


// --- Proiettili lungo la canna (RPG, Milkor) ---------------------------------
//
// I colpi istantanei passano da CalcWeaponFire; razzi e granate invece il gioco
// li lancia nella direzione del corpo (17/10: con il controller girato di 30
// gradi il razzo partiva a 30 gradi dalla canna). Quando le munizioni dell'arma
// in mano calano, per qualche frame si cercano i proiettili nuovi lanciati da
// Walker (Instigator) e se ne ruota la velocita' dal riferimento del corpo a
// quello della canna: il razzo va dove punta la canna e la granata del Milkor
// conserva il suo tiro a parabola rispetto alla canna.
constexpr uintptr_t kOffActorInstigator = 0xE8;
int g_projRedirectFrames = 0;
std::vector<void*> g_projRedirected;

// Granata lanciata col gesto: per 2 s dal lancio la nuova granata di Walker
// (proiettile con "Grenade" nel nome, Instigator = Walker) prende la direzione
// della mano al momento del lancio, con la velocita' scelta dal gioco. Prima
// partiva verso il mirino, qualunque fosse il gesto.
void RedirectThrownGrenade(void* pawn) {
    static ULONGLONG handled = 0;
    unsigned long long stamp = vr::control::g_throwStamp.load();
    if (!pawn || !stamp || stamp == handled || GetTickCount64() - stamp > 2000) return;
    if (!g_projClass) {
        uint32_t d0 = 0, n0 = 0;
        TrySafeReadUint32(reinterpret_cast<void*>(kGObjectsAddr), &d0);
        TrySafeReadUint32(reinterpret_cast<void*>(kGObjectsAddr + 4), &n0);
        for (uint32_t i = 0; i < n0 && !g_projClass; ++i) {
            uint32_t o = 0;
            if (TrySafeReadUint32(reinterpret_cast<void*>(d0 + 4 * i), &o) && o &&
                vr::script::ObjectName(reinterpret_cast<void*>(o)) == "Projectile" &&
                vr::script::ClassName(reinterpret_cast<void*>(o)) == "Class")
                g_projClass = reinterpret_cast<void*>(o);
        }
        if (!g_projClass) return;
    }
    static int frame = 0;
    if (++frame % 2) return;  // la scansione degli oggetti costa: un frame su due
    // Direzione della mano (SteamVR: x destra, y su, -z avanti) -> mondo, come
    // gli spostamenti delle mani (riferimento del ricentramento, poi yaw del corpo).
    float t[3] = {vr::control::g_throwDir[0].load(), vr::control::g_throwDir[1].load(), vr::control::g_throwDir[2].load()};
    float sy = std::sin(g_hmdYawReference), cy = std::cos(g_hmdYawReference);
    float f = t[0] * sy - t[2] * cy, r = t[0] * cy + t[2] * sy, u = t[1];
    float b = g_bodyYaw.load() * (6.28318530718f / 65536.0f);
    float dir[3] = {f * std::cos(b) - r * std::sin(b), f * std::sin(b) + r * std::cos(b), u};
    uint32_t data = 0, num = 0;
    TrySafeReadUint32(reinterpret_cast<void*>(kGObjectsAddr), &data);
    TrySafeReadUint32(reinterpret_cast<void*>(kGObjectsAddr + 4), &num);
    for (uint32_t i = 0; i < num; ++i) {
        uint32_t o = 0, inst = 0;
        if (!TrySafeReadUint32(reinterpret_cast<void*>(data + 4 * i), &o) || !o) continue;
        void* obj = reinterpret_cast<void*>(o);
        if (!IsProjectile(obj)) continue;
        TrySafeReadUint32(static_cast<char*>(obj) + kOffActorInstigator, &inst);
        if (reinterpret_cast<void*>(inst) != pawn) continue;
        if (std::find(g_projRedirected.begin(), g_projRedirected.end(), obj) != g_projRedirected.end()) continue;
        std::string cls = vr::script::ClassName(obj);
        if (cls.find("Grenade") == std::string::npos && cls.find("grenade") == std::string::npos) continue;
        if (cls.find("Milkor") != std::string::npos) continue;  // lanciagranate: ci pensa RedirectNewProjectiles
        float v[3];
        if (!SafeCopy(v, static_cast<char*>(obj) + kOffActorVelocity, 12)) continue;
        float speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (speed < 100.0f) continue;  // ancora in mano
        float nv[3] = {dir[0] * speed, dir[1] * speed, dir[2] * speed};
        SafeCopy(static_cast<char*>(obj) + kOffActorVelocity, nv, 12);
        g_projRedirected.push_back(obj);
        if (g_projRedirected.size() > 64) g_projRedirected.erase(g_projRedirected.begin());
        handled = stamp;
        char buf[220];
        snprintf(buf, sizeof(buf), "GRENADE: %s thrown along the hand (%.2f, %.2f, %.2f), speed %.0f", cls.c_str(), dir[0],
                 dir[1], dir[2], speed);
        vr::log::Line(buf);
        return;
    }
}

void RedirectNewProjectiles(void* pawn) {
    static void* lastWeapon = nullptr;
    static int32_t lastAmmo[2] = {-1, -1};
    uint32_t weapon = 0;
    int32_t ammo[2] = {-1, -1};
    if (!pawn || !TrySafeReadUint32(static_cast<char*>(pawn) + kOffPawnCurrentWeapon, &weapon) || !weapon) {
        if (pawn) vr::control::g_weaponPtr = 0;
        return;
    }
    {
        int32_t a[2] = {-1, -1};
        SafeCopy(&a[0], reinterpret_cast<char*>(weapon) + 0x764, 4);
        SafeCopy(&a[1], reinterpret_cast<char*>(weapon) + 0x778, 4);
        vr::control::g_weaponPtr = weapon;
        vr::control::g_weaponAmmo[0] = a[0];
        vr::control::g_weaponAmmo[1] = a[1];
    }
    // Solo lanciatori (RPG, Milkor): la scansione non costa nulla con le altre armi.
    bool launcher = CurrentWeaponInfo(reinterpret_cast<void*>(weapon)).launcher;
    if (vr::control::g_fireLog) {
        static ULONGLONG last = 0;
        if (GetTickCount64() - last > 1000) {
            last = GetTickCount64();
            char b[160];
            snprintf(b, sizeof(b), "PROJECTILE redirect check: weapon=%s launcher=%d trigger=%d frames=%d",
                     g_weaponInfo.cls.c_str(), (int)launcher, (int)vr::control::g_rightTriggerHeld.load(), g_projRedirectFrames);
            vr::log::Line(b);
        }
    }
    if (!launcher) return;
    // Innesco: grilletto premuto (e qualche frame dopo) o caricatore/munizioni in calo
    // (m_currentAmmoCount +0x764, m_currentMagAmmo +0x778; con l'RPG il primo resta a 1).
    SafeCopy(&ammo[0], reinterpret_cast<char*>(weapon) + 0x764, 4);
    SafeCopy(&ammo[1], reinterpret_cast<char*>(weapon) + 0x778, 4);
    bool dropped = reinterpret_cast<void*>(weapon) == lastWeapon &&
                   ((lastAmmo[0] >= 0 && ammo[0] < lastAmmo[0]) || (lastAmmo[1] >= 0 && ammo[1] < lastAmmo[1]));
    if (dropped || vr::control::g_rightTriggerHeld) g_projRedirectFrames = 20;
    lastWeapon = reinterpret_cast<void*>(weapon);
    lastAmmo[0] = ammo[0];
    lastAmmo[1] = ammo[1];
    if (g_projRedirectFrames <= 0 || !vr::control::g_muzzleAim || !g_armsActive || !pawn) {
        if (g_projRedirectFrames > 0 && vr::control::g_fireLog) {
            static ULONGLONG last = 0;
            if (GetTickCount64() - last > 500) {
                last = GetTickCount64();
                vr::log::Line(std::string("PROJECTILE scan skipped: muzzleaim=") + (vr::control::g_muzzleAim ? "1" : "0") +
                              " arms=" + (g_armsActive ? "1" : "0"));
            }
        }
        return;
    }
    --g_projRedirectFrames;
    if (!g_projClass) {
        uint32_t data = 0, num = 0;
        TrySafeReadUint32(reinterpret_cast<void*>(kGObjectsAddr), &data);
        TrySafeReadUint32(reinterpret_cast<void*>(kGObjectsAddr + 4), &num);
        for (uint32_t i = 0; i < num && !g_projClass; ++i) {
            uint32_t o = 0;
            if (TrySafeReadUint32(reinterpret_cast<void*>(data + 4 * i), &o) && o &&
                vr::script::ObjectName(reinterpret_cast<void*>(o)) == "Projectile" &&
                vr::script::ClassName(reinterpret_cast<void*>(o)) == "Class")
                g_projClass = reinterpret_cast<void*>(o);
        }
        if (!g_projClass) return;
    }
    float bo[3], bd[3];
    if (!GunBarrel(pawn, bo, bd)) return;
    // Riferimento in cui il gioco ha lanciato (corpo) e riferimento della canna.
    float af[3], ar[3], au[3], cf[3], cr[3], cu[3];
    RotatorToAxes(g_bodyPitch.load(), g_bodyYaw.load(), 0, af, ar, au);
    float horiz = std::sqrt(bd[0] * bd[0] + bd[1] * bd[1]);
    RotatorToAxes(RadiansToUnreal(std::atan2(bd[2], horiz)), RadiansToUnreal(std::atan2(bd[1], bd[0])), 0, cf, cr, cu);
    uint32_t data = 0, num = 0;
    TrySafeReadUint32(reinterpret_cast<void*>(kGObjectsAddr), &data);
    TrySafeReadUint32(reinterpret_cast<void*>(kGObjectsAddr + 4), &num);
    for (uint32_t i = 0; i < num; ++i) {
        uint32_t o = 0, inst = 0;
        if (!TrySafeReadUint32(reinterpret_cast<void*>(data + 4 * i), &o) || !o) continue;
        void* obj = reinterpret_cast<void*>(o);
        if (!IsProjectile(obj)) continue;
        // Del giocatore: Instigator = Walker, oppure Owner = arma in mano o Walker
        // (il razzo dell'RPG e' la testata caricata nel tubo, di proprieta'
        // dell'arma, che al lancio viene staccata e accelerata).
        uint32_t projOwner = 0;
        TrySafeReadUint32(static_cast<char*>(obj) + 0xA8, &projOwner);  // Actor.Owner
        TrySafeReadUint32(static_cast<char*>(obj) + kOffActorInstigator, &inst);
        bool mineProj = reinterpret_cast<void*>(inst) == pawn || reinterpret_cast<void*>(projOwner) == pawn ||
                        projOwner == weapon;
        static ULONGLONG lastScanLog = 0;
        if (vr::control::g_fireLog && mineProj && GetTickCount64() - lastScanLog > 1000) {
            lastScanLog = GetTickCount64();
            std::string pcls = vr::script::ClassName(obj);
            uint32_t owner = projOwner;
            float vv[3] = {0, 0, 0};
            SafeCopy(vv, static_cast<char*>(obj) + kOffActorVelocity, 12);
            char b[240];
            snprintf(b, sizeof(b), "PROJECTILE scan: %s instigator=0x%08X owner=0x%08X pawn=%p mine=%d vel=(%.0f %.0f %.0f)",
                     pcls.c_str(), inst, owner, pawn, (int)mineProj, vv[0], vv[1], vv[2]);
            vr::log::Line(b);
        }
        if (!mineProj) continue;
        if (std::find(g_projRedirected.begin(), g_projRedirected.end(), obj) != g_projRedirected.end()) continue;
        float v[3];
        if (!SafeCopy(v, static_cast<char*>(obj) + kOffActorVelocity, 12)) continue;
        if (v[0] * v[0] + v[1] * v[1] + v[2] * v[2] < 100.0f * 100.0f) continue;  // non ancora lanciato
        float f = v[0] * af[0] + v[1] * af[1] + v[2] * af[2];
        float r = v[0] * ar[0] + v[1] * ar[1] + v[2] * ar[2];
        float u = v[0] * au[0] + v[1] * au[1] + v[2] * au[2];
        // Componente laterale tolta (il gioco converge dal punto di lancio verso
        // il mirino del corpo: lungo la canna non serve, a 20 m erano ~40 cm);
        // resta il pallonetto verticale, con la stessa velocita'.
        float speed = std::sqrt(f * f + r * r + u * u), fu = std::sqrt(f * f + u * u);
        if (fu > 1.0f) {
            f *= speed / fu;
            u *= speed / fu;
        }
        r = 0.0f;
        float nv[3];
        for (int k = 0; k < 3; ++k) nv[k] = cf[k] * f + cr[k] * r + cu[k] * u;
        SafeCopy(static_cast<char*>(obj) + kOffActorVelocity, nv, 12);
        float nh = std::sqrt(nv[0] * nv[0] + nv[1] * nv[1]);
        int32_t rot[3] = {RadiansToUnreal(std::atan2(nv[2], nh)), RadiansToUnreal(std::atan2(nv[1], nv[0])), 0};
        SafeCopy(static_cast<char*>(obj) + kOffActorRotation, rot, 12);
        vr::haptics::Shot(g_weaponInfo.cls, g_twoHandActive);
        GunKick(g_weaponInfo.cls);
        g_projRedirected.push_back(obj);
        if (g_projRedirected.size() > 64) g_projRedirected.erase(g_projRedirected.begin());
        if (vr::control::g_fireLog) {
            char b[300];
            snprintf(b, sizeof(b), "PROJECTILE redirected: %s velocity (%.1f, %.1f, %.1f) -> (%.1f, %.1f, %.1f), barrel (%.3f, %.3f, %.3f)",
                     vr::script::ClassName(obj).c_str(), v[0], v[1], v[2], nv[0], nv[1], nv[2], bd[0], bd[1], bd[2]);
            vr::log::Line(b);
        }
    }
}

// Arma in mano = postazione fissa (torretta, minigun dell'elicottero): la mira
// col controller ruota la torretta (UpdateHeadTracking), non la camera.
std::atomic<bool> g_onTurret{false};
std::atomic<int32_t> g_turretYawUnits{0};
bool TurretAimActive() {
    return g_onTurret.load() && vr::control::g_turretAimHand && vr::control::g_aimHand && !vr::control::g_cinemaActive;
}

void UpdateArms(const float* base, int32_t bodyYaw) {
    char* cam = static_cast<char*>(g_cameraPtr.load());
    bool hand = vr::control::g_aimHand && vr::control::g_aimFollowsBody && g_aimValid && g_handValid;
    void* pawn = HasCameraSignature(cam) ? PlayerPawnIfViewTarget(cam) : nullptr;
    // Scene guidate dal gioco (bCinematicMode: giocatore bloccato, porte,
    // arrampicate, prese): le mani seguono l'animazione del gioco, non i
    // controller che le staccavano dalle maniglie (audit 19/10).
    bool want = vr::control::g_arms && hand && vr::control::g_firstPerson && pawn && !vr::control::g_cinemaActive &&
                !g_gameLockedScene && PawnAlive(pawn);
    uint32_t weapon = 0, mesh = 0;
    {
        uint32_t w = 0;
        g_onTurret = pawn && TrySafeReadUint32(static_cast<char*>(pawn) + kOffPawnCurrentWeapon, &w) && w &&
                     CurrentWeaponInfo(reinterpret_cast<void*>(w)).stationary;
    }
    if (want && TrySafeReadUint32(static_cast<char*>(pawn) + kOffPawnCurrentWeapon, &weapon) && weapon &&
        CurrentWeaponInfo(reinterpret_cast<void*>(weapon)).stationary)
        want = false;  // postazioni fisse: le mani restano sulle maniglie dell'animazione
    if (want && !TrySafeReadUint32(static_cast<char*>(pawn) + kOffPawnMesh, &mesh)) want = false;
    if (!want || !mesh) {
        if (g_rig.mesh) ReleaseArms();
        return;
    }
    if (reinterpret_cast<void*>(mesh) != g_rig.mesh && !AcquireArms(reinterpret_cast<void*>(mesh))) {
        ReleaseArms();
        g_rig.mesh = reinterpret_cast<void*>(mesh);  // non ritentare a ogni frame con la stessa mesh
        return;
    }
    if (!g_rig.ctrl[0]) return;
    if (!StillAlive(g_rig.mesh, g_rig.meshCls) || !StillAlive(g_rig.ctrl[0], g_rig.ctrlCls[0])) {
        // Oggetti distrutti (livello scaricato): si abbandonano senza scriverci.
        for (auto& c : g_rig.ctrl) c = nullptr;
        g_rig.mesh = nullptr;
        g_armsActive = false;
        return;
    }
    for (int i = 1; i < ArmRig::kCount; ++i)
        if (g_rig.ctrl[i] && !StillAlive(g_rig.ctrl[i], g_rig.ctrlCls[i])) g_rig.ctrl[i] = nullptr;
    for (int i = 4; i < ArmRig::kCount; ++i) {
        if (!g_rig.ctrl[i]) continue;
        static const float zero3[3] = {0, 0, 0};
        char* c = static_cast<char*>(g_rig.ctrl[i]);
        SafeCopy(c + kOffCtrlStrength, zero3, 4);
        SafeCopy(c + kOffCtrlStrengthTarget, zero3, 4);
        SafeCopy(c + kOffCtrlBlendTimeToGo, zero3, 4);
        // Niente forza ricalcolata dai nodi d'animazione (difensivo).
        uint32_t cf = 0;
        if (SafeCopy(&cf, c + kOffCtrlFlags, 4)) {
            cf &= ~(kCtrlByAnimMetadata | kCtrlStrengthFromAnimNode);
            SafeCopy(c + kOffCtrlFlags, &cf, 4);
        }
    }
    if (vr::control::g_armBonesRequested.exchange(false)) LogBones(g_rig.mesh);
    if (vr::control::g_bodySizeRequested.exchange(false)) LogBodySize(g_rig.mesh);
    bool debugNow = vr::control::g_armDebugFrames.load() > 0;
    // Relazione mano/arma: rimisurata a ogni cambio d'arma e ogni ~30 frame,
    // solo con il modello davvero nella mano (durante il cambio d'arma il
    // fucile puo' stare sulla schiena o nella fondina).
    // La relazione valida resta in uso finche' una nuova non e' confermata
    // (MeasureHandInWeapon): prima la si buttava al cambio d'arma e durante
    // l'animazione la mano usava l'identita', ruotata di 90 gradi (audit 19/10).
    void* gunComp = CurrentGunComponent(pawn);
    static void* lastGunComp = nullptr;
    static int remeasure = 0;
    static bool measureDue[2] = {true, true};
    if (gunComp != lastGunComp) {
        lastGunComp = gunComp;
        measureDue[0] = true;
    }
    bool gunInHand = false;
    if (gunComp) {
        struct {
            int32_t name[2];
            int32_t space;
            float ret[3];
        } hp = {{vr::script::NameIndex("Hand_Right"), 0}, 0, {0, 0, 0}};
        float gm[16];
        if (hp.name[0] >= 0 && vr::script::CallFunction(g_rig.mesh, "GetBoneLocation", &hp) &&
            SafeCopy(gm, static_cast<char*>(gunComp) + kOffCompLocalToWorld, sizeof(gm))) {
            float dx = gm[12] - hp.ret[0], dy = gm[13] - hp.ret[1], dz = gm[14] - hp.ret[2];
            gunInHand = dx * dx + dy * dy + dz * dz < 20.0f * 20.0f;
        }
    }
    if (++remeasure >= 30) {
        remeasure = 0;
        if (gunInHand) measureDue[0] = true;
    }
    for (int side = 0; side < 2; ++side)
        if ((measureDue[side] || !g_handInWeaponValid[side] || debugNow) && (side == 1 || gunInHand || !gunComp)) {
            MeasureHandInWeapon(g_rig.mesh, side, side == 0 ? gunComp : nullptr);
            measureDue[side] = false;
        }
    // Punto medio tra gli occhi, come per l'arma.
    float center[3] = {base[0], base[1], base[2]};
    if (g_drawEye == 0) {
        std::memcpy(g_armEyeBase, base, sizeof(g_armEyeBase));
        g_armEyeBaseValid = true;
        return;
    }
    if (g_drawEye == 1 && g_armEyeBaseValid)
        for (int i = 0; i < 3; ++i) center[i] = 0.5f * (base[i] + g_armEyeBase[i]);
    g_armEyeBaseValid = false;
    float actorLoc[3];
    int32_t actorRot[3];
    float scale = 1.0f;
    char* p = static_cast<char*>(pawn);
    if (!SafeCopy(actorLoc, p + kOffActorLocation, 12) || !SafeCopy(actorRot, p + kOffActorRotation, 12) ||
        !SafeCopy(&scale, p + kOffActorDrawScale, 4))
        return;
    if (!(scale > 0.01f)) scale = 1.0f;
    float af[3], ar[3], au[3];
    RotatorToAxes(actorRot[0], actorRot[1], actorRot[2], af, ar, au);
    bool debug = vr::control::g_armDebugFrames.exchange(0) > 0;
    float rOff[3] = {g_handOffset[0], g_handOffset[1], g_handOffset[2]};
    float rPos[3], rAxes[3][3];
    HandTarget(0, center, bodyYaw, rOff, g_bodyPitch.load() + g_aimDeltaPitch.load(), g_bodyYaw.load() + g_aimDeltaYaw.load(),
               g_handRoll.load(), rPos, rAxes);
    WriteHand(0, rPos, rAxes, actorLoc, af, ar, au, scale, debug);
    // Mano sinistra: segue il controller; vicina all'astina si aggancia alla
    // presa del gioco rispetto alla mano destra (SupportGripRelation), con le
    // dita chiuse sull'arma, qualunque sia la rotazione del controller (calcio
    // da gunstock). Prima restava aperta dov'era il controller, o tornava
    // all'animazione del gioco, che non segue l'arma mossa dalla mod (19/10).
    static float gripBlend = 0.0f;
    static bool gripOn = false;
    static double gripLastMs = 0.0;
    double gripNow = ProfMs();
    float gripDt = gripLastMs > 0.0 ? static_cast<float>((gripNow - gripLastMs) * 0.001) : 0.0f;
    gripLastMs = gripNow;
    if (gripDt > 0.1f) gripDt = 0.1f;
    // Senza controller sinistro (spento, fuori vista) la sinistra impugna
    // l'arma; senza arma torna all'animazione del gioco. Prima restava
    // l'ultima posa scritta o l'animazione, che nel camion e uscendo dalla
    // copertura portava il pugno davanti alla vista (playtest 19/10).
    bool leftCtrl = g_leftValid.load();
    static bool leftReleased = false;
    if (g_rig.ctrl[1]) {
        float lOff[3] = {g_leftOffset[0], g_leftOffset[1], g_leftOffset[2]};
        float lPos[3] = {rPos[0], rPos[1], rPos[2]}, lAxes[3][3];
        std::memcpy(lAxes, rAxes, sizeof(lAxes));
        if (leftCtrl)
            HandTarget(1, center, bodyYaw, lOff, g_bodyPitch.load() + g_leftPitch.load(), g_bodyYaw.load() + g_leftYaw.load(),
                       g_leftRoll.load(), lPos, lAxes);
        // Relazione della presa fissata per arma: si accetta quando resta
        // ferma 0,3 s e poi segue solo variazioni piccole (3 unita'). Le
        // animazioni del gioco che spostano IK_Hand_L (ricarica, corsa, lancio)
        // non trascinano piu' via la mano dall'astina; per uscire dalla presa
        // si usa l'ultima relazione valida, senza scatti (audit 19/10).
        static void* relGun = nullptr;
        static bool relValid = false, haveLastRel = false;
        static float cPos[3], cRot[3][3], candPos[3];
        static double candSince = 0.0;
        float relPos[3], relRot[3][3], gPos[3], gAxes[3][3];
        if (gunComp != relGun) {
            relGun = gunComp;
            relValid = false;
            candSince = 0.0;
        }
        bool fresh = gunInHand && SupportGripRelation(g_rig.mesh, relPos, relRot);
        if (fresh) {
            auto near3 = [](const float* a, const float* b, float lim) {
                float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
                return x * x + y * y + z * z < lim * lim;
            };
            if (relValid) {
                if (near3(relPos, cPos, 3.0f)) {
                    std::memcpy(cPos, relPos, sizeof(cPos));
                    std::memcpy(cRot, relRot, sizeof(cRot));
                }
            } else if (candSince > 0.0 && near3(relPos, candPos, 1.5f)) {
                if (gripNow - candSince > 300.0) {
                    relValid = true;
                    std::memcpy(cPos, relPos, sizeof(cPos));
                    std::memcpy(cRot, relRot, sizeof(cRot));
                }
            } else {
                candSince = gripNow;
                std::memcpy(candPos, relPos, sizeof(candPos));
            }
        }
        if (relValid) haveLastRel = true;
        bool canGrip = vr::control::g_leftGrip && gunInHand && relValid;
        bool canPose = canGrip || (gripBlend > 0.0f && haveLastRel);
        if (canPose) {
            std::memcpy(relPos, cPos, sizeof(relPos));
            std::memcpy(relRot, cRot, sizeof(relRot));
        }
        float dist = 1e9f;
        if (canPose) {
            for (int i = 0; i < 3; ++i) {
                gPos[i] = rPos[i] + scale * (relPos[0] * rAxes[0][i] + relPos[1] * rAxes[1][i] + relPos[2] * rAxes[2][i]);
                for (int r = 0; r < 3; ++r)
                    gAxes[r][i] = relRot[r][0] * rAxes[0][i] + relRot[r][1] * rAxes[1][i] + relRot[r][2] * rAxes[2][i];
            }
            if (!leftCtrl) {
                std::memcpy(lPos, gPos, sizeof(lPos));
                std::memcpy(lAxes, gAxes, sizeof(lAxes));
            }
            float dx = lPos[0] - gPos[0], dy = lPos[1] - gPos[1], dz = lPos[2] - gPos[2];
            dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        }
        // Presa fuori portata (fucile teso in avanti o verso destra; la
        // camera e' piu' alta e avanti delle spalle di Walker): la presa
        // scorre lungo l'arma verso la mano destra quanto basta, al massimo
        // 14 unita', invece di lasciare la mano a mezz'aria.
        static float slide = 0.0f;
        float slideWant = 0.0f;
        float shoulder[3], reach = 0.0f;
        if (canGrip && ArmReach(g_rig.mesh, 1, shoulder, &reach)) {
            reach *= 0.97f;
            const float* f = rAxes[0];
            float D[3] = {gPos[0] - shoulder[0], gPos[1] - shoulder[1], gPos[2] - shoulder[2]};
            float dd = D[0] * D[0] + D[1] * D[1] + D[2] * D[2];
            if (dd > reach * reach) {
                float df = D[0] * f[0] + D[1] * f[1] + D[2] * f[2];
                float disc = df * df - (dd - reach * reach);
                slideWant = disc >= 0.0f ? df - std::sqrt(disc) : df;
                slideWant = slideWant < 0.0f ? 0.0f : (slideWant > 14.0f ? 14.0f : slideWant);
            }
        }
        // Sale in fretta (la mano deve arrivarci) e scende lenta: camminando le
        // spalle ondeggiano e la presa scorreva avanti e indietro di 8 unita'.
        slide += (slideWant - slide) * (std::min)(1.0f, gripDt / (slideWant > slide ? 0.08f : 0.8f));
        if (debug && canGrip) {
            char b[260];
            snprintf(b, sizeof(b),
                     "ARMS grip: rel=(%.1f %.1f %.1f) dist=%.1f shoulder=(%.1f, %.1f, %.1f) reach=%.1f slide=%.1f/%.1f",
                     relPos[0], relPos[1], relPos[2], dist, shoulder[0], shoulder[1], shoulder[2], reach, slide, slideWant);
            vr::log::Line(b);
        }
        if (canPose)
            for (int i = 0; i < 3; ++i) gPos[i] -= slide * rAxes[0][i];
        // Isteresi: presa entro leftgrip <cm>, rilascio oltre 1,4 volte.
        float engage = vr::control::g_leftGripCm * 0.01f * vr::control::g_posScale;
        bool on = canGrip && (!leftCtrl || dist < (gripOn ? engage * 1.4f : engage));
        if (on != gripOn) {
            char b[120];
            snprintf(b, sizeof(b), "ARMS: left hand %s the weapon (%.0f units from the grip).", on ? "on" : "off", dist);
            vr::log::Line(b);
        }
        gripOn = on;
        gripBlend += (on ? 1.0f : -1.0f) * gripDt / 0.12f;
        gripBlend = gripBlend < 0.0f ? 0.0f : (gripBlend > 1.0f ? 1.0f : gripBlend);
        if (gripBlend > 0.0f && canPose) {
            float t = gripBlend * gripBlend * (3.0f - 2.0f * gripBlend);
            for (int i = 0; i < 3; ++i) lPos[i] += (gPos[i] - lPos[i]) * t;
            float mixed[3][3];
            SlerpAxes(lAxes, gAxes, t, mixed);
            std::memcpy(lAxes, mixed, sizeof(mixed));
        }
        if (!leftCtrl && !canPose) {
            // Niente controller ne' arma: la sinistra torna al gioco.
            if (!leftReleased) {
                for (int i : {1, 3})
                    if (g_rig.ctrl[i] && StillAlive(g_rig.ctrl[i], g_rig.ctrlCls[i]))
                        SafeCopy(static_cast<char*>(g_rig.ctrl[i]) + kCtrlSaveFrom, g_rig.saved[i], kCtrlSaveLen);
                leftReleased = true;
            }
            g_leftDriven = false;
        } else {
            WriteHand(1, lPos, lAxes, actorLoc, af, ar, au, scale, debug);
            leftReleased = false;
            g_leftDriven = true;
        }
    } else {
        gripOn = false;
        gripBlend = 0.0f;
        g_leftDriven = false;
    }
    g_leftOnGun = gripOn;
    vr::control::g_leftOnGun = gripOn;
    g_armsActive = true;
    DisableAimAssist(cam, reinterpret_cast<void*>(weapon));
    if (debug) {
        float mo[3], md[3];
        if (GunBarrel(pawn, mo, md)) {
            char b5[300];
            snprintf(b5, sizeof(b5),
                     "ARMS: barrel origin=(%.1f, %.1f, %.1f) axis=(%.3f %.3f %.3f) barrel aim=%d (%d, %d) marker=(%.1f, %.1f, %.1f)",
                     mo[0], mo[1], mo[2], md[0], md[1], md[2], (int)g_muzzleAimValid.load(), g_muzzleAimPitch.load(),
                     g_muzzleAimYaw.load(), g_muzzleTarget[0], g_muzzleTarget[1], g_muzzleTarget[2]);
            vr::log::Line(b5);
        }
        char buf[200];
        snprintf(buf, sizeof(buf), "ARMS: pawn loc=(%.1f, %.1f, %.1f) rot=(%d, %d, %d) scale=%.2f eyes=(%.1f, %.1f, %.1f)",
                 actorLoc[0], actorLoc[1], actorLoc[2], actorRot[0], actorRot[1], actorRot[2], scale, center[0],
                 center[1], center[2]);
        vr::log::Line(buf);
        {
            float mf[3], mr[3], mu[3];
            RotatorToAxes(g_bodyPitch.load() + g_aimDeltaPitch.load(), g_bodyYaw.load() + g_aimDeltaYaw.load(), 0, mf, mr, mu);
            char b4[200];
            snprintf(b4, sizeof(b4), "ARMS: aim forward=(%.2f %.2f %.2f) hand roll=%d twohand=%d left valid=%d",
                     mf[0], mf[1], mf[2], g_handRoll.load(), (int)g_twoHandActive.load(), (int)g_leftValid.load());
            vr::log::Line(b4);
        }
        LogBoneLocation(g_rig.mesh, "Hand_Right");
        LogBoneLocation(g_rig.mesh, "Hand_Left");
        LogBoneLocation(g_rig.mesh, "Weapon_Right");
        for (const char* bn : {"Hand_Right", "Weapon_Right", "IK_Hand_R"}) {
            float ax[3][3];
            if (BoneAxes(g_rig.mesh, bn, ax)) {
                char b2[200];
                snprintf(b2, sizeof(b2), "ARMS: axes %s X=(%.2f %.2f %.2f) Y=(%.2f %.2f %.2f) Z=(%.2f %.2f %.2f)", bn,
                         ax[0][0], ax[0][1], ax[0][2], ax[1][0], ax[1][1], ax[1][2], ax[2][0], ax[2][1], ax[2][2]);
                vr::log::Line(b2);
            }
        }
        LogBoneLocation(g_rig.mesh, "Ellbow_Right");
        LogBoneLocation(g_rig.mesh, "Shoulder_Right");
    }
}

// Indicatore di mira: posizione in coordinate NDC della vista di ciascun
// occhio (calcolata sul thread di gioco, disegnata in Present).

class GetPlayerViewPointThunk {
public:
    void Detour(float* outLoc, int32_t* outRot) {
        g_GetPlayerViewPoint_Original(this, outLoc, outRot);
        if (!g_inCalcSceneView || !outRot || vr::control::g_cinemaActive) return;
        // Direzione del corpo (senza la testa): serve a ruotare lo
        // spostamento della testa nel mondo.
        bool hand = vr::control::g_aimHand && vr::control::g_aimFollowsBody && g_aimValid;
        // Rotazione del corpo letta una volta per tick (occhio sinistro) e
        // riusata per il destro: su una torretta in volo cambiava tra i due
        // disegni e gli occhi guardavano in direzioni diverse.
        static int32_t tickBody[2] = {0, 0};
        if (g_drawEye != 1) {
            tickBody[0] = g_bodyYaw.load();
            tickBody[1] = g_bodyPitch.load();
        }
        int32_t bodyYawT = tickBody[0], bodyPitchT = tickBody[1];
        int32_t bodyYaw = hand ? bodyYawT
                               : outRot[1] - (vr::control::g_aimFollowsBody ? 0 : HeadYaw());
        if (hand) {
            // La camera del gioco punta col controller: la vista e' corpo + testa.
            // Il rollio del gioco (scosse) resta fuori: solo quello della testa.
            outRot[0] = (TurretAimActive() ? 0 : bodyPitchT) + HeadPitch();
            outRot[1] = bodyYawT + HeadYaw();
            outRot[2] = 0;
            // Cannocchiale: la lente guarda lungo la canna (il reticolo e' dove
            // vanno i colpi), non dove guarda la testa.
            if (g_scopeActive && g_muzzleAimValid) {
                outRot[0] = g_muzzleAimPitch.load();
                outRot[1] = g_muzzleAimYaw.load();
            }
        } else if (vr::control::g_aimFollowsBody) {
            outRot[0] += HeadPitch();  // FRotator: Pitch, Yaw, Roll
            outRot[1] += HeadYaw();
        }
        outRot[2] += HeadRoll();
        // Display inclinati (Pimax e simili): ogni occhio guarda ruotato di
        // qualche grado verso l'esterno. Le tangenti di GetProjectionRaw sono
        // nel sistema dell'occhio, quindi la vista va ruotata come l'occhio
        // (solo con la proiezione decentrata, che usa quelle tangenti).
        float cf[3], cr[3], cu[3];
        if (g_drawEye >= 0 && g_drawOffCenter && vr::output::EyeCant(g_drawEye, cf, cr, cu)) {
            float hf[3], hr[3], hu[3];
            RotatorToAxes(outRot[0], outRot[1], outRot[2], hf, hr, hu);
            float ef[3], er[3], eu[3];
            for (int i = 0; i < 3; ++i) {
                ef[i] = cf[0] * hf[i] + cf[1] * hr[i] + cf[2] * hu[i];
                er[i] = cr[0] * hf[i] + cr[1] * hr[i] + cr[2] * hu[i];
                eu[i] = cu[0] * hf[i] + cu[1] * hr[i] + cu[2] * hu[i];
            }
            AxesToRotator(ef, er, eu, outRot);
        }
        if (outLoc) {
            float b = bodyYaw * (6.28318530718f / 65536.0f);
            float ho[3];
            HeadOffset(ho);
            float f = ho[0], r = ho[1], u = ho[2];
            // Su un mezzo la cabina non blocca i trace (collisione della testa
            // cieca): la testa sale al massimo 6 unita' sopra quella di Walker,
            // altrimenti alzandosi si usciva dal tetto dell'elicottero (19/10).
            {
                char* camU = static_cast<char*>(g_cameraPtr.load());
                void* pawnU = HasCameraSignature(camU) ? PlayerPawnIfViewTarget(camU) : nullptr;
                if (pawnU && !PawnOnFirmGround(pawnU) && u > 6.0f) u = 6.0f;
            }
            // Spostamento dell'occhio di questo Draw: tolto qui e rimesso alla
            // fine, cosi' smorzamento dell'altezza e collisione lavorano sul
            // centro della testa. Prima l'occhio destro prendeva l'altezza
            // smorzata dell'occhio sinistro (testa inclinata: occhi alla
            // stessa altezza, disparita' verticale) e la collisione accorciava
            // la distanza tra gli occhi vicino ai muri (audit 19/10).
            float eyeOff[3] = {0.0f, 0.0f, 0.0f};
            if (g_drawEye == 0 || g_drawEye == 1) std::memcpy(eyeOff, g_eyeOffsetApplied[g_drawEye.load()], sizeof(eyeOff));
            float rawLoc[3] = {outLoc[0], outLoc[1], outLoc[2]};  // dal gioco (diagnostica camlog)
            float base[3] = {outLoc[0] - eyeOff[0], outLoc[1] - eyeOff[1], outLoc[2] - eyeOff[2] + ViewHeightExtra()};
            // Su un mezzo (elicottero, camion) Walker e' seduto o inginocchiato:
            // l'altezza fissa sopra il Pawn metteva la vista nel tetto
            // dell'elicottero o sopra (19/10, nel visore). Si usa l'altezza della
            // sua testa: base del collo + 12 unita' (testa e collo sono nascosti
            // con scala 0, quindi "head" coincide con l'attacco del collo).
            {
                char* camM = static_cast<char*>(g_cameraPtr.load());
                void* pawnM = HasCameraSignature(camM) ? PlayerPawnIfViewTarget(camM) : nullptr;
                uint32_t meshM = 0;
                if (pawnM && !PawnOnFirmGround(pawnM) && TrySafeReadUint32(static_cast<char*>(pawnM) + kOffPawnMesh, &meshM) &&
                    meshM) {
                    struct {
                        int32_t name[2];
                        int32_t space;
                        float ret[3];
                    } hp = {{vr::script::NameIndex("Neck"), 0}, 0, {0, 0, 0}}, sp = {{vr::script::NameIndex("Spine3"), 0}, 0, {0, 0, 0}};
                    // Posizione completa dalla testa di Walker: collo + 12 unita'
                    // lungo la colonna (Spine3 -> Neck). Con l'elicottero
                    // inclinato nelle virate e nello schianto, la posizione del
                    // Pawn con l'altezza "verso l'alto del mondo" finiva sopra o
                    // sotto la cabina (19/10, nel visore).
                    if (hp.name[0] >= 0 && sp.name[0] >= 0 &&
                        vr::script::CallFunction(reinterpret_cast<void*>(meshM), "GetBoneLocation", &hp) &&
                        vr::script::CallFunction(reinterpret_cast<void*>(meshM), "GetBoneLocation", &sp)) {
                        float up[3] = {hp.ret[0] - sp.ret[0], hp.ret[1] - sp.ret[1], hp.ret[2] - sp.ret[2]};
                        float ul = std::sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
                        float pl[3];
                        int32_t pyaw = 0;
                        if (ul > 1.0f && up[2] / ul > 0.2f && SafeCopy(pl, static_cast<char*>(pawnM) + 0x54, 12) &&
                            SafeCopy(&pyaw, static_cast<char*>(pawnM) + 0x64, 4)) {
                            float eye[3];
                            for (int i = 0; i < 3; ++i) eye[i] = hp.ret[i] + up[i] / ul * 12.0f;
                            // Le ossa e il Pawn non sono aggiornati nello stesso
                            // momento rispetto all'elicottero che si muove: la testa
                            // dalle ossa oscillava di decine di unita' rispetto alla
                            // cabina (sopra o sotto l'elicottero, 19/10). Si tiene
                            // solo la sua posizione media rispetto al Pawn (che si
                            // muove rigido col mezzo), nel riferimento del Pawn,
                            // filtrata su ~1 s.
                            static float relF[3] = {0, 0, 0};
                            static bool haveRel = false;
                            static double lastRelT = 0.0;
                            float yr = pyaw * (6.28318530718f / 65536.0f), cy = std::cos(yr), sy = std::sin(yr);
                            float d[3] = {eye[0] - pl[0], eye[1] - pl[1], eye[2] - pl[2]};
                            float rel[3] = {d[0] * cy + d[1] * sy, -d[0] * sy + d[1] * cy, d[2]};
                            double nowR = ProfMs() * 0.001;
                            float dtR = lastRelT > 0.0 ? static_cast<float>(nowR - lastRelT) : 0.0f;
                            lastRelT = nowR;
                            bool sane = rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2] < 90.0f * 90.0f;
                            if (sane) {
                                if (!haveRel || dtR > 0.5f) {
                                    std::memcpy(relF, rel, sizeof(relF));
                                    haveRel = true;
                                } else {
                                    float k = 1.0f - std::exp(-dtR / 1.0f);
                                    for (int i = 0; i < 3; ++i) relF[i] += (rel[i] - relF[i]) * k;
                                }
                            }
                            if (haveRel) {
                                base[0] = pl[0] + relF[0] * cy - relF[1] * sy;
                                base[1] = pl[1] + relF[0] * sy + relF[1] * cy;
                                base[2] = pl[2] + relF[2];
                            }
                        }
                    }
                }
            }
            // Stessa posizione della testa per i due occhi del tick: ogni Draw
            // rilegge la camera del gioco, che su un mezzo in movimento
            // (elicottero) o in una scena scriptata cambia tra il disegno
            // dell'occhio sinistro e quello del destro: i due occhi vedevano il
            // mondo da punti diversi, immagine doppia (19/10, nel visore).
            {
                static float tickBaseXY[2] = {0.0f, 0.0f};
                static bool haveTickBase = false;
                if (g_drawEye == 1 && haveTickBase) {
                    base[0] = tickBaseXY[0];
                    base[1] = tickBaseXY[1];
                    haveTickBase = false;
                } else if (g_drawEye == 0) {
                    tickBaseXY[0] = base[0];
                    tickBaseXY[1] = base[1];
                    haveTickBase = true;
                }
            }
            // "crouchview real": accucciandosi (copertura) il gioco abbassa il
            // Pawn di 20 unita' (40 cm, capsula da 45 a 25, piedi fermi) e con
            // lui la vista, senza che il giocatore si muova. Si toglie quella
            // differenza: l'altezza della vista resta quella della testa vera
            // sul pavimento (audit 19/10). "crouchview game": come il gioco.
            if (vr::control::g_crouchViewReal && !vr::control::g_seated) {
                char* camC = static_cast<char*>(g_cameraPtr.load());
                void* pawnC = HasCameraSignature(camC) ? PlayerPawnIfViewTarget(camC) : nullptr;
                if (PawnOnFirmGround(pawnC)) base[2] += GameCrouchDrop(pawnC);  // sui mezzi come il gioco
            }
            // Scene guidate dal gioco in 3D (bCinematicMode con la camera su
            // Walker: si inginocchia, viene afferrato, si arrampica): la vista
            // scende con la testa di Walker invece di restare sospesa all'altezza
            // in piedi (audit 19/10). Solo in basso e smorzato (0,3 s): le
            // camminate guidate non fanno oscillare la vista.
            {
                static float drop = 0.0f;
                static double lastT = 0.0;
                float want = 0.0f;
                char* camK = static_cast<char*>(g_cameraPtr.load());
                void* pawnK = g_gameLockedScene && HasCameraSignature(camK) ? PlayerPawnIfViewTarget(camK) : nullptr;
                uint32_t meshK = 0;
                if (pawnK && g_drawEye != 1 && TrySafeReadUint32(static_cast<char*>(pawnK) + kOffPawnMesh, &meshK) && meshK) {
                    struct {
                        int32_t name[2];
                        int32_t space;
                        float ret[3];
                    } hp = {{vr::script::NameIndex("Neck"), 0}, 0, {0, 0, 0}};
                    if (hp.name[0] >= 0 && vr::script::CallFunction(reinterpret_cast<void*>(meshK), "GetBoneLocation", &hp)) {
                        float d = base[2] - (hp.ret[2] + 12.0f);  // occhi ~12 unita' sopra l'attacco del collo
                        want = d > 15.0f ? (d < 80.0f ? d - 15.0f : 65.0f) : 0.0f;
                    }
                }
                if (g_drawEye != 1) {
                    double now = ProfMs() * 0.001;
                    float dt = lastT > 0.0 ? static_cast<float>(now - lastT) : 0.0f;
                    lastT = now;
                    if (dt > 0.0f && dt < 0.5f) drop += (want - drop) * (1.0f - std::exp(-dt / 0.3f));
                    else drop = want;
                }
                base[2] -= drop;
            }
            // Altezza della vista ammorbidita (solo la vista: la mira resta
            // quella del gioco). Salti oltre 80 unita' (teletrasporti,
            // checkpoint) passano subito.
            static float smoothZ = 0.0f;
            if (g_drawEye == 1) {
                base[2] = smoothZ;  // stesso centro dell'occhio sinistro di questo tick
            } else {
                static double lastT = 0.0;
                double now = ProfMs() * 0.001;
                float tau = vr::control::g_eyeHeightSmooth;
                // Su un mezzo niente smorzamento: l'elicottero sale e scende
                // veloce e la vista restava indietro, sopra o sotto la cabina.
                {
                    char* camS = static_cast<char*>(g_cameraPtr.load());
                    void* pawnS = HasCameraSignature(camS) ? PlayerPawnIfViewTarget(camS) : nullptr;
                    if (pawnS && !PawnOnFirmGround(pawnS)) tau = 0.0f;
                }
                if (lastT == 0.0 || tau <= 0.0f || std::fabs(base[2] - smoothZ) > 80.0f || now - lastT > 0.5) {
                    smoothZ = base[2];
                } else {
                    float a = 1.0f - std::exp(-static_cast<float>(now - lastT) / tau);
                    smoothZ += (base[2] - smoothZ) * a;
                }
                lastT = now;
                base[2] = smoothZ;
            }
            // Protezione della testa: se tra il corpo e la testa c'e' un
            // ostacolo, la testa si ferma ~8 unita' prima. Il controllo parte
            // dal centro del Pawn (all'altezza degli occhi), sempre libero:
            // gli occhi sono g_fpForward unita' piu' avanti e con Walker
            // contro un muro (ostaggi del cap. 2, 18/10) finivano dentro il
            // muro, il trace colpiva subito e la vista restava bloccata li'.
            float origin[3] = {base[0], base[1], base[2]};
            bool fromPawn = false;
            if (g_pawnViewLocValid && vr::control::g_headCollision) {
                float px = g_pawnLocAtView[0] - base[0], py = g_pawnLocAtView[1] - base[1];
                float lim = std::fabs(vr::control::g_fpForward.load()) + 45.0f;
                if (px * px + py * py < lim * lim) {  // stesso Pawn di questa vista
                    origin[0] = g_pawnLocAtView[0];
                    origin[1] = g_pawnLocAtView[1];
                    fromPawn = true;
                }
            }
            // Muro davanti al corpo: gli occhi stanno fp unita' davanti al
            // centro di Walker, quindi in copertura contro una parete alta
            // (Walker rivolto al muro a ~17 unita', ostaggio nell'aereo del
            // cap. 1, 19/10) finivano nel muro e la protezione li fermava a 8
            // unita' dalla superficie: tutta la vista era muro. La vista (con
            // braccia e arma) arretra lungo l'avanti del corpo finche' il muro
            // e' ad almeno kWallGap unita'. Il corpo resta dov'e': spostato
            // indietro con la vista (e con 20 unita') la posa di copertura si
            // vedeva deformata (19/10, nel visore).
            static float wallPull = 0.0f;  // calcolato sull'occhio sinistro
            if (g_drawEye != 1) {
                float want = 0.0f;
                float fx = base[0] - origin[0], fy = base[1] - origin[1];
                float fl = std::sqrt(fx * fx + fy * fy);
                if (fromPawn && fl > 1.0f) {
                    const float kWallGap = 12.0f, kMaxBehind = 10.0f;
                    fx /= fl;
                    fy /= fl;
                    float probe = fl + kWallGap;
                    float end[3] = {origin[0] + fx * probe, origin[1] + fy * probe, origin[2]};
                    char* camForTrace = static_cast<char*>(g_cameraPtr.load());
                    void* self = HasCameraSignature(camForTrace) ? PlayerPawnIfViewTarget(camForTrace) : nullptr;
                    float hitTime;
                    if (TraceLine(self, origin, end, &hitTime)) {
                        want = fl - (hitTime * probe - kWallGap);  // fino a fl + kWallGap
                        if (want > fl) {
                            // Dietro il centro: solo se alle spalle c'e' spazio.
                            float maxBack = kMaxBehind;
                            float bend[3] = {origin[0] - fx * (kMaxBehind + 8.0f), origin[1] - fy * (kMaxBehind + 8.0f),
                                             origin[2]};
                            float bt;
                            if (TraceLine(self, origin, bend, &bt)) maxBack = bt * (kMaxBehind + 8.0f) - 8.0f;
                            maxBack = maxBack < 0.0f ? 0.0f : maxBack;
                            if (want > fl + maxBack) want = fl + maxBack;
                        }
                        if (want < 0.0f) want = 0.0f;
                    }
                    // Allontanandosi dal muro la vista torna avanti in ~0,2 s;
                    // avvicinandosi arretra al massimo a 150 unita'/s (prima di
                    // colpo: girandosi vicino a un angolo la vista saltava
                    // indietro di 25 unita' in un frame). La collisione della
                    // testa resta a proteggere dal muro.
                    static double lastPullT = 0.0;
                    double nowT = ProfMs() * 0.001;
                    float dt = static_cast<float>(nowT - lastPullT);
                    lastPullT = nowT;
                    if (dt > 0.0f && dt < 0.5f) {
                        if (want < wallPull) want = wallPull + (want - wallPull) * (1.0f - std::exp(-dt / 0.2f));
                        else if (want > wallPull + 150.0f * dt) want = wallPull + 150.0f * dt;
                    }
                    base[0] -= fx * want;
                    base[1] -= fy * want;
                }
                wallPull = want;
                g_wallPull.store(want, std::memory_order_relaxed);
            } else if (wallPull > 0.0f) {
                float fx = base[0] - origin[0], fy = base[1] - origin[1];
                float fl = std::sqrt(fx * fx + fy * fy);
                if (fl > 1.0f) {
                    base[0] -= fx / fl * wallPull;
                    base[1] -= fy / fl * wallPull;
                }
            }
            // UE3: X avanti, Y destra, Z su.
            float target[3] = {base[0] + f * std::cos(b) - r * std::sin(b), base[1] + f * std::sin(b) + r * std::cos(b),
                               base[2] + u};
            float dx = target[0] - origin[0], dy = target[1] - origin[1], dz = target[2] - origin[2];
            float len = std::sqrt(dx * dx + dy * dy + dz * dz);
            static float tickT = 1.0f;  // calcolato sull'occhio sinistro, riusato dal destro
            float t = 1.0f;
            static float smoothT = 1.0f;
            static double lastTT = 0.0;
            if (g_drawEye == 1) {
                t = tickT;
            } else if (vr::control::g_headCollision && len > 0.5f) {
                const float margin = 8.0f;
                float ext[3] = {origin[0] + dx / len * (len + margin), origin[1] + dy / len * (len + margin),
                                origin[2] + dz / len * (len + margin)};
                float hitTime;
                char* camForTrace = static_cast<char*>(g_cameraPtr.load());
                void* self = HasCameraSignature(camForTrace) ? PlayerPawnIfViewTarget(camForTrace) : nullptr;
                if (TraceLine(self, origin, ext, &hitTime)) {
                    float allowed = hitTime * (len + margin) - margin;
                    t = allowed <= 0.0f ? 0.0f : (allowed < len ? allowed / len : 1.0f);
                    g_headBlocked.fetch_add(1, std::memory_order_relaxed);
                }
            }
            if (g_drawEye != 1) {
                // Uscendo dall'ostacolo la testa torna libera in ~0,1 s
                // (prima saltava avanti e indietro su ringhiere e spigoli);
                // entrando si ferma subito, per non vedere attraverso.
                double nowTT = ProfMs() * 0.001;
                float dtt = lastTT > 0.0 ? static_cast<float>(nowTT - lastTT) : 1.0f;
                lastTT = nowTT;
                if (t < smoothT || dtt > 0.5f) smoothT = t;
                else smoothT += (t - smoothT) * (1.0f - std::exp(-dtt / 0.1f));
                t = smoothT;
                tickT = t;
            }
            if (g_drawEye == 0) {
                for (int i = 0; i < 3; ++i) {
                    g_lastViewLoc[i] = outLoc[i];
                    g_lastViewRot[i] = outRot[i];
                    g_lastViewBase[i] = base[i];
                }
                static float lastView[3] = {0, 0, 0};
                float dj = std::sqrt((outLoc[0] - lastView[0]) * (outLoc[0] - lastView[0]) +
                                     (outLoc[1] - lastView[1]) * (outLoc[1] - lastView[1]) +
                                     (outLoc[2] - lastView[2]) * (outLoc[2] - lastView[2]));
                if (dj < 500.0f && dj > g_maxViewJump) g_maxViewJump = dj;  // >500: teletrasporto/checkpoint
                // Salto della vista oltre 80 unita' in un tick (checkpoint,
                // ripartenza, teletrasporto scriptato): dissolvenza dal nero.
                // Solo salti veri (il movimento del tick molto piu' grande dei
                // precedenti), fuori dalle scene scriptate: in elicottero o nelle
                // scene di schianto la vista si muove veloce in continuo e ogni
                // tick diventava una dissolvenza (19/10, nel visore: lag e
                // problemi nelle scene degli elicotteri).
                static float prevDj = 0.0f;
                if (dj > 80.0f && dj > 3.0f * prevDj + 40.0f && vr::control::g_fades && !g_gameLockedScene)
                    vr::output::Blink(0.3f);
                prevDj = dj;
                // Movimento forzato per la vignettatura: base della vista (senza
                // testa) che si muove senza lo stick (veicoli, spinte del gioco)
                // e rotazioni del corpo non fatte dal giocatore.
                {
                    static float lastBase[3] = {0, 0, 0};
                    static int32_t lastYaw = 0;
                    static double lastT = 0.0;
                    double now = ProfMs() * 0.001;
                    float dt = lastT > 0.0 ? static_cast<float>(now - lastT) : 0.0f;
                    lastT = now;
                    float bd = std::sqrt((base[0] - lastBase[0]) * (base[0] - lastBase[0]) +
                                         (base[1] - lastBase[1]) * (base[1] - lastBase[1]));
                    int32_t yawNow = g_bodyYaw.load();
                    float dyaw = std::fabs(static_cast<float>(static_cast<int16_t>(yawNow - lastYaw))) * (360.0f / 65536.0f);
                    float forced = 0.0f;
                    if (dt > 0.0f && dt < 0.2f && bd < 200.0f) {
                        float speed = bd / dt;  // unita'/s
                        if (vr::control::g_motionAmount < 0.1f) forced = (std::max)(forced, (speed - 40.0f) / 250.0f);
                        if (!vr::control::g_stickTurning) forced = (std::max)(forced, (dyaw / dt - 20.0f) / 120.0f);
                    }
                    vr::control::g_forcedMotion = forced < 0.0f ? 0.0f : (forced > 1.0f ? 1.0f : forced);
                    for (int i = 0; i < 3; ++i) lastBase[i] = base[i];
                    lastYaw = yawNow;
                }
                for (int i = 0; i < 3; ++i) lastView[i] = outLoc[i];
            }
            // Nel cannocchiale entrambi gli occhi vedono la stessa immagine
            // (niente disparita' ingrandita 5-6 volte). Non esattamente zero:
            // con origini identiche il riconoscimento dell'occhio (anello delle
            // origini) sceglieva a caso e le immagini si scambiavano; il 2%
            // della meta' IPD (~1,6 mm) basta a distinguerle (oltre la tolleranza di 0,01 anche a coordinate da 30000) e non si vede
            // (audit 20/10).
            float eyeK = g_scopeActive ? 0.05f : 1.0f;
            outLoc[0] = origin[0] + dx * t + eyeOff[0] * eyeK;
            outLoc[1] = origin[1] + dy * t + eyeOff[1] * eyeK;
            outLoc[2] = origin[2] + dz * t + eyeOff[2] * eyeK;
            if (vr::control::g_camLogFrames.load() > 0) {
                if (g_drawEye != 1) vr::control::g_camLogFrames.fetch_sub(1);
                float pl[3] = {0, 0, 0};
                char* camL = static_cast<char*>(g_cameraPtr.load());
                void* pawnL = HasCameraSignature(camL) ? PlayerPawnIfViewTarget(camL) : nullptr;
                if (pawnL) SafeCopy(pl, static_cast<char*>(pawnL) + 0x54, 12);
                float wl[3] = {0, 0, 0};
                uint32_t wpn = 0;
                if (pawnL && TrySafeReadUint32(static_cast<char*>(pawnL) + kOffPawnCurrentWeapon, &wpn) && wpn)
                    SafeCopy(wl, reinterpret_cast<char*>(wpn) + 0x54, 12);
                char b[400];
                snprintf(b, sizeof(b),
                         "CAMLOG: t=%.1f eye %d base (%.2f, %.2f, %.2f) view (%.2f, %.2f, %.2f) head t=%.2f wall pull %.1f rot (%d, %d, %d) "
                         "pawn (%.1f, %.1f, %.1f) firm %d raw (%.1f, %.1f, %.1f) weapon (%.1f, %.1f, %.1f)",
                         ProfMs(), g_drawEye.load(), base[0], base[1], base[2], outLoc[0], outLoc[1], outLoc[2], t, wallPull,
                         outRot[0], outRot[1], outRot[2], pl[0], pl[1], pl[2], (int)PawnOnFirmGround(pawnL), rawLoc[0],
                         rawLoc[1], rawLoc[2], wl[0], wl[1], wl[2]);
                vr::log::Line(b);
            }
            // Il rendering usa questa origine (testa 6DOF compresa): va nella
            // lista delle origini per occhio, altrimenti col visore vero (testa
            // spostata) nessun frame viene riconosciuto e il visore non riceve
            // immagini (17/10, prima prova del 6DOF reale).
            if (g_drawEye >= 0) RecordEyeOrigin(outLoc, g_drawEye);
            UpdateAimMarker(base, outLoc, outRot);
            UpdateArms(base, bodyYaw);
            UpdateGunInHand(base, bodyYaw);
            if (vr::control::g_gunLogFrames.load() > 0 && g_drawEye != 1) {
                // Diagnostica (gunlog N): canna dell'arma rispetto agli occhi a
                // ogni tick, per vedere se l'arma trema camminando.
                vr::control::g_gunLogFrames.fetch_sub(1);
                char* camG = static_cast<char*>(g_cameraPtr.load());
                void* pawnG = HasCameraSignature(camG) ? PlayerPawnIfViewTarget(camG) : nullptr;
                float mo[3], md[3], al[3] = {0, 0, 0};
                if (pawnG && GunBarrel(pawnG, mo, md)) {
                    SafeCopy(al, static_cast<char*>(pawnG) + 0x54, 12);
                    char b[400];
                    uint32_t gunFlags = 0;
                    void* gcomp = CurrentGunComponent(pawnG);
                    if (gcomp) TrySafeReadUint32(static_cast<char*>(gcomp) + kOffCompFlags, &gunFlags);
                    float st[ArmRig::kCount] = {};
                    for (int i = 0; i < ArmRig::kCount; ++i)
                        if (g_rig.ctrl[i]) SafeCopy(&st[i], static_cast<char*>(g_rig.ctrl[i]) + kOffCtrlStrength, 4);
                    // Osso della mano rispetto agli occhi: salta anche lui o solo l'arma?
                    struct {
                        int32_t name[2];
                        int32_t space;
                        float ret[3];
                    } hp = {{vr::script::NameIndex("Hand_Right"), 0}, 0, {0, 0, 0}};
                    if (g_rig.mesh && hp.name[0] >= 0) vr::script::CallFunction(g_rig.mesh, "GetBoneLocation", &hp);
                    st[4] = hp.ret[0] - outLoc[0];
                    st[5] = hp.ret[1] - outLoc[1];
                    // Mano sinistra rispetto alla destra (presa sull'astina).
                    struct {
                        int32_t name[2];
                        int32_t space;
                        float ret[3];
                    } lp = {{vr::script::NameIndex("Hand_Left"), 0}, 0, {0, 0, 0}};
                    if (g_rig.mesh && lp.name[0] >= 0) vr::script::CallFunction(g_rig.mesh, "GetBoneLocation", &lp);
                    int32_t pawnYaw = 0;
                    SafeCopy(&pawnYaw, static_cast<char*>(pawnG) + 0x64, 4);
                    st[1] = static_cast<float>(pawnYaw & 0xFFFF) * (360.0f / 65536.0f);
                    st[3] = static_cast<float>(g_bodyYaw.load() & 0xFFFF) * (360.0f / 65536.0f);
                    snprintf(b, sizeof(b),
                             "GUNLOG: t=%.1f eye %d view (%.2f, %.2f, %.2f) barrel-view (%.2f, %.2f, %.2f) pawn (%.2f, %.2f, %.2f) "
                             "arms=%d flags=%08X strength %.2f %.2f %.2f %.2f %.2f %.2f lh (%.2f, %.2f, %.2f)",
                             ProfMs(), g_drawEye.load(), outLoc[0], outLoc[1], outLoc[2], mo[0] - outLoc[0],
                             mo[1] - outLoc[1], mo[2] - outLoc[2], al[0], al[1], al[2], (int)g_armsActive.load(), gunFlags,
                             st[0], st[1], st[2], st[3], st[4], st[5], lp.ret[0] - hp.ret[0], lp.ret[1] - hp.ret[1],
                             lp.ret[2] - hp.ret[2]);
                    vr::log::Line(b);
                }
            }
        }
    }
};

// Volata e asse della canna del fucile in mano (LocalToWorld del componente
// dell'arma, PrimitiveComponent +0x70: righe X, Y, Z, traslazione).
bool GunBarrel(void* pawn, float* origin, float* axis) {
    if (!pawn) return false;
    void* comp = CurrentGunComponent(pawn);
    float m[16];
    if (!comp || !SafeCopy(m, static_cast<char*>(comp) + kOffCompLocalToWorld, sizeof(m))) return false;
    float len = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
    if (!(len > 0.5f && len < 2.0f) || std::fabs(m[15] - 1.0f) > 1e-3f) return false;
    for (int i = 0; i < 3; ++i) {
        axis[i] = m[i] / len;
        origin[i] = m[12 + i];
    }
    return true;
}

// Punto di partenza di colpi e mirino lungo la canna: la volata (osso
// YWeapon.m_muzzleBoneName, +0x2E8, del modello dell'arma) proiettata
// sull'asse; prima si partiva 10 unita' davanti all'impugnatura (mirino) o
// dove il gioco proiettava la sua origine (colpi: 13-37 unita'), quindi
// oggetti vicini alla volata venivano attraversati. Se tra la spalla destra e
// la volata c'e' un ostacolo (arma infilata nel riparo o nel muro) si parte
// dall'ostacolo: il colpo lo colpisce invece di passarci attraverso.
constexpr uintptr_t kOffWeaponMuzzleBone = 0x2E8;
void MuzzleStart(void* pawn, const float* bo, const float* bd, float* start) {
    static void* cachedComp = nullptr;
    static float cachedT = 10.0f;
    static int age = 0;
    void* comp = CurrentGunComponent(pawn);
    if (comp != cachedComp || ++age > 60) {
        age = 0;
        cachedComp = comp;
        cachedT = 10.0f;
        uint32_t weapon = 0;
        int32_t name[2] = {0, 0};
        if (comp && TrySafeReadUint32(static_cast<char*>(pawn) + kOffPawnCurrentWeapon, &weapon) && weapon &&
            SafeCopy(name, reinterpret_cast<char*>(weapon) + kOffWeaponMuzzleBone, 8) && name[0] > 0) {
            struct {
                int32_t name[2];
                int32_t space;
                float ret[3];
            } hp = {{name[0], name[1]}, 0, {0, 0, 0}};
            if (vr::script::CallFunction(comp, "GetBoneLocation", &hp)) {
                float d = (hp.ret[0] - bo[0]) * bd[0] + (hp.ret[1] - bo[1]) * bd[1] + (hp.ret[2] - bo[2]) * bd[2];
                if (d > 2.0f && d < 150.0f) cachedT = d;
            }
        }
    }
    for (int i = 0; i < 3; ++i) start[i] = bo[i] + bd[i] * cachedT;
    g_muzzlePenetration = 0.0f;
    if (!g_rig.mesh) return;
    struct {
        int32_t name[2];
        int32_t space;
        float ret[3];
    } sp = {{vr::script::NameIndex("Arm_Right"), 0}, 0, {0, 0, 0}};
    float hit = 1.0f;
    // La penetrazione si misura sulla volata senza l'arretramento gia'
    // applicato all'arma, altrimenti l'arma andrebbe avanti e indietro.
    float push = g_gunPushApplied.load();
    float unpushed[3] = {start[0] + bd[0] * push, start[1] + bd[1] * push, start[2] + bd[2] * push};
    if (sp.name[0] >= 0 && vr::script::CallFunction(g_rig.mesh, "GetBoneLocation", &sp) &&
        TraceLine(pawn, sp.ret, unpushed, &hit) && hit < 1.0f) {
        float back = 1.0f - 2.0f / ((std::max)(1.0f, cachedT));  // 2 unita' prima dell'ostacolo
        float h = hit * (back > 0.0f ? back : 0.0f);
        float len = std::sqrt((unpushed[0] - sp.ret[0]) * (unpushed[0] - sp.ret[0]) +
                              (unpushed[1] - sp.ret[1]) * (unpushed[1] - sp.ret[1]) +
                              (unpushed[2] - sp.ret[2]) * (unpushed[2] - sp.ret[2]));
        g_muzzlePenetration = (1.0f - hit) * len;
        for (int i = 0; i < 3; ++i) start[i] = sp.ret[i] + (unpushed[i] - sp.ret[i]) * h;
    }
}

void UpdateAimMarkerImpl(const float* aimOrigin, const float* eyeLoc, const int32_t* viewRot);

// Copia mirino e riquadro appena calcolati nell'ultima etichetta di questo
// occhio (ancora in coda: il suo Present arriva dopo la fine del Draw).
void StampMarkerOnTag(int eye) {
    std::lock_guard<std::mutex> lock(g_frameTagMutex);
    uint32_t tick = g_stereoDraws.load(std::memory_order_relaxed);
    for (auto it = g_frameTags.rbegin(); it != g_frameTags.rend(); ++it) {
        if (it->eye != eye) continue;
        if (it->tick != tick) break;  // nessuna etichetta per questo tick
        it->markerSet = true;
        it->markerVis = g_markerVisible[eye];
        it->targetVis = g_targetVisible[eye];
        for (int i = 0; i < 2; ++i) {
            it->markerNdc[i] = g_markerNdc[eye][i];
            it->targetNdc[i] = g_targetNdc[eye][i];
        }
        break;
    }
}

void UpdateAimMarker(const float* aimOrigin, const float* eyeLoc, const int32_t* viewRot) {
    UpdateAimMarkerImpl(aimOrigin, eyeLoc, viewRot);
    if (g_drawEye == 0 || g_drawEye == 1) StampMarkerOnTag(g_drawEye);
}

void UpdateAimMarkerImpl(const float* aimOrigin, const float* eyeLoc, const int32_t* viewRot) {
    int eye = g_drawEye;
    if (eye < 0 || eye > 1) eye = 0;
    bool hand = vr::control::g_aimHand && vr::control::g_aimFollowsBody && g_aimValid;
    char* cam = static_cast<char*>(g_cameraPtr.load());
    // "marker off" nasconde solo il mirino: mira dalla canna e ADS restano
    // (prima si spegnevano anche loro, audit 19/10).
    if (!hand || !HasCameraSignature(cam) || !PlayerPawnIfViewTarget(cam)) {
        g_markerVisible[eye] = false;
        g_targetVisible[eye] = false;
        if (eye == 0) vr::control::g_adsActive = false;
        return;
    }
    const float range = 20000.0f;
    float p[3];
    void* pawn = PlayerPawnIfViewTarget(cam);
    float muzzle[3], barrel[3];
    if (vr::control::g_muzzleAim && g_armsActive && GunBarrel(pawn, muzzle, barrel)) {
        // Mira con le tacche, dalle pose tracciate (non dalla canna disegnata:
        // la mira del gioco attivata da LT sposta l'arma e lo stato
        // lampeggiava a ogni frame). Riferimento del ricentramento, unita':
        // occhio = spostamento della testa, raggio = controller destro con la
        // sua direzione. Le tacche stanno ~4-6 unita' sopra l'impugnatura:
        // dentro se l'occhio e' entro 8 unita' dal raggio e 4-45 unita' dietro
        // il controller; fuori sopra 11.
        if (eye == 0) {
            float ho[3];
            HeadOffset(ho);
            float c[3] = {g_handOffset[0].load(), g_handOffset[1].load(), g_handOffset[2].load()};
            float yaw = static_cast<float>(g_aimDeltaYaw.load()) * (6.28318530718f / 65536.0f);
            float pitch = static_cast<float>(g_aimDeltaPitch.load()) * (6.28318530718f / 65536.0f);
            float dir[3] = {std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch)};
            float v[3] = {ho[0] - c[0], ho[1] - c[1], ho[2] - c[2]};
            float along = v[0] * dir[0] + v[1] * dir[1] + v[2] * dir[2];
            float perp[3] = {v[0] - dir[0] * along, v[1] - dir[1] * along, v[2] - dir[2] * along};
            float off = std::sqrt(perp[0] * perp[0] + perp[1] * perp[1] + perp[2] * perp[2]);
            bool ads = vr::control::g_adsActive;
            bool behind = along < -4.0f && along > -45.0f;
            // Si guarda lungo l'arma: canna entro 30 gradi dalla direzione della
            // testa (un'arma girata di lato poteva passare vicino all'occhio).
            float hy = static_cast<float>(HeadYaw()) * (6.28318530718f / 65536.0f);
            float hp = static_cast<float>(HeadPitch()) * (6.28318530718f / 65536.0f);
            float hf[3] = {std::cos(hp) * std::cos(hy), std::cos(hp) * std::sin(hy), std::sin(hp)};
            bool lookingAlong = hf[0] * dir[0] + hf[1] * dir[1] + hf[2] * dir[2] > 0.966f;  // 15 gradi
            if (!vr::control::g_adsEnabled || !behind || !g_handValid || !lookingAlong) ads = false;
            else if (!ads && off < 6.0f) ads = true;   // 12 cm (8 dava falsi positivi col fucile al fianco)
            else if (ads && off > 9.0f) ads = false;
            if (ads != vr::control::g_adsActive.load())
                vr::log::Line(ads ? "ADS: gun at the eye (sights), marker hidden" : "ADS: end");
            vr::control::g_adsActive = ads;
        }
        // Raggio dalla canna del fucile visibile: il mirino va dove arriva.
        float start[3], end[3];
        MuzzleStart(pawn, muzzle, barrel, start);
        for (int i = 0; i < 3; ++i) end[i] = start[i] + barrel[i] * range;
        float hitTime = 1.0f;
        TraceLine(pawn, start, end, &hitTime);
        for (int i = 0; i < 3; ++i) p[i] = start[i] + (end[i] - start[i]) * hitTime;
        // Il gioco cerca il bersaglio con un raggio dalla camera (occhi) lungo
        // la sua rotazione e poi spara dalla volata verso quel punto: la
        // camera deve puntare dagli occhi al punto in cui arriva la canna.
        // (17/10: con la rotazione della canna il raggio della camera correva
        // parallelo alla canna, spostato verso la testa, e i colpi andavano
        // tutti a sinistra.)
        std::memcpy(g_muzzleTarget, p, sizeof(g_muzzleTarget));
        float d[3] = {p[0] - aimOrigin[0], p[1] - aimOrigin[1], p[2] - aimOrigin[2]};
        float horiz = std::sqrt(d[0] * d[0] + d[1] * d[1]);
        g_muzzleAimYaw = RadiansToUnreal(std::atan2(d[1], d[0]));
        g_muzzleAimPitch = RadiansToUnreal(std::atan2(d[2], horiz));
        g_muzzleAimValid = true;
    } else {
        if (eye == 0) vr::control::g_adsActive = false;
        g_muzzleAimValid = false;
        // Direzione di mira = rotazione che la logica di gioco usa (corpo + controller).
        float af[3], ar[3], au[3];
        RotatorToAxes(g_bodyPitch.load() + g_aimDeltaPitch.load(), g_bodyYaw.load() + g_aimDeltaYaw.load(), 0, af, ar, au);
        float end[3] = {aimOrigin[0] + af[0] * range, aimOrigin[1] + af[1] * range, aimOrigin[2] + af[2] * range};
        float hitTime = 1.0f;
        TraceLine(pawn, aimOrigin, end, &hitTime);  // il Pawn stesso va ignorato
        float dist = range * hitTime;
        for (int i = 0; i < 3; ++i) p[i] = aimOrigin[i] + af[i] * dist;
    }
    // Proiezione nella vista dell'occhio.
    float vf[3], vrt[3], vu[3];
    RotatorToAxes(viewRot[0], viewRot[1], viewRot[2], vf, vrt, vu);
    {
        bool fresh = GetTickCount64() - g_squadTargetMs.load() < 200;
        bool vis = false;
        if (fresh) {
            float tv[3] = {g_squadTargetLoc[0] - eyeLoc[0], g_squadTargetLoc[1] - eyeLoc[1], g_squadTargetLoc[2] - eyeLoc[2]};
            float tz = tv[0] * vf[0] + tv[1] * vf[1] + tv[2] * vf[2];
            if (tz > 1.0f) {
                float tx = tv[0] * vrt[0] + tv[1] * vrt[1] + tv[2] * vrt[2];
                float ty = tv[0] * vu[0] + tv[1] * vu[1] + tv[2] * vu[2];
                float fovT = vr::control::g_fovOverride > 0.0f ? vr::control::g_fovOverride.load() : 90.0f;
                float th2 = std::tan(fovT * 0.5f / 57.2957795f);
                UINT w2 = g_backBufferW, h2 = g_backBufferH;
                float tv2 = (w2 && h2) ? th2 * h2 / w2 : th2;
                float nx, ny;
                TanToNdc(eye, g_drawOffCenter, tx / tz, ty / tz, th2, tv2, &nx, &ny);
                g_targetNdc[eye][0] = nx;
                g_targetNdc[eye][1] = ny;
                vis = true;
            }
        }
        g_targetVisible[eye] = vis;
    }
    float v[3] = {p[0] - eyeLoc[0], p[1] - eyeLoc[1], p[2] - eyeLoc[2]};
    float z = v[0] * vf[0] + v[1] * vf[1] + v[2] * vf[2];
    if (z < 1.0f) {
        g_markerVisible[eye] = false;
        return;
    }
    float x = v[0] * vrt[0] + v[1] * vrt[1] + v[2] * vrt[2];
    float y = v[0] * vu[0] + v[1] * vu[1] + v[2] * vu[2];
    float fov = vr::control::g_fovOverride > 0.0f ? vr::control::g_fovOverride.load() : 90.0f;  // senza VR: FOV del gioco
    float th = std::tan(fov * 0.5f / 57.2957795f);
    UINT w = g_backBufferW, h = g_backBufferH;
    float tv = (w && h) ? th * h / w : th;
    float nx, ny;
    TanToNdc(eye, g_drawOffCenter, x / z, y / z, th, tv, &nx, &ny);
    g_markerNdc[eye][0] = nx;
    g_markerNdc[eye][1] = ny;
    // Milkor: le granate fanno una parabola, un mirino in linea retta
    // indicherebbe un punto sbagliato: niente mirino (audit 19/10).
    bool lob = g_weaponInfo.cls.find("Milkor") != std::string::npos;
    g_markerVisible[eye] = !vr::control::g_adsActive && vr::control::g_aimMarker && !lob;
}


// --- Diagnostica: nemici davanti alla vista ma non disegnati -----------------
//
// 18/10: sporgendosi col corpo dalla copertura alcuni nemici (dentro un
// pullman) sparivano. Ogni 0,5 s: nemici vivi entro 2500 unita' e 35 gradi
// dalla direzione della vista che il motore non disegna da piu' di 0,3 s
// (Actor.LastRenderTime) vengono scritti nel log con vista, occhi di Walker e
// stato nascosto, per capire se e' occlusione, volumi di visibilita' o altro.
void VisibilityCheck(void* pawn) {
    static ULONGLONG last = 0;
    if (!pawn || vr::control::g_cinemaActive || GetTickCount64() - last < 500) return;
    last = GetTickCount64();
    uint32_t worldInfo = 0, p = 0;
    float now = 0.0f;
    if (!TrySafeReadUint32(static_cast<char*>(pawn) + 0xEC, &worldInfo) || !worldInfo ||
        !TrySafeReadUint32(reinterpret_cast<char*>(worldInfo) + 0x4F4, &p) ||
        !SafeCopy(&now, reinterpret_cast<char*>(worldInfo) + 0x454, 4))
        return;
    float fwd[3], rt[3], up[3];
    RotatorToAxes(g_lastViewRot[0], g_lastViewRot[1], 0, fwd, rt, up);
    for (int guard = 0; p && guard < 512; ++guard) {
        char* other = reinterpret_cast<char*>(p);
        uint32_t next = 0, flags = 0, mesh = 0, meshFlags = 0;
        TrySafeReadUint32(other + 0x218, &next);
        int32_t health = 0;
        float loc[3], lastRender = 0.0f;
        if (other != pawn && TrySafeReadUint32(other + 0x304, reinterpret_cast<uint32_t*>(&health)) && health > 0 &&
            SafeCopy(loc, other + 0x54, 12) && SafeCopy(&lastRender, other + 0xF8, 4)) {
            float v[3] = {loc[0] - g_lastViewLoc[0], loc[1] - g_lastViewLoc[1], loc[2] - g_lastViewLoc[2]};
            float d = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            float cosA = d > 1.0f ? (v[0] * fwd[0] + v[1] * fwd[1] + v[2] * fwd[2]) / d : 0.0f;
            std::string cls = vr::script::ClassName(other);
            bool squad = cls.find("Adams") != std::string::npos || cls.find("Lugo") != std::string::npos;
            if (!squad && d < 2500.0f && cosA > 0.82f && now - lastRender > 0.3f) {
                TrySafeReadUint32(other + 0x9C, &flags);
                TrySafeReadUint32(other + kOffPawnMesh, &mesh);
                if (mesh) TrySafeReadUint32(reinterpret_cast<char*>(mesh) + 0x134, &meshFlags);
                float ho[3];
                HeadOffset(ho);
                char b[420];
                snprintf(b, sizeof(b),
                         "VISCHECK: %s not drawn for %.1f s, %.0f units ahead (%.0f deg) | actor hidden=%d mesh hidden=%d | "
                         "view (%.0f %.0f %.0f) Walker eye (%.0f %.0f %.0f) head offset %.0f | enemy (%.0f %.0f %.0f) | peek=%d",
                         cls.c_str(), now - lastRender, d, std::acos(cosA > 1 ? 1 : cosA) * 57.3f, (flags & 0x4) ? 1 : 0,
                         (meshFlags & 0x4) ? 1 : 0, g_lastViewLoc[0], g_lastViewLoc[1], g_lastViewLoc[2], g_lastViewBase[0],
                         g_lastViewBase[1], g_lastViewBase[2], std::sqrt(ho[0] * ho[0] + ho[1] * ho[1] + ho[2] * ho[2]),
                         loc[0], loc[1], loc[2], (int)vr::control::g_coverPeek.load());
                vr::log::Line(b);
            }
        }
        p = next;
    }
}

// --- Aiuto alle esecuzioni ------------------------------------------------------
//
// Il gioco sceglie il nemico a terra da giustiziare con B dentro un box nello
// spazio del Pawn (YGamePawn.m_executionDetectionParams +0x1D48: avanti
// -40..95, lati +-95, altezza +-90 unita'), cioe' davanti al corpo di
// Walker. In VR il corpo non e' dove si guarda (gira solo oltre 45 gradi di
// testa, mai in copertura) e un nemico di lato o dietro non veniva mai scelto
// (audit 20/10). Due aiuti:
//  - box un po' piu' largo su ogni nuovo Pawn di Walker (scarto occhi-corpo,
//    resto della room-scale);
//  - alla pressione di B (tasto o gesto) il corpo si gira subito verso il
//    nemico a terra piu' vicino entro 2 m (m_isDownButNotOut / m_isKnockedDown,
//    +0x5FC); la vista resta ferma (stessa compensazione di "turnwithhead").
//    B arriva al gioco dopo (cancello di 120 ms o tick successivo).
constexpr uintptr_t kOffWorldInfoPawnList = 0x4F4;
constexpr uintptr_t kOffPawnNextPawn = 0x218;
constexpr uintptr_t kOffPawnHealth = 0x304;
constexpr uintptr_t kOffActorWorldInfo = 0xEC;
constexpr uintptr_t kOffPawnExecDetection = 0x1D48;  // YS_SpecialMoveDetectionParams (3 coppie min/max)
constexpr uintptr_t kOffPawnDbnoFlags = 0x5FC;
constexpr uint32_t kDbnoBits = 0x10000 | 0x40000;  // m_isKnockedDown | m_isDownButNotOut

bool IsFriendOrCivilian(const std::string& cls) {
    return cls.find("Adams") != std::string::npos || cls.find("Lugo") != std::string::npos ||
           cls.find("Walker") != std::string::npos || cls.find("Civil") != std::string::npos ||
           cls.find("Refugee") != std::string::npos;
}

void ExecutionAssist(void* pc, void* pawn) {
    static int diag = 0;
    if (diag++ == 0) {
        char d[96];
        snprintf(d, sizeof(d), "EXECUTE: assist active (pc %p pawn %p)", pc, pawn);
        vr::log::Line(d);
    }
    if (!pawn || !pc || !vr::control::g_execAssist) return;
    static void* widenedFor = nullptr;
    if (pawn != widenedFor) {
        widenedFor = pawn;
        float cur[6] = {};
        if (SafeCopy(cur, static_cast<char*>(pawn) + kOffPawnExecDetection, sizeof(cur)) && cur[1] > 0.0f &&
            cur[1] < 400.0f) {
            float wide[6] = {(std::min)(cur[0], -80.0f), (std::max)(cur[1], 120.0f), (std::min)(cur[2], -110.0f),
                             (std::max)(cur[3], 110.0f), (std::min)(cur[4], -120.0f), cur[5]};
            SafeCopy(static_cast<char*>(pawn) + kOffPawnExecDetection, wide, sizeof(wide));
            char b[200];
            snprintf(b, sizeof(b), "EXECUTE: detection box widened, forward %.0f..%.0f side %.0f..%.0f height %.0f..%.0f",
                     wide[0], wide[1], wide[2], wide[3], wide[4], wide[5]);
            vr::log::Line(b);
        }
    }
    static uint64_t handled = 0;
    uint64_t intent = vr::control::g_meleeIntentMs;
    if (intent == handled || GetTickCount64() - intent > 400) return;
    handled = intent;
    if (vr::control::g_cinemaActive || g_gameLockedScene || TurretAimActive()) return;
    int32_t myHealth = 0;
    float me[3];
    uint32_t worldInfo = 0, p = 0;
    if (!TrySafeReadUint32(static_cast<char*>(pawn) + kOffPawnHealth, reinterpret_cast<uint32_t*>(&myHealth)) ||
        myHealth <= 0 || !SafeCopy(me, static_cast<char*>(pawn) + 0x54, 12) ||
        !TrySafeReadUint32(static_cast<char*>(pawn) + kOffActorWorldInfo, &worldInfo) || !worldInfo ||
        !TrySafeReadUint32(reinterpret_cast<char*>(worldInfo) + kOffWorldInfoPawnList, &p))
        return;
    char* best = nullptr;
    float bestD = 200.0f, bestLoc[3] = {};
    for (int guard = 0; p && guard < 512; ++guard) {
        char* other = reinterpret_cast<char*>(p);
        uint32_t next = 0, flags = 0;
        TrySafeReadUint32(other + kOffPawnNextPawn, &next);
        int32_t health = 0;
        float loc[3];
        if (other != pawn && TrySafeReadUint32(other + kOffPawnHealth, reinterpret_cast<uint32_t*>(&health)) &&
            health > 0 && TrySafeReadUint32(other + kOffPawnDbnoFlags, &flags) && (flags & kDbnoBits) &&
            SafeCopy(loc, other + 0x54, 12) && std::fabs(loc[2] - me[2]) < 150.0f) {
            float d = std::sqrt((loc[0] - me[0]) * (loc[0] - me[0]) + (loc[1] - me[1]) * (loc[1] - me[1]));
            if (d < bestD && !IsFriendOrCivilian(vr::script::ClassName(other))) {
                bestD = d;
                best = other;
                std::memcpy(bestLoc, loc, sizeof(loc));
            }
        }
        p = next;
    }
    if (!best) return;
    int32_t pcYaw = 0;
    if (!TrySafeReadUint32(static_cast<char*>(pc) + 0x64, reinterpret_cast<uint32_t*>(&pcYaw))) return;
    int32_t target = RadiansToUnreal(std::atan2(bestLoc[1] - me[1], bestLoc[0] - me[0]));
    int32_t delta = static_cast<int16_t>(target - pcYaw);  // la via piu' corta
    if (std::abs(delta) > 65536 / 72) {  // sotto 5 gradi non serve
        if (AddControllerYaw(pc, delta)) {
            g_hmdYawReference += vr::control::g_yawSign * static_cast<float>(delta) * (6.28318530718f / 65536.0f);
            // Il gioco gira il Pawn da m_lastRotation (+0x1020) verso
            // m_rotationToFace (+0xDE8) a velocita' limitata: scritti tutti e
            // tre, il corpo e' girato subito (provato 20/10: resta dov'e').
            int32_t pawnYaw = target;
            SafeCopy(static_cast<char*>(pawn) + 0x64, &pawnYaw, 4);
            SafeCopy(static_cast<char*>(pawn) + 0x1020 + 4, &pawnYaw, 4);
            SafeCopy(static_cast<char*>(pawn) + 0xDE8 + 4, &pawnYaw, 4);
        }
    }
    char b[160];
    snprintf(b, sizeof(b), "EXECUTE: B near %s %.0f units away, body turned %.0f degrees toward it",
             vr::script::ClassName(best).c_str(), bestD, delta * 360.0f / 65536.0f);
    vr::log::Line(b);
}

// Direzione dell'ultimo colpo subito rispetto allo sguardo, per il giubbotto
// bHaptics: Pawn.LastHitBy (+0x3C4, il Controller di chi ha sparato) ->
// Pawn (+0x1F8) -> posizione. NAN se non nota.
float HitAngleFromHead(void* pawn) {
    uint32_t ctrl = 0, attacker = 0;
    float me[3], at[3];
    if (!pawn || !TrySafeReadUint32(static_cast<char*>(pawn) + 0x3C4, &ctrl) || !ctrl ||
        !TrySafeReadUint32(reinterpret_cast<char*>(ctrl) + 0x1F8, &attacker) || !attacker || attacker == reinterpret_cast<uintptr_t>(pawn) ||
        !SafeCopy(me, static_cast<char*>(pawn) + 0x54, 12) || !SafeCopy(at, reinterpret_cast<char*>(attacker) + 0x54, 12))
        return std::nanf("");
    float toAttacker = std::atan2(at[1] - me[1], at[0] - me[0]);
    float view = g_lastViewRot[1] * (6.28318530718f / 65536.0f);
    return std::remainder(toAttacker - view, 6.28318530718f);
}

// --- Comando alla squadra lungo la canna ---------------------------------------
//
// Tenendo il tasto squadra il gioco sceglie il bersaglio dell'ordine tra gli
// oggetti "marcabili" piu' vicini al centro dello schermo
// (m_currentSelectedTacticalMarkableBestScreenDist): in VR e' il centro della
// vista, cioe' la testa, mentre si mira con l'arma (18/10, provato: testa sul
// nemico -> selezionato, arma sul nemico -> niente). Qui, finche' la modalita'
// comando e' attiva, si sceglie il nemico vivo e visibile piu' vicino alla
// linea della canna e lo si passa al gioco con SetAsCurrentSelectedMarkable
// (distanza dal centro 0: vince sulla scelta dello schermo).
constexpr uintptr_t kOffPcSelectedMarkable = 0xE98;  // m_currentSelectedTacticalMarkable

// Bersagli degli ordini che non sono Pawn: YCheapShooter (nemici semplificati
// che sparano da lontano, es. il cecchino sul cartellone del capitolo 1, che
// il tutorial chiede di far abbattere: 18/10, "non riesco a dare l'ordine").
// La lista dei Pawn non li contiene: si cercano in GObjects (classe e
// sottoclassi), ogni 2 s mentre il tasto squadra e' tenuto.
constexpr uint32_t kOffCheapShooterHealth = 0x264;  // YCheapShooter.m_health (float)
std::vector<void*> CheapShooters() {
    static uint32_t cls = 0;
    static std::vector<void*> cache;
    // Prossima scansione (tempo assoluto). Prima "last = ora + 60000" con il
    // test senza segno "ora - last < 2000" non attendeva mai: senza la classe
    // GObjects era scandito per nome a ogni frame col tasto tenuto (audit 19/10).
    static ULONGLONG nextScan = 0;
    if (GetTickCount64() < nextScan) return cache;
    nextScan = GetTickCount64() + 2000;
    cache.clear();
    uint32_t data = 0, num = 0;
    if (!TrySafeReadUint32(reinterpret_cast<void*>(kGObjectsAddr), &data) ||
        !TrySafeReadUint32(reinterpret_cast<void*>(kGObjectsAddr + 4), &num) || !data || num > 2000000)
        return cache;
    if (!cls) {
        for (uint32_t i = 0; i < num && !cls; ++i) {
            uint32_t o = 0;
            if (TrySafeReadUint32(reinterpret_cast<void*>(data + 4 * i), &o) && o &&
                vr::script::ObjectName(reinterpret_cast<void*>(o)) == "YCheapShooter" &&
                vr::script::ClassName(reinterpret_cast<void*>(o)) == "Class")
                cls = o;
        }
        if (!cls) {
            nextScan = GetTickCount64() + 60000;  // classe non trovata: si riprova tra un minuto
            return cache;
        }
    }
    for (uint32_t i = 0; i < num; ++i) {
        uint32_t o = 0;
        if (!TrySafeReadUint32(reinterpret_cast<void*>(data + 4 * i), &o) || !o) continue;
        uint32_t c = ObjClass(reinterpret_cast<void*>(o));
        for (int depth = 0; c && depth < 4; ++depth) {  // classe o sottoclasse
            if (c == cls) {
                cache.push_back(reinterpret_cast<void*>(o));
                break;
            }
            uint32_t super = 0;
            if (!TrySafeReadUint32(reinterpret_cast<char*>(c) + 0x3C, &super)) break;
            c = super;
        }
    }
    return cache;
}

void UpdateSquadCommandAim(char* cam, void* pawn) {
    static void* lastPick = nullptr;
    static uint32_t lastPickCls = 0;  // classe al momento della scelta (oggetto ancora valido?)
    // Diagnostica sempre attiva (20/10: tester bloccati al cecchino sul
    // cartellone, nei log nessuna traccia dell'ordine): a ogni pressione e
    // rilascio del tasto squadra, perche' la mod sceglie o non sceglie.
    static bool wasHeld = false;
    static void* everPicked = nullptr;
    bool held = vr::control::g_squadButtonHeld;
    if (held != wasHeld) {
        wasHeld = held;
        uint32_t sel = 0;
        if (HasCameraSignature(cam)) TrySafeReadUint32(cam - 0x77C + kOffPcSelectedMarkable, &sel);
        char b[300];
        if (held) {
            everPicked = nullptr;
            snprintf(b, sizeof(b),
                     "SQUAD: button pressed (commandaim gun=%d, pawn=%d, aim hand=%d, arms=%d, barrel aim=%d, cheap "
                     "shooters=%u); game target %s",
                     (int)vr::control::g_commandFollowsGun.load(), pawn != nullptr, (int)vr::control::g_aimHand.load(),
                     (int)g_armsActive.load(), (int)g_muzzleAimValid.load(), (unsigned)CheapShooters().size(),
                     sel ? vr::script::ClassName(reinterpret_cast<void*>(sel)).c_str() : "none");
        } else {
            snprintf(b, sizeof(b), "SQUAD: button released; mod target %s, game target %s",
                     everPicked ? vr::script::ClassName(everPicked).c_str() : "none",
                     sel ? vr::script::ClassName(reinterpret_cast<void*>(sel)).c_str() : "none");
        }
        vr::log::Line(b);
    }
    // m_squadCommandModeOn non si accende tenendo il tasto (la selezione del
    // gioco gira sempre, per l'HUD): conta il tasto squadra passato al gioco.
    if (!vr::control::g_commandFollowsGun || !pawn || !vr::control::g_aimHand || !g_armsActive || !held) {
        lastPick = nullptr;
        return;
    }
    // Raggio dagli occhi verso il punto in cui arriva la canna (il mirino
    // verde): lo stesso della camera logica del gioco. La visibilita' si
    // controlla dagli occhi, come fa il gioco: la canna all'altezza del fianco
    // dietro una copertura "vede" solo la copertura.
    float bo[3], bd[3];
    if (!g_muzzleAimValid) return;
    std::memcpy(bo, cam + 0x10, sizeof(bo));
    float dl = std::sqrt((g_muzzleTarget[0] - bo[0]) * (g_muzzleTarget[0] - bo[0]) +
                         (g_muzzleTarget[1] - bo[1]) * (g_muzzleTarget[1] - bo[1]) +
                         (g_muzzleTarget[2] - bo[2]) * (g_muzzleTarget[2] - bo[2]));
    if (dl < 1.0f) return;
    for (int i = 0; i < 3; ++i) bd[i] = (g_muzzleTarget[i] - bo[i]) / dl;
    uint32_t worldInfo = 0, p = 0;
    if (!TrySafeReadUint32(static_cast<char*>(pawn) + kOffActorWorldInfo, &worldInfo) || !worldInfo ||
        !TrySafeReadUint32(reinterpret_cast<char*>(worldInfo) + kOffWorldInfoPawnList, &p))
        return;
    void* best = nullptr;
    float bestScore = 1e9f;
    // Bersaglio gia' scelto: resta finche' e' vicino al mirino (1,5 volte la
    // tolleranza). Cambiarlo a ogni frame tra due nemici vicini annullava la
    // convalida del gioco ("not taken by the game", 18/10 in combattimento).
    float lastPickScore = -1.0f;
    static ULONGLONG lastDiag = 0;
    bool diag = vr::control::g_fireLog && GetTickCount64() - lastDiag > 1000;
    if (diag) lastDiag = GetTickCount64();
    // Due passate: lungo la canna e, se li' non c'e' nessuno, lungo lo
    // sguardo. Il gioco mostra il triangolo rosso del bersaglio al centro
    // della vista: chi lo conosce muove la testa per metterlo sul nemico, non
    // il fucile (20/10, cecchino sul cartellone: "spostano il triangolo").
    float hd[3], hr[3], hu[3];
    RotatorToAxes(g_lastViewRot[0], g_lastViewRot[1], 0, hd, hr, hu);
    const uint32_t pawnList = p;
    for (int pass = 0; pass < 2 && !best; ++pass) {
        const float* dir = pass ? hd : bd;
        p = pawnList;
        for (int guard = 0; p && guard < 512; ++guard) {
            char* other = reinterpret_cast<char*>(p);
            uint32_t next = 0;
            TrySafeReadUint32(other + kOffPawnNextPawn, &next);
            int32_t health = 0;
            float loc[3];
            if (other != pawn && TrySafeReadUint32(other + kOffPawnHealth, reinterpret_cast<uint32_t*>(&health)) &&
                health > 0 && SafeCopy(loc, other + 0x54, 12)) {
                std::string cls = vr::script::ClassName(other);
                bool friendOrCivil = cls.find("Adams") != std::string::npos || cls.find("Lugo") != std::string::npos ||
                                     cls.find("Walker") != std::string::npos || cls.find("Civil") != std::string::npos ||
                                     cls.find("Refugee") != std::string::npos;
                loc[2] += 30.0f;  // petto
                float v[3] = {loc[0] - bo[0], loc[1] - bo[1], loc[2] - bo[2]};
                float along = v[0] * dir[0] + v[1] * dir[1] + v[2] * dir[2];
                if (!friendOrCivil && along > 50.0f) {
                    float perp[3] = {v[0] - dir[0] * along, v[1] - dir[1] * along, v[2] - dir[2] * along};
                    float off = std::sqrt(perp[0] * perp[0] + perp[1] * perp[1] + perp[2] * perp[2]);
                    // Tolleranza: 3 gradi, e almeno ~45 unita' (un corpo) da vicino.
                    float tol = std::fmax(along * 0.0524f, 45.0f);
                    float score = off / along;
                    float hitTime = 1.0f;
                    bool blocked = TraceLine(pawn, bo, loc, &hitTime) &&
                                   hitTime * std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) <
                                       std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) - 60.0f;
                    if (diag) {
                        char b[200];
                        snprintf(b, sizeof(b), "SQUAD check: %s along=%.0f off=%.0f tol=%.0f blocked=%d (hit %.2f)",
                                 cls.c_str(), along, off, tol, (int)blocked, hitTime);
                        vr::log::Line(b);
                    }
                    if (other == lastPick && !blocked && off < 1.5f * tol) lastPickScore = score;
                    if (off < tol && score < bestScore) {
                        if (!blocked) {
                            best = other;
                            bestScore = score;
                        }
                    }
                }
            }
            p = next;
        }
        // Nemici semplificati (cecchini sui cartelloni, tiratori lontani): niente
        // controllo di visibilita' (stanno dietro parapetti, sono bersagli
        // scriptati apposta per gli ordini); il gioco li convalida comunque.
        for (void* cs : CheapShooters()) {
            if (cs == pawn || !StillAlive(cs, ObjClass(cs))) continue;
            float health = 0.0f, loc[3];
            if (!SafeCopy(&health, static_cast<char*>(cs) + kOffCheapShooterHealth, 4) || !(health > 0.0f) ||
                !SafeCopy(loc, static_cast<char*>(cs) + 0x54, 12))
                continue;
            float v[3] = {loc[0] - bo[0], loc[1] - bo[1], loc[2] - bo[2]};
            float along = v[0] * dir[0] + v[1] * dir[1] + v[2] * dir[2];
            if (along <= 50.0f) continue;
            float perp[3] = {v[0] - dir[0] * along, v[1] - dir[1] * along, v[2] - dir[2] * along};
            float off = std::sqrt(perp[0] * perp[0] + perp[1] * perp[1] + perp[2] * perp[2]);
            float tol = std::fmax(along * 0.0699f, 60.0f);  // 4 gradi: bersagli piccoli e lontani
            if (diag) {
                char b[200];
                snprintf(b, sizeof(b), "SQUAD check: %s (cheap shooter) along=%.0f off=%.0f tol=%.0f health=%.0f",
                         vr::script::ClassName(cs).c_str(), along, off, tol, health);
                vr::log::Line(b);
            }
            if (cs == lastPick && off < 1.5f * tol) lastPickScore = off / along;
            if (off < tol && off / along < bestScore) {
                best = cs;
                bestScore = off / along;
            }
        }
    }
    // Si cambia bersaglio solo se il nuovo e' nettamente piu' centrato.
    if (lastPick && lastPickScore >= 0.0f && best != lastPick && (!best || bestScore > 0.6f * lastPickScore)) {
        best = lastPick;
        bestScore = lastPickScore;
    }
    // Nessun bersaglio lungo la canna ma uno gia' agganciato in questa
    // pressione: resta finche' e' vivo, fino al rilascio. Prima, spostando
    // un attimo l'arma (rinculo, mano tremante, cecchino piccolo e lontano)
    // tornava la scelta del gioco al centro della vista e al rilascio non
    // partiva nessun ordine (20/10, cecchino sul cartellone: "game target none").
    if (!best && lastPick && StillAlive(lastPick, lastPickCls)) {
        bool alive = false;
        int32_t hp = 0;
        float fhp = 0.0f;
        if (vr::script::ClassName(lastPick).find("CheapShooter") != std::string::npos)
            alive = SafeCopy(&fhp, static_cast<char*>(lastPick) + kOffCheapShooterHealth, 4) && fhp > 0.0f;
        else
            alive = TrySafeReadUint32(static_cast<char*>(lastPick) + kOffPawnHealth, reinterpret_cast<uint32_t*>(&hp)) &&
                    hp > 0;
        if (alive) best = lastPick;
    }
    if (!best) {
        lastPick = nullptr;
        return;
    }
    struct {
        void* markable;
        float screenDist;
    } parms = {best, 0.0f};
    // Il gioco convalida la scelta in modo asincrono (traccia di visibilita'):
    // l'esito si controlla qualche frame dopo, non subito.
    static void* pending = nullptr;
    static int pendingFrames = 0;
    if (pending && ++pendingFrames >= 10) {
        uint32_t sel = 0;
        TrySafeReadUint32(cam - 0x77C + kOffPcSelectedMarkable, &sel);
        vr::log::Line("SQUAD: command target along the barrel: " + vr::script::ClassName(pending) +
                      (reinterpret_cast<void*>(sel) == pending ? " (selected)" : " (not taken by the game)"));
        pending = nullptr;
    }
    vr::script::CallFunction(cam - 0x77C, "SetAsCurrentSelectedMarkable", &parms);
    everPicked = best;
    {
        float tl[3];
        if (SafeCopy(tl, static_cast<char*>(best) + 0x54, 12)) {
            tl[2] += 30.0f;  // petto
            for (int i = 0; i < 3; ++i) g_squadTargetLoc[i] = tl[i];
            g_squadTargetMs = GetTickCount64();
        }
    }
    if (best != lastPick) {
        lastPick = best;
        lastPickCls = ObjClass(best);
        pending = best;
        pendingFrames = 0;
    }
}


// --- Raggio del colpo ---------------------------------------------------------
//
// YWeapon::CalcWeaponFire(FVector StartTrace, FVector EndTrace, FImpactInfo*)
// (0x004A3D50, chiamata da execCalcWeaponFire 0x00C27130) riceve il raggio del
// colpo istantaneo. Per l'arma del giocatore, con la mira dalla canna, il
// raggio va dalla sua origine al punto in cui arriva la canna: il colpo va
// dove punta la canna del fucile visibile, qualunque calcolo faccia lo script
// prima (17/10: i colpi andavano a sinistra del mirino).
constexpr uintptr_t kCalcWeaponFireAddr = 0x004A3D50;
constexpr uintptr_t kOffPawnArsenalMgr = 0x1590;   // YGamePawn.m_arsenalManager
constexpr uintptr_t kOffArsenalAccuracy = 0x81C;   // YArsenalManager.m_accuracy, m_oldAccuracy
struct FVec3 {
    float x, y, z;
};
typedef void(__thiscall* CalcWeaponFire_t)(void* self, FVec3 start, FVec3 end, void* impact);
bool GunBarrel(void* pawn, float* origin, float* axis);
CalcWeaponFire_t g_CalcWeaponFire_Original = nullptr;

void __fastcall Hooked_CalcWeaponFire(void* self, void* /*edx*/, FVec3 start, FVec3 end, void* impact) {
    char* cam = static_cast<char*>(g_cameraPtr.load());
    void* pawn = HasCameraSignature(cam) ? PlayerPawnIfViewTarget(cam) : nullptr;
    uint32_t weapon = 0;
    bool mine = pawn && TrySafeReadUint32(static_cast<char*>(pawn) + kOffPawnCurrentWeapon, &weapon) &&
                reinterpret_cast<void*>(weapon) == self;
    FVec3 origEnd = end;
    float bo[3], bd[3];
    if (mine && vr::control::g_muzzleAim && g_muzzleAimValid && g_armsActive && GunBarrel(pawn, bo, bd)) {
        // Il raggio del colpo diventa quello della canna, lo stesso del mirino:
        // il gioco lo fa partire 25-30 unita' fuori dall'asse (17/10, misurato
        // col punto d'impatto) e verso il bersaglio passava da un'altra parte,
        // fermandosi a volte su ostacoli che la canna non "vede". Origine =
        // punto dell'asse della canna piu' vicino all'origine del gioco.
        float range = std::sqrt((end.x - start.x) * (end.x - start.x) + (end.y - start.y) * (end.y - start.y) +
                                (end.z - start.z) * (end.z - start.z));
        if (range > 1.0f) {
            float ms[3];
            MuzzleStart(pawn, bo, bd, ms);
            start = {ms[0], ms[1], ms[2]};
            end = {start.x + bd[0] * range, start.y + bd[1] * range, start.z + bd[2] * range};
        }
    }
    // Dispersione: l'arsenale tiene l'imprecisione del giocatore (0 = colpo
    // perfetto, sale fino a 1 sparando) e CalcWeaponFire la applica attorno al
    // raggio. Con la mira dalla canna (spread off) vale 0 durante il colpo:
    // i colpi vanno dove punta la canna, come chiede la VR.
    float savedAcc[2] = {0, 0};
    char* arsenal = nullptr;
    uint32_t arsenalPtr = 0;
    bool zeroSpread = mine && !vr::control::g_spread && vr::control::g_muzzleAim && g_armsActive &&
                      TrySafeReadUint32(static_cast<char*>(pawn) + kOffPawnArsenalMgr, &arsenalPtr) && arsenalPtr;
    if (zeroSpread) {
        arsenal = reinterpret_cast<char*>(arsenalPtr);
        static const float zero2[2] = {0, 0};
        if (!SafeCopy(savedAcc, arsenal + kOffArsenalAccuracy, 8) || !SafeCopy(arsenal + kOffArsenalAccuracy, zero2, 8))
            zeroSpread = false;
    }
    g_CalcWeaponFire_Original(self, start, end, impact);
    if (zeroSpread) SafeCopy(arsenal + kOffArsenalAccuracy, savedAcc, 8);
    if (mine) {
        vr::haptics::Shot(CurrentWeaponInfo(self).cls, g_twoHandActive);
        if (g_armsActive) GunKick(CurrentWeaponInfo(self).cls);
    }
    if (mine && vr::control::g_fireLog) {
        // FImpactInfo: HitActor +0, HitLocation +4.
        float hit[3] = {0, 0, 0};
        SafeCopy(hit, static_cast<char*>(impact) + 4, 12);
        uint32_t hitActor = 0;
        TrySafeReadUint32(impact, &hitActor);
        std::string hitName = hitActor ? vr::script::ClassName(reinterpret_cast<void*>(hitActor)) : "none";
        if (!GunBarrel(pawn, bo, bd)) bo[0] = bo[1] = bo[2] = bd[0] = bd[1] = bd[2] = 0.0f;
        char b[600];
        snprintf(b, sizeof(b),
                 "SHOT: from (%.1f, %.1f, %.1f) to (%.1f, %.1f, %.1f), game wanted (%.1f, %.1f, %.1f), barrel -> (%.1f, %.1f, %.1f), "
                 "barrel origin (%.1f, %.1f, %.1f) axis (%.3f, %.3f, %.3f), impact (%.1f, %.1f, %.1f) on %s",
                 start.x, start.y, start.z, end.x, end.y, end.z, origEnd.x, origEnd.y, origEnd.z, g_muzzleTarget[0],
                 g_muzzleTarget[1], g_muzzleTarget[2], bo[0], bo[1], bo[2], bd[0], bd[1], bd[2], hit[0], hit[1], hit[2],
                 hitName.c_str());
        vr::log::Line(b);
    }
}

void* GetPlayerViewPointThunkAddress() {
    union {
        void (GetPlayerViewPointThunk::*mfp)(float*, int32_t*);
        void* fp;
    } converter;
    converter.mfp = &GetPlayerViewPointThunk::Detour;
    return converter.fp;
}

// USkeletalMeshComponent::UpdateSkelPose(float DeltaTime, UBOOL bTickFaceFX)
// @ 0x006455E0 (chiamata da execForceSkelUpdate a 0x008306E0). Diagnostica
// "skellog N": per la mesh di Walker chi lo chiama e dove finisce la mano.
// Distanza tra l'osso Hand_Right e il bersaglio dell'IK della mano destra
// (scritto da DriveHand nello spazio dell'attore), nel mondo.
float g_lastShoulderToTarget = 0.0f;  // diagnostica: spalla destra -> bersaglio IK
float HandToEffectorDistance(void* mesh, int side = 0) {
    float eff[3], loc[3], scale = 1.0f;
    int32_t rot[3];
    char* p = static_cast<char*>(g_fpPawn);
    if (!g_rig.ctrl[side]) return 0.0f;
    if (!SafeCopy(eff, static_cast<char*>(g_rig.ctrl[side]) + kOffLimbEffector, 12) || !SafeCopy(loc, p + 0x54, 12) ||
        !SafeCopy(rot, p + kOffActorRotation, 12) || !SafeCopy(&scale, p + kOffActorDrawScale, 4))
        return 0.0f;
    if (!(scale > 0.01f)) scale = 1.0f;
    float af[3], ar[3], au[3];
    RotatorToAxes(rot[0], rot[1], rot[2], af, ar, au);
    float target[3];
    for (int i = 0; i < 3; ++i) target[i] = loc[i] + (af[i] * eff[0] + ar[i] * eff[1] + au[i] * eff[2]) * scale;
    static const int32_t handNames[2] = {vr::script::NameIndex("Hand_Right"), vr::script::NameIndex("Hand_Left")};
    const int32_t handName = handNames[side];
    struct {
        int32_t name[2];
        int32_t space;
        float ret[3];
    } hp = {{handName, 0}, 0, {0, 0, 0}};
    if (handName < 0 || !vr::script::CallFunction(mesh, "GetBoneLocation", &hp)) return 0.0f;
    if (side == 0 && vr::control::g_skelLogCalls.load() > 0) {
        static const int32_t shName = vr::script::NameIndex("Shoulder_Right");
        struct {
            int32_t name[2];
            int32_t space;
            float ret[3];
        } sp = {{shName, 0}, 0, {0, 0, 0}};
        if (shName >= 0 && vr::script::CallFunction(mesh, "GetBoneLocation", &sp)) {
            float sx = sp.ret[0] - target[0], sy = sp.ret[1] - target[1], sz = sp.ret[2] - target[2];
            g_lastShoulderToTarget = std::sqrt(sx * sx + sy * sy + sz * sz);
        }
    }
    // Bersaglio oltre la portata del braccio (granata, pugno, braccio teso):
    // la mano buona sta sul punto raggiungibile verso il bersaglio, non sul
    // bersaglio. Senza questo ogni allungamento oltre la portata sembrava un
    // tick cattivo e congelava lo scheletro per 150 ms (audit 19/10).
    float shoulder[3], reach = 0.0f;
    if (ArmReach(mesh, side, shoulder, &reach)) {
        float v[3] = {target[0] - shoulder[0], target[1] - shoulder[1], target[2] - shoulder[2]};
        float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (len > reach && len > 1e-3f)
            for (int i = 0; i < 3; ++i) target[i] = shoulder[i] + v[i] * (reach / len);
    }
    float dx = hp.ret[0] - target[0], dy = hp.ret[1] - target[1], dz = hp.ret[2] - target[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void KeepGoodBones(void* mesh, bool log) {
    if (!PawnAlive(g_fpPawn)) return;  // ragdoll: le ossa le muove la fisica
    static std::vector<uint8_t> good[2];
    static void* goodMesh = nullptr;
    static uint32_t restored = 0, checks = 0;
    // Per mano (0 destra, 1 sinistra, controllata solo se la guida la mod).
    static double farSince[2] = {0.0, 0.0};   // da quando la mano e' lontana (ms)
    static bool sustained[2] = {false, false};  // lontana davvero: pose accettate finche' non torna
    char* m = static_cast<char*>(mesh);
    uint32_t data[2] = {0, 0}, num[2] = {0, 0};
    for (int a = 0; a < 2; ++a) {
        uintptr_t off = a == 0 ? kOffMeshSpaceBases : kOffMeshLocalAtoms;
        if (!TrySafeReadUint32(m + off, &data[a]) || !TrySafeReadUint32(m + off + 4, &num[a]) || !data[a] ||
            num[a] == 0 || num[a] > 1024)
            return;
    }
    // Anche la mano sinistra: con la presa sull'astina, in un aggiornamento
    // su cinque circa (camminando o fermi) la sinistra finiva 10-15 unita'
    // lontana dalla presa per un frame (19/10, GUNLOG "lh").
    float dist[2] = {HandToEffectorDistance(mesh, 0), g_leftDriven ? HandToEffectorDistance(mesh, 1) : 0.0f};
    float d = dist[0];
    ++checks;
    // I tick cattivi durano al massimo ~12 ms di fila (misurato a 300 fps):
    // lontana per 150 ms la mano lo e' davvero (braccio teso oltre la portata)
    // e si accettano le pose finche' non torna vicina al bersaglio.
    double nowMs = ProfMs();
    bool bad = false;
    for (int h = 0; h < 2; ++h) {
        if (dist[h] <= vr::control::g_armSkelFixDist) {
            sustained[h] = false;
            farSince[h] = 0.0;
        } else if (farSince[h] == 0.0) {
            farSince[h] = nowMs;
        } else if (nowMs - farSince[h] > 150.0) {
            sustained[h] = true;
        }
        if (dist[h] > vr::control::g_armSkelFixDist && !sustained[h]) {
            bad = true;
            d = dist[h];
        }
    }
    if (!bad) {
        for (int a = 0; a < 2; ++a) {
            good[a].resize(num[a] * kBoneAtomSize);
            if (!SafeCopy(good[a].data(), reinterpret_cast<void*>(data[a]), good[a].size())) good[a].clear();
        }
        goodMesh = mesh;
        if (log) {
            char b[120];
            snprintf(b, sizeof(b), "SKELFIX: hand %.1f units from its IK target", d);
            vr::log::Line(b);
        }
        return;
    }
    if (goodMesh != mesh) return;
    bool ok = true;
    for (int a = 0; a < 2; ++a)
        ok = ok && good[a].size() == num[a] * kBoneAtomSize &&
             SafeCopy(reinterpret_cast<void*>(data[a]), good[a].data(), good[a].size());
    ++restored;
    if (log || restored <= 3 || restored % 1000 == 0) {
        char b[200];
        snprintf(b, sizeof(b), "SKELFIX: hand %.1f units from its IK target, previous bones restored%s (%u of %u updates)", d,
                 ok ? "" : " FAILED", restored, checks);
        vr::log::Line(b);
    }
}

constexpr uintptr_t kUpdateSkelPoseAddr = 0x006455E0;
typedef void(__thiscall* UpdateSkelPose_t)(void* self, float dt, int tickFaceFx);
UpdateSkelPose_t g_UpdateSkelPose_Original = nullptr;

class UpdateSkelPoseThunk {
public:
    void Detour(float dt, int tickFaceFx) {
        void* ret = _ReturnAddress();
        bool log = this == g_rig.mesh && vr::control::g_skelLogCalls.load() > 0;
        float lrt = 0.0f;
        int32_t ignore = 0;
        uint8_t limbPre[0x40] = {};  // campi del limb destro prima della composizione (0xBC..0xFB)
        if (log) {
            SafeCopy(&lrt, reinterpret_cast<char*>(this) + 0x1F0, 4);
            SafeCopy(&ignore, reinterpret_cast<char*>(this) + 0x3A0, 4);
            if (g_rig.ctrl[0]) SafeCopy(limbPre, static_cast<char*>(g_rig.ctrl[0]) + 0xBC, sizeof(limbPre));
        }
        g_UpdateSkelPose_Original(this, dt, tickFaceFx);
        // Ossa composte senza le mani IK: in tick isolati, camminando, l'albero
        // d'animazione del gioco dava la posa dell'animazione pur con gli
        // stessi ingressi del nostro IK (bersaglio, spazi, assi, flag,
        // LastRenderTime, altri controlli: tutti verificati uguali), e l'arma
        // agganciata saltava all'anca per un frame (fucile sdoppiato
        // camminando, 19/10). Si riconosce dalla mano destra lontana dal suo
        // bersaglio (8+ unita'; nei tick buoni 0-6, nei cattivi ~32) e si
        // rimettono le ossa dell'ultimo aggiornamento buono: lo scheletro resta
        // fermo per quel tick e l'arma, agganciata dopo, resta in mano.
        if (this == g_rig.mesh && g_armsActive && g_rig.ctrl[0] && g_fpPawn && vr::control::g_armSkelFix)
            KeepGoodBones(this, log);
        if (log) {
            vr::control::g_skelLogCalls.fetch_sub(1);
            struct {
                int32_t name[2];
                int32_t space;
                float ret[3];
            } hp = {{vr::script::NameIndex("Hand_Right"), 0}, 0, {0, 0, 0}};
            vr::script::CallFunction(this, "GetBoneLocation", &hp);
            float st = 0.0f, eff[3] = {0, 0, 0}, tr[3] = {0, 0, 0};
            int32_t mrot[3] = {0, 0, 0};
            if (g_rig.ctrl[0]) {
                SafeCopy(&st, static_cast<char*>(g_rig.ctrl[0]) + kOffCtrlStrength, 4);
                SafeCopy(eff, static_cast<char*>(g_rig.ctrl[0]) + kOffLimbEffector, 12);
            }
            SafeCopy(tr, reinterpret_cast<char*>(this) + kOffMeshTranslationX, 12);
            SafeCopy(mrot, reinterpret_cast<char*>(this) + kOffCompRotation, 12);
            // Forza e bersaglio dei controlli IKBone del gioco (indici 4 e 5).
            float ib[4] = {-1, -1, -1, -1};
            for (int k = 0; k < 2; ++k)
                if (g_rig.ctrl[4 + k]) {
                    SafeCopy(&ib[2 * k], static_cast<char*>(g_rig.ctrl[4 + k]) + kOffCtrlStrength, 4);
                    SafeCopy(&ib[2 * k + 1], static_cast<char*>(g_rig.ctrl[4 + k]) + kOffCtrlStrengthTarget, 4);
                }
            // Rotazione e posizione dell'attore proprietario al momento dell'aggiornamento.
            uint32_t owner = reinterpret_cast<uint32_t>(g_fpPawn);
            // LocalToWorld della mesh: asse X (eff) e origine (tr) al momento dell'aggiornamento.
            float l2w[16];
            if (SafeCopy(l2w, reinterpret_cast<char*>(this) + kOffCompLocalToWorld, sizeof(l2w))) {
                eff[0] = l2w[0];
                eff[1] = l2w[1];
                eff[2] = l2w[2];
                tr[0] = l2w[12];
                tr[1] = l2w[13];
                tr[2] = l2w[14];
            }
            if (owner) SafeCopy(mrot, reinterpret_cast<char*>(owner) + kOffActorRotation, 12);
            const float* pe = reinterpret_cast<const float*>(limbPre);         // EffectorLocation
            const float* pj = reinterpret_cast<const float*>(limbPre + 0x18);  // JointTargetLocation
            char b[480];
            snprintf(b, sizeof(b),
                     "SKELLOG: t=%.1f from %p dt=%.4f face=%d lastRender=%.3f ignoreCtrl=%d ikStrength=%.2f hand (%.1f, %.1f, %.1f) "
                     "effector (%.1f, %.1f, %.1f) meshTr (%.1f, %.1f, %.1f) meshRot (%d, %d, %d) | pre eff (%.1f, %.1f, %.1f) "
                     "spaces %d/%d axes %d/%d joint (%.1f, %.1f, %.1f) limbFlags %02X stretch %.2f..%.2f",
                     ProfMs(), ret, dt, tickFaceFx, lrt, ignore, st, hp.ret[0], hp.ret[1], hp.ret[2], eff[0], eff[1], eff[2],
                     tr[0], tr[1], tr[2], mrot[0], mrot[1], mrot[2], pe[0], pe[1], pe[2], limbPre[0xC], limbPre[0xD],
                     limbPre[0xE], limbPre[0xF], pj[0], pj[1], pj[2], limbPre[0x2C],
                     *reinterpret_cast<const float*>(limbPre + 0x30), *reinterpret_cast<const float*>(limbPre + 0x34));
            vr::log::Line(b);
        }
    }
};

void* UpdateSkelPoseThunkAddress() {
    union {
        void (UpdateSkelPoseThunk::*mfp)(float, int);
        void* fp;
    } converter;
    converter.mfp = &UpdateSkelPoseThunk::Detour;
    return converter.fp;
}

class CalcSceneViewThunk {
public:
    void* Detour(void* a, void* b, void* c, void* d, void* e) {
        g_localPlayer = this;
        static int logged = 0;
        if (logged < 3) {
            const float* v = reinterpret_cast<const float*>(reinterpret_cast<char*>(this) + 0x68);
            char buf[200];
            std::snprintf(buf, sizeof(buf), "CalcSceneView: LocalPlayer=0x%p PC=0x%p Origin=(%.3f, %.3f) Size=(%.3f, %.3f) args=%p %p %p %p %p",
                          this, *reinterpret_cast<void**>(reinterpret_cast<char*>(this) + 0x40), v[0], v[1], v[2], v[3],
                          a, b, c, d, e);
            vr::log::Line(buf);
            ++logged;
        }
        if (vr::control::g_cinemaActive) {
            // Schermo virtuale: il gioco torna a disegnare in 16:9 (bande
            // sopra e sotto); bConstrainAspectRatio era stato azzerato da noi.
            char* pc = *reinterpret_cast<char**>(reinterpret_cast<char*>(this) + 0x40);
            uint32_t camPtr = 0, flags = 0;
            if (pc && TrySafeReadUint32(pc + 0x38C, &camPtr) && camPtr &&
                TrySafeReadUint32(reinterpret_cast<char*>(camPtr) + 0x208, &flags) && !(flags & 2)) {
                *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(camPtr) + 0x208) = flags | 2u;
            }
        } else if (vr::control::g_fullViewport) {
            float* originAndSize = reinterpret_cast<float*>(reinterpret_cast<char*>(this) + 0x68);
            originAndSize[0] = 0.0f;
            originAndSize[1] = 0.0f;
            originAndSize[2] = 1.0f;
            originAndSize[3] = 1.0f;
            // LocalPlayer.Actor (+0x40) = PlayerController; PC.PlayerCamera
            // (+0x38C); Camera.bConstrainAspectRatio = bit 2 di +0x208
            // (ConstrainedAspectRatio 16:9 a +0x210). Verificato dal vivo.
            char* pc = *reinterpret_cast<char**>(reinterpret_cast<char*>(this) + 0x40);
            uint32_t camPtr = 0, flags = 0;
            if (pc && TrySafeReadUint32(pc + 0x38C, &camPtr) && camPtr &&
                TrySafeReadUint32(reinterpret_cast<char*>(camPtr) + 0x208, &flags) && (flags & 2)) {
                *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(camPtr) + 0x208) = flags & ~2u;
            }
        }
        // "aim body": la rotazione della testa entra solo nella vista
        // renderizzata, tramite l'hook di GetPlayerViewPoint attivo soltanto
        // durante CalcSceneView. (Modificare la camera PC+0x77C qui non ha
        // effetto: la POV letta da CalcSceneView arriva da un'altra copia.)
        g_inCalcSceneView = true;
        void* r = g_CalcSceneView_Original(this, a, b, c, d, e);
        g_inCalcSceneView = false;
        return r;
    }
};

void* GetCalcSceneViewThunkAddress() {
    union {
        void* (CalcSceneViewThunk::*mfp)(void*, void*, void*, void*, void*);
        void* fp;
    } converter;
    converter.mfp = &CalcSceneViewThunk::Detour;
    return converter.fp;
}

bool InstallDeviceHooks(IDirect3DDevice9* device) {
    if (g_deviceHooksInstalled) return true;

    void* presentTarget = VTableEntry(device, kIDirect3DDevice9_Present);
    void* resetTarget = VTableEntry(device, kIDirect3DDevice9_Reset);
    {
        struct { int index; void* detour; void** original; } draws[] = {
            {81, reinterpret_cast<void*>(&Hooked_DrawPrimitive), reinterpret_cast<void**>(&g_DrawPrimitive_Original)},
            {82, reinterpret_cast<void*>(&Hooked_DrawIndexedPrimitive), reinterpret_cast<void**>(&g_DrawIndexedPrimitive_Original)},
            {83, reinterpret_cast<void*>(&Hooked_DrawPrimitiveUP), reinterpret_cast<void**>(&g_DrawPrimitiveUP_Original)},
            {84, reinterpret_cast<void*>(&Hooked_DrawIndexedPrimitiveUP), reinterpret_cast<void**>(&g_DrawIndexedPrimitiveUP_Original)},
        };
        for (auto& h : draws) {
            void* t = VTableEntry(device, h.index);
            if (MH_CreateHook(t, h.detour, h.original) == MH_OK) MH_EnableHook(t);
        }
    }


    void* beginSceneTarget = VTableEntry(device, kIDirect3DDevice9_BeginScene);
    void* endSceneTarget = VTableEntry(device, kIDirect3DDevice9_EndScene);
    void* setTransformTarget = VTableEntry(device, kIDirect3DDevice9_SetTransform);
    void* setVsConstTarget = VTableEntry(device, kIDirect3DDevice9_SetVertexShaderConstantF);

    bool ok = true;
    ok &= MH_CreateHook(presentTarget, &Hooked_Present,
                         reinterpret_cast<void**>(&g_Present_Original)) == MH_OK;
    ok &= MH_CreateHook(beginSceneTarget, &Hooked_BeginScene,
                         reinterpret_cast<void**>(&g_BeginScene_Original)) == MH_OK;
    ok &= MH_CreateHook(endSceneTarget, &Hooked_EndScene,
                         reinterpret_cast<void**>(&g_EndScene_Original)) == MH_OK;
    ok &= MH_CreateHook(setTransformTarget, &Hooked_SetTransform,
                         reinterpret_cast<void**>(&g_SetTransform_Original)) == MH_OK;
    ok &= MH_CreateHook(setVsConstTarget, &Hooked_SetVertexShaderConstantF,
                         reinterpret_cast<void**>(&g_SetVertexShaderConstantF_Original)) == MH_OK;

    ok &= MH_CreateHook(resetTarget, &Hooked_Reset,
                         reinterpret_cast<void**>(&g_Reset_Original)) == MH_OK;
    ok &= MH_EnableHook(presentTarget) == MH_OK;
    ok &= MH_EnableHook(resetTarget) == MH_OK;
    ok &= MH_EnableHook(beginSceneTarget) == MH_OK;
    ok &= MH_EnableHook(endSceneTarget) == MH_OK;
    ok &= MH_EnableHook(setTransformTarget) == MH_OK;
    ok &= MH_EnableHook(setVsConstTarget) == MH_OK;

    if (ok) {
        vr::log::Line("IDirect3DDevice9 hooks installed (Present/EndScene/SetTransform/SetVertexShaderConstantF).");
        g_deviceHooksInstalled = true;
    } else {
        vr::log::Line("ERROR: one or more device hooks failed.");
    }

    // Aggancio alla funzione nativa fissa che ci da' il puntatore al Pawn
    // (vedi commento sopra kExecGetPawnViewLocationAddr) e avvio OpenVR per
    // il tracking della testa del Quest 3. Indipendente dagli hook D3D9
    // sopra: se fallisce, il mod continua a funzionare normalmente.
    void* pawnFnTarget = reinterpret_cast<void*>(kExecGetPawnViewLocationAddr);
    if (MH_CreateHook(pawnFnTarget, GetPawnHookThunkAddress(),
                       reinterpret_cast<void**>(&g_ExecGetPawnViewLocation_Original)) == MH_OK &&
        MH_EnableHook(pawnFnTarget) == MH_OK) {
        vr::log::Line("Hook on APawn::execGetPawnViewLocation installed (captures the Pawn pointer).");
    } else {
        vr::log::Line("ERROR: hook on APawn::execGetPawnViewLocation failed.");
    }

    void* viewTargetFnTarget = reinterpret_cast<void*>(kExecGetViewTargetAddr);
    if (MH_CreateHook(viewTargetFnTarget, GetViewTargetHookThunkAddress(),
                       reinterpret_cast<void**>(&g_ExecGetViewTarget_Original)) == MH_OK &&
        MH_EnableHook(viewTargetFnTarget) == MH_OK) {
        vr::log::Line("Hook on APlayerController::execGetViewTarget installed.");
    } else {
        vr::log::Line("ERROR: hook on APlayerController::execGetViewTarget failed.");
    }

    void* cameraFnTarget = reinterpret_cast<void*>(kCameraHookAddr);
    if (MH_CreateHook(cameraFnTarget, GetCameraHookThunkAddress(),
                       reinterpret_cast<void**>(&g_CameraSetter_Original)) == MH_OK &&
        MH_EnableHook(cameraFnTarget) == MH_OK) {
        vr::log::Line("Real camera hook installed (0x007ECAB2, from a public cheat table).");
    } else {
        vr::log::Line("ERROR: real camera hook failed.");
    }

    void* drawTarget = reinterpret_cast<void*>(kViewportDrawAddr);
    if (MH_CreateHook(drawTarget, GetViewportDrawThunkAddress(),
                      reinterpret_cast<void**>(&g_ViewportDraw_Original)) == MH_OK &&
        MH_EnableHook(drawTarget) == MH_OK) {
        vr::log::Line("Hook on FViewport::Draw installed (synchronous stereo).");
    } else {
        vr::log::Line("ERROR: hook on FViewport::Draw failed.");
    }

    void* fovTarget = reinterpret_cast<void*>(kGetFovAngleAddr);
    if (MH_CreateHook(fovTarget, GetFovThunkAddress(),
                      reinterpret_cast<void**>(&g_GetFovAngle_Original)) == MH_OK &&
        MH_EnableHook(fovTarget) == MH_OK) {
        vr::log::Line("Hook on APlayerController::GetFOVAngle installed.");
    } else {
        vr::log::Line("ERROR: hook on GetFOVAngle failed.");
    }

    void* projTarget = reinterpret_cast<void*>(kPerspectiveMatrixAddr);
    if (MH_CreateHook(projTarget, GetPerspectiveThunkAddress(),
                      reinterpret_cast<void**>(&g_PerspectiveMatrix_Original)) == MH_OK &&
        MH_EnableHook(projTarget) == MH_OK) {
        vr::log::Line("Hook on FPerspectiveMatrix installed (off-center eye projection).");
    } else {
        vr::log::Line("ERROR: hook on FPerspectiveMatrix failed.");
    }

    void* gpvpTarget = reinterpret_cast<void*>(kGetPlayerViewPointAddr);
    if (MH_CreateHook(gpvpTarget, GetPlayerViewPointThunkAddress(),
                      reinterpret_cast<void**>(&g_GetPlayerViewPoint_Original)) == MH_OK &&
        MH_EnableHook(gpvpTarget) == MH_OK) {
        vr::log::Line("Hook on GetPlayerViewPoint installed (head only in the view).");
    }

    void* uspTarget = reinterpret_cast<void*>(kUpdateSkelPoseAddr);
    if (MH_CreateHook(uspTarget, UpdateSkelPoseThunkAddress(),
                      reinterpret_cast<void**>(&g_UpdateSkelPose_Original)) == MH_OK &&
        MH_EnableHook(uspTarget) == MH_OK) {
        vr::log::Line("Hook on USkeletalMeshComponent::UpdateSkelPose installed.");
    }

    void* cwfTarget = reinterpret_cast<void*>(kCalcWeaponFireAddr);
    if (MH_CreateHook(cwfTarget, reinterpret_cast<void*>(&Hooked_CalcWeaponFire),
                      reinterpret_cast<void**>(&g_CalcWeaponFire_Original)) == MH_OK &&
        MH_EnableHook(cwfTarget) == MH_OK) {
        vr::log::Line("Hook on YWeapon::CalcWeaponFire installed (shots from the barrel).");
    } else {
        vr::log::Line("ERROR: hook on CalcWeaponFire failed.");
    }

    void* csvTarget = reinterpret_cast<void*>(kCalcSceneViewAddr);
    if (MH_CreateHook(csvTarget, GetCalcSceneViewThunkAddress(),
                      reinterpret_cast<void**>(&g_CalcSceneView_Original)) == MH_OK &&
        MH_EnableHook(csvTarget) == MH_OK) {
        vr::log::Line("Hook on ULocalPlayer::CalcSceneView installed (full-screen view).");
    } else {
        vr::log::Line("ERROR: hook on CalcSceneView failed.");
    }

    PatchIat(kIatMemcpy, "memcpy", reinterpret_cast<void*>(&Hooked_memcpy), &g_memcpyReal);
    InstallAudioListenerHook();
    PatchIat(kIatMemmove, "memmove", reinterpret_cast<void*>(&Hooked_memmove), &g_memmoveReal);

    g_openVrReady = InitOpenVR();

    return ok;
}

// --- D3D9Ex: le risorse D3DPOOL_MANAGED non esistono piu' -------------------
// Diventano D3DPOOL_DEFAULT + D3DUSAGE_DYNAMIC (bloccabili per caricare i
// dati); con D3D9Ex le risorse DEFAULT sopravvivono al Reset, quindi il
// gioco puo' continuare a trattarle come "managed".
typedef HRESULT(WINAPI* CreateTexture_t)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                         IDirect3DTexture9**, HANDLE*);
typedef HRESULT(WINAPI* CreateVolumeTexture_t)(IDirect3DDevice9*, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                               IDirect3DVolumeTexture9**, HANDLE*);
typedef HRESULT(WINAPI* CreateCubeTexture_t)(IDirect3DDevice9*, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                             IDirect3DCubeTexture9**, HANDLE*);
typedef HRESULT(WINAPI* CreateVertexBuffer_t)(IDirect3DDevice9*, UINT, DWORD, DWORD, D3DPOOL,
                                              IDirect3DVertexBuffer9**, HANDLE*);
typedef HRESULT(WINAPI* CreateIndexBuffer_t)(IDirect3DDevice9*, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                             IDirect3DIndexBuffer9**, HANDLE*);
CreateTexture_t g_CreateTexture_Original = nullptr;
CreateVolumeTexture_t g_CreateVolumeTexture_Original = nullptr;
CreateCubeTexture_t g_CreateCubeTexture_Original = nullptr;
CreateVertexBuffer_t g_CreateVertexBuffer_Original = nullptr;
CreateIndexBuffer_t g_CreateIndexBuffer_Original = nullptr;
std::atomic<uint32_t> g_managedConverted{0}, g_managedFailed{0};

DWORD ExUsage(DWORD usage) {
    // DYNAMIC non e' ammesso insieme a render target / depth / mipmap automatiche.
    if (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL | D3DUSAGE_AUTOGENMIPMAP)) return usage;
    return usage | D3DUSAGE_DYNAMIC;
}

void CountManaged(HRESULT hr, const char* what) {
    if (SUCCEEDED(hr)) {
        uint32_t n = ++g_managedConverted;
        if (n == 1 || n % 2000 == 0) vr::log::Line("GPUSHARE: MANAGED resources converted: " + std::to_string(n));
    } else {
        uint32_t n = ++g_managedFailed;
        if (n <= 10) vr::log::Line(std::string("GPUSHARE: conversion failed (") + what + ") hr=" + std::to_string(hr));
    }
}

HRESULT WINAPI Hooked_CreateTexture(IDirect3DDevice9* d, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt,
                                    D3DPOOL pool, IDirect3DTexture9** out, HANDLE* shared) {
    if (pool != D3DPOOL_MANAGED) return g_CreateTexture_Original(d, w, h, levels, usage, fmt, pool, out, shared);
    HRESULT hr = g_CreateTexture_Original(d, w, h, levels, ExUsage(usage), fmt, D3DPOOL_DEFAULT, out, shared);
    if (FAILED(hr)) hr = g_CreateTexture_Original(d, w, h, levels, usage, fmt, D3DPOOL_DEFAULT, out, shared);
    CountManaged(hr, "texture");
    return hr;
}

HRESULT WINAPI Hooked_CreateVolumeTexture(IDirect3DDevice9* d, UINT w, UINT h, UINT depth, UINT levels, DWORD usage,
                                          D3DFORMAT fmt, D3DPOOL pool, IDirect3DVolumeTexture9** out, HANDLE* shared) {
    if (pool != D3DPOOL_MANAGED)
        return g_CreateVolumeTexture_Original(d, w, h, depth, levels, usage, fmt, pool, out, shared);
    HRESULT hr =
        g_CreateVolumeTexture_Original(d, w, h, depth, levels, ExUsage(usage), fmt, D3DPOOL_DEFAULT, out, shared);
    if (FAILED(hr)) hr = g_CreateVolumeTexture_Original(d, w, h, depth, levels, usage, fmt, D3DPOOL_DEFAULT, out, shared);
    CountManaged(hr, "volume");
    return hr;
}

HRESULT WINAPI Hooked_CreateCubeTexture(IDirect3DDevice9* d, UINT edge, UINT levels, DWORD usage, D3DFORMAT fmt,
                                        D3DPOOL pool, IDirect3DCubeTexture9** out, HANDLE* shared) {
    if (pool != D3DPOOL_MANAGED) return g_CreateCubeTexture_Original(d, edge, levels, usage, fmt, pool, out, shared);
    HRESULT hr = g_CreateCubeTexture_Original(d, edge, levels, ExUsage(usage), fmt, D3DPOOL_DEFAULT, out, shared);
    if (FAILED(hr)) hr = g_CreateCubeTexture_Original(d, edge, levels, usage, fmt, D3DPOOL_DEFAULT, out, shared);
    CountManaged(hr, "cubemap");
    return hr;
}

HRESULT WINAPI Hooked_CreateVertexBuffer(IDirect3DDevice9* d, UINT len, DWORD usage, DWORD fvf, D3DPOOL pool,
                                         IDirect3DVertexBuffer9** out, HANDLE* shared) {
    if (pool != D3DPOOL_MANAGED) return g_CreateVertexBuffer_Original(d, len, usage, fvf, pool, out, shared);
    HRESULT hr = g_CreateVertexBuffer_Original(d, len, usage, fvf, D3DPOOL_DEFAULT, out, shared);
    CountManaged(hr, "vertex buffer");
    return hr;
}

HRESULT WINAPI Hooked_CreateIndexBuffer(IDirect3DDevice9* d, UINT len, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                        IDirect3DIndexBuffer9** out, HANDLE* shared) {
    if (pool != D3DPOOL_MANAGED) return g_CreateIndexBuffer_Original(d, len, usage, fmt, pool, out, shared);
    HRESULT hr = g_CreateIndexBuffer_Original(d, len, usage, fmt, D3DPOOL_DEFAULT, out, shared);
    CountManaged(hr, "index buffer");
    return hr;
}

void InstallExResourceHooks(IDirect3DDevice9* device) {
    struct H {
        int index;
        void* detour;
        void** original;
    } hooks[] = {
        {kIDirect3DDevice9_CreateTexture, reinterpret_cast<void*>(&Hooked_CreateTexture),
         reinterpret_cast<void**>(&g_CreateTexture_Original)},
        {kIDirect3DDevice9_CreateVolumeTexture, reinterpret_cast<void*>(&Hooked_CreateVolumeTexture),
         reinterpret_cast<void**>(&g_CreateVolumeTexture_Original)},
        {kIDirect3DDevice9_CreateCubeTexture, reinterpret_cast<void*>(&Hooked_CreateCubeTexture),
         reinterpret_cast<void**>(&g_CreateCubeTexture_Original)},
        {kIDirect3DDevice9_CreateVertexBuffer, reinterpret_cast<void*>(&Hooked_CreateVertexBuffer),
         reinterpret_cast<void**>(&g_CreateVertexBuffer_Original)},
        {kIDirect3DDevice9_CreateIndexBuffer, reinterpret_cast<void*>(&Hooked_CreateIndexBuffer),
         reinterpret_cast<void**>(&g_CreateIndexBuffer_Original)},
    };
    for (auto& h : hooks) {
        void* t = VTableEntry(device, h.index);
        bool ok = MH_CreateHook(t, h.detour, h.original) == MH_OK && MH_EnableHook(t) == MH_OK;
        if (!ok) vr::log::Line("GPUSHARE: resource creation hook failed (index " + std::to_string(h.index) + ")");
    }
    vr::log::Line("GPUSHARE: MANAGED resource conversion hooks installed.");
}

HRESULT WINAPI Hooked_CreateDevice(
    IDirect3D9* self, UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
    DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters,
    IDirect3DDevice9** ppReturnedDeviceInterface) {
    vr::log::Line("CreateDevice called (Adapter=" + std::to_string(Adapter) + ").");
    // Vsync off: il ritmo lo da' WaitGetPoses del visore, e in stereo
    // sincrono ci sono due Present per tick.
    if (pPresentationParameters) pPresentationParameters->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    if (pPresentationParameters) {
        vr::display::UnclampWindow(pPresentationParameters->hDeviceWindow ? pPresentationParameters->hDeviceWindow
                                                                          : hFocusWindow,
                                   static_cast<int>(pPresentationParameters->BackBufferWidth),
                                   static_cast<int>(pPresentationParameters->BackBufferHeight));
    }

    HRESULT hr = E_FAIL;
    if (g_d3d9ex) {
        IDirect3D9Ex* ex = nullptr;
        IDirect3DDevice9Ex* devEx = nullptr;
        if (SUCCEEDED(self->QueryInterface(__uuidof(IDirect3D9Ex), reinterpret_cast<void**>(&ex))) && ex) {
            hr = ex->CreateDeviceEx(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters,
                                    nullptr, &devEx);
            ex->Release();
        }
        if (SUCCEEDED(hr) && devEx) {
            *ppReturnedDeviceInterface = devEx;
            vr::log::Line("GPUSHARE: device created with CreateDeviceEx.");
            InstallExResourceHooks(devEx);
        } else {
            vr::log::Line("GPUSHARE: CreateDeviceEx failed hr=" + std::to_string(hr) + ", normal device.");
            g_d3d9ex = false;
        }
    }
    if (!g_d3d9ex) {
        hr = g_CreateDevice_Original(self, Adapter, DeviceType, hFocusWindow, BehaviorFlags,
                                     pPresentationParameters, ppReturnedDeviceInterface);
    }

    if (SUCCEEDED(hr) && ppReturnedDeviceInterface && *ppReturnedDeviceInterface) {
        vr::log::Line("CreateDevice succeeded, installing the device hooks.");
        InstallDeviceHooks(*ppReturnedDeviceInterface);
    } else {
        vr::log::Line("CreateDevice failed or null device, hr=" + std::to_string(hr));
    }

    return hr;
}

}  // namespace

bool InstallIDirect3D9Hook(IDirect3D9* realD3D9) {
    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED) {
        vr::log::Line("ERROR: MH_Initialize failed.");
        return false;
    }

    void* createDeviceTarget = VTableEntry(realD3D9, kIDirect3D9_CreateDevice);

    if (MH_CreateHook(createDeviceTarget, &Hooked_CreateDevice,
                       reinterpret_cast<void**>(&g_CreateDevice_Original)) != MH_OK) {
        vr::log::Line("ERROR: MH_CreateHook on CreateDevice failed.");
        return false;
    }
    if (MH_EnableHook(createDeviceTarget) != MH_OK) {
        vr::log::Line("ERROR: MH_EnableHook on CreateDevice failed.");
        return false;
    }

    vr::log::Line("Hook on IDirect3D9::CreateDevice installed.");
    return true;
}

}  // namespace vr::hooks

namespace vr::hooks {

bool g_d3d9ex = false;

bool GpuShareRequested() {
    char path[MAX_PATH] = {};
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCSTR>(&GpuShareRequested), &self);
    GetModuleFileNameA(self, path, MAX_PATH);
    std::string dir(path);
    dir = dir.substr(0, dir.find_last_of("\\/"));
    if (GetFileAttributesA((dir + "\\specopsvr_gpushare").c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    // Riga "gpushare on|off" in specopsvr.ini (letta qui perche' il tipo di
    // device si decide prima che l'ini venga eseguito). Senza riga: on.
    bool on = true;
    FILE* f = nullptr;
    if (fopen_s(&f, (dir + "\\specopsvr.ini").c_str(), "r") == 0 && f) {
        char line[256];
        // Solo la prima parola dopo "gpushare", senza badare a spazi iniziali,
        // maiuscole e commenti ("off" in un commento la spegneva; audit 20/10).
        while (fgets(line, sizeof(line), f)) {
            char key[32] = {}, val[32] = {};
            if (std::sscanf(line, " %31s %31s", key, val) < 1 || _stricmp(key, "gpushare") != 0) continue;
            on = !(_stricmp(val, "off") == 0 || _stricmp(val, "0") == 0 || _stricmp(val, "false") == 0 ||
                   _stricmp(val, "no") == 0);
        }
        fclose(f);
    }
    return on;
}

}  // namespace vr::hooks
