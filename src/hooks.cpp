#include "game_addrs.h"
#include "hooks.h"
#include "logger.h"
#include "control.h"
#include "screenshot.h"
#include "vr_output.h"
#include "vr_toast.h"
#include "ui_text.h"
#include "vr_wrist.h"
#include "vr_menu.h"
#include "vr_session.h"
#include "script_call.h"
#include "pure_utils.h"
#include "twohand_math.h"
#include "display_spoof.h"
#include "haptics_ext.h"
#include "audio_diag.h"
#include "vr_input.h"
#include "props.h"
#include "real_d3d9.h"

#include <tlhelp32.h>
#include <intrin.h>
#include <MinHook.h>
#include <openvr.h>
#include "vr_runtime.h"
#include "vr_openxr.h"
#include "vr_fake.h"
#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#include <cstdint>
#include <cmath>
#include <map>
#include <cstdio>
#include <cstring>
#include <sstream>
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

typedef HRESULT(WINAPI* Clear_t)(IDirect3DDevice9* self, DWORD Count, const D3DRECT* pRects, DWORD Flags, D3DCOLOR Color,
                                 float Z, DWORD Stencil);
Clear_t g_Clear_Original = nullptr;

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
bool TurretAimActive();
// Fine dello schermo virtuale e ultimo salto della vista subito dopo (nero di assestamento).
std::atomic<uint64_t> g_cinemaEndMs{0};
std::atomic<uint64_t> g_settleJumpMs{0};
std::atomic<uint64_t> g_teleportMs{0};
// Ricentramento: 150 ms di nero istantaneo, il mondo non "scivola" sotto gli occhi (R-C3).
std::atomic<uint64_t> g_blinkMs{0};
// Yaw grezzo del visore (radianti) dell'ultimo campionamento.
std::atomic<float> g_hmdYawRaw{0.0f};
// Tempo speso nelle scansioni di GObjects (proiettili, granate, cecchini) dal
// thread di gioco, azzerato a ogni riga PROFILE (L12: costo mai misurato).
std::atomic<double> g_profScanMs{0.0};
double ProfMs();
struct ScanTimer {
    double t0 = ProfMs();
    ~ScanTimer() {
        double d = ProfMs() - t0, cur = g_profScanMs.load();
        while (!g_profScanMs.compare_exchange_weak(cur, cur + d)) {}
    }
};
// Minigun dell'elicottero in mano (YWeapon_TurretMiniGunOnFlight).
std::atomic<bool> g_flightGunHeld{false};
// YGamePawn.m_specialMove (byte, YE_SpecialMove) e mossa animata dal gioco in corso.
constexpr uintptr_t kOffPawnSpecialMove = 0x491;
std::atomic<bool> g_pawnSpecialAnim{false};
// Mosse in cui mani e corpo seguono l'animazione del gioco (valori di
// YE_SpecialMove). Fuori: coperture, scatti, girate, colpi subiti, pugno.
bool IsBodyAnimSpecialMove(uint8_t sm) {
    return (sm >= 2 && sm <= 6) ||    // scavalcare / salire sulla copertura
           (sm >= 23 && sm <= 34) ||  // rianimare, esecuzioni, intel, raccogliere armi e casse
           sm == 35 || sm == 37 ||    // discesa, animazione personalizzata
           sm == 41 || sm == 43 ||    // scivolata, mortaio
           (sm >= 44 && sm <= 52) ||  // corda, zipline, scudo umano
           (sm >= 54 && sm <= 57) ||  // atterrato / rialzarsi
           (sm >= 59 && sm <= 64) ||  // torrette (entrata/uscita), inciampi nella sabbia
           sm == 66 || sm == 67;      // adrenalina
}
// Mosse legate alla copertura (il gioco gira Walker verso il riparo: la
// rotazione va tolta dalla vista, movimento nel riferimento del corpo):
// scavalcare 2-6, girata SWAT 10, uscite di lato 11-15, scatto in copertura
// 16-18, aggancio 19, uscita 20-21, fine sporgersi 22. Fuori (M9): scatti
// 38-41 e girata rapida 42 (la vista deve restare dove guarda il giocatore,
// movimento con la testa), colpi subiti 9 e 58, inciampi 63-64.
bool IsCoverLikeSpecialMove(uint8_t sm) {
    if (sm == 0) return false;
    if (sm == 9 || sm == 58 || (sm >= 38 && sm <= 42) || sm == 63 || sm == 64) return false;
    return true;
}
// Mosse in cui la testa di Walker scende davvero (la vista la segue, come
// nelle scene scriptate): afferrato per un'esecuzione 26, corda 44-47,
// zipline 48, scudo umano subito 50/52, atterrato e rialzarsi 54-57.
// Non scavalcare e raccogliere (M8: la vista oscillava di decine di unita').
bool IsNeckFollowSpecialMove(uint8_t sm) {
    return sm == 26 || (sm >= 44 && sm <= 48) || sm == 50 || sm == 52 || (sm >= 54 && sm <= 57);
}
// YE_SpecialMove letto dal gioco (UEnum, 21/10, R-T3): i log SPECIALMOVE e le
// tabelle qui sopra si leggono per nome.
const char* SpecialMoveName(uint8_t sm) {
    static const char* const kNames[] = {
        "None", "Cancel", "MantleOverCover", "VaultOverCoverDropDown", "VaultOverCover", "VaultOverCoverRun",
        "VaultOverCoverAuto", "AIMeleeAttack", "PlayMeleeAttack", "TakeHit", "SwatTurn", "SlipOutFront", "SlipOutSide",
        "SlipOutAlong", "SlipOutAway", "SlipOutAround", "DashIntoCoverSprint", "DashIntoCoverRun", "DashIntoCoverWalk",
        "SnapToCover", "LeaveCoverCenter", "LeaveCoverEdge", "LeavePeeking", "Revive", "ReviveSelf", "BeingRevived",
        "BeingExecuted", "Execute", "IntelOp", "IntelOpStopSucceeded", "IntelOpStopFailed", "PickUpWeapon",
        "PickUpWeaponFromRack", "PickUpFromCrate", "PickUpFromBox", "DropDown", "SpecialDeathAnimation", "CustomAnimation",
        "StartSprintFromWalk", "StartSprintFromRun", "StopSprint", "SprintSlide", "Quickturn", "Mortar", "FastRopePrepare",
        "FastRopeRappel", "FastRopeRappelStraight", "FastRopeRappelStraightNoTurn", "ZiplineTraverse", "TakeHumanShield",
        "TakenAsHumanShield", "ReleaseHumanShield", "ReleasedFromHumanShield", "PlayDeathAnim", "KnockDownPawn",
        "RecoverFromRagdollToDBNOOrNormal", "RecoverFromRagdollToAnimDBNO", "RecoverFromRagdollToDeathDBNO",
        "PlayHitReactionAnim", "TurretEnter", "TurretLeave", "TurretEnterRemote", "TurretLeaveQuick", "StumbleSandBomb",
        "StumbleSandPool", "MoveToPos", "InjectAdrenaline", "ReviveFromDead"};
    return sm < sizeof(kNames) / sizeof(kNames[0]) ? kNames[sm] : "?";
}
bool SafeCopy(void* dst, const void* src, size_t n);
extern std::atomic<bool> g_scopeActive;
extern std::atomic<bool> g_presentScope;
extern std::atomic<uint32_t> g_canvasProjectCalls;
uint32_t ObjClass(void* obj);
bool StillAlive(void* obj, uint32_t cls);
// PlayerController vivo (camera di questo tick o puntatore catturato, entrambi
// verificati in GObjects), oppure null: niente scritture su memoria riusata
// dopo un cambio di livello o una ripartenza (H4).
void* LivePlayerController();
// Giocatore bloccato da una scena (bCinematicMode) o da una mossa animata dal
// gioco, aggiornato dal thread di gioco.
std::atomic<bool> g_gameLockedScene{false};
// Solo bCinematicMode (scena scriptata): dopo, la rotazione di Walker e' voluta.
std::atomic<bool> g_cinematicScene{false};
// La vista segue l'attacco del collo di Walker (scene scriptate, corda, atterrato).
std::atomic<bool> g_neckFollowScene{false};

void* VTableEntry(void* obj, int index) {
    void** vtable = *reinterpret_cast<void***>(obj);
    return vtable[index];
}

void GpuQueriesRelease();

void ReleaseMirrorChain();

HRESULT WINAPI Hooked_Reset(IDirect3DDevice9* self, D3DPRESENT_PARAMETERS* params) {
    vr::output::OnBeforeReset();
    GpuQueriesRelease();
    ReleaseMirrorChain();  // swap chain della finestra specchio (risorsa DEFAULT)
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
    if (!vr::control::g_headSampleOnDraw) {
        // Ora della posa (lancio, props.cpp): non qui. Con 2-3 letture per
        // tick sul thread di rendering l'ora sarebbe quella dell'ultima, non
        // della posa usata dal tick: senza ora la storia delle mani usa
        // quella del tick, come prima della voce 4 (revisione 26/10).
        vr::control::g_poseStampMs = 0.0;
        UpdateHeadTracking();
    }
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
const uintptr_t& kGSystemSettings = vr::addr::g[vr::addr::GSystemSettings];
const uintptr_t& kGSystemSettingsVTable = vr::addr::g[vr::addr::GSystemSettingsVTable];
constexpr int kSettingMotionBlur = 4 + 0x30;
constexpr int kSettingDepthOfField = 4 + 0x38;
// AmbientOcclusion +0x3C (stessa tabella, 28/10). In VR fa ghosting (tester
// 28/10): il preset VR lo spegne all'avvio, ma il menu Grafica del gioco lo
// riaccendeva. Con vrpreset on resta spento anche dopo.
constexpr int kSettingAmbientOcclusion = 4 + 0x3C;

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
    auto* ao = reinterpret_cast<int32_t*>(kGSystemSettings + kSettingAmbientOcclusion);
    if (vr::control::g_vrPreset && *ao != 0) {
        *ao = 0;
        vr::log::Line("POSTFX: ambient occlusion switched back off (ghosting in VR; vrpreset off to allow it).");
    }
}

// --- Hidden-area mesh (R-C7) -------------------------------------------------
//
// Le zone dell'immagine fuori dalla lente non si vedono mai: SteamVR le
// descrive con una mesh per occhio (GetHiddenAreaMesh). Quando il gioco
// azzera lo Z-buffer della scena di un occhio, subito dopo si scrive
// profondita' 0 in quelle zone (Clear con rettangoli: 2 per striscia, 48
// strisce), cosi' il test di profondita' scarta i pixel prima dello shader.
// La mesh viene rasterizzata una volta in una griglia 64x64 per occhio e
// ridotta a intervalli per riga, ristretti di una cella (mai bordi visibili).
extern std::atomic<int> g_drawEye;
extern std::atomic<UINT> g_backBufferW, g_backBufferH;
constexpr int kHamRows = 48, kHamGrid = 64;
struct HamRows {
    bool valid = false;
    int triangles = 0;
    float left[kHamRows], right[kHamRows];  // frazione nascosta da sinistra / da destra per riga
};
HamRows g_ham[2];
unsigned g_hamGen = 0;

void BuildHam(int eye) {
    HamRows& h = g_ham[eye];
    h = HamRows();
    vr::IVRSystem* sys = vr::rt::System();
    if (!sys) return;
    vr::HiddenAreaMesh_t m = sys->GetHiddenAreaMesh(eye == 0 ? vr::Eye_Left : vr::Eye_Right, vr::k_eHiddenAreaMesh_Standard);
    if (!m.pVertexData || m.unTriangleCount == 0) return;
    h.triangles = static_cast<int>(m.unTriangleCount);
    static bool grid[kHamGrid][kHamGrid];
    std::memset(grid, 0, sizeof(grid));
    auto inside = [](float px, float py, const vr::HmdVector2_t* t) {
        auto side = [](float ax, float ay, float bx, float by, float cx, float cy) {
            return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
        };
        float s1 = side(t[0].v[0], t[0].v[1], t[1].v[0], t[1].v[1], px, py);
        float s2 = side(t[1].v[0], t[1].v[1], t[2].v[0], t[2].v[1], px, py);
        float s3 = side(t[2].v[0], t[2].v[1], t[0].v[0], t[0].v[1], px, py);
        bool neg = s1 < 0 || s2 < 0 || s3 < 0, pos = s1 > 0 || s2 > 0 || s3 > 0;
        return !(neg && pos);
    };
    for (uint32_t t = 0; t < m.unTriangleCount; ++t)
        for (int gy = 0; gy < kHamGrid; ++gy)
            for (int gx = 0; gx < kHamGrid; ++gx)
                if (!grid[gy][gx] && inside((gx + 0.5f) / kHamGrid, (gy + 0.5f) / kHamGrid, m.pVertexData + 3 * t))
                    grid[gy][gx] = true;
    // Intervalli per striscia: celle nascoste consecutive dal bordo, tutte le
    // righe della striscia (intersezione), meno una cella di margine.
    for (int r = 0; r < kHamRows; ++r) {
        int y0 = r * kHamGrid / kHamRows, y1 = (r + 1) * kHamGrid / kHamRows;
        int l = kHamGrid, rr = kHamGrid;
        for (int gy = y0; gy < y1; ++gy) {
            int cl = 0;
            while (cl < kHamGrid && grid[gy][cl]) ++cl;
            int cr = 0;
            while (cr < kHamGrid && grid[gy][kHamGrid - 1 - cr]) ++cr;
            l = (std::min)(l, cl);
            rr = (std::min)(rr, cr);
        }
        h.left[r] = l > 1 ? (l - 1) / static_cast<float>(kHamGrid) : 0.0f;
        h.right[r] = rr > 1 ? (rr - 1) / static_cast<float>(kHamGrid) : 0.0f;
    }
    h.valid = true;
    char b[96];
    std::snprintf(b, sizeof(b), "HAM: eye %d mesh with %d triangles rasterized into %d strips.", eye, h.triangles, kHamRows);
    vr::log::Line(b);
}

HRESULT WINAPI Hooked_Clear(IDirect3DDevice9* self, DWORD Count, const D3DRECT* pRects, DWORD Flags, D3DCOLOR Color, float Z,
                            DWORD Stencil) {
    HRESULT hr = g_Clear_Original(self, Count, pRects, Flags, Color, Z, Stencil);
    int eye = g_drawEye.load();
    if (!vr::control::g_ham || eye < 0 || eye > 1 || !(Flags & D3DCLEAR_ZBUFFER) || Count != 0 || Z < 0.5f) return hr;
    D3DVIEWPORT9 vp;
    if (FAILED(self->GetViewport(&vp)) || vp.Width < 256 || vp.Height < 256) return hr;
    // Solo la scena dell'occhio (viewport grande): non le shadow map ne' i
    // buffer piccoli dei post-processi.
    if (vp.Width * 2 < g_backBufferW || vp.Height * 2 < g_backBufferH) return hr;
    unsigned gen = vr::session::Generation();
    if (gen != g_hamGen) {
        g_hamGen = gen;
        BuildHam(0);
        BuildHam(1);
    }
    const HamRows& h = g_ham[eye];
    if (!h.valid) return hr;
    D3DRECT r[2 * kHamRows];
    DWORD n = 0;
    LONG vx = static_cast<LONG>(vp.X), vy = static_cast<LONG>(vp.Y), vw = static_cast<LONG>(vp.Width),
         vh = static_cast<LONG>(vp.Height);
    for (int i = 0; i < kHamRows; ++i) {
        LONG y0 = vy + vh * i / kHamRows;
        LONG y1 = vy + vh * (i + 1) / kHamRows;
        if (h.left[i] > 0.0f) r[n++] = {vx, y0, vx + static_cast<LONG>(vw * h.left[i]), y1};
        if (h.right[i] > 0.0f) r[n++] = {vx + static_cast<LONG>(vw * (1.0f - h.right[i])), y0, vx + vw, y1};
    }
    if (n) g_Clear_Original(self, n, r, D3DCLEAR_ZBUFFER, 0, 0.0f, 0);  // profondita' 0 = davanti a tutto
    return hr;
}

void CheckPresentHook(IDirect3DDevice9* device);

HRESULT WINAPI Hooked_EndScene(IDirect3DDevice9* self) {
    uint64_t count = ++g_frameCount;
    if (count == 1) {
        vr::log::Line("First EndScene intercepted: hook pipeline active.");
    } else if (count % 300 == 0) {
        vr::log::Line("EndScene: " + std::to_string(count) + " frames rendered so far.");
    }
    if (vr::control::g_vsDiag) EndFrameVsConstAnalysis();
    vr::display::KeepSubclass();
    CheckPresentHook(self);
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
// Ultimo cambio di ScreenPercentage (ProfMs) e tick lungo da misurare dopo
// (riga DYNRES con il costo di "scale set", WI-1).
std::atomic<double> g_dynResChangeMs{0.0};
std::atomic<int> g_dynResCostPending{0};  // tick da misurare: quello del cambio e il seguente
// p90 della GPU per tick stereo visto dal controllore (riga GPU), WI-1.
std::atomic<float> g_dynResP90{0.0f};
std::atomic<int> g_presentPercent{100};
// Percentuale della geometria dell'immagine (ritaglio dell'occhio, mirino, HUD,
// riquadri, vignettatura): sempre 100. In gioco il gioco riallarga la scena
// ridotta a tutto il back buffer: cattura del 27/10 nel visore, al 75% la stessa
// inquadratura del 100%. Il "riquadro al centro" del 21/10 era dei menu; il
// "dynres test" misurava solo DrawIndexedPrimitive e non vedeva il passaggio
// finale a tutto schermo. Col ritaglio del riquadro ogni passo della
// risoluzione dinamica ingrandiva l'immagine attorno al centro del buffer,
// diverso per occhio con la proiezione decentrata: occhi diversi, due mirini
// (utente, VDXR a 120 Hz). La ScreenPercentage vera resta in g_screenPercent
// (GPU, righe DYNRES).
constexpr int kGeometryPercent = 100;
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
    int pct = 100;  // ScreenPercentage con cui il frame e' stato disegnato (risoluzione dinamica)
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
// Campioni per la risoluzione dinamica (WI-1): ms per tick stereo, quando sono
// arrivati e a che percentuale era disegnato il frame. Dal thread di
// rendering (GpuFrameEnd) a UpdateDynamicResolution.
struct DynResSample {
    double t;
    float ms;
    int pct;
};
std::mutex g_dynSampleMutex;
DynResSample g_dynSamples[256];
int g_dynSampleCount = 0;

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
    q.pct = g_screenPercent.load();
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
            {
                std::lock_guard<std::mutex> lockS(g_dynSampleMutex);
                if (g_dynSampleCount < 256) g_dynSamples[g_dynSampleCount++] = {ProfMs() * 0.001, static_cast<float>(tick), r.pct};
            }
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
    int hlState = 0;                 // oggetto evidenziato (props.cpp)
    float hlNdc[4] = {0, 0, 0, 0};
    bool laserVis = false;           // raggio dalla canna (laser on)
    float laserNdc[2] = {0, 0};      // partenza del raggio (la fine e' il mirino)
    bool scope = false;  // cannocchiale attivo in questo disegno (L4: la maschera seguiva con un frame di ritardo)
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
// Tick con la spinta della testa che cambia lato, limitata (follow-up 27/10; riga VIEW "hcside"). Review del
// follow-up: contati solo i tick con una spinta nuova dall'altro lato (non il rilascio dopo), e solo su un Pawn.
std::atomic<uint32_t> g_headPushSideTicks{0};
// Review WI-2 (26/10): piani usati dalla collisione della testa nell'ultimo
// tick (0 = nessuno o ritorno a t) e costo delle trace della vista (tempo in
// microsecondi e chiamate a SingleLineCheck, cumulativi: riga VIEW trus / trn).
std::atomic<int> g_headPlanes{0};
// f3 (27/10): i piani dell'ultimo tick vengono tutti da Pawn (1: cambio di lato dolce; riga VIEW "ppawn").
std::atomic<int> g_headPlanesPawn{0};
std::atomic<uint64_t> g_viewTraceUs{0}, g_viewTraceCalls{0};
// Unita' di cui la vista arretra per un muro davanti al corpo (vedi
// GetPlayerViewPointThunk); il corpo in prima persona arretra con lei.
std::atomic<float> g_wallPull{0.0f};
// Review 2 del follow-up (27/10): il corpo che torna sotto la testa (BodyFollow)
// si ferma se l'arretramento dal muro e' attivo o lo diventerebbe col corpo
// girato verso la testa (GetPlayerViewPointThunk, occhio sinistro). Bit: 1
// arretramento attivo, 2 trace basse agganciate, 4 trace agli occhi lungo lo yaw
// della testa, 8 trace basse lungo lo yaw della testa (riga VIEW "ablk").
std::atomic<int> g_bodyAlignBlock{0};
// Telemetria STATS: passaggi allo schermo virtuale e salto massimo della vista
// (occhio sinistro) da un tick all'altro.
std::atomic<uint32_t> g_cinemaSwitches{0};
float g_maxViewJump = 0.0f;
void LogStats();
// Indicatore di mira: NDC per occhio (thread di gioco -> Present).
std::atomic<float> g_markerNdc[2][2];
std::atomic<bool> g_markerVisible[2];
std::atomic<float> g_laserNdc[2][2];   // partenza del raggio per occhio (laser on)
std::atomic<bool> g_laserVisible[2];
std::atomic<bool> g_markerFrameValid{false};
void DrawAimMarker(IDirect3DDevice9* dev, int eye);
// Bersaglio del comando alla squadra (tasto squadra tenuto): posizione nel
// mondo (thread di gioco) e NDC per occhio, disegnato come riquadro arancione.
float g_squadTargetLoc[3] = {0, 0, 0};
std::atomic<unsigned long long> g_squadTargetMs{0};
std::atomic<float> g_targetNdc[2][2];
std::atomic<bool> g_targetVisible[2];
// Oggetto evidenziato (props.cpp): rettangolo NDC per occhio (min x, min y,
// max x, max y) e stato (1 puntato, 2 agganciato).
std::atomic<float> g_hlNdc[2][4];
std::atomic<int> g_hlState[2];
// Mirino del frame in presentazione (thread di rendering), dall'etichetta del
// frame. Prima si leggevano i valori globali, che il thread di gioco poteva
// avere gia' riscritto per il tick seguente: un occhio mostrava il mirino di
// un altro frame e muovendosi lo si vedeva doppio (19/10).
struct PresentMarker {
    int hlState = 0;
    float hlNdc[4] = {0, 0, 0, 0};
    bool vis = false, targetVis = false, laserVis = false;
    float ndc[2] = {0, 0}, targetNdc[2] = {0, 0}, laserNdc[2] = {0, 0};
};
PresentMarker g_presentMarker;
std::atomic<uint32_t> g_markerFromTag{0}, g_markerFromGlobals{0};

double ProfMs();
extern double g_profOutput, g_profPresent;
// Ultimo tick (riga HITCH, WI-1): uscita VR e Present del monitor. Scritti
// dal thread del Present, letti da quello di gioco: atomici (review WI-1).
std::atomic<double> g_lastOutputMs{0.0}, g_lastPresentMs2{0.0};

void PresentMarkerFromGlobals(int eye) {
    g_markerFromGlobals.fetch_add(1, std::memory_order_relaxed);
    // Frame senza occhio (schermo virtuale, caricamenti): niente mirino ne'
    // riquadro del bersaglio. Prima restavano quelli dell'ultimo frame 3D,
    // disegnati sopra filmati e caricamenti (audit 20/10).
    if (eye < 0 || vr::control::g_cinemaActive) {
        g_presentMarker.vis = false;
        g_presentMarker.targetVis = false;
        g_presentMarker.hlState = 0;
        return;
    }
    int e = eye == 1 ? 1 : 0;
    g_presentMarker.vis = g_markerVisible[e];
    g_presentMarker.targetVis = g_targetVisible[e];
    g_presentMarker.laserVis = g_laserVisible[e];
    g_presentMarker.hlState = g_hlState[e];
    for (int i = 0; i < 4; ++i) g_presentMarker.hlNdc[i] = g_hlNdc[e][i];
    for (int i = 0; i < 2; ++i) {
        g_presentMarker.ndc[i] = g_markerNdc[e][i];
        g_presentMarker.targetNdc[i] = g_targetNdc[e][i];
        g_presentMarker.laserNdc[i] = g_laserNdc[e][i];
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
    // Finestra 16:9 senza bordi (registrazioni video): la parte centrale
    // dell'occhio destro ritagliata a 16:9, sempre 1920x1080 (22/10: prima era
    // alta quanto lo schermo con le proporzioni dell'occhio, quasi quadrata).
    // Dal 28/10 e' un'opzione del menu per OBS ("Window Capture" di "SpecOpsVR
    // mirror", che cattura anche una finestra coperta): non piu' sempre in primo
    // piano, sta dietro alle altre e non copre OBS ne' il desktop.
    const int w = 1920, hgt = 1080;
    DWORD style = WS_POPUP | WS_VISIBLE;
    HWND h = CreateWindowExW(WS_EX_NOACTIVATE, wc.lpszClassName, L"SpecOpsVR mirror", style, 0, 0, w, hgt, nullptr,
                             nullptr, wc.hInstance, nullptr);
    if (!h) {
        vr::log::Line("MIRRORWIN: window creation failed.");
        return;
    }
    ShowWindow(h, SW_SHOWNOACTIVATE);
    SetWindowPos(h, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
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

// Swap chain aggiuntiva sulla finestra specchio (thread di rendering): la
// fascia centrale 16:9 del back buffer (occhio destro) ci finisce con uno
// StretchRect, cosi' la finestra mostra quello che si vede nel visore senza
// stirarlo. Rilasciata al Reset del device e alla chiusura della finestra.
IDirect3DSwapChain9* g_mirrorChain = nullptr;
HWND g_mirrorChainHwnd = nullptr;

void ReleaseMirrorChain() {
    if (g_mirrorChain) g_mirrorChain->Release();
    g_mirrorChain = nullptr;
    g_mirrorChainHwnd = nullptr;
}

void PresentMirror(IDirect3DDevice9* dev, HWND mirror) {
    if (!mirror || !IsWindow(mirror)) {
        if (g_mirrorChain) ReleaseMirrorChain();
        return;
    }
    if (g_mirrorChain && g_mirrorChainHwnd != mirror) ReleaseMirrorChain();
    RECT rc = {};
    GetClientRect(mirror, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;
    if (!g_mirrorChain) {
        D3DPRESENT_PARAMETERS pp = {};
        pp.BackBufferWidth = rc.right;
        pp.BackBufferHeight = rc.bottom;
        pp.BackBufferFormat = D3DFMT_X8R8G8B8;
        pp.BackBufferCount = 1;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.hDeviceWindow = mirror;
        pp.Windowed = TRUE;
        pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
        HRESULT hr = dev->CreateAdditionalSwapChain(&pp, &g_mirrorChain);
        if (FAILED(hr) || !g_mirrorChain) {
            g_mirrorChain = nullptr;
            static bool logged = false;
            if (!logged) {
                logged = true;
                char b[96];
                snprintf(b, sizeof(b), "MIRRORWIN: swap chain creation failed (0x%08lX).", static_cast<unsigned long>(hr));
                vr::log::Line(b);
            }
            return;
        }
        g_mirrorChainHwnd = mirror;
        vr::log::Line("MIRRORWIN: 16:9 swap chain " + std::to_string(rc.right) + "x" + std::to_string(rc.bottom) + " created.");
    }
    IDirect3DSurface9* src = nullptr;
    IDirect3DSurface9* dst = nullptr;
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &src)) && src &&
        SUCCEEDED(g_mirrorChain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &dst)) && dst) {
        D3DSURFACE_DESC d = {};
        src->GetDesc(&d);
        // Ritaglio centrale 16:9 dell'occhio (piu' alto che largo): tutta la
        // larghezza, la fascia centrale in altezza.
        LONG w = static_cast<LONG>(d.Width), h = w * 9 / 16;
        if (h > static_cast<LONG>(d.Height)) {
            h = static_cast<LONG>(d.Height);
            w = h * 16 / 9;
        }
        RECT crop = {(static_cast<LONG>(d.Width) - w) / 2, (static_cast<LONG>(d.Height) - h) / 2, 0, 0};
        crop.right = crop.left + w;
        crop.bottom = crop.top + h;
        dev->StretchRect(src, &crop, dst, nullptr, D3DTEXF_LINEAR);
        g_mirrorChain->Present(nullptr, nullptr, nullptr, nullptr, 0);
    }
    if (src) src->Release();
    if (dst) dst->Release();
}

// Present intercettati (qualunque via): EndScene li confronta con i propri per
// accorgersi di un hook di Present scavalcato (vedi CheckPresentHook).
std::atomic<uint32_t> g_presentHits{0};
// Present gia' in corso su questo thread: un secondo aggancio (vtable sopra
// l'hook nel codice, o la swap chain del device) passa diritto.
thread_local int t_presentDepth = 0;

template <class F>
HRESULT PresentBody(IDirect3DDevice9* self, F&& callOriginal) {
    g_presentHits.fetch_add(1, std::memory_order_relaxed);
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
            // Cannocchiale: il tag ha lo stato del disegno prima (GetFovAngle lo
            // rileva durante il disegno); con quello di adesso anche il primo
            // frame col cannocchiale va senza posa (review WI-1). All'uscita un
            // frame in piu' senza posa, come prima del WI-1.
            const bool scopeNow = tag.scope || g_scopeActive.load();
            if (eye >= 0) vr::output::SetRenderPose(eye, tag.poseValid ? &tag.pose : nullptr, tag.tick, tag.offCenter,
                                                 tag.poseValid ? tag.sampleMs : 0.0, scopeNow);
            g_presentOffCenter = tag.offCenter;
            g_presentPercent = tag.screenPercent;
            if (tag.markerSet) {
                g_markerFromTag.fetch_add(1, std::memory_order_relaxed);
                g_presentMarker.vis = tag.markerVis;
                g_presentMarker.targetVis = tag.targetVis;
                g_presentMarker.laserVis = tag.laserVis;
                g_presentMarker.hlState = tag.hlState;
                for (int i = 0; i < 4; ++i) g_presentMarker.hlNdc[i] = tag.hlNdc[i];
                for (int i = 0; i < 2; ++i) {
                    g_presentMarker.ndc[i] = tag.markerNdc[i];
                    g_presentMarker.targetNdc[i] = tag.targetNdc[i];
                    g_presentMarker.laserNdc[i] = tag.laserNdc[i];
                }
                g_presentScope = tag.scope;
            } else {
                PresentMarkerFromGlobals(eye);
                g_presentScope = g_scopeActive.load();
            }
            if (eye >= 0) vr::output::SetRenderScreenPercent(eye, tag.screenPercent);
        } else if (eye >= 0) {
            PresentMarkerFromGlobals(eye);
            // Senza tag: la geometria non dipende dalla ScreenPercentage
            // (kGeometryPercent), quindi niente coppia scartata dopo un cambio.
            vr::output::SetRenderPose(eye, nullptr, 0, false, 0.0, g_scopeActive.load(), false);
            vr::output::SetRenderScreenPercent(eye, kGeometryPercent);
            g_presentOffCenter = false;
            g_presentPercent = kGeometryPercent;
        }
    }
    RecordPresentEye(eye);
    DrawAimMarker(self, eye);
    if (vr::control::g_gpuTimer) GpuFrameEnd(eye);
    if (vr::control::g_dynResTestFrames > 0 && eye >= 0) {
        vr::control::g_dynResTestFrames.fetch_sub(1);
        UINT expect = static_cast<UINT>(g_backBufferW * (g_screenPercent / 100.0f) + 0.5f);
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
    const double outMs = ProfMs() - tOut;
    g_lastOutputMs = outMs;
    g_profOutput += outMs;
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
    PresentMirror(self, mirror);
    HRESULT hr = callOriginal();
    const double presMs = ProfMs() - tPres;
    g_lastPresentMs2 = presMs;
    g_profPresent += presMs;
    return hr;
}

HRESULT WINAPI Hooked_Present(IDirect3DDevice9* self, const RECT* src, const RECT* dst, HWND wnd,
                              const RGNDATA* dirty) {
    if (t_presentDepth > 0) return g_Present_Original(self, src, dst, wnd, dirty);
    ++t_presentDepth;
    HRESULT hr = PresentBody(self, [&] { return g_Present_Original(self, src, dst, wnd, dirty); });
    --t_presentDepth;
    return hr;
}

// Aggancio di riserva (issue #3, 28/10): sul PC di un tester il nostro hook nel
// codice di Present smetteva di essere chiamato dopo il primo frame (EndScene a
// 650 fps, Present fermo a 1, niente al visore; RivaTuner iniettato nel gioco
// riscrive l'inizio di Present). Se succede, si aggancia la voce della vtable
// del device e, se neanche cosi' arrivano Present, quella della swap chain
// implicita (gioco che presenta dalla swap chain).
Present_t g_PresentVtPrev = nullptr;
typedef HRESULT(WINAPI* SwapPresent_t)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
SwapPresent_t g_SwapPresentPrev = nullptr;

HRESULT WINAPI Hooked_PresentVt(IDirect3DDevice9* self, const RECT* src, const RECT* dst, HWND wnd,
                                const RGNDATA* dirty) {
    if (t_presentDepth > 0) return g_PresentVtPrev(self, src, dst, wnd, dirty);
    ++t_presentDepth;
    HRESULT hr = PresentBody(self, [&] { return g_PresentVtPrev(self, src, dst, wnd, dirty); });
    --t_presentDepth;
    return hr;
}

HRESULT WINAPI Hooked_SwapPresent(IDirect3DSwapChain9* sc, const RECT* src, const RECT* dst, HWND wnd,
                                  const RGNDATA* dirty, DWORD flags) {
    IDirect3DDevice9* dev = nullptr;
    if (t_presentDepth > 0 || sc == g_mirrorChain || FAILED(sc->GetDevice(&dev)) || !dev)
        return g_SwapPresentPrev(sc, src, dst, wnd, dirty, flags);
    dev->Release();  // il device vive quanto la swap chain
    ++t_presentDepth;
    HRESULT hr = PresentBody(dev, [&] { return g_SwapPresentPrev(sc, src, dst, wnd, dirty, flags); });
    --t_presentDepth;
    return hr;
}

bool SwapVtableEntry(void* obj, int index, void* replacement, void** previous) {
    void** entry = *reinterpret_cast<void***>(obj) + index;
    DWORD old;
    if (!VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *previous = *entry;
    *entry = replacement;
    VirtualProtect(entry, sizeof(void*), old, &old);
    FlushInstructionCache(GetCurrentProcess(), entry, sizeof(void*));
    return true;
}

std::string DescribeCodeStart(void* fn) {
    const uint8_t* p = static_cast<const uint8_t*>(fn);
    char b[96];
    std::snprintf(b, sizeof(b), "%s [%02X %02X %02X %02X %02X]",
                  DescribeAddress(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(fn))).c_str(), p[0], p[1], p[2],
                  p[3], p[4]);
    std::string s = b;
    if (p[0] == 0xE9) {  // jmp rel32: chi ha agganciato
        uint32_t to = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)) + 5 + *reinterpret_cast<const int32_t*>(p + 1);
        s += " -> " + DescribeAddress(to);
    }
    return s;
}

// Da EndScene (thread di rendering): 300 EndScene di fila senza un Present
// intercettato = hook scavalcato. Livello 1 vtable del device, livello 2 swap chain.
std::atomic<bool> g_testPresentUnhook{false};
void* g_presentTarget = nullptr;

void CheckPresentHook(IDirect3DDevice9* device) {
    static uint32_t lastHits = 0, quietEnds = 0;
    static int level = 0;
    if (g_testPresentUnhook.exchange(false) && g_presentTarget) {
        MH_DisableHook(g_presentTarget);
        vr::log::Line("PRESENT: test: our hook in Present's code removed (presentunhook).");
    }
    const uint32_t hits = g_presentHits.load(std::memory_order_relaxed);
    if (hits != lastHits) {
        lastHits = hits;
        quietEnds = 0;
        return;
    }
    if (++quietEnds < 300 || level >= 2) return;
    quietEnds = 0;
    if (level == 0) {
        level = 1;
        void* entry = VTableEntry(device, kIDirect3DDevice9_Present);
        vr::log::Line("PRESENT: 300 frames without our Present hook being called (another program rewrote it?). "
                      "Present code: " + DescribeCodeStart(g_presentTarget) + ", vtable entry: " + DescribeCodeStart(entry) +
                      ". Hooking the device vtable instead.");
        if (!SwapVtableEntry(device, kIDirect3DDevice9_Present, reinterpret_cast<void*>(&Hooked_PresentVt),
                             reinterpret_cast<void**>(&g_PresentVtPrev)))
            vr::log::Line("PRESENT: vtable hook FAILED.");
        return;
    }
    level = 2;
    IDirect3DSwapChain9* sc = nullptr;
    if (SUCCEEDED(device->GetSwapChain(0, &sc)) && sc) {
        vr::log::Line("PRESENT: still no Present through the device: hooking the swap chain's Present (" +
                      DescribeCodeStart(VTableEntry(sc, 3)) + ").");
        if (!SwapVtableEntry(sc, 3, reinterpret_cast<void*>(&Hooked_SwapPresent),
                             reinterpret_cast<void**>(&g_SwapPresentPrev)))
            vr::log::Line("PRESENT: swap chain hook FAILED.");
        sc->Release();
    }
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
// Cannocchiale del frame in presentazione (dall'etichetta del disegno, L4).
std::atomic<bool> g_presentScope{false};

// Cannocchiale: la maschera del gioco copre un'area 16:9 centrata; sopra e
// sotto (back buffer quasi quadrato) si vedrebbe la scena ingrandita senza
// maschera: si anneriscono le due strisce.
// Dal 19/10 (audit) il cannocchiale e' una lente: fuori da un cerchio al
// centro dell'occhio (raggio 0,42 della mezza altezza del ritaglio, ~25 gradi)
// tutto nero, come guardando nel cannocchiale; la vista ingrandita a tutto
// campo, girando con la testa, dava nausea. Il cerchio e' fatto di strisce
// orizzontali annerite fuori dalla corda (128 rettangoli in un Clear).
void DrawScopeBars(IDirect3DDevice9* dev, IDirect3DSurface9* bb, int eye = 0) {
    if (!g_presentScope) return;  // dal disegno in presentazione, non dallo stato attuale (L4)
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

// Pixel per grado al centro dell'immagine dell'occhio (R-C2): mirino e
// riquadro a dimensione angolare costante, qualunque siano risoluzione del
// visore, risoluzione dinamica e campo visivo. Senza VR: 90 gradi.
extern float g_mainViewProj[16];
float PixelsPerDegree(const D3DSURFACE_DESC& d, float pk) {
    // FOV orizzontale della scena disegnata (matrice c0-c3 della vista
    // principale, come in LogStats); altrimenti quello chiesto, altrimenti 90.
    const float* mv = g_mainViewProj;
    auto n3 = [](float a, float b2, float c) { return std::sqrt(a * a + b2 * b2 + c * c); };
    float cw = n3(mv[3], mv[7], mv[11]), cx = n3(mv[0], mv[4], mv[8]);
    float fov = (cw > 0.0f && cx > 0.0f) ? 2.0f * std::atan(cw / cx) * 57.2957795f : 0.0f;
    if (!(fov > 5.0f && fov < 175.0f)) fov = vr::control::g_fovOverride > 0.0f ? vr::control::g_fovOverride.load() : 90.0f;
    float half = std::tan(fov * 0.5f * 0.01745329252f);
    if (half < 0.1f) half = 0.1f;
    return (d.Width * pk * 0.5f / half) * 0.01745329252f;
}

// Riquadro arancione (angoli) sul bersaglio dell'ordine alla squadra.
// Oggetto da prendere evidenziato (props.cpp): angoli luminosi attorno al suo
// rettangolo proiettato, azzurri se puntato o a portata, dorati e piu' spessi
// se agganciato; un bordo scuro sotto per leggerli sulla sabbia. Voce 8:
// rossi su una granata nemica a terra (3), piu' spessi se la sinistra la
// puo' prendere (4).
void DrawHighlight(IDirect3DDevice9* dev, IDirect3DSurface9* bb, const D3DSURFACE_DESC& d) {
    int st = g_presentMarker.hlState;
    if (st <= 0) return;
    const float* n = g_presentMarker.hlNdc;
    if (n[2] < -1.0f || n[0] > 1.0f || n[3] < -1.0f || n[1] > 1.0f) return;
    float pk = g_presentPercent / 100.0f;
    float ppd = PixelsPerDegree(d, pk);
    auto px = [&](float nx) { return static_cast<LONG>((nx * 0.5f * pk + 0.5f) * d.Width); };
    auto py = [&](float ny) { return static_cast<LONG>((0.5f - ny * 0.5f * pk) * d.Height); };
    LONG pad = (std::max)(3L, static_cast<LONG>(0.25f * ppd));
    LONG x0 = px(n[0]) - pad, x1 = px(n[2]) + pad, y0 = py(n[3]) - pad, y1 = py(n[1]) + pad;
    LONG w = x1 - x0, hgt = y1 - y0;
    if (w < 4 || hgt < 4) return;
    LONG t = (std::max)(2L, static_cast<LONG>((st == 2 || st == 4 ? 0.22f : 0.14f) * ppd + 0.5f));
    LONG l = (std::max)(t * 3, (std::min)(w, hgt) / 4);
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
    auto corners = [&](LONG g, D3DCOLOR color) {
        D3DRECT r[8] = {
            {x0 - g, y0 - g, x0 + l + g, y0 + t + g}, {x0 - g, y0 - g, x0 + t + g, y0 + l + g},
            {x1 - l - g, y0 - g, x1 + g, y0 + t + g}, {x1 - t - g, y0 - g, x1 + g, y0 + l + g},
            {x0 - g, y1 - t - g, x0 + l + g, y1 + g}, {x0 - g, y1 - l - g, x0 + t + g, y1 + g},
            {x1 - l - g, y1 - t - g, x1 + g, y1 + g}, {x1 - t - g, y1 - l - g, x1 + g, y1 + g},
        };
        dev->Clear(8, r, D3DCLEAR_TARGET, color, 1.0f, 0);
    };
    corners(1, D3DCOLOR_XRGB(0, 0, 0));
    corners(0, st >= 3 ? D3DCOLOR_XRGB(255, 40, 30) : (st == 2 ? D3DCOLOR_XRGB(255, 200, 60) : D3DCOLOR_XRGB(140, 220, 255)));
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, scissor);
    dev->SetViewport(&oldVp);
    if (oldRt) {
        dev->SetRenderTarget(0, oldRt);
        oldRt->Release();
    }
}

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
    // Mezzo lato 1,2 gradi, angoli 0,5 gradi, spessore 0,14 gradi.
    float ppd = PixelsPerDegree(d, pk);
    const LONG s = (std::max)(8L, static_cast<LONG>(1.2f * ppd + 0.5f)), l = (std::max)(4L, static_cast<LONG>(0.5f * ppd + 0.5f)),
               t = (std::max)(2L, static_cast<LONG>(0.14f * ppd + 0.5f));
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

// Raggio laser (laser on): segmento dalla partenza (25 unita' davanti alla
// canna) al punto d'impatto (il mirino), fatto di quadratini con Clear come il
// mirino: 0,08 gradi di spessore, rosso con bordo scuro.
void DrawLaser(IDirect3DDevice9* dev, IDirect3DSurface9* bb, const D3DSURFACE_DESC& d) {
    if (!g_presentMarker.laserVis) return;
    float pk = g_presentPercent / 100.0f;
    auto px = [&](const float* ndc, LONG* cx, LONG* cy) {
        *cx = static_cast<LONG>((ndc[0] * 0.5f * pk + 0.5f) * d.Width);
        *cy = static_cast<LONG>((0.5f - ndc[1] * 0.5f * pk) * d.Height);
    };
    LONG x0, y0, x1, y1;
    px(g_presentMarker.laserNdc, &x0, &y0);
    px(g_presentMarker.ndc, &x1, &y1);
    if (std::fabs(g_presentMarker.ndc[0]) > 1.5f || std::fabs(g_presentMarker.ndc[1]) > 1.5f) return;
    float ppd = PixelsPerDegree(d, pk);
    LONG th = (std::max)(2L, static_cast<LONG>(0.08f * ppd + 0.5f));
    float len = std::sqrt(static_cast<float>((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0)));
    int n = static_cast<int>(len / (th * 0.8f)) + 1;
    if (n > 400) n = 400;
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
    static D3DRECT r[400];
    auto draw = [&](LONG grow, D3DCOLOR color) {
        DWORD cnt = 0;
        for (int i = 0; i < n; ++i) {
            float t = n > 1 ? static_cast<float>(i) / (n - 1) : 0.0f;
            LONG cx = x0 + static_cast<LONG>((x1 - x0) * t), cy = y0 + static_cast<LONG>((y1 - y0) * t);
            if (cx < -th || cy < -th || cx > static_cast<LONG>(d.Width) + th || cy > static_cast<LONG>(d.Height) + th) continue;
            r[cnt++] = {cx - th / 2 - grow, cy - th / 2 - grow, cx + th / 2 + 1 + grow, cy + th / 2 + 1 + grow};
        }
        if (cnt) dev->Clear(cnt, r, D3DCLEAR_TARGET, color, 1.0f, 0);
    };
    draw(1, D3DCOLOR_XRGB(40, 0, 0));
    draw(0, D3DCOLOR_XRGB(255, 50, 40));
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
    DrawHighlight(dev, bb, d);
    DrawLaser(dev, bb, d);
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

    // Spazio 0,2 gradi, bracci 0,55 gradi, spessore 0,1 gradi, bordo 0,07 (R-C2).
    float ppd = PixelsPerDegree(d, pk);
    const LONG gap = (std::max)(3L, static_cast<LONG>(0.2f * ppd + 0.5f)), len = (std::max)(6L, static_cast<LONG>(0.55f * ppd + 0.5f)),
               th = (std::max)(2L, static_cast<LONG>(0.1f * ppd + 0.5f)), b = (std::max)(1L, static_cast<LONG>(0.07f * ppd + 0.5f));
    {
        static LONG lastLen = -1;
        if (len != lastLen) {
            lastLen = len;
            char lb[160];
            snprintf(lb, sizeof(lb), "MARKER: %u px wide, scene %d%%, fov %.1f -> %.1f px/deg: gap %ld len %ld thickness %ld", d.Width,
                     g_presentPercent.load(), vr::control::g_fovOverride.load(), ppd, gap, len, th);
            vr::log::Line(lb);
        }
    }
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
    float full[16];  // effetti a tutta vista: nello spazio dell'occhio, ristretta come la scena
    bool fullEye = false;  // "full" gia' calcolata da AdjustCanvasMatrix (proiezione decentrata)
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

// fullOut (facoltativo): matrice per gli effetti a tutta vista (sangue ai
// bordi, lampi) con la proiezione decentrata: come l'HUD ma a scala 1 e a
// distanza infinita. Prima si usava la matrice originale, uguale in pixel nei
// due occhi: con i centri ottici in punti diversi del back buffer il sangue
// aveva una disparita' enorme, si vedeva doppio e copriva la mira (tester
// 22/10, "aiming impossible when there is blood in my eyes", "cross-eyed").
bool AdjustCanvasMatrix(UINT start, const float* data, UINT count, float* out, float* fullOut = nullptr,
                        bool* fullOk = nullptr) {
    if (fullOk) *fullOk = false;
    // Il Canvas carica la matrice in c0-c3 o in c6-c9 a seconda dello shader.
    if ((start != 0 && start != 6) || count != 4 || !IsCanvasMatrix(data)) return false;
    if (vr::control::g_cinemaActive) return false;  // schermo virtuale: HUD/sottotitoli come sul monitor (la Scaleform dei menu non passa di qui)
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
        if (fullOut && fullOk) {
            float parInf = par - ((depth > 0.0f && half > 0.0f) ? (eye == 0 ? 1.0f : -1.0f) * half / depth : 0.0f);
            std::memcpy(fullOut, data, sizeof(float) * 16);
            for (int i = 0; i < 4; ++i) {
                fullOut[4 * i + 0] = fullOut[4 * i + 0] * th * pj.m00 + (parInf * pj.m00 + pj.m20) * fullOut[4 * i + 3];
                fullOut[4 * i + 1] = fullOut[4 * i + 1] * tv * pj.m11 + (parY * pj.m11 + pj.m21) * fullOut[4 * i + 3];
                fullOut[4 * i + 0] *= pk;
                fullOut[4 * i + 1] *= pk;
            }
            *fullOk = true;
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
    float hud[16], fullEye[16];
    bool fullEyeOk = false;
    if (AdjustCanvasMatrix(StartRegister, pConstantData, Vector4fCount, hud, fullEye, &fullEyeOk)) {
        g_canvasState.adjusted = true;
        g_canvasState.reg = StartRegister;
        std::memcpy(g_canvasState.orig, pConstantData, sizeof(g_canvasState.orig));
        std::memcpy(g_canvasState.adj, hud, sizeof(g_canvasState.adj));
        g_canvasState.fullEye = fullEyeOk;
        if (fullEyeOk) {
            std::memcpy(g_canvasState.full, fullEye, sizeof(g_canvasState.full));
        } else {
            FrameTag cur;
            bool tagOk = PeekRenderingTag(&cur);
            float pk = (tagOk ? cur.screenPercent : g_presentPercent.load()) / 100.0f;
            if (!(pk > 0.2f && pk <= 1.0f) || (tagOk ? cur.scope : g_scopeActive.load())) pk = 1.0f;
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
const uintptr_t& kExecGetPawnViewLocationAddr = vr::addr::g[vr::addr::ExecGetPawnViewLocation];
const uintptr_t& kExecGetViewTargetAddr = vr::addr::g[vr::addr::ExecGetViewTarget];  // APlayerController::execGetViewTarget
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
const uintptr_t& kCameraHookAddr = vr::addr::g[vr::addr::CameraHook];
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
// Indici FName di PlayerController/LocalPlayer, risolti fuori dall'hook
// (ResolveCameraClassNames, tick di gioco). Il valore-firma 0.05 a +0x64 non
// basta da solo: il 23/10 all'inizio della scena degli ostaggi (cambio di
// controller) un altro oggetto con 0.05 a +0x64 e' passato da qui ed e'
// diventato "la camera"; il suo finto LocalPlayer puntava nel codice dell'exe
// e la scrittura del ViewState dell'occhio destro ha fatto crashare il gioco.
std::atomic<int32_t> g_pcNameIndex{-1}, g_lpNameIndex{-1};

// Solo letture intere protette (niente stringhe/allocazioni: usata anche
// dall'hook del setter con ST0 carico). UObject: Name +0x2C, Class +0x34,
// UStruct::SuperField +0x3C.
bool ObjIsANameIndex(const void* obj, int32_t nameIndex) {
    uint32_t c = 0;
    if (!obj || nameIndex < 0 || !TrySafeReadUint32(const_cast<char*>(static_cast<const char*>(obj)) + 0x34, &c)) return false;
    for (int i = 0; c && i < 32; ++i) {
        uint32_t name = 0;
        if (!TrySafeReadUint32(reinterpret_cast<char*>(c) + 0x2C, &name)) return false;
        if (static_cast<int32_t>(name) == nameIndex) return true;
        if (!TrySafeReadUint32(reinterpret_cast<char*>(c) + 0x3C, &c)) return false;
    }
    return false;
}

void ResolveCameraClassNames() {
    if (g_pcNameIndex.load() < 0) g_pcNameIndex = vr::script::NameIndex("PlayerController");
    if (g_lpNameIndex.load() < 0) g_lpNameIndex = vr::script::NameIndex("LocalPlayer");
}

// Camera vera: firma + l'oggetto 0x77C prima e' una PlayerController. Finche'
// l'indice del nome non e' noto (primi istanti) basta la firma, come prima.
bool IsPlayerCameraCandidate(void* p) {
    if (!HasCameraSignature(p)) return false;
    int32_t pcIdx = g_pcNameIndex.load(std::memory_order_relaxed);
    return pcIdx < 0 || ObjIsANameIndex(static_cast<char*>(p) - 0x77C, pcIdx);
}

extern "C" void __cdecl CameraHookCheck(void* dest) {
    if (IsPlayerCameraCandidate(dest)) {
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
uint32_t g_playerControllerCls = 0;  // classe al momento della cattura (StillAlive)
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
            g_playerControllerCls = ObjClass(self);
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
            // Il runtime finto ("runtime fake", collaudi 25/10) non tocca SteamVR ne'
            // OpenXR: con novr parte solo lui.
            if (!vr::rt::FakeChosen()) {
                vr::log::Line("OpenVR NOT initialised (specopsvr_novr file present).");
                return false;
            }
            vr::log::Line("specopsvr_novr file present: SteamVR and OpenXR stay off, only the fake runtime starts (runtime fake).");
        }
    }
    // Background all'avvio, Scene solo con il visore indossato (vr_session.h).
    return vr::session::Init();
}

// Estrae lo yaw (rotazione attorno all'asse verticale Y) dalla matrice di
// posa OpenVR (convenzione Y-up, mano destra). Il segno/offset esatto
// potrebbe richiedere una calibrazione empirica la prima volta che si
// prova nel visore.
// Guardando quasi dritto in basso (o in alto) la proiezione orizzontale
// dell'avanti e' corta e rumorosa: si aggiunge quella dell'asse "su" della
// testa, che in quel caso punta avanti (in basso) o indietro (in alto).
// Prima lo yaw saltava e poteva far girare il corpo (audit 19/10).
// Il calcolo sta in vr::pure::BlendedYaw (25/10, coperto dai test offline):
// non e' lo yaw esatto con la testa inclinata di lato (vr::pure::ExactEuler).
float YawFromOpenVrPose(const vr::HmdMatrix34_t& m) { return vr::pure::BlendedYaw(m.m); }

// Estrae anche il pitch (rotazione attorno all'asse X/laterale) dalla
// stessa matrice di posa, stessa convenzione di YawFromOpenVrPose.
float PitchFromOpenVrPose(const vr::HmdMatrix34_t& m) { return vr::pure::AsinPitch(m.m); }

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
const uintptr_t& kIatMemcpy = vr::addr::g[vr::addr::IatMemcpy];
const uintptr_t& kIatMemmove = vr::addr::g[vr::addr::IatMemmove];

typedef void*(__cdecl* Memcpy_t)(void* dst, const void* src, size_t n);
Memcpy_t g_memcpyReal = nullptr;
Memcpy_t g_memmoveReal = nullptr;

// Delta calcolati dal thread di rendering (UpdateHeadTracking), applicati
// dal thread di gioco (memcpy). Unita' Rotator UE3 (65536 = 360 gradi).
std::atomic<int32_t> g_deltaYaw{0};
std::atomic<int32_t> g_deltaRoll{0};
// Direzione della testa rispetto al corpo (unita'), dallo yaw stabile
// (YawFromOpenVrPose): verso del movimento, posizione del corpo, aiuto alle
// esecuzioni. La vista usa g_deltaYaw (yaw esatto, WI-1 25/10).
std::atomic<int32_t> g_headHeadingYaw{0};
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
// g_bodyYaw letto dalla camera del gioco (senza le rotazioni della mod fatte dopo nello stesso tick).
std::atomic<int32_t> g_bodyYawAtCamera{0};
// Review 2 del follow-up (27/10): rotazione scritta da UpdatePawnFacing nello
// yaw del Pawn in questo Draw (0 se nessuna). Dal f3 solo diagnostica: UpdateArms
// non anticipa piu' (vedi MeshL2WSnap, sotto).
std::atomic<int32_t> g_pawnFacingStep{0};
std::atomic<int32_t> g_armsLeadUsed{0};  // riga GUNLOG "lead": dal f3 correzione di yaw dell'ultimo aggiornamento delle ossa
// Diagnostica f3 (riga SKELDIAG con "gunlog"): chiamate di UpdateSkelPose sulla mesh di Walker dall'ultima
// riga GUNLOG (indirizzo di ritorno, yaw della LocalToWorld della mesh prima e dopo, yaw del Pawn), yaw con cui
// UpdateArms ha convertito le mani e yaw della mesh in quel momento. Solo con gunlog attivo.
struct SkelPoseDiag {
    int n = 0, facing = 0, arms = 0, restored = 0, roomShift = 0;
    void* ret[4] = {};
    float pre[4] = {}, post[4] = {}, actor[4] = {}, fixDeg[4] = {}, fixUnits[4] = {};
    float armsYaw = 0.0f, armsMesh = 0.0f, restoreDeg = 0.0f, restoreHand = 0.0f, restoreSeam = 0.0f;
};
SkelPoseDiag g_skelPoseDiag;
float CompL2WYawDeg(const void* comp);
float TrunkYawRelDeg(void* mesh);  // f4, diagnostica SKELDIAG "trunk"
// f3 (27/10), causa delle punte di un tick della canna (misurata con SKELDIAG): il gioco aggiorna le ossa di
// Walker (UpdateSkelPose, dal tick della mesh 0x00D35E30) con la LocalToWorld della mesh di PRIMA: la
// LocalToWorld si aggiorna dopo, dalla rotazione del Pawn di adesso. I bersagli IK sono nello spazio dell'attore
// (rotazione di adesso) e il gioco li porta nello spazio della mesh con la LocalToWorld vecchia: con il Pawn
// girato da UpdatePawnFacing (scrittura diretta di Rotation, nessun aggiornamento delle componenti) l'arma
// disegnata stava avanti di un passo. L'anticipo della review 2 lo compensava, ma (1) nei tick in cui
// KeepGoodBones rimette le ossa buone del tick prima (circa 1 su 6, il gioco da' la posa dell'animazione) quelle
// ossa erano di un passo prima: canna avanti di un passo per un tick (le punte), (2) oltre 5 gradi (tick lunghi)
// l'anticipo era 0. Ora: (a) prima dell'aggiornamento delle ossa la LocalToWorld della mesh e' quella che il
// gioco calcolera' subito dopo (relativa all'attore presa all'inizio del Draw, Translation di adesso, attore di
// adesso), poi rimessa com'era: la conversione e' esatta per qualunque passo, nessun anticipo; (b) le ossa
// rimesse da KeepGoodBones si correggono (f4: trasformazione rigida intera, solo le braccia, vedi
// KeepGoodBones). "armslead off" = nessuna delle due (collaudi).
struct MeshL2WSnap {
    void* mesh = nullptr;
    void* pawn = nullptr;
    bool valid = false;     // relativa stabile (due scatti di fila uguali)
    float rel[4][3] = {};   // LocalToWorld nel riferimento dell'attore (righe X, Y, Z; origine)
    float trans[3] = {};    // Translation della mesh allo scatto
    float cand[4][3] = {};  // ultimo scatto (candidato)
    float candTrans[3] = {};
    bool haveCand = false;
};
MeshL2WSnap g_meshL2WSnap;
// f4 (27/10): riferimento delle mani nel mondo dell'ultimo UpdateArms (origine del tracciamento e yaw, come
// HandTarget e props SetHandFrame): le ossa buone lo ricordano con la LocalToWorld con cui sono state composte,
// quelle rimesse si portano dove le mani sono adesso (vr::pure::ue3m::RestoreCorrection). Thread di gioco.
vr::pure::ue3m::HandFrameW g_armsFrameW{};
bool g_armsFrameValid = false;
// Review del f3 (bassa): tick con una rotazione della mod in cui la LocalToWorld non si e' potuta correggere
// (scatto non valido: posa del gioco che gira la mesh rispetto all'attore); allora UpdateArms anticipa ancora del
// passo (come la review 2). Contatori per la riga SKELFIX ogni 30 s.
uint32_t g_l2wTurnTicks = 0, g_l2wTurnUnfixed = 0;
void SnapMeshL2W(void* pawn);
std::atomic<int32_t> g_bodyPitch{0};
// Spostamento della testa in unita' Unreal nel sistema di riferimento del
// corpo al momento del ricentramento: [0] avanti, [1] destra, [2] su.
std::atomic<float> g_headOffset[3];
// Passo room-scale fatto in questo tick (mondo, XY): il POV del gioco viene
// dal tick, prima del Draw in cui il Pawn si sposta, quindi la base della
// vista resterebbe indietro di un frame (L8). Si somma alla base del tick.
float g_roomStepWorld[2] = {0.0f, 0.0f};
bool g_roomStepValid = false;
// Spostamenti dello scostamento della testa fatti dalla mod (mondo, XY, unita'),
// non dalla testa: compensazione del perno degli occhi quando il corpo gira e
// passi room-scale. Sommati qui fino al prossimo calcolo del movimento forzato
// (vignettatura), che li toglie (review del follow-up 27/10: ogni corpo che
// tornava sotto la testa faceva scattare la vignettatura piena). Passi
// room-scale contati per la riga VIEW (rstep).
float g_headShiftWorld[2] = {0.0f, 0.0f};
std::atomic<uint32_t> g_roomSteps{0};
std::atomic<float> g_forcedMotionOld{0.0f};  // diagnostica: il calcolo di prima (riga VIEW forcedold)

// AActor::moveSmooth nativa (0x0088FB70, __thiscall(actor, const FVector*)
// -> UBOOL): MoveActor con salita dei gradini e scivolamento lungo i muri.
// Chiamata diretta: MoveSmooth e' "native(3969)" e UObject::ProcessEvent
// (0x005AAF20) ignora in silenzio le funzioni con iNative != 0 (UFunction
// +0x98), quindi CallFunction "riusciva" senza muovere nulla (21/10: la
// room-scale non ha mai spostato Walker, ne' con Move ne' con MoveSmooth).
// execMoveSmooth azzera prima il bit 0x400 di Actor+0xA4 (bJustTeleported).
const uintptr_t& kActorMoveSmoothAddr = vr::addr::g[vr::addr::ActorMoveSmooth];
bool CallMoveSmoothNative(void* actor, const float* delta, int* ret) {
    typedef int(__thiscall * MoveSmooth_t)(void*, const float*);
    if (!kActorMoveSmoothAddr) return false;
    __try {
        *reinterpret_cast<uint32_t*>(static_cast<char*>(actor) + 0xA4) &= ~0x400u;
        *ret = reinterpret_cast<MoveSmooth_t>(kActorMoveSmoothAddr)(actor, delta);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Sui mezzi (elicottero, camion: Pawn non a terra) la testa non puo'
// uscire dall'abitacolo: spostamento orizzontale della testa entro 25 unita'
// (R-B2 "cabin box"); a piedi nessun limite (room-scale).
bool PawnOnFirmGround(void* pawn);
void* PlayerPawnIfViewTarget(char* cam);
void ClampCabinOffset() {
    char* cam = static_cast<char*>(g_cameraPtr.load());
    void* pawn = HasCameraSignature(cam) ? PlayerPawnIfViewTarget(cam) : nullptr;
    if (!pawn || PawnOnFirmGround(pawn)) return;
    float f = g_headOffset[0].load(), r = g_headOffset[1].load();
    float len = std::sqrt(f * f + r * r);
    const float kBox = 25.0f;
    if (len > kBox) {
        g_headOffset[0] = f * kBox / len;
        g_headOffset[1] = r * kBox / len;
    }
}

// Sposta il Pawn di (dx, dy) nel mondo con moveSmooth (M10/R-B4): moved =
// spostamento reale (gradini e muri lo riducono).
// f4 (review del f3): ossa di Walker spostate di -d nel mondo (spazio della componente), vedi KeepGoodBones.
bool ShiftBonesForRoomStep(void* pawn, const float d[3]);
bool RoomStepMove(void* pawn, float dx, float dy, float* moved) {
    float before[3], after[3];
    if (!pawn || !SafeCopy(before, static_cast<char*>(pawn) + 0x54, 12)) return false;
    float delta[3] = {dx, dy, 0.0f};
    int ret = 0;
    // f4: MoveSmooth aggiorna subito la mesh e l'arma agganciata con le ossa composte per la posizione di prima:
    // mani e arma disegnate (la volata, con "muzzleaim" l'origine dei colpi) si spostavano del passo per un frame
    // (5 unita') e tornavano al tick dopo. Le ossa si spostano prima di -passo: per questo frame tutto il corpo
    // resta dov'era nel mondo (lo scheletro composto per la posizione di prima) e il tick dopo lo ricompone nel
    // posto nuovo; la vista non cambia comunque. Se il Pawn si muove meno (ostacolo), il resto si rimette dopo.
    const bool shifted = ShiftBonesForRoomStep(pawn, delta);
    if (!CallMoveSmoothNative(pawn, delta, &ret) || !SafeCopy(after, static_cast<char*>(pawn) + 0x54, 12)) {
        if (shifted) {
            const float back[3] = {-delta[0], -delta[1], -delta[2]};
            ShiftBonesForRoomStep(pawn, back);
        }
        return false;
    }
    if (shifted) {
        const float rest[3] = {(after[0] - before[0]) - delta[0], (after[1] - before[1]) - delta[1], (after[2] - before[2]) - delta[2]};
        // Ossa a -delta, volute a -spostamento vero: altri -(vero - delta).
        if (rest[0] * rest[0] + rest[1] * rest[1] + rest[2] * rest[2] > 1e-4f) ShiftBonesForRoomStep(pawn, rest);
    }
    moved[0] = after[0] - before[0];
    moved[1] = after[1] - before[1];
    if (moved[0] * moved[0] + moved[1] * moved[1] > 1e-6f) {
        g_roomStepWorld[0] = moved[0];
        g_roomStepWorld[1] = moved[1];
        g_roomStepValid = true;
        g_roomSteps.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}
// Controller destro: posizione rispetto alla testa al ricentramento (unita'
// del gioco: avanti, destra, su, nel riferimento del corpo) e rollio.
std::atomic<float> g_handOffset[3];
std::atomic<int32_t> g_handRoll{0};
std::atomic<bool> g_handValid{false};
std::atomic<bool> g_twoHandActive{false};  // per DUMPCAM
std::atomic<bool> g_weaponShort{false};     // pistole e Uzi: la sinistra avvolge la destra (CurrentWeaponInfo)
std::atomic<bool> g_leftOnGun{false};      // mano sinistra agganciata all'arma (UpdateArms)
std::atomic<bool> g_leftDriven{false};     // mano sinistra guidata dalla mod (KeepGoodBones)
// Controller sinistro: posizione come g_handOffset, direzione relativa come
// g_aimDelta* (yaw rispetto al riferimento, pitch assoluto) e rollio.
std::atomic<float> g_leftOffset[3];
std::atomic<int32_t> g_leftYaw{0}, g_leftPitch{0}, g_leftRoll{0};
std::atomic<bool> g_leftValid{false};
// Riferimento con cui sono stati calcolati gli scostamenti delle mani qui
// sopra (per i salti del riferimento degli oggetti tenuti, props.cpp): yaw del
// riferimento della testa in unita' di rotatore (x g_yawSign) e scostamento
// dell'origine del tracciamento nel riferimento del corpo (unita'). Le mani
// stanno in center + Rz(bodyYaw) x scostamento: un punto fermo nella stanza
// sta in center + Rz(bodyYaw) x (origine + Rz(-riferimento) x posizione), e
// cambia nel mondo solo con l'imbardata effettiva bodyYaw - riferimento.
// Copertura e torretta girano corpo e riferimento insieme: le mani restano
// ferme nel mondo e non e' un salto (revisione 25/10).
std::atomic<int32_t> g_trackRefYaw{0};
std::atomic<float> g_trackOriginOff[3];
int32_t RadiansToUnreal(float rad);

// Mira a due mani: vedi "Presa a due mani (WI-4b)" dopo ApplyLeftControllerAngles
// (vr::twohand::Solver, twohand_math.h). Prima qui ApplyTwoHandAim / TwoHandBlend:
// linea tra le origini raw, scarto che decadeva in 0,7 s, filtro da 80 ms.

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
        if (v >= vr::addr::TextBegin() && v < vr::addr::TextEnd()) g_copyStack[n++] = v;
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
const uintptr_t& kNearClipPlaneAddr = vr::addr::g[vr::addr::NearClipPlane];

void ApplyNearPlane(char* cam) {
    static float original = -1.0f;
    float* nearPlane = reinterpret_cast<float*>(kNearClipPlaneAddr);
    if (!nearPlane) return;
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

// Avanzamento degli occhi rispetto al centro di Walker applicato nell'ultima
// copia della camera (fp a terra, 0 sui mezzi, 0 se la vista non e' su Walker).
std::atomic<float> g_fwdApplied{0.0f};
// Sporgenza di Walker fuori dal riparo (UpdateCoverLean), in unita' del mondo:
// la vista (e con lei mani e arma) la segue.
std::atomic<float> g_coverLean[2] = {{0.0f}, {0.0f}};

// Diagnostica (viewlog): yaw (unita') con cui ApplyFirstPerson ha portato gli occhi fp avanti.
// Yaw (unita') con cui ApplyFirstPerson ha messo gli occhi fp avanti: direzione
// dell'arretramento dal muro (WI-2; prima solo diagnostica g_viewDiagFpYaw).
std::atomic<int32_t> g_fpYawApplied{0};

void ApplyFirstPerson(char* cam) {
    g_fwdApplied = 0.0f;
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
    g_fwdApplied = fwd;  // per le correzioni del perno (girando, gli occhi non devono scivolare)
    g_fpYawApplied = static_cast<int32_t>(std::lround(yaw * (65536.0f / 6.28318530718f)));
    loc[0] = p[0] + fwd * std::cos(yaw) + g_coverLean[0].load(std::memory_order_relaxed);
    loc[1] = p[1] + fwd * std::sin(yaw) + g_coverLean[1].load(std::memory_order_relaxed);
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
bool g_lastMovieSeen = false;  // filmato Bink a tutto schermo in corso (schermo virtuale piatto anche con cinema3d)

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
// Neck di Walker in piedi sopra il centro del Pawn (media, senza il nostro
// abbassamento; UpdateFirstPersonBody), -1000 = non ancora misurato (WI-3).
std::atomic<float> g_walkerNeckStandZ{-1000.0f};
std::atomic<double> g_lastRecenterMs{-1.0e9};  // ultimo ricentramento (ProfMs), per la discesa vera (WI-3)
float ViewHeightExtra() {
    // Seduti (WI-3): occhi all'altezza degli occhi di Walker (36,25 sopra il
    // centro del Pawn, misurata), non piu' fp (40); la testa vera dal riferimento
    // seduto si aggiunge a parte. Costante: il Neck misurato dal vivo dipende dal
    // terreno (sulle dune la mesh del gioco scende di ~4 unita') e la vista
    // cambierebbe altezza camminando; resta nella riga BODY (neckstand).
    if (vr::control::g_seated) return vr::pure::kWalkerEyeStand - vr::control::g_fpEyeHeight;
    if (vr::control::g_heightAuto && vr::control::g_eyeHeightAuto >= 0.0f)
        return vr::control::g_eyeHeightAuto - vr::control::g_fpEyeHeight;
    // "height fixed" (WI-3): gli occhi di Walker (prima fp, 3,75 unita' sopra).
    if (!vr::control::g_heightAuto) return vr::pure::kWalkerEyeStand - vr::control::g_fpEyeHeight;
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
// Walker a piedi su terreno fermo, torretta o no (WI-2): Physics Walking o
// Falling, nessun veicolo, Base ferma. Base in movimento = oltre 20 unita'/s
// nell'ultimo secondo (vr::pure::BaseMovingFast; prima 0,5 unita' tra due
// chiamate qualsiasi, anche nello stesso tick: un oggetto fisico che si
// assestava sotto Walker faceva cambiare modo alla vista). La velocita' si
// misura al piu' una volta ogni 5 ms (piu' chiamate per tick).
bool PawnOnStaticGround(void* pawn) {
    if (!pawn) return false;
    char* p = static_cast<char*>(pawn);
    uint32_t phys = 0, base = 0, veh = 0;
    // Actor.Physics: 1 PHYS_Walking, 2 PHYS_Falling. Anche cadendo (gradini,
    // salti da una sporgenza) Walker e' a piedi: prima contava come mezzo e
    // per un attimo avanzamento, base della vista e corpo cambiavano modo (M2).
    if (!TrySafeReadUint32(p + 0x94, &phys) || ((phys & 0xFF) != 1 && (phys & 0xFF) != 2)) return false;
    if (TrySafeReadUint32(p + 0x3B8, &veh) && veh) return false;
    if (!TrySafeReadUint32(p + 0xAC, &base)) return false;
    static uint32_t lastBase = 0;
    static float lastLoc[3] = {0, 0, 0};
    static double lastT = 0.0, lastMove = -1.0e9;
    float loc[3];
    const double now = ProfMs() * 0.001;
    if (base && SafeCopy(loc, reinterpret_cast<char*>(base) + 0x54, 12)) {
        if (base != lastBase) {
            lastMove = -1.0e9;
            lastBase = base;
            lastT = now;
            std::memcpy(lastLoc, loc, sizeof(loc));
        } else if (now - lastT >= 0.005) {
            const float dx = loc[0] - lastLoc[0], dy = loc[1] - lastLoc[1], dz = loc[2] - lastLoc[2];
            if (vr::pure::BaseMovingFast(std::sqrt(dx * dx + dy * dy + dz * dz), static_cast<float>(now - lastT))) lastMove = now;
            lastT = now;
            std::memcpy(lastLoc, loc, sizeof(loc));
        }
    } else {
        lastBase = 0;
        lastMove = -1.0e9;
    }
    return now - lastMove >= 1.0;
}
// Come sopra, e non a una torretta: corpo, room-scale, avanzamento fp e
// rotazione del corpo restano "sul mezzo" su ogni torretta.
bool PawnOnFirmGround(void* pawn) { return !g_onTurret && PawnOnStaticGround(pawn); }
// Base del Pawn = mondo (nessuna, o bStatic / bWorldGeometry, Actor +0x9C bit
// 0x2 / 0x100): un elicottero fermo in volo stazionario non lo e'.
bool PawnBaseIsWorld(void* pawn) {
    uint32_t base = 0, flags = 0;
    if (!pawn || !TrySafeReadUint32(static_cast<char*>(pawn) + 0xAC, &base)) return false;
    if (!base) return true;
    return TrySafeReadUint32(reinterpret_cast<char*>(base) + 0x9C, &flags) && (flags & 0x102);
}
// Vista "a terra" (WI-2): a piedi su terreno fermo, anche a una torretta fissa
// a terra (base del mondo): formula di terra (centro del Pawn + altezza +
// testa, smorzamento). Prima ogni torretta usava la base del collo di Walker
// (filtrata su 1 s) e il limite di 6 unita' della testa, fatti per la cabina
// dell'elicottero.
bool ViewOnFirmGround(void* pawn) { return PawnOnStaticGround(pawn) && (!g_onTurret || PawnBaseIsWorld(pawn)); }

bool PawnAlive(void* pawn) {
    uint32_t hp = 0;
    return pawn && TrySafeReadUint32(static_cast<char*>(pawn) + 0x304, &hp) && static_cast<int32_t>(hp) > 0;
}

// Corpo girato con la testa anche da fermi (23/10, utente: "il collo si gira
// a 360 gradi senza muovere il corpo"). Il gioco gira il Pawn verso la
// PlayerController solo camminando (misurato: PC +90 gradi da fermo, Pawn
// immobile; al primo passo si allinea in ~1 s). "turnwithhead" gira la PC,
// quindi da fermi Walker restava com'era: girandosi si vedevano le sue spalle
// e il collo nascosto. Qui Pawn.Rotation.Yaw (+0x64) segue lo yaw della PC
// (la mesh lo segue al tick dopo; la capsula e' un cilindro). Non in
// copertura, nelle mosse animate, sulle torrette, sui mezzi, nelle scene e da
// morti: li' l'orientamento lo decide il gioco.
void UpdatePawnFacing(char* cam, void* pawn) {
    g_pawnFacingStep = 0;
    if (!pawn || !HasCameraSignature(cam) || vr::control::g_mode == vr::control::kModeOff ||
        !vr::control::g_firstPerson || vr::control::g_cinemaActive || vr::control::g_inCover ||
        vr::control::g_gameLocked || g_pawnSpecialAnim || TurretAimActive() || !PawnAlive(pawn) ||
        !PawnOnFirmGround(pawn))
        return;
    // Copertura letta anche qui (m_coverFaceState +0x498): g_inCover lo
    // aggiorna solo il percorso del visore.
    uint32_t cover = 0;
    if (!TrySafeReadUint32(static_cast<char*>(pawn) + 0x498, &cover) || (cover & 0xFF) != 0) return;
    int32_t pcYaw = 0, pawnYaw = 0;
    if (!SafeCopy(&pcYaw, cam - 0x77C + 0x64, 4) || !SafeCopy(&pawnYaw, static_cast<char*>(pawn) + 0x64, 4)) return;
    int32_t d = static_cast<int16_t>(static_cast<uint16_t>(pcYaw - pawnYaw));
    if (std::abs(d) < 32) return;  // sotto 0,2 gradi
    int32_t v = pawnYaw + d;
    if (!SafeCopy(static_cast<char*>(pawn) + 0x64, &v, 4)) return;
    g_pawnFacingStep = d;  // diagnostica (prima l'anticipo di UpdateArms, sostituito nel f3)
    if (vr::control::g_gunLogFrames.load() > 0) ++g_skelPoseDiag.facing;
    static bool logged = false;
    if (!logged && std::abs(d) > 1820) {  // 10 gradi
        logged = true;
        vr::log::Line("BODY: Walker turned with the view while standing still (" + std::to_string(d * 360 / 65536) +
                      " degrees; the game only turns him when walking).");
    }
}

// Sporgersi dal riparo: mirando in copertura (m_aimingReplicated, +0x5F0 bit
// 0x800, con LT o sporgendosi col corpo: coverpeek) l'animazione porta la
// testa di Walker oltre lo spigolo, ma la vista restava sulla testa vera,
// dietro il riparo: si vedeva il muro e bisognava uscire camminando nella
// stanza (scena degli ostaggi, cap. 1, 23/10: "il corpo rimane attaccato al
// muro, bisogna muoversi fisicamente fuori copertura"; misurato: testa 30
// unita' a destra, 60 cm, colpi dallo spigolo). La vista segue la parte
// laterale (rispetto al Pawn) dello spostamento dell'osso Head dall'origine
// della mesh: la traslazione della mesh decisa dalla mod si annulla, l'altezza
// resta della testa vera (alzarsi da un riparo basso lo fa gia' il giocatore).
bool MeshOwnedBy(void* mesh, void* pawn);
void UpdateCoverLean(char* cam, void* pawn) {
    constexpr uintptr_t kMeshOff = 0x3A8, kL2wOff = 0x70;  // Pawn.Mesh, PrimitiveComponent.LocalToWorld
    static double lastT = 0.0;
    double now = ProfMs() * 0.001;
    float dt = lastT > 0.0 ? static_cast<float>(now - lastT) : 0.011f;
    lastT = now;
    if (dt <= 0.0f || dt > 0.1f) dt = 0.011f;
    float target[2] = {0.0f, 0.0f};
    uint32_t cover = 0, flags = 0, mesh = 0;
    int32_t pawnYaw = 0;
    float l2w[16];
    if (pawn && HasCameraSignature(cam) && vr::control::g_coverLeanEnabled && vr::control::g_mode != vr::control::kModeOff &&
        vr::control::g_firstPerson && !vr::control::g_cinemaActive && PawnAlive(pawn) &&
        TrySafeReadUint32(static_cast<char*>(pawn) + 0x498, &cover) && (cover & 0xFF) != 0 &&
        TrySafeReadUint32(static_cast<char*>(pawn) + 0x5F0, &flags) && (flags & 0x800) &&
        TrySafeReadUint32(static_cast<char*>(pawn) + kMeshOff, &mesh) && mesh &&
        SafeCopy(&pawnYaw, static_cast<char*>(pawn) + 0x64, 4) &&
        SafeCopy(l2w, reinterpret_cast<char*>(mesh) + kL2wOff, sizeof(l2w))) {
        static int32_t headName = -1;
        if (headName < 0) headName = vr::script::NameIndex("Head");
        struct {
            int32_t name[2];
            int32_t space;
            float ret[3];
        } hp = {{headName, 0}, 0, {0, 0, 0}};
        // MeshOwnedBy (WI-3 fix 1): mai GetBoneLocation su una mesh non piu' (o non ancora) di Walker.
        if (headName >= 0 && MeshOwnedBy(reinterpret_cast<void*>(mesh), pawn) &&
            vr::script::CallFunction(reinterpret_cast<void*>(mesh), "GetBoneLocation", &hp) &&
            std::isfinite(hp.ret[0]) && std::isfinite(hp.ret[1])) {
            float a = pawnYaw * (6.28318530718f / 65536.0f);
            float rx = -std::sin(a), ry = std::cos(a);  // destra del Pawn
            float lat = (hp.ret[0] - l2w[12]) * rx + (hp.ret[1] - l2w[13]) * ry;
            // Fermi in copertura la testa sta entro ~5 unita' dall'asse.
            constexpr float kDead = 6.0f, kMax = 45.0f;
            lat = std::fabs(lat) < kDead ? 0.0f : lat - std::copysign(kDead, lat);
            lat = (std::max)(-kMax, (std::min)(kMax, lat));
            target[0] = rx * lat;
            target[1] = ry * lat;
        }
    }
    // Segue l'animazione con una costante di tempo di 80 ms (niente scatti
    // dall'osso da un frame all'altro), anche rientrando.
    float k = 1.0f - std::exp(-dt / 0.08f);
    float cur[2] = {g_coverLean[0].load(), g_coverLean[1].load()};
    for (int i = 0; i < 2; ++i) {
        cur[i] += (target[i] - cur[i]) * k;
        if (std::fabs(cur[i]) < 0.05f && target[i] == 0.0f) cur[i] = 0.0f;
        g_coverLean[i] = cur[i];
    }
    static bool logged = false;
    float len = std::sqrt(cur[0] * cur[0] + cur[1] * cur[1]);
    if (!logged && len > 15.0f) {
        logged = true;
        vr::log::Line("COVER: Walker leans out of cover " + std::to_string(static_cast<int>(len)) +
                      " units, the view goes with him (coverlean).");
    }
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
const uintptr_t& kViewportDrawAddr = vr::addr::g[vr::addr::ViewportDraw];
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
void UpdatePhysicalCrouch(void* pc, void* pawn);
void UpdateCrouchView(void* pawn);
void RunHitTest(void* pawn);
void UpdateSquadCommandAim(char* cam, void* pawn);
void PublishWristData(char* cam);
void InteractionScan(void* pawn);
void InteractionAffordance();
extern float g_handWorld[2][3];
extern bool g_handWorldValid[2];
extern float g_handAxesWorld[2][3][3];
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
                      "%.2f ms (max %.2f) | dynres: stereo tick p90 %.2f ms, EMA %.2f",
                      n ? sum / n : 0.0, mx, n, hz > 0 ? 1000.0f / hz : 0.0f, g_screenPercent.load(),
                      ln ? ls / ln : 0.0, lm, g_dynResP90.load(), g_gpuTickMsEma.load());
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
const uintptr_t& kAllocateViewStateAddr = vr::addr::g[vr::addr::AllocateViewState];
constexpr uint32_t kOffPcPlayer = 0x388, kOffLpViewState = 0x88;
typedef void*(__cdecl* AllocateViewState_t)();

void* CallAllocateViewState() {
    if (!kAllocateViewStateAddr) return nullptr;
    __try {
        return reinterpret_cast<AllocateViewState_t>(kAllocateViewStateAddr)();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// FSceneViewStateInterface::Destroy (slot 0 della vtable; nessun oggetto C++
// qui: __try non convive con la distruzione di temporanei).
bool CallDestroyViewState(uint32_t state) {
    __try {
        typedef void(__thiscall * Destroy_t)(void*);
        (*reinterpret_cast<Destroy_t*>(*reinterpret_cast<uintptr_t*>(state)))(reinterpret_cast<void*>(state));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Indirizzo di LocalPlayer.ViewState del giocatore (nullptr se non valido).
uint32_t* ViewStateSlot(char* cam) {
    uint32_t lp = 0, vs = 0;
    if (!IsPlayerCameraCandidate(cam) || !TrySafeReadUint32(cam - 0x77C + kOffPcPlayer, &lp) || !lp) return nullptr;
    // Il valore viene SCRITTO (ViewState dell'occhio destro): solo su un vero
    // LocalPlayer (crash del 23/10 con un puntatore nel codice dell'exe).
    if (!ObjIsANameIndex(reinterpret_cast<void*>(lp), g_lpNameIndex.load())) return nullptr;
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
        // Il motore, quando ricrea il suo stato (FUN_00c60790), prima chiama
        // lo slot 0 della vtable del vecchio (FSceneViewStateInterface::Destroy,
        // cancellazione rimandata al thread di rendering): lo stesso per il
        // nostro, che prima restava allocato con le sue query di occlusione
        // a ogni cambio di livello (L5; 2 cambi per sessione di regress).
        if (mine) {
            vr::log::Line(CallDestroyViewState(mine) ? "STEREO: previous right-eye view state destroyed."
                                                     : "STEREO: exception destroying the previous right-eye view state.");
            mine = 0;
        }
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
// Lato piu' lungo al massimo 4032, proporzioni mantenute (desktop finto di
// display_spoof a 4096, bordi della finestra compresi).
// Dopo un setres sullo schermo virtuale il gioco puo' restare disposto in un
// riquadro al 65% (menu tagliato, 21/10: setres 0,3 s dopo l'inizio della
// Scene); spegnere e riaccendere lo schermo virtuale lo sistemava. Qui per
// questi frame CalcSceneView toglie bConstrainAspectRatio e poi lo rimette:
// stesso effetto senza lampo.
std::atomic<int> g_relayoutFrames{0};

void UpdateAutoResolution(char* cam) {
    static ULONGLONG last = 0;
    static bool lastMenu = false;
    if (!vr::control::g_autoResolution || !vr::control::g_offCenter) return;
    // Schermo virtuale da almeno 1,5 s (menu, caricamenti, filmati lunghi; le
    // inquadrature brevi delle scene non fanno reset del device). Al menu
    // iniziale il gioco ha comunque una pedina: non si puo' usare quella.
    static ULONGLONG cinemaSince = 0;
    ULONGLONG nowA = GetTickCount64();
    if (!vr::control::g_cinemaActive) cinemaSince = 0;
    else if (!cinemaSince) cinemaSince = nowA;
    bool menu = vr::control::g_menu169 && cinemaSince && nowA - cinemaSince >= 1500;
    if (menu == lastMenu && nowA - last < 5000) return;  // subito al cambio menu/gioco
    lastMenu = menu;
    last = GetTickCount64();
    uint32_t w = 0, h = 0;
    {
        vr::session::Shared vrLock(true);
        if (!vrLock.ok() || !vr::session::IsScene() || !vr::rt::System()) return;
        vr::rt::System()->GetRecommendedRenderTargetSize(&w, &h);
    }
    if (w < 640 || h < 640) return;
    // Desktop finto a 4096: la finestra con i bordi deve starci (64 px di margine).
    vr::pure::FitRecommendedSize(w, h, 4032u);
    // Menu del gioco e filmati (schermo virtuale): back buffer 16:9. I menu UIScene si dispongono per l'altezza del
    // viewport: con il back buffer alto quanto un occhio (2112x2304) il menu
    // era largo 4096 px e tagliato a destra, e la fascia 16:9 lo tagliava
    // sopra e sotto. A 2112x1188 e' completo, come sul monitor (21/10).
    if (menu) h = (w * 9 / 16) & ~7u;
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
    vr::log::Line(std::string("AUTORES: ") + (menu ? "game menu on the virtual screen, 16:9 " : "SteamVR recommends ") +
                  std::to_string(w) + "x" + std::to_string(h) + (menu ? "" : " per eye") + ", game is " +
                  std::to_string(bw) + "x" + std::to_string(bh) + ": " + cmd);
    vr::script::ExecConsole(cam - 0x77C, cmd);
    if (vr::control::g_cinemaActive) g_relayoutFrames = 20;
}

void UpdateDynamicResolution(char* cam, bool mono) {
    // Controllore con isteresi e memoria dei livelli falliti (WI-1, vedi
    // vr::pure::DynResController): prima 80% <-> 85% ogni 0,7-1,4 s.
    static vr::pure::DynResController ctl;
    // Il gioco salva ScreenPercentage nelle sue impostazioni: dopo una sessione
    // con la risoluzione dinamica partiva al 65% (menu iniziale disposto a
    // tutta finestra ma scena al 65%: "menu tagliato", 21/10, verificato
    // senza visore) mentre qui si credeva al 100%. All'avvio si impone il 100%.
    static bool forced = false;
    if (!forced && HasCameraSignature(cam)) {
        forced = true;
        vr::script::ExecConsole(cam - 0x77C, "scale set ScreenPercentage 100");
        g_screenPercent = 100;
        vr::log::Line("DYNRES: ScreenPercentage forced to 100% at start (the game saves the last value).");
    }
    if (!mono) ApplyVrPreset(cam);
    // Anche con lo schermo virtuale (mono): il menu iniziale restava disposto
    // in un riquadro al 65% del frame (voci fuori dal riquadro, "menu
    // tagliato", 21/10) finche' un setres non faceva ridisporre il viewport;
    // con il visore indossato a gioco gia' avviato il setres arrivava solo al
    // primo disegno stereo, cioe' mai sul menu.
    if (HasCameraSignature(cam)) UpdateAutoResolution(cam);
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
    } else {
        const double nowS = ProfMs() * 0.001;
        if (ctl.cur != cur) ctl.Reset(cur, nowS);  // cambiata da fuori (avvio, test, schermo virtuale)
        ctl.minPct = vr::control::g_dynResMin.load();
        ctl.p.down = vr::control::g_dynResDown.load();
        ctl.p.slow = vr::control::g_dynResSlow.load();
        ctl.p.up = vr::control::g_dynResUp.load();
        ctl.p.dwell = vr::control::g_dynResDwellSec.load();
        ctl.p.backoff = vr::control::g_dynResBackoffSec.load();
        {
            std::lock_guard<std::mutex> lock(g_dynSampleMutex);
            for (int i = 0; i < g_dynSampleCount; ++i) ctl.AddSample(g_dynSamples[i].t, g_dynSamples[i].ms, g_dynSamples[i].pct);
            g_dynSampleCount = 0;
        }
        want = ctl.Update(nowS, 1000.0f / hz);
        g_dynResP90 = ctl.lastP90;
    }
    {
        // Invio nello stesso tick: solo con la GPU sotto meta' del frame del
        // visore; si aspetta al massimo un quarto di frame.
        float gpu = g_gpuTickMsEma, frame = hz > 30.0f ? 1000.0f / hz : 0.0f;
        bool ok = vr::control::g_lowLatency && frame > 0.0f && gpu > 0.0f && gpu < 0.5f * frame;
        vr::output::SetSameTickWait(ok ? 0.25f * frame : 0.0f);
    }
    if (want == cur || !HasCameraSignature(cam)) return;
    const double t0 = ProfMs();
    vr::script::ExecConsole(cam - 0x77C, "scale set ScreenPercentage " + std::to_string(want));
    const double cost = ProfMs() - t0;
    g_screenPercent = want;
    g_dynResChangeMs = ProfMs();
    g_dynResCostPending = 2;
    const vr::pure::DynResController::Fail& f = ctl.fail[vr::pure::DynResController::Slot(cur)];
    ctl.Reset(want, ProfMs() * 0.001);
    char b[260];
    std::snprintf(b, sizeof(b),
                  "DYNRES: scene %d%% -> %d%% (GPU p90 %.1f ms per tick over 2 s, EMA %.1f, headset frame %.1f ms; %d%% failed %d "
                  "times; \"scale set\" took %.1f ms)",
                  cur, want, ctl.lastP90, g_gpuTickMsEma.load(), hz > 0 ? 1000.0f / hz : 0.0f, cur, want < cur ? f.count : 0, cost);
    vr::log::Line(b);
}

class ViewportDrawThunk {
public:
    void Detour(int bShouldPresent) {
        void* self = this;
        const double tickStart = ProfMs();
        // Attesa del mutex OpenXR di questo thread nel tracciamento della testa
        // (per differenza del totale del thread, review 2 WI-1).
        const bool xrWait = vr::rt::Active() == vr::rt::Backend::OpenXR;
        const double headWaitBefore = xrWait ? vr::xr::ThreadLockWaitMs() : 0.0;
        {
            // f3: LocalToWorld della mesh appena aggiornata dal tick, prima delle rotazioni e dei passi della mod
            // di questo Draw (UpdateHeadTracking, UpdatePawnFacing).
            char* camS = static_cast<char*>(g_cameraPtr.load());
            SnapMeshL2W(HasCameraSignature(camS) ? PlayerPawnIfViewTarget(camS) : nullptr);
        }
        if (vr::control::g_headSampleOnDraw) {
            // Ora delle pose di questo Draw (velocita' del lancio, props.cpp):
            // solo qui, sul thread di gioco, una lettura per tick.
            const double poseAt = ProfMs();
            UpdateHeadTracking();
            vr::control::g_poseStampMs = poseAt;
        }
        // Riga HITCH per sorgente (WI-1): testa (attesa del mutex OpenXR compresa).
        const double headMs = ProfMs() - tickStart;
        const double headWaitMs = xrWait ? vr::xr::ThreadLockWaitMs() - headWaitBefore : 0.0;
        const double dynResChangeBefore = g_dynResChangeMs.load();
        char* cam = static_cast<char*>(g_cameraPtr.load());
        vr::script::Tick(HasCameraSignature(cam) ? cam - 0x77C : nullptr,
                         HasCameraSignature(cam) ? PlayerPawnIfViewTarget(cam) : nullptr);
        UpdatePhysicalCrouch(HasCameraSignature(cam) ? cam - 0x77C : nullptr,
                             HasCameraSignature(cam) ? PlayerPawnIfViewTarget(cam) : nullptr);
        UpdateCrouchView(HasCameraSignature(cam) ? PlayerPawnIfViewTarget(cam) : nullptr);  // WI-3, dopo l'accucciata vera
        UpdatePawnFacing(cam, HasCameraSignature(cam) ? PlayerPawnIfViewTarget(cam) : nullptr);
        UpdateCoverLean(cam, HasCameraSignature(cam) ? PlayerPawnIfViewTarget(cam) : nullptr);
        RunHitTest(HasCameraSignature(cam) ? PlayerPawnIfViewTarget(cam) : nullptr);
        // WI-3: la gestione del corpo vale sempre in prima persona (Walker opaco,
        // testa nascosta, ancora del collo, KeepGoodBones e dita); "fpbody arms"
        // cambia solo cosa si disegna. Prima "fpbody off" la spegneva tutta:
        // Walker semitrasparente, testa visibile, arma all'anca camminando.
        UpdateFirstPersonBody(HasCameraSignature(cam) ? PlayerPawnIfViewTarget(cam) : nullptr);
        UpdateProjectileLog();
        RedirectNewProjectiles(HasCameraSignature(cam) ? PlayerPawnIfViewTarget(cam) : nullptr);
        RedirectThrownGrenade(HasCameraSignature(cam) ? PlayerPawnIfViewTarget(cam) : nullptr);
        UpdateSquadCommandAim(cam, HasCameraSignature(cam) ? PlayerPawnIfViewTarget(cam) : nullptr);
        PublishWristData(cam);
        InteractionScan(HasCameraSignature(cam) ? PlayerPawnIfViewTarget(cam) : nullptr);  // fase 0 (interactlog)
        InteractionAffordance();                                                             // fase 1 (prossimita')
        vr::props::Watchdog();
        if (vr::control::g_visCheck) VisibilityCheck(HasCameraSignature(cam) ? PlayerPawnIfViewTarget(cam) : nullptr);
        // Mirino del gioco: al centro dello schermo segue la testa e non l'arma;
        // con la mira dalla canna lo sostituisce il mirino verde.
        // YHUD.m_hideCrosshair (+0x480, bit 0x40) sull'HUD del PlayerController (+0x3EC).
        // Anche sulle postazioni fisse mirate col controller (minigun
        // dell'elicottero): li' non c'e' la canna in mano ma il mirino verde
        // c'e' lo stesso, e restavano due mirini (playtest 20/10).
        {
            static bool weHid = false;
            bool hide = HasCameraSignature(cam) && vr::control::g_aimHand &&
                        (g_muzzleAimValid || TurretAimActive()) && !vr::control::g_gameCrosshair;
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
        g_lastMovieSeen = movie;
        // Mosse speciali animate dal gioco per tutto il corpo (corda, zipline,
        // scavalcare, raccogliere, scudo umano, torrette...): come le scene
        // scriptate, mani e corpo all'animazione. Nella discesa con la corda
        // del capitolo 1 le mani restavano sui controller e il corpo
        // spostato/girato: "corpo deforme" (21/10).
        bool specialAnim = false, neckFollow = false;
        if (!noWalker) {
            uint8_t sm = 0;
            if (SafeCopy(&sm, static_cast<char*>(PlayerPawnIfViewTarget(cam)) + kOffPawnSpecialMove, 1)) {
                specialAnim = IsBodyAnimSpecialMove(sm);
                neckFollow = IsNeckFollowSpecialMove(sm);
            }
            static uint8_t lastSm = 0;
            if (sm != lastSm) {
                char b[160];
                snprintf(b, sizeof(b), "SPECIALMOVE: %u (%s) -> %u (%s)%s", lastSm, SpecialMoveName(lastSm), sm, SpecialMoveName(sm),
                         specialAnim ? " (body animated by the game)" : "");
                vr::log::Line(b);
                // Fase 3: impulso d'impatto all'inizio di esecuzione (27) e
                // corpo a corpo (7, 8) nelle due mani.
                if (sm == 27 || sm == 7 || sm == 8) {
                    vr::haptics::Melee();
                    vr::input::PulseHand(0, 120);
                    vr::input::PulseHand(1, 120);
                }
                lastSm = sm;
            }
        }
        g_pawnSpecialAnim = specialAnim;
        g_cinematicScene = cinematicMode;
        g_neckFollowScene = cinematicMode || neckFollow;
        g_gameLockedScene = cinematicMode || specialAnim;
        vr::control::g_gameLocked = cinematicMode || specialAnim;
        {
            // WorldInfo.Pauser (+0x46C) != None: menu di pausa (niente gesti).
            uint32_t wi = 0, pauser = 0;
            bool paused = HasCameraSignature(cam) && TrySafeReadUint32(cam - 0x77C + 0xEC, &wi) && wi &&
                          TrySafeReadUint32(reinterpret_cast<char*>(wi) + 0x46C, &pauser) && pauser;
            // Una riga a ogni cambio (review WI-4c round 2: collaudo dei grip tenuti in pausa).
            if (paused != vr::control::g_gamePaused.load()) vr::log::Line(paused ? "PAUSE: game paused." : "PAUSE: game resumed.");
            vr::control::g_gamePaused = paused;
        }
        // Menu di pausa (29/10, tester: "the menu is too close and to the right"):
        // la Scaleform della pausa non passa dalla matrice dell'HUD, quindi in 3D
        // era disegnata uguale in pixel nei due occhi: vicina e spostata. Sullo
        // schermo virtuale, come gli altri menu del gioco.
        bool wanted = vr::control::g_cinemaEnabled &&
                      (vr::control::g_cinemaForce || lockedScene || noWalker || vr::control::g_gamePaused);
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
            // Ritorno dallo schermo virtuale: nero per 0,7 s e fino a 0,3 s dopo
            // l'ultimo salto della vista (al massimo 1,5 s). All'inizio della
            // scena dell'elicottero Walker restava ~0,5 s a terra e poi saltava
            // di 15000 unita' sull'elicottero: prima persona, lampo, scena, tre
            // dissolvenze di fila (21/10).
            ULONGLONG nowS = GetTickCount64(), end = g_cinemaEndMs.load();
            // Minigun dell'elicottero: dopo la scena la camera resta ferma ~1,5 s
            // a terra e poi salta sull'elicottero; nero finche' non si muove
            // (al massimo 3 s).
            static ULONGLONG baseFor = 0;
            static float endBase[3] = {0, 0, 0};
            bool stillAtStart = false;
            if (end && HasCameraSignature(cam)) {
                const float* cl = reinterpret_cast<const float*>(cam + 0x10);
                // Riferimento: dove sta la camera alla fine dei primi 0,7 s (nei
                // primi tick e' ancora quella della scena).
                if (baseFor != end || nowS - end < 700) {
                    baseFor = end;
                    for (int i = 0; i < 3; ++i) endBase[i] = cl[i];
                }
                float dx = cl[0] - endBase[0], dy = cl[1] - endBase[1], dz = cl[2] - endBase[2];
                float d2 = dx * dx + dy * dy + dz * dz;
                stillAtStart = d2 < 1.0f;
                // Salto sull'elicottero visto qui, nello stesso tick (il disegno
                // lo vedrebbe dopo: un tick senza nero tra le due attese).
                if (nowS - end < 3000 && d2 > 2000.0f * 2000.0f && g_teleportMs.load() < end) g_teleportMs = nowS;
            }
            bool settle = !vr::control::g_cinemaActive &&
                          ((end && (nowS - end < 700 || (nowS - end < 1500 && nowS - g_settleJumpMs.load() < 300) ||
                                    (nowS - end < 3000 && g_flightGunHeld && stillAtStart))) ||
                           nowS - g_teleportMs.load() < 600 ||
                           nowS - g_blinkMs.load() < 150);  // ricentramento: battito di ciglia (R-C3)
            {
                static ULONGLONG settleStart = 0;
                if (settle && !settleStart) settleStart = nowS;
                if (!settle && settleStart) {
                    char b[100];
                    snprintf(b, sizeof(b), "FADE: black held %.2f s while the view settled", (nowS - settleStart) / 1000.0);
                    vr::log::Line(b);
                    settleStart = 0;
                }
            }
            // WI-2: camera del gioco lontana da Walker (oltre 200 unita') mentre lo
            // schermo virtuale sta per arrivare: nero dal primo tick invece di
            // 150-600 ms della camera della scena in 3D con la testa sopra (un
            // salto del punto di vista, nausea). Le inquadrature vicine delle
            // scene scriptate restano come prima (19/10: lampi neri continui).
            // Review WI-2 (26/10): le inquadrature brevi (0,1-0,6 s) restano in VR
            // per l'isteresi qui sotto, e col nero da sole diventavano lampi neri
            // (19/10). Quindi: un solo tick lontano non conta (2 tick di fila), e
            // il nero resta finche' la camera non e' su Walker da 0,3 s: una serie
            // di inquadrature con brevi ritorni su Walker e' un solo nero, mai
            // nero-VR-nero. Riga a fine nero: durata e inquadrature (conteggi per il visore).
            bool farCam = false;
            if (noWalker && !dead && wanted && !vr::control::g_cinemaActive && pawnPtr && HasCameraSignature(cam)) {
                float pl[3];
                const float* cl = reinterpret_cast<const float*>(cam + 0x10);
                if (SafeCopy(pl, reinterpret_cast<char*>(pawnPtr) + 0x54, 12)) {
                    const float ex = cl[0] - pl[0], ey = cl[1] - pl[1], ez = cl[2] - pl[2];
                    farCam = ex * ex + ey * ey + ez * ez > 200.0f * 200.0f;
                }
            }
            bool cutaway = false;
            {
                static int farTicks = 0, shots = 0;
                static ULONGLONG lastFarMs = 0, cutStartMs = 0, shotStartMs = 0;
                // Riga per ogni inquadratura lontana di almeno 2 tick (review WI-2 round 2:
                // conteggi e durate per la sessione nel visore): durata e come finisce.
                if (farCam && farTicks == 0) shotStartMs = nowS;
                if (!farCam && farTicks >= 2 && shotStartMs) {
                    char b[160];
                    snprintf(b, sizeof(b), "CUTSHOT: far shot %.2f s (%d ticks), %s", (nowS - shotStartMs) / 1000.0, farTicks,
                             vr::control::g_cinemaActive ? "then the virtual screen" : "then back on Walker");
                    vr::log::Line(b);
                }
                farTicks = farCam ? farTicks + 1 : 0;
                if (farTicks == 2) ++shots;
                if (farTicks >= 2) lastFarMs = nowS;
                cutaway = !vr::control::g_cinemaActive && lastFarMs && nowS - lastFarMs < 300;
                if (cutaway && !cutStartMs) {
                    cutStartMs = nowS;
                    vr::log::Line("FADE: the game camera left Walker (cutaway): black until the virtual screen or 0.3 s back on Walker.");
                }
                if (!cutaway && cutStartMs) {
                    char b[160];
                    snprintf(b, sizeof(b), "FADE: cutaway black over after %.2f s (%d far shot%s), %s", (nowS - cutStartMs) / 1000.0, shots,
                             shots == 1 ? "" : "s", vr::control::g_cinemaActive ? "virtual screen" : "back in VR");
                    vr::log::Line(b);
                    cutStartMs = 0;
                    shots = 0;
                    lastFarMs = 0;
                }
            }
            vr::output::HoldBlack(vr::control::g_fades && vr::control::g_cinemaEnabled && !vr::control::g_cinemaForce &&
                                      ((noWalker && dead && !vr::control::g_cinemaActive) || cutaway || settle),
                                  settle || cutaway);
        }
        // Isteresi: momenti brevi di modalita' cinematica durante il gioco
        // (porte, eventi scriptati) restano in VR. Passare allo schermo
        // virtuale per mezzo secondo e tornare indietro, nel visore, e' un
        // lampo (segnalazione 18/10: "flash of light ... during gameplay").
        // Inizio delle scene scriptate (21/10, elicotteri all'inizio del
        // capitolo 1): la camera passa da Walker per pochi decimi di secondo
        // tra un'inquadratura e l'altra; con 0,15 s per tornare in VR e 0,6 s
        // per ripartire lo schermo virtuale si vedeva prima persona, scena in
        // 3D, schermo, prima persona, schermo. Ora: uscita dallo schermo dopo
        // 0,8 s su Walker; i 0,6 s di tolleranza solo dopo 2 s di gioco vero in
        // VR (prima, e all'avvio, 0,15 s).
        static ULONGLONG wantedSince = 0, unwantedSince = 0, vrSince = 0;
        ULONGLONG nowMs = GetTickCount64();
        bool cinema = vr::control::g_cinemaActive.load();
        if (wanted) {
            unwantedSince = 0;
            if (!wantedSince) wantedSince = nowMs;
            // Camera lontana da Walker per meno di 0,6 s (inquadrature brevi da 0,1-0,5 s
            // nei log): resta in VR, niente lampo dello schermo virtuale.
            bool settled = vrSince && nowMs - vrSince >= 2000;
            bool teleported = nowMs - g_teleportMs.load() < 1000;
            ULONGLONG need = vr::control::g_cinemaForce || teleported ? 0 : (settled ? 600 : 150);
            if (nowMs - wantedSince >= need) cinema = true;
        } else {
            wantedSince = 0;
            if (!unwantedSince) unwantedSince = nowMs;
            if (nowMs - unwantedSince >= 800) cinema = false;
        }
        // Solo il tempo con la camera davvero su Walker (all'avvio i tick di
        // caricamento senza camera contavano come gioco: 0,6 s in prima persona).
        if (cinema || !HasCameraSignature(cam)) vrSince = 0;
        else if (!vrSince && !wanted) vrSince = nowMs;
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
            if (!cinema) g_cinemaEndMs = nowMs;
            if (vr::control::g_fades) vr::output::Blink(0.3f);
        }
        UpdateDynamicResolution(cam, cinema || half <= 0 || !vr::control::g_syncStereo);
        // Schermo virtuale in stereo (R-C6, "cinema3d on"): due disegni con la
        // camera del gioco spostata di +-0,4 IPD lungo la sua destra (niente
        // testa), uno per occhio, composti fianco a fianco sull'overlay. I
        // filmati Bink restano piatti (un solo disegno).
        if (cinema && vr::control::g_cinema3d && half > 0 && HasCameraSignature(cam) && !g_lastMovieSeen) {
            float* loc = reinterpret_cast<float*>(cam + 0x10);
            float base[3] = {loc[0], loc[1], loc[2]};
            int32_t yawUnits = *reinterpret_cast<int32_t*>(cam + kCameraOffsetYaw);
            float yaw = yawUnits * (6.28318530718f / 65536.0f);
            float rx = -std::sin(yaw), ry = std::cos(yaw);  // destra della camera (UE3: X avanti, Y destra)
            for (int eye = 0; eye < 2; ++eye) {
                float d = (eye == 0 ? -1.0f : 1.0f) * half * 0.4f;
                loc[0] = base[0] + rx * d;
                loc[1] = base[1] + ry * d;
                loc[2] = base[2];
                g_drawEye = eye;
                if (bShouldPresent) {
                    FrameTag tag;
                    tag.eye = eye;
                    tag.tick = g_stereoDraws.load(std::memory_order_relaxed);
                    PushFrameTag(tag);
                }
                g_ViewportDraw_Original(self, bShouldPresent);
            }
            g_stereoDraws.fetch_add(1, std::memory_order_relaxed);
            g_drawEye = -1;
            loc[0] = base[0];
            loc[1] = base[1];
            loc[2] = base[2];
            return;
        }
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
        const double drawStart = ProfMs();
        double drawEye[2] = {0.0, 0.0};
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
                tag.screenPercent = kGeometryPercent;  // scena riallargata dal gioco (vedi kGeometryPercent)
                tag.tick = g_stereoDraws.load(std::memory_order_relaxed);
                tag.poseValid = g_headSnap.poseValid;
                tag.pose = g_headSnap.pose;
                tag.sampleMs = g_headSnap.sampleMs;
                tag.scope = g_scopeActive;
                PushFrameTag(tag);
            }
            if (eye == 1 && vsRight) *vsSlot = vsRight;
            g_ViewportDraw_Original(self, bShouldPresent);
            if (eye == 1 && vsRight) *vsSlot = vsEngine;
            drawEye[eye] = ProfMs() - t0;
            g_profDraw[eye] += drawEye[eye];
        }
        {
            static double last = 0.0;
            // Scatti: tick oltre 20 ms (a 72-90 Hz = almeno un frame perso) e il
            // piu' lungo, che la media dei 300 tick nasconde.
            static int longTicks = 0;
            static double maxTick = 0.0;
            double now = ProfMs();
            // Confine del tick anche per l'attesa del mutex OpenXR (review 2
            // WI-1): attesa del thread di gioco in questo tick (stesso intervallo
            // di dt), nell'istogramma per tick solo se l'intervallo e' un tick.
            const float tickWaitMs = xrWait ? vr::xr::EndGameTickLockWait(last > 0.0 && now - last < 1000.0) : 0.0f;
            // Oltre 1 s senza disegni stereo = schermo virtuale o caricamento,
            // non un tick: escluso (prima gonfiava la media dopo ogni cinematica).
            if (last > 0.0 && now - last < 1000.0) {
                double dt = now - last;
                if (const int pend = g_dynResCostPending.load()) {
                    g_dynResCostPending = pend - 1;
                    char db[120];
                    std::snprintf(db, sizeof(db), "DYNRES: %s %.1f ms", pend == 2 ? "tick with the ScreenPercentage change" : "next tick",
                                  dt);
                    vr::log::Line(db);
                }
                g_profTick += dt;
                if (dt > 20.0) ++longTicks;
                if (dt > maxTick) maxTick = dt;
                // Dove va il tempo di un tick lento (21/10: 2-5 al secondo nella
                // scena dell'elicottero): mod prima del disegno, disegno dei due
                // occhi, fuori dal disegno (logica del gioco, Present, attesa).
                static ULONGLONG lastHitchLog = 0;
                if (dt > 20.0 && GetTickCount64() - lastHitchLog > 2000) {
                    lastHitchLog = GetTickCount64();
                    char hb[420];
                    // Per sorgente (WI-1): nella parte della mod la testa e il
                    // cambio di ScreenPercentage; fuori dal disegno l'uscita VR e
                    // il Present del monitor (il resto e' il gioco). Attesa del
                    // mutex OpenXR (review 2 WI-1): quella del thread di gioco in
                    // questo tick (tutte le sue chiamate, input compreso), e la
                    // parte nel tracciamento della testa; prima il totale di
                    // tutti i thread dal tick prima, scritto come "nella testa".
                    std::snprintf(hb, sizeof(hb),
                                  "HITCH: tick %.1f ms = mod before draw %.1f, draw left %.1f right %.1f, outside the "
                                  "draw %.1f | head tracking %.1f, ScreenPercentage change %s, last VR output %.1f, "
                                  "monitor Present %.1f | OpenXR lock wait on the game thread this tick %.2f ms (in head "
                                  "tracking %.2f)",
                                  dt, drawStart - tickStart, drawEye[0], drawEye[1], dt - (now - tickStart), headMs,
                                  g_dynResChangeMs.load() != dynResChangeBefore ? "yes" : "no", g_lastOutputMs.load(),
                                  g_lastPresentMs2.load(), tickWaitMs, headWaitMs);
                    vr::log::Line(hb);
                }
            }
            last = now;
            if (++g_profCount >= 300) {
                char b[300];
                static unsigned lastPolls = 0, lastProject = 0;
                unsigned polls = vr::input::PadPolls();
                unsigned project = g_canvasProjectCalls.load();
                double scanMs = g_profScanMs.exchange(0.0);
                std::snprintf(b, sizeof(b), "PROFILE: tick %.2f ms (%.1f fps stereo), Draw left eye %.2f ms, right %.2f ms, "
                              "VR output %.2f ms, monitor Present %.2f ms, rest %.2f ms, pad polls per tick %.2f, "
                              "object scans %.2f ms/tick, Canvas.Project %.1f/tick",
                              g_profTick / g_profCount, 1000.0 * g_profCount / g_profTick, g_profDraw[0] / g_profCount,
                              g_profDraw[1] / g_profCount, g_profOutput / g_profCount, g_profPresent / g_profCount,
                              (g_profTick - g_profDraw[0] - g_profDraw[1]) / g_profCount,
                              static_cast<double>(polls - lastPolls) / g_profCount, scanMs / g_profCount,
                              static_cast<double>(project - lastProject) / g_profCount);
                lastPolls = polls;
                lastProject = project;
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
const uintptr_t& kGetFovAngleAddr = vr::addr::g[vr::addr::GetFovAngle];
typedef float(__thiscall* GetFovAngle_t)(void* self);
GetFovAngle_t g_GetFovAngle_Original = nullptr;

constexpr uintptr_t kOffPawnCurrentWeaponFov = 0x1594;  // YGamePawn.m_currentWeapon


// FPerspectiveMatrix(HalfFOV, Width, Height, MinZ) (0x009EAE80, thiscall, il
// costruttore restituisce this). Si cambia solo la matrice costruita da
// ULocalPlayer::CalcSceneView (ritorno a 0x0075CAC5 o 0x0075CBBA) durante il
// Draw di un occhio: M[0][0], M[1][1] (scala) e M[2][0], M[2][1] (centro),
// riga z della convenzione a vettori riga di UE3.
const uintptr_t& kPerspectiveMatrixAddr = vr::addr::g[vr::addr::PerspectiveMatrix];
typedef void*(__thiscall* PerspectiveMatrix_t)(void* self, float halfFov, float w, float h, float minZ);
PerspectiveMatrix_t g_PerspectiveMatrix_Original = nullptr;

class PerspectiveThunk {
public:
    void* Detour(float halfFov, float w, float h, float minZ) {
        void* r = g_PerspectiveMatrix_Original(this, halfFov, w, h, minZ);
        uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
        if (g_drawOffCenter && (ret == vr::addr::g[vr::addr::CsvRetPerspectiveA] || ret == vr::addr::g[vr::addr::CsvRetPerspectiveB])) {
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
        g_bodyYawAtCamera = yaw;  // follow-up 27/10: corpo con cui il gioco ha fatto la sua camera
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
    if (!slot || !expected || *entry != expected) {
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
const uintptr_t& kIatFmodSetListener = vr::addr::g[vr::addr::IatFmodSetListener];

int __stdcall Hooked_SetListener(void* self, int listener, const FmodVec* pos, const FmodVec* vel, const FmodVec* fwd,
                                 const FmodVec* up) {
    // Dispositivo predefinito cambiato: l'uscita FMOD si riapre qui, sul
    // thread del gioco, con il suo EventSystem.
    if (vr::audio::ReopenPending()) vr::audio::ReopenOutput(self);
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
    if (!entry || !expected || *entry != expected) {
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
    static float hz = 0.0f;
    static ULONGLONG lastProps = 0;
    if (GetTickCount64() - lastProps > 2000) {
        lastProps = GetTickCount64();
        hz = sys->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_DisplayFrequency_Float);
    }
    float frame = hz > 30.0f ? 1.0f / hz : 1.0f / 90.0f;
    // Tempo fino ai fotoni del frame in preparazione secondo il runtime
    // (SteamVR: prossimo vsync + vsync->fotoni; OpenXR: predictedDisplayTime).
    float toPhotons = vr::rt::SecondsToPhotons();
    float s = toPhotons + frame * vr::control::g_predictFrames;
    if (vr::control::g_predictAuto) {
        // Ritardo reale misurato dall'uscita (campionamento -> fotoni), diverso
        // per ogni PC, visore e collegamento; finche' non c'e', un frame in piu'.
        float measured = vr::output::PhotonLatencyMs();
        s = measured > 0.0f ? measured * 0.001f : toPhotons + frame;
    }
    // Al massimo "predictmax" ms (150, WI-1; prima 100 fissi): VDXR a 72 Hz misura ~95 ms.
    const float cap = vr::control::g_predictMaxMs.load() * 0.001f;
    return s < 0.0f ? 0.0f : (s > cap ? cap : s);
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

// Riga AIM del telaio dell'arma (WI-4a): una per mano a ogni cambio di presa,
// angolo, gunyaw o raggio di puntamento (misurato da OpenXR o costante).
void LogGunFrame(int physicalHand, const vr::pure::GunFrameParams& p, const char* source) {
    static float last[2][5] = {{1e9f}, {1e9f}};
    static const char* lastSrc[2] = {nullptr, nullptr};
    const float cur[5] = {p.angleDeg, p.yawDeg, p.aim.pitch, p.aim.yaw, p.aim.roll};
    float* l = last[physicalHand & 1];
    static int lastHold[2] = {-1, -1};  // presa effettiva (torretta, WI-4c)
    const int hold = vr::control::EffectiveHoldMode();
    bool same = lastSrc[physicalHand & 1] == source && lastHold[physicalHand & 1] == hold;
    lastHold[physicalHand & 1] = hold;
    for (int i = 0; i < 5; ++i) same = same && std::fabs(cur[i] - l[i]) < 0.01f;
    if (same) return;
    for (int i = 0; i < 5; ++i) l[i] = cur[i];
    lastSrc[physicalHand & 1] = source;
    char b[420];
    std::snprintf(b, sizeof(b),
                  "AIM: gun frame, %s hand: %s hold, gun %+.1f deg above the pointing ray, gun yaw %+.1f deg (outward +); pointing ray "
                  "%.2f deg below the controller pose (yaw %+.2f, roll %+.2f; %s), runtime %s -> barrel %+.2f deg on the raw -Z.",
                  physicalHand ? "left" : "right", vr::control::EffectiveHoldMode() == 1 ? "gunstock" : "normal", p.angleDeg,
                  vr::control::g_gunYawDeg.load(), -p.aim.pitch, p.aim.yaw, p.aim.roll, source, vr::rt::BackendName(),
                  p.aim.pitch + p.angleDeg);
    vr::log::Line(b);
}

// Rotatore (yaw rispetto al riferimento della testa, pitch, roll; unita') di
// una posa, con gli angoli ESATTI (WI-4a): prima lo yaw era quello "di
// direzione" della testa (BlendedYaw, avanti + |avanti.y| x su), che con il
// controller inclinato di lato spostava la canna di lato: 5/10/14 gradi a
// 10/20/30 gradi di rollio col telaio di prima, 7,6 gradi mirando a +-30 gradi
// anche col raggio giusto. Con i tre angoli esatti FRotationMatrix rifa' la
// posa (test "Euler round trip"), anche quasi in verticale.
void ControllerRotator(const float m[3][4], int32_t* yaw, int32_t* pitch, int32_t* roll) {
    const vr::pure::Euler e = vr::pure::ExactEuler(m);
    *yaw = vr::control::g_yawSign * RadiansToUnreal(vr::pure::WrapPi(e.yaw - g_hmdYawReference));
    *pitch = vr::control::g_pitchSign * RadiansToUnreal(e.pitch);
    *roll = vr::control::g_rollSign * RadiansToUnreal(e.roll);
}

// Punto del polso dell'arma (telaio ruotato attorno al palmo, WI-4a): metri
// nello spazio del tracciamento, per lo scostamento della mano (UpdateHeadTracking).
float g_gunWristPos[3] = {0.0f, 0.0f, 0.0f};

// Mira dalla matrice raw del controller dell'arma (OpenVR): telaio dell'arma
// (WI-4a, vr::pure::GunFrameFromRaw) = raggio di puntamento del controller
// (posa aim di OpenXR / laser di SteamVR) ruotato di gunangle (o stockangle col
// calcio) attorno al suo asse destro e di gunyaw attorno al suo "su", con la
// rotazione attorno al palmo; poi gli angoli esatti. Prima: aimpitch sul raw
// -Z, 39,4 gradi sopra il raggio con il predefinito 0 ("gun angle is
// completely off", tester 25/10), e yaw miscelato. La stessa funzione serve il
// visore e "synthctrl" (collaudi senza visore).
// Telaio dell'arma con una mano dell'ultimo ApplyGunControllerAim (spazio del
// tracciamento, origine = punto del polso): ingresso della presa a due mani (WI-4b).
float g_gunFrame[3][4] = {};

// Mira, rollio e polso da un telaio dell'arma (quello con una mano, o quello
// corretto dalla presa a due mani, WI-4b).
void ApplyGunFrame(const float gm[3][4]) {
    int32_t y = 0, pi = 0, r = 0;
    ControllerRotator(gm, &y, &pi, &r);
    g_aimDeltaYaw = y;
    g_aimDeltaPitch = pi;
    g_aimValid = true;
    g_handRoll = r;
    for (int i = 0; i < 3; ++i) g_gunWristPos[i] = gm[i][3];
}

void ApplyGunControllerAim(const vr::HmdMatrix34_t& c) {
    const int phys = vr::control::PhysicalHand(0);
    const char* src = nullptr;
    const vr::pure::GunFrameParams p = vr::control::GunFrameFor(phys, true, &src);
    LogGunFrame(phys, p, src);
    float gm[3][4];
    vr::pure::GunFrameFromRaw(c.m, p, gm);
    std::memcpy(g_gunFrame, gm, sizeof(gm));
    ApplyGunFrame(gm);
}

// Angoli della mano sinistra dalla sua matrice raw, stesso riferimento della
// destra (visore e "synthctrl left"). WI-4a: un solo telaio rigido, il raw
// del controller senza l'angolo dell'arma (la mano disegnata e gli assi della
// mano per props.cpp come col predefinito di prima, ora con gli angoli
// esatti). Prima il beccheggio veniva dal raw ruotato di aimpitch e yaw e
// rollio dal raw: non era un telaio (2-5 gradi di errore in avanti, 5-7 sull'asse
// su a 20-30 gradi di rollio). La direzione in cui la mano PUNTA (ordini alla
// squadra, guanti gravitazionali, "movedir hand") e' il raggio di puntamento
// del controller, come il laser del menu: g_leftAim*.
// Review WI-4a (round 1): "movedir hand" (g_leftYawUnits) usa lo yaw di
// DIREZIONE del raggio (vr::pure::PointingHeadingYaw, miscelato come quello
// della testa), non quello esatto: con la mano rilassata il raggio sta a 80-90
// gradi in giu' e lo yaw esatto girava di 11-45 gradi con 2 gradi di tremolio
// (camminata in una direzione non scelta). g_leftAim* (vettore dei guanti,
// continuo anche in verticale) restano esatti.
std::atomic<int32_t> g_leftAimYaw{0}, g_leftAimPitch{0};
void ApplyLeftControllerAngles(const vr::HmdMatrix34_t& lm) {
    int32_t y = 0, p = 0, r = 0;
    ControllerRotator(lm.m, &y, &p, &r);
    g_leftYaw = y;
    g_leftPitch = p;
    g_leftRoll = r;
    float aim[3][4], la[3][4];
    const vr::pure::AimInRaw aimInRaw = vr::control::GunFrameFor(vr::control::PhysicalHand(1), false).aim;
    vr::pure::AimRotInRaw(aimInRaw, aim);
    vr::pure::MulRot3(lm.m, aim, la);
    int32_t ay = 0, ap = 0, ar = 0;
    ControllerRotator(la, &ay, &ap, &ar);
    g_leftAimYaw = ay;
    g_leftAimPitch = ap;
    vr::control::g_leftYawUnits =
        vr::control::g_yawSign * RadiansToUnreal(vr::pure::WrapPi(vr::pure::PointingHeadingYaw(lm.m, aimInRaw) - g_hmdYawReference));
    vr::control::g_leftYawValid = true;
}

// "synthctrl": matrice raw finta di un controller con yaw/pitch/roll (gradi)
// rispetto al corpo (riferimento della testa), posizione nulla (la posizione
// della mano resta quella di synthhand / synthleft). E' la posa fisica del
// controller, come quella del runtime: yawsign, pitchsign e rollsign valgono
// dopo, come per un controller vero. Il riferimento e' in radianti raw (lo
// stesso che ApplyGunControllerAim sottrae): prima era moltiplicato per
// yawsign e con "yawsign -1" e un riferimento rimasto da "mode hmd" lo yaw
// risultava sbagliato (review 25/10).
vr::HmdMatrix34_t SynthControllerMatrix(int h) {
    vr::HmdMatrix34_t m = {};
    const float k = 0.01745329252f;
    vr::pure::PoseFromEuler(vr::control::g_synthCtrlDeg[h][1] * k, g_hmdYawReference + vr::control::g_synthCtrlDeg[h][0] * k,
                            vr::control::g_synthCtrlDeg[h][2] * k, m.m);
    return m;
}

// --- Presa a due mani (WI-4b, bug 7 del tester: "2 handing weapons doesn't work correctly") ---
//
// Un solo risolutore (vr::twohand::Solver, twohand_math.h) per mira, mano
// sinistra disegnata (UpdateArms), dispersione (g_twoHandActive), vibrazione e
// veti dei gesti (control::g_twoHandEngaged). Prima due prove diverse (origine
// raw della sinistra entro 12 cm dalla linea del controller destro per la mira,
// entro 20 cm dalla presa disegnata per la mano) non erano d'accordo: una mano
// sull'astina disegnata stava a 14,6 cm dalla linea della mira (disegnata
// sull'arma, mira e dispersione a una mano); quando la mira si agganciava
// puntava la linea tra le origini raw (21,6 gradi fuori dalla canna per la
// presa del gioco) e lo scarto decadeva in 0,7 s: l'arma girava da sola.
// L'astina viene dalla presa disegnata (UpdateArms, un tick prima): g_thPub.
struct TwoHandPub {
    vr::twohand::Guard guard;  // col controller sinistro parallelo al destro (senza la posa della sinistra)
    bool gunInHand = false;    // modello dell'arma nella mano (UpdateArms)
    bool measured = false;     // astina dalla presa disegnata (false = modello tipico: mancini)
    uint32_t tick = 0;         // g_thTicks della pubblicazione
    // Review WI-4b round 1 (M4): la presa disegnata (polso sinistro rispetto al
    // destro, coordinate dell'arma, m) e i parametri, per rifare l'astina in
    // TwoHandStep con la rotazione ATTUALE del controller sinistro.
    bool haveQ = false, shortGun = false, gunLeft = false, gripFix = true;
    float q[3] = {}, armOffM[3] = {}, offM[3] = {};
};
// Sinistra disegnata sul modello dell'astina nella presa (M3): spostamento
// dalla presa disegnata (avanti, destra, su del telaio dell'arma, metri).
std::atomic<float> g_thDrawShift[3];
// Sinistra disegnata rispetto alla destra disegnata (unita' del mondo; riga VIEW "lrel", diagnostica del rilascio).
std::atomic<float> g_diagLeftRel[3];
std::mutex g_thPubMutex;
TwoHandPub g_thPub;
vr::twohand::Solver g_thSolver;
uint32_t g_thTicks = 1;             // passi della presa a due mani (uno per tick, prima di UpdateArms)
vr::twohand::Output g_thOut;        // ultimo passo (riga TWOHAND)
int g_thVeto = 0;
bool g_thLeftValid = false;
float g_thDiagPalmCm[2][3] = {};    // palmo destro e sinistro, cm nel corpo (come synthhand / synthleft)
float g_thDiagGuardCm[3] = {};      // palmo sinistro voluto sulla presa disegnata, cm nel corpo
float g_thDiagSynthLeft[3] = {};    // synthleft (cm) che mette il palmo sinistro li', con la rotazione attuale della sinistra
float g_thDiagSynthLeftDrawn[3] = {};  // ... sulla presa dell'arma disegnata (telaio finale, scarto rimasto compreso)
bool OnStationaryWeapon();          // g_onTurret (UpdateArms)

// Spazio del tracciamento -> cm nel riferimento del corpo (avanti, destra, su), come g_handOffset / synthleft.
void TrackToBodyCm(const float p[3], const float ref[3], float out[3]) {
    const float sy = std::sin(g_hmdYawReference), cy = std::cos(g_hmdYawReference);
    const float dx = p[0] - ref[0], dy = p[1] - ref[1], dz = p[2] - ref[2];
    out[0] = (dx * sy - dz * cy) * 100.0f;
    out[1] = (dx * cy + dz * sy) * 100.0f;
    out[2] = dy * 100.0f;
}

// Fine della presa senza passo (destra persa, cambio di modo): niente scarto.
void TwoHandReset() {
    g_thSolver.Reset();
    g_thOut = vr::twohand::Output();
    g_twoHandActive = false;
    vr::control::g_twoHandEngaged = false;
    vr::control::g_leftNearBarrel = false;
    for (auto& v : g_thDrawShift) v = 0.0f;
}

// Un passo della presa a due mani (visore e pose finte). gun: telaio dell'arma
// con una mano (WI-4a, origine = punto del polso); rightRaw / leftRaw: pose raw
// nello spazio del tracciamento (posizione compresa); ref: origine per i cm del
// corpo (riga TWOHAND). Ritorna true se il telaio dell'arma cambia (outGun).
bool TwoHandStep(const float gun[3][4], const float rightRaw[3][4], bool leftValid, const float leftRaw[3][4], const float ref[3],
                 float outGun[3][4]) {
    namespace th = vr::twohand;
    const double nowMs = ProfMs();
    static double lastMs = 0.0;
    const float dt = lastMs > 0.0 ? static_cast<float>((nowMs - lastMs) * 0.001) : 0.0f;
    lastMs = nowMs;
    TwoHandPub pub;
    {
        std::lock_guard<std::mutex> lock(g_thPubMutex);
        pub = g_thPub;
    }
    // Astina pubblicata da UpdateArms al tick prima (o a questo): braccia
    // guidate. A tick, non a millisecondi: con 300 ms un tick lungo (436 ms
    // con "twohandinfo", caricamenti) la dava per vecchia e la presa cadeva.
    const uint32_t thTick = ++g_thTicks;
    const bool fresh = pub.tick != 0 && thTick - pub.tick <= 2;
    const vr::pure::GunFrameParams gp = vr::control::GunFrameFor(vr::control::PhysicalHand(0), true);
    const bool supLeft = vr::control::PhysicalHand(1) == 1;
    th::Input in;
    in.dt = dt;
    std::memcpy(in.gun, gun, sizeof(in.gun));
    std::memcpy(in.rightRaw, rightRaw, sizeof(in.rightRaw));
    vr::pure::PalmOf(rightRaw, gp.left, gp.gripFix, in.palmR);
    in.leftValid = leftValid;
    if (leftValid) {
        std::memcpy(in.leftRaw, leftRaw, sizeof(in.leftRaw));
        vr::pure::PalmOf(leftRaw, supLeft, gp.gripFix, in.palmL);
    }
    in.grip = vr::control::g_gripRaw[1].load() || vr::control::g_gripSim[1].load();
    int veto = 0;
    if (!vr::control::g_twoHand) veto |= th::kVetoOff;
    if (!vr::control::g_leftGrip) veto |= th::kVetoNoGrip;
    if (OnStationaryWeapon()) veto |= th::kVetoStationary;
    if (vr::control::g_cinemaActive || g_gameLockedScene) veto |= th::kVetoScene;
    if (!fresh || !pub.gunInHand) veto |= th::kVetoNoGun;
    if (vr::control::g_propHeld[1]) veto |= th::kVetoProp;
    // Grip stretto vicino a un oggetto (o coi guanti puntati): e' la presa
    // dell'oggetto. Con la mano gia' sull'arma il grip resta dell'arma.
    if (vr::control::g_propZone[1] && in.grip && !g_thSolver.engaged) veto |= th::kVetoPropGrip;
    const int busy = vr::control::g_leftHandBusy.load();
    if (busy & 1) veto |= th::kVetoReload;
    if (busy & 2) veto |= th::kVetoGrenade;
    in.veto = veto;
    in.gunstock = vr::control::g_holdMode == 1;
    in.mountValid = vr::control::StockMount(in.mountPos, in.mountRot);
    if (fresh) in.guard = pub.guard;
    if (fresh && pub.haveQ && leftValid) {
        // Review WI-4b round 1 (M4): il palmo voluto dalla rotazione ATTUALE del
        // controller sinistro (polso disegnato sulla presa + R_L * palmo nel
        // raw). Prima: controller sinistro parallelo al destro, e con la
        // sinistra ruotata di 30/45/60 gradi (normale sotto un'astina) la mano
        // vista sulla presa disegnata era a 5,3/7,8/10,2 cm dal modello. Nella
        // presa conta l'astina fissata alla presa (Solver::held): le rotazioni
        // del polso sinistro dopo la presa non muovono la canna.
        // Review round 2 (medio): nelle coordinate dell'arma DISEGNATA (telaio
        // con una mano girato dello scarto rimasto), quella su cui si prova la
        // presa: prima del telaio con una mano, e con uno scarto di 10-19 gradi
        // il modello stava 1,7-3,3 cm fuori dalla presa vista (la ripresa
        // girava l'arma di 2,3-4,8 gradi).
        float m[3][3], t[3], drawn[3][4];
        g_thSolver.DrawnRot(gun, drawn);
        th::LeftToGun(drawn, leftRaw, m);
        th::PalmTargetFromDrawnGripRot(pub.q, m, pub.gunLeft, pub.gripFix, pub.armOffM, t);
        in.guard = th::MakeGuard(t[0] + pub.offM[0], t[1] + pub.offM[1], t[2] + pub.offM[2], pub.shortGun);
    }
    th::Params& prm = g_thSolver.p;
    prm.engageM = vr::control::g_twoHandCm * 0.01f;
    prm.releaseM = vr::control::g_twoHandReleaseCm * 0.01f;
    if (prm.releaseM < prm.engageM + 0.01f) prm.releaseM = prm.engageM + 0.01f;
    prm.dwellS = vr::control::g_twoHandDwellSec;
    prm.filterMinHz = vr::control::g_twoHandFilterHz;
    prm.stockM = vr::control::g_stockTolCm * 0.01f;
    prm.stockDeg = vr::control::g_stockTolDeg;
    const bool wasEngaged = g_thSolver.engaged;
    const th::Output prevOut = g_thOut;  // valori della presa prima del rilascio (riga AIM)
    const th::Output o = g_thSolver.Step(in);
    g_thOut = o;
    g_thVeto = veto;
    g_thLeftValid = leftValid;
    g_twoHandActive = o.engaged;
    vr::control::g_twoHandEngaged = o.engaged;
    for (int i = 0; i < 3; ++i) g_thDrawShift[i] = o.engaged ? o.drawShift[i] : 0.0f;
    // Review WI-4b round 1 (M-a): "sinistra sull'arma" per i gesti (vr_input:
    // il grip alla pressione non e' una granata) con la STESSA geometria della
    // presa: palmo entro twohandrelease dall'astina (o da quella disegnata con
    // lo scarto rimasto), o vicino alla posa sul calcio. Prima vr_input usava
    // l'origine raw entro 15 cm dalla linea dell'arma (fino a 45 cm avanti):
    // in fondo all'astina, 7 cm fuori o con la sinistra ruotata il grip
    // afferrato di slancio (prima dei 150 ms di sosta) era LB e bloccava la presa.
    {
        const bool softBusy = (veto & (th::kVetoProp | th::kVetoPropGrip | th::kHardVetoes | th::kVetoOff | th::kVetoNoGrip)) != 0;
        bool onGun = false;
        if (leftValid && !softBusy) {
            onGun = o.engaged || (in.guard.valid && o.nearDist < prm.releaseM);
            if (in.gunstock && in.mountValid && o.mountCm >= 0.0f)
                onGun = onGun || (o.mountCm < prm.stockM * 150.0f && o.mountDeg < prm.stockDeg * 1.5f);
        }
        vr::control::g_leftNearBarrel = onGun;
    }
    // Diagnostica (TWOHAND): palmi e presa disegnata in cm del corpo, e il
    // synthleft che mette il palmo sinistro sulla presa con la rotazione attuale.
    TrackToBodyCm(in.palmR, ref, g_thDiagPalmCm[0]);
    if (leftValid) TrackToBodyCm(in.palmL, ref, g_thDiagPalmCm[1]);
    // Presa disegnata nel telaio dell'arma (inclinazione compresa, review round 1).
    if (in.guard.valid) {
        float f[3], r[3], u[3], pt[3];
        th::GunAxes(gun, f, r, u);
        for (int i = 0; i < 3; ++i) pt[i] = in.palmR[i] + in.guard.fwd * f[i] + in.guard.right * r[i] + in.guard.up * u[i];
        TrackToBodyCm(pt, ref, g_thDiagGuardCm);
        if (leftValid) {
            // origine raw = palmo - R * (palmo nel raw)
            float lp[3];
            vr::pure::PalmOf(leftRaw, supLeft, gp.gripFix, lp);
            float org[3];
            for (int i = 0; i < 3; ++i) org[i] = pt[i] - (lp[i] - leftRaw[i][3]);
            TrackToBodyCm(org, ref, g_thDiagSynthLeft);
            // ... e sull'arma DISEGNATA (telaio finale: scarto rimasto o presa).
            const th::Guard& gd = g_thSolver.engaged ? g_thSolver.held : in.guard;
            th::GunAxes(o.gun, f, r, u);
            for (int i = 0; i < 3; ++i) pt[i] = in.palmR[i] + gd.fwd * f[i] + gd.right * r[i] + gd.up * u[i] - (lp[i] - leftRaw[i][3]);
            TrackToBodyCm(pt, ref, g_thDiagSynthLeftDrawn);
        }
    }
    // Righe AIM: presa e rilascio (al piu' 20 ogni 10 s), e quanto manca ogni 2 s.
    {
        static double winStart = 0.0;
        static int inWin = 0;
        if (nowMs - winStart > 10000.0) {
            winStart = nowMs;
            inWin = 0;
        }
        char b[360];
        if (o.event == 1 && inWin++ < 20) {
            std::snprintf(b, sizeof(b),
                          "AIM: two-handed grip (%s, %s): left palm %.0f cm along the barrel, %.1f cm from the grip line of the drawn gun "
                          "(%.1f from the one-hand one); gun turned %.1f deg toward the left hand.",
                          o.how, o.why, o.along * 100.0f, o.nearDist < 1e8f ? o.nearDist * 100.0f : -1.0f, o.dist < 1e8f ? o.dist * 100.0f : -1.0f,
                          o.targetDeg);
            vr::log::Line(b);
            if (o.mountCm >= 0.0f) {
                std::snprintf(b, sizeof(b), "AIM: two-handed grip on the stock: left controller %.1f cm / %.1f deg from the saved mount.",
                              o.mountCm, o.mountDeg);
                vr::log::Line(b);
            }
        } else if (o.event == 2 && inWin++ < 20) {
            if (o.fading)
                std::snprintf(b, sizeof(b),
                              "AIM: two-handed grip released (%s, palm %.1f cm from the barrel): gun %.1f deg off the gun hand, back to it "
                              "in %.2f s (left hand moved %.1f deg from the grip, hands %.1f deg apart, %.1f cm past the barrel end).",
                              o.why, o.dist < 1e8f ? o.dist * 100.0f : -1.0f, o.corrDeg, g_thSolver.fadeT, prevOut.departDeg,
                              prevOut.apartDeg, prevOut.endCm);
            else
                std::snprintf(b, sizeof(b),
                              "AIM: two-handed grip released (%s): gun %.1f deg off the gun hand, it eases back only while the gun hand "
                              "moves.",
                              o.why, o.corrDeg);
            vr::log::Line(b);
        }
        static double lastNear = 0.0;
        // Distanza dall'arma DISEGNATA (la prova di presa, review round 2).
        if (!o.engaged && !wasEngaged && !veto && leftValid && in.guard.valid && o.nearDist < prm.engageM * 3.0f &&
            o.nearDist >= prm.engageM && nowMs - lastNear > 2000.0) {
            lastNear = nowMs;
            std::snprintf(b, sizeof(b),
                          "AIM: left palm %.0f cm from the barrel (%.0f cm along), two-handed grip within %.0f cm (grip, or hold it there %.0f ms).",
                          o.nearDist * 100.0f, o.along * 100.0f, prm.engageM * 100.0f, prm.dwellS * 1000.0f);
            vr::log::Line(b);
        }
    }
    if (o.aimChanged) std::memcpy(outGun, o.gun, sizeof(o.gun));
    return o.aimChanged;
}

// --- Torretta: modi della presa (WI-4c, bug 1 del tester) ---
//
// Prima la torretta seguiva solo il polso della mano dell'arma: con le due mani
// sulle maniglie la sinistra non contava e spostare le maniglie non mirava
// (la prova a due mani del fucile non poteva agganciarsi con le mani affiancate);
// il tester ha mirato solo dopo il suggerimento "right hand". Ora due modi
// ("turretgrip", Basic "Turret grip"; auto segue "Controller hold"):
//  - NORMAL: le due mani sulle maniglie, grip stretti, girano la torretta come
//    un manubrio (vr::pure::Handlebar: yaw dalla linea fra i palmi, beccheggio
//    medio dei due telai); una mano sola mira come prima; i grip non mandano
//    RB / LB al gioco (vr_input);
//  - GUNSTOCK: mira lungo il calcio come col fucile (telaio con stockangle).
vr::pure::Handlebar g_turretBar;
vr::pure::HandlebarOut g_turretBarOut;  // ultimo passo (riga TURRET)
// Torrette a terra tenute per le maniglie (vr::pure::TurretHandles, visore
// 27/10): mira dal perno davanti alle mani, come una mitragliatrice vera.
vr::pure::TurretHandles g_turretHandles;
vr::pure::TurretHandlesOut g_turretHandlesOut;
// Diagnostica della riga TURRET (visore 27/10, utente: "devo fare una X tra i
// due controller per andare a destra", mani vicine come maniglie): yaw / pitch /
// roll (gradi) dei telai raw delle due mani fisiche, del telaio dell'arma e
// palmi (cm, spazio del tracciamento).
struct TurretHandsDiag {
    bool valid = false, supValid = false;
    float gunYpr[3] = {0, 0, 0}, rawG[3] = {0, 0, 0}, rawS[3] = {0, 0, 0};
    float palmG[3] = {0, 0, 0}, palmS[3] = {0, 0, 0};
} g_turretHandsDiag;
int g_turretGripNow = -1;               // questo tick: -1 nessuna torretta, 0 NORMAL, 1 GUNSTOCK

// Inizio di UpdateHeadTracking, prima dei telai delle mani: la presa della
// torretta decide l'angolo del telaio (HoldAngleDeg: gunangle con NORMAL,
// stockangle con GUNSTOCK, anche se "Controller hold" e' l'altro).
void TurretGripBegin() {
    const int g = TurretAimActive() ? vr::pure::ResolveTurretGrip(vr::control::g_turretGrip.load(), vr::control::g_holdMode.load()) : -1;
    vr::control::g_turretHoldNow = g;
    vr::control::g_turretGripsAreHandles = g == 0;
    if (g != 0 && g_turretGripNow == 0) {
        g_turretBar.Reset();
        g_turretBarOut = vr::pure::HandlebarOut();
        g_turretHandles.Reset();
        g_turretHandlesOut = vr::pure::TurretHandlesOut();
    }
    g_turretGripNow = g;
}

// Dopo il telaio dell'arma (visore e pose finte): con la presa NORMAL su una
// torretta la mira viene dal manubrio quando le due mani lo tengono (passaggio
// di 100 ms da e verso la mira con una mano). gun: telaio dell'arma della mano
// dell'arma (tracciamento); gunRaw / supRaw: pose raw delle due mani.
void TurretGripStep(const float gun[3][4], const float gunRaw[3][4], bool supValid, const float supRaw[3][4]) {
    if (g_turretGripNow != 0) return;
    namespace vp = vr::pure;
    const double nowMs = ProfMs();
    static double lastMs = 0.0;
    float dt = lastMs > 0.0 ? static_cast<float>((nowMs - lastMs) * 0.001) : 0.0f;
    lastMs = nowMs;
    // Dopo un buco (passo non chiamato: un'altra torretta, controller perso,
    // presa GUNSTOCK, scatto del gioco) un dt nominale, non il buco: prima dt
    // fino a 0,1 s = blendSec portava il peso 0 -> 1 in un tick (review WI-4c
    // round 1: +9,1 gradi in un tick rimettendo "turretgrip normal"). Handlebar
    // limita comunque il passo a 1/30 s.
    if (dt > 0.1f || dt <= 0.0f) dt = 0.011f;
    g_turretBar.p = vp::HandlebarParamsFor(vr::control::g_turretBarMinCm.load(), vr::control::g_turretBarMaxCm.load(),
                                           vr::control::g_turretBarAlignDeg.load(), vr::control::g_turretBarSlopeDeg.load(),
                                           vr::control::g_turretBarBlendSec.load(), vr::control::g_turretBarSmoothHz.load(),
                                           vr::control::g_turretBarBeta.load());
    const int gunPhys = vr::control::PhysicalHand(0), supPhys = vr::control::PhysicalHand(1);
    const vp::GunFrameParams gpG = vr::control::GunFrameFor(gunPhys, true);
    const vp::GunFrameParams gpS = vr::control::GunFrameFor(supPhys, true);
    float palmG[3], palmS[3] = {0.0f, 0.0f, 0.0f}, fwdG[3], fwdS[3] = {0.0f, 0.0f, 0.0f};
    vp::PalmOf(gunRaw, gpG.left, gpG.gripFix, palmG);
    const vp::Euler eg = vp::ExactEuler(gun);
    for (int i = 0; i < 3; ++i) fwdG[i] = -gun[i][2];
    float pitchS = 0.0f;
    if (supValid) {
        vp::PalmOf(supRaw, gpS.left, gpS.gripFix, palmS);
        float sf[3][4];
        vp::GunFrameFromRaw(supRaw, gpS, sf);  // stesso angolo della presa: niente salto passando a una mano
        pitchS = vp::ExactEuler(sf).pitch;
        for (int i = 0; i < 3; ++i) fwdS[i] = -sf[i][2];
    }
    // Mani FISICHE: la maniglia sinistra resta a sinistra anche per i mancini.
    const bool gunLeft = gunPhys == 1;
    vp::HandlebarIn in;
    in.dt = dt;
    // Manubrio solo sulla minigun dell'elicottero: le torrette a terra hanno le
    // maniglie dietro il perno (TurretHandles qui sotto, visore 27/10).
    in.allowed = g_flightGunHeld.load();
    in.rightValid = gunLeft ? supValid : true;
    in.leftValid = gunLeft ? true : supValid;
    for (int i = 0; i < 3; ++i) {
        in.palmR[i] = gunLeft ? palmS[i] : palmG[i];
        in.palmL[i] = gunLeft ? palmG[i] : palmS[i];
        in.fwdR[i] = gunLeft ? fwdS[i] : fwdG[i];
        in.fwdL[i] = gunLeft ? fwdG[i] : fwdS[i];
    }
    in.pitchR = gunLeft ? pitchS : eg.pitch;
    in.pitchL = gunLeft ? eg.pitch : pitchS;
    // Servono i due grip (in quale ordine non conta): mani appoggiate in grembo
    // o affiancate senza stringere non girano la torretta.
    in.gripR = vr::control::g_gripRaw[0].load() || vr::control::g_gripSim[0].load();
    in.gripL = vr::control::g_gripRaw[1].load() || vr::control::g_gripSim[1].load();
    in.oneYaw = eg.yaw;
    {
        TurretHandsDiag& dg = g_turretHandsDiag;
        const vp::Euler er = vp::ExactEuler(gunRaw);
        const float r2d = 57.2957795f;
        dg.valid = true;
        dg.supValid = supValid;
        dg.gunYpr[0] = eg.yaw * r2d, dg.gunYpr[1] = eg.pitch * r2d, dg.gunYpr[2] = eg.roll * r2d;
        dg.rawG[0] = er.yaw * r2d, dg.rawG[1] = er.pitch * r2d, dg.rawG[2] = er.roll * r2d;
        if (supValid) {
            const vp::Euler es = vp::ExactEuler(supRaw);
            dg.rawS[0] = es.yaw * r2d, dg.rawS[1] = es.pitch * r2d, dg.rawS[2] = es.roll * r2d;
        }
        for (int i = 0; i < 3; ++i) {
            dg.palmG[i] = palmG[i] * 100.0f;
            dg.palmS[i] = palmS[i] * 100.0f;
        }
    }
    in.onePitch = eg.pitch;
    const vp::HandlebarOut o = g_turretBar.Step(in);
    g_turretBarOut = o;
    {
        static double winStart = 0.0, lastWhy = 0.0;
        static int inWin = 0;
        if (nowMs - winStart > 10000.0) {
            winStart = nowMs;
            inWin = 0;
        }
        char b[400];
        // Scarto manubrio / mano dell'arma all'aggancio e al rilascio (review
        // WI-4c round 1, basso): il passaggio di 100 ms gira la torretta di
        // tanto; nel visore dice se serve tenerlo e farlo calare col movimento
        // della mano, come la presa a due mani del fucile (WI-4b).
        const float offDeg = vp::WrapPi(o.barYaw - in.oneYaw) * 57.2957795f;
        const float offPitchDeg = (o.barPitch - in.onePitch) * 57.2957795f;
        if (o.event == 1 && inWin++ < 20) {
            std::snprintf(b, sizeof(b),
                          "TURRET: handlebar grip on: palms %.0f cm apart, line %.0f deg from the controllers' forward, slope %.0f deg; "
                          "turret yaw from the line between the palms; handlebar vs gun hand yaw %+.1f pitch %+.1f deg (blend %.0f ms).",
                          o.sepM * 100.0f, o.alignDeg, o.slopeDeg, offDeg, offPitchDeg, g_turretBar.p.blendSec * 1000.0f);
            vr::log::Line(b);
        } else if (o.event == 2 && inWin++ < 20) {
            std::snprintf(b, sizeof(b),
                          "TURRET: handlebar grip off (%s): back to the gun hand in %.0f ms (palms %.0f cm apart, %.0f deg, slope %.0f deg; "
                          "handlebar vs gun hand yaw %+.1f pitch %+.1f deg).",
                          o.why, g_turretBar.p.blendSec * 1000.0f, o.sepM * 100.0f, o.alignDeg, o.slopeDeg, offDeg, offPitchDeg);
            vr::log::Line(b);
        } else if (!o.engaged && in.gripR && in.gripL && nowMs - lastWhy > 2000.0) {
            // Grip stretti ma niente manubrio: perche' (collaudi, visore).
            lastWhy = nowMs;
            std::snprintf(b, sizeof(b), "TURRET: both grips held, no handlebar: %s (palms %.0f cm apart, %.0f deg, slope %.0f deg).", o.why,
                          o.sepM * 100.0f, o.alignDeg, o.slopeDeg);
            vr::log::Line(b);
        }
    }
    if (o.weight > 0.0f) {
        g_aimDeltaYaw = vr::control::g_yawSign * RadiansToUnreal(vp::WrapPi(o.yaw - g_hmdYawReference));
        g_aimDeltaPitch = vr::control::g_pitchSign * RadiansToUnreal(o.pitch);
    }
    // Torrette a terra: le due mani sulle maniglie (palmi entro 3-90 cm) mirano
    // col perno davanti alle mani (vr::pure::TurretHandles): mani a sinistra ->
    // canna a destra, mani giu' -> canna su. Prima mirava il solo controller
    // dell'arma (tenuto in verticale girava col polso). Non sulla minigun
    // dell'elicottero (utente: "solo le torrette sono a perno credo").
    {
        float sep = 1e9f;
        if (supValid) {
            const float dx = palmG[0] - palmS[0], dy = palmG[1] - palmS[1], dz = palmG[2] - palmS[2];
            sep = std::sqrt(dx * dx + dy * dy + dz * dz);
        }
        const bool want = !g_flightGunHeld.load() && supValid && sep >= 0.03f && sep <= 0.90f;
        const vp::TurretHandlesOut ho = g_turretHandles.Step(want, palmG, palmS, eg.yaw, eg.pitch, dt,
                                                             vr::control::g_turretPivotCm.load() * 0.01f);
        g_turretHandlesOut = ho;
        if (ho.event) {
            char hb[240];
            std::snprintf(hb, sizeof(hb),
                          "TURRET: two-hand handles %s (palms %.0f cm apart; pivot %.0f cm in front of the hands, aim from it yaw %+.1f "
                          "pitch %+.1f deg, gun hand alone yaw %+.1f).",
                          ho.event == 1 ? "on" : "off", sep < 1e8f ? sep * 100.0f : -1.0f, ho.lever * 100.0f, ho.pivotYaw * 57.2957795f,
                          ho.pivotPitch * 57.2957795f, eg.yaw * 57.2957795f);
            vr::log::Line(hb);
        }
        if (ho.weight > 0.0f && o.weight <= 0.0f) {
            g_aimDeltaYaw = vr::control::g_yawSign * RadiansToUnreal(vp::WrapPi(ho.yaw - g_hmdYawReference));
            g_aimDeltaPitch = vr::control::g_pitchSign * RadiansToUnreal(ho.pitch);
        }
    }
}

// Posa raw di un telaio dell'arma (inversa di GunFrameFromRaw): per le pose
// finte senza synthctrl (synthaim), dove c'e' solo la direzione dell'arma.
void RawFromGunFrame(const float gm[3][4], const vr::pure::GunFrameParams& p, float raw[3][4]) {
    float g[3][4];
    vr::pure::GunRotInRaw(p, g);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) raw[i][j] = gm[i][0] * g[j][0] + gm[i][1] * g[j][1] + gm[i][2] * g[j][2];
    // polso = palmo - G o, palmo = origine raw + R o  ->  origine raw = polso + G o - R o
    const float o[3] = {p.gripFix ? (p.left ? vr::pure::kGripOriginX : -vr::pure::kGripOriginX) : 0.0f,
                        p.gripFix ? vr::pure::kGripOriginY : 0.0f, p.gripFix ? vr::pure::kGripOriginZ : 0.0f};
    for (int i = 0; i < 3; ++i)
        raw[i][3] = gm[i][3] + (gm[i][0] - raw[i][0]) * o[0] + (gm[i][1] - raw[i][1]) * o[1] + (gm[i][2] - raw[i][2]) * o[2];
}

// Altezza degli occhi per "height auto" da quella della testa in piedi (metri
// sul pavimento) meno il centro del Pawn in piedi (45 unita' sopra i piedi),
// entro kEyeHeightMin..55 unita'. "synthheight".
// Minimo = gli occhi misurati di Walker (WI-3 fix 1, 36,25; WI-2 aveva 30 in
// attesa della misura, prima ancora 25). 30 era l'altezza del suo collo: chi
// ha la testa sotto ~1,62 m in piedi, "seated off" o seduto riconosciuto in
// piedi aveva gli occhi nel collo di Walker, col colletto sotto il mento (bug
// 5). Si limita solo il riferimento (StableEyeHeight): la testa vale 1:1.
constexpr float kEyeHeightMin = vr::pure::kWalkerEyeStand, kEyeHeightMax = 55.0f;
float EyeHeightAutoFromStandingM(float meters) {
    float h = meters * vr::control::g_posScale - 45.0f;
    return h < kEyeHeightMin ? kEyeHeightMin : (h > kEyeHeightMax ? kEyeHeightMax : h);
}

// Diagnostica (viewlog): riferimento della posizione della testa (metri, spazio del runtime).
float g_viewDiagPosRef[3] = {0.0f, 0.0f, 0.0f};
float g_viewDiagRefY = 0.0f;  // riferimento verticale della vista (WI-2, m)
// Diagnostica (riga TURRET): corpo (g_bodyYaw) quando la rotazione della testa
// di questo tick e' stata fissata (g_deltaYaw). Quanto il corpo cambia dopo, nello
// stesso tick, la vista lo mostra subito: e' l'anticipo della vista sulla
// torretta (b1/b2), compensato solo al tick dopo.
int32_t g_bodyYawAtHeadSample = 0;

// Torretta: il PlayerController (e con lui la torretta) ruota verso il
// controller destro; la vista non gira e il beccheggio del corpo non entra
// nella vista. Prima la torretta seguiva lo stick e solo in parte il
// controller ("had to aim my headset", 18/10). Scesi dalla torretta: il
// beccheggio scritto sul PlayerController restava (lo stick su/giu' e'
// escluso), e razzi, lanciagranate e il controllo dell'esecuzione partivano
// inclinati (audit 20/10).
// Funzione unica per il visore e le pose finte (25/10: in "mode synth" la
// torretta non seguiva synthaim e i collaudi senza visore non la vedevano).
//
// WI-1 (25/10, bug 1 e 2 del tester), misurato sul runtime finto con la mano
// girata di 30 gradi: la vista girava con tutto il passo della torretta (+10,9
// gradi nel tick del colpo di polso) e poi tornava indietro in ~1 s (24 gradi
// di movimento della vista per un netto di 0). Cause:
//  - lo yaw era sommato anche a g_bodyYaw (AddControllerYaw), che la vista di
//    questo tick legge, ma il riferimento della testa si spostava solo al tick
//    dopo: ora il passo va solo sul PlayerController, il corpo della vista lo
//    prende al tick dopo dal gioco, insieme al riferimento (TurretCompensation);
//  - l'errore non era avvolto a +-180 gradi: con riferimento e controller ai
//    due lati dei 180 gradi del tracciamento la torretta saltava di 133 gradi
//    nel verso sbagliato (misurato, testa a 175 gradi);
//  - guadagno fino a 0,94 nei tick lunghi: ora al massimo 0,5;
//  - la compensazione usava il cambio del corpo, base compresa: con
//    l'elicottero che gira contro il passo non toglieva nulla e la torretta
//    strisciava (WI-0). Ora si toglie il passo eseguito, al netto della base.
// Riga TURRET (2 al secondo, piu' i tick con un passo, un anticipo o un
// rifiuto grandi): bersaglio, PlayerController, passo di questo tick, parte
// rifiutata dal gioco del passo precedente (al netto della base: il gioco porta
// lo yaw del PlayerController con la base 1:1, misurato in elicottero 25/10),
// rotazione della base e anticipo della vista (corpo cambiato dopo la posa
// della testa di questo tick: ora 0).
struct TurretBook {
    bool pending = false;   // passo scritto, da contare al tick dopo
    int32_t step = 0;       // passo scritto (unita')
    int32_t err = 0;        // errore di yaw da cui era calcolato (isteresi del blocco, 27/10)
    int32_t pcAfter = 0;    // yaw del PlayerController subito dopo la scrittura
    int32_t baseYaw = 0;    // yaw della base (elicottero, mezzo) alla scrittura
    bool baseValid = false;
    void* pc = nullptr;     // PlayerController e base della scrittura: se cambiano
    uint32_t base = 0;      // (livello, morte, discesa) il conto non vale
    // Ultimo conteggio (riga TURRET).
    int32_t refused = 0, executed = 0, moved = 0, baseDelta = 0, bodyVsPc = 0;
    float baseFactor = 1.0f;   // termine della base usato (0 = base che non porta il PlayerController)
    int basePhysics = -1;      // Physics della base al conteggio (-1 = nessuna base o non letto)
    bool baseIsTurret = false; // la base e' l'attore della torretta (m_turretInteraction -> m_turretActor)
    bool afterTurret = false;  // conteggio fatto dopo l'uscita dalla torretta
    bool counted = false;      // conteggio fatto in questo tick (blocco al limite, WI-4c)
};
TurretBook g_turretBook;

// Yaw della base del Pawn (Pawn.Base +0xAC, Rotation.Yaw +0x64).
bool PawnBaseYaw(int32_t* out, uint32_t* baseOut = nullptr) {
    char* camB = static_cast<char*>(g_cameraPtr.load());
    void* pawnB = HasCameraSignature(camB) ? PlayerPawnIfViewTarget(camB) : nullptr;
    uint32_t base = 0;
    const bool ok = pawnB && TrySafeReadUint32(static_cast<char*>(pawnB) + 0xAC, &base) && base &&
                    SafeCopy(out, reinterpret_cast<char*>(base) + 0x64, 4);
    if (baseOut) *baseOut = ok ? base : 0;
    return ok;
}

// Inizio del tick, prima della posa della testa: quanto del passo scritto al
// tick prima il gioco ha davvero eseguito, al netto della base (unita' del
// corpo). Il chiamante sposta il riferimento della testa di altrettanto, nello
// stesso tick in cui il corpo della vista (g_bodyYaw, dal gioco) lo contiene.
int32_t TurretCompensation() {
    TurretBook& b = g_turretBook;
    if (!b.pending) return 0;
    b.pending = false;
    void* pc = LivePlayerController();
    int32_t pcNow = 0, baseNow = 0;
    // Stesso PlayerController e stessa base, se no (livello, morte, salita o
    // discesa da un mezzo) la differenza non e' il passo: niente conto. Non
    // serve essere ancora in torretta (review WI-1): il passo scritto
    // nell'ultimo tick in torretta e' gia' nel corpo anche se il giocatore e'
    // sceso, e' partita una cinematica o la mira col controller e' spenta;
    // prima non si contava e la vista girava di quel passo in un tick.
    if (!pc || pc != b.pc || !SafeCopy(&pcNow, static_cast<char*>(pc) + 0x64, 4)) return 0;
    uint32_t baseNowPtr = 0;
    const bool baseOk = PawnBaseYaw(&baseNow, &baseNowPtr);
    if (baseOk != b.baseValid || (baseOk && baseNowPtr != b.base)) return 0;
    b.afterTurret = !TurretAimActive();
    b.baseDelta = baseOk ? static_cast<int16_t>(baseNow - b.baseYaw) : 0;
    b.moved = static_cast<int16_t>(pcNow - b.pcAfter);
    b.bodyVsPc = static_cast<int16_t>(g_bodyYaw.load() - pcNow);  // 0 = il corpo della vista contiene gia' il passo
    // Termine della base con la regola del gioco (review 2 WI-1,
    // vr::pure::TurretBaseTerm): UpdateTurretAiming aggiunge il modificatore
    // della base solo se Base.Physics (+0x94) == PHYS_Interpolating (7,
    // l'InterpActor dell'elicottero, dove il fattore 1 e' misurato); ogni altra
    // base (torretta a terra, base = la torretta stessa) non porta il
    // PlayerController: 0, anche se segue il passo in ritardo. In piu' la
    // guardia di prima (TurretBaseFactor): 0 se la base gira col passo nello
    // stesso tick (con 1 tutto il passo eseguito risulterebbe rifiutato e la
    // vista girerebbe col passo, review WI-1).
    uint8_t basePhys = 0;
    b.basePhysics = baseOk && SafeCopy(&basePhys, reinterpret_cast<char*>(baseNowPtr) + 0x94, 1) ? basePhys : -1;
    // Base = l'attore della torretta (YGamePawn.m_turretInteraction +0x1184 ->
    // YTrigger_TurretInteraction.m_turretActor +0x224, offset dalla riflessione
    // 26/10): ha Physics 7 anche lui, ma la sua rotazione e' la mira, non un
    // mezzo che porta il PlayerController.
    b.baseIsTurret = false;
    {
        char* camT = static_cast<char*>(g_cameraPtr.load());
        void* pawnT = HasCameraSignature(camT) ? PlayerPawnIfViewTarget(camT) : nullptr;
        uint32_t inter = 0, turretActor = 0;
        if (baseOk && pawnT && TrySafeReadUint32(static_cast<char*>(pawnT) + 0x1184, &inter) && inter &&
            TrySafeReadUint32(reinterpret_cast<char*>(inter) + 0x224, &turretActor))
            b.baseIsTurret = turretActor != 0 && turretActor == baseNowPtr;
    }
    b.baseFactor = vr::pure::TurretBaseTerm(b.basePhysics, b.step, b.baseDelta, b.baseIsTurret);
    b.refused = vr::pure::TurretRefused(b.pcAfter, pcNow, b.baseDelta, b.baseFactor);
    b.executed = vr::pure::TurretExecuted(b.step, b.refused);  // al massimo il passo, nel suo verso
    b.counted = true;
    if (b.afterTurret && b.executed != 0) {
        char lb[160];
        std::snprintf(lb, sizeof(lb), "TURRET: last step after leaving the turret aim, executed %+.3f deg taken off the view.",
                      b.executed * (360.0f / 65536.0f));
        vr::log::Line(lb);
    }
    return b.executed;
}

// Supporto della torretta: Rotation dell'attore della torretta (Pawn.m_turretInteraction
// +0x1184 -> m_turretActor +0x224), il centro dei limiti (WI-4c: misurato, non gira
// con la mira; la base del Pawn e' l'elicottero o il terreno).
bool TurretMountRotation(int32_t rot[3]) {
    char* camT = static_cast<char*>(g_cameraPtr.load());
    void* pawnT = HasCameraSignature(camT) ? PlayerPawnIfViewTarget(camT) : nullptr;
    uint32_t inter = 0, act = 0;
    return pawnT && TrySafeReadUint32(static_cast<char*>(pawnT) + 0x1184, &inter) && inter &&
           TrySafeReadUint32(reinterpret_cast<char*>(inter) + 0x224, &act) && act &&
           SafeCopy(rot, reinterpret_cast<char*>(act) + 0x60, 12);
}

// Mira del gioco (YGamePawn +0x14A8, il rotatore che UpdateTurretAiming usa:
// uguale alla Rotation del PlayerController a regime, WI-4c 26/10; nei passi
// grandi un tick del gioco dietro: 11,4 gradi nel passo a -60 della corsa synth).
bool TurretGameAim(int32_t* pitch, int32_t* yaw) {
    char* camT = static_cast<char*>(g_cameraPtr.load());
    void* pawnT = HasCameraSignature(camT) ? PlayerPawnIfViewTarget(camT) : nullptr;
    int32_t r[2] = {0, 0};
    if (!pawnT || !SafeCopy(r, static_cast<char*>(pawnT) + 0x14A8, 8)) return false;
    *pitch = r[0];
    *yaw = r[1];
    return true;
}

vr::pure::TurretYawHold g_turretYawHold;
vr::pure::TurretLimitLatch g_turretLatch;
vr::pure::TurretLimitBuzz g_turretBuzz;

void TurretAimStep() {
    static ULONGLONG lastLog = 0;
    TurretBook& book = g_turretBook;
    {
        static bool wasTurret = false;
        bool turret = TurretAimActive();
        void* pcT = LivePlayerController();
        if (wasTurret && !turret && pcT && !vr::control::g_stickPitch) {
            int32_t zero = 0;
            SafeCopy(static_cast<char*>(pcT) + 0x60, &zero, 4);
            vr::log::Line("TURRET: left, PlayerController pitch reset to 0.");
        }
        if (wasTurret && !turret) {
            g_turretBar.Reset();
            g_turretHandles.Reset();
            g_turretHandlesOut = vr::pure::TurretHandlesOut();
            g_turretLatch.Reset();
            g_turretYawHold.Reset();
        }
        if (!wasTurret && turret) {
            const bool normal = g_turretGripNow != 1;
            static const char* const kGripName[] = {"auto", "normal", "gunstock"};
            const int tg = vr::control::g_turretGrip.load();
            vr::log::Line(std::string("TURRET: on (") + (vr::control::g_mode == vr::control::kModeSynthetic ? "synthetic poses" : "headset") +
                          "), turret grip " +
                          (normal ? (g_flightGunHeld.load() ? "NORMAL (both grips held = handlebars; one hand also aims)"
                                                   : "NORMAL (both hands on the handles: pivot like a real machine gun, turretpivot; one hand also aims)")
                                : "GUNSTOCK (aimed along the stock)") +
                          " from turretgrip " + kGripName[tg >= 0 && tg <= 2 ? tg : 0] + (g_flightGunHeld.load() ? ", helicopter minigun" : "") +
                          ", limits " + (vr::control::g_turretLimits.load() ? "on" : "off") + ".");
            book = TurretBook{};
            lastLog = 0;
            g_turretLatch.Reset();
            g_turretYawHold.Reset();
            g_turretBuzz.Reset();
            // Guida (b1: "it wasn't until I saw the hint to use right hand only
            // that I was able to aim"): alla prima torretta della sessione.
            static bool toasted = false;
            if (!toasted && vr::control::g_hints.load() != 0 && vr::control::g_mode != vr::control::kModeSynthetic) {
                toasted = true;
                // Testi brevi (ui_text.h, misurati nei test offline): prima tagliati
                // dopo ~38 caratteri (review WI-4c round 1).
                vr::toast::Notify(normal ? vr::ui::kToastTurretNormal : vr::ui::kToastTurretGunstock);
            }
        }
        wasTurret = turret;
    }
    void* pcTurret = LivePlayerController();
    if (!(TurretAimActive() && g_aimValid && pcTurret)) {
        book.pending = false;
        book.counted = false;
        return;
    }
    char* pc = static_cast<char*>(pcTurret);
    const double nowS = ProfMs() * 0.001;
    // Blocco al limite del gioco (WI-4c): conteggio del passo del tick prima.
    if (book.counted) {
        book.counted = false;
        const int ev = g_turretLatch.Count(book.step, book.refused, book.err);
        if (ev == 1 || ev == -1) {
            vr::input::PulseHand(0, 40);
            vr::input::PulseHand(1, 40);
            char lb[220];
            std::snprintf(lb, sizeof(lb),
                          "TURRET: limit latch on (%s): the game took back %.2f of %.2f deg 3 ticks in a row, the aim 1 deg or more past; no more push that way.",
                          ev > 0 ? "right" : "left", book.refused * (360.0f / 65536.0f), book.step * (360.0f / 65536.0f));
            vr::log::Line(lb);
        } else if (ev == -2) {
            vr::log::Line("TURRET: limit latch off (a probe step was accepted).");
        }
    }
    // Guadagno per tempo, non per tick (L17: a 300 fps la torretta inseguiva
    // il controller 3 volte piu' in fretta che a 90): costante di tempo 30 ms,
    // come 0,35 per tick a 90 Hz, ma al massimo 0,5 per tick (WI-1).
    static double lastTurretT = 0.0;
    float dtT = lastTurretT > 0.0 ? static_cast<float>(nowS - lastTurretT) : 0.011f;
    lastTurretT = nowS;
    if (dtT > 0.1f || dtT <= 0.0f) dtT = 0.011f;
    const float gain = vr::pure::TurretGain(dtT);
    int32_t yawBefore = 0;
    const bool yawRead = SafeCopy(&yawBefore, pc + 0x64, 4);
    uint32_t pitchNow = 0;
    const bool pitchRead = TrySafeReadUint32(pc + 0x60, &pitchNow);
    // Con la canna oltre 70 gradi dall'orizzonte lo yaw resta fermo (review
    // WI-4a: lo yaw esatto del telaio li' gira di decine di gradi con pochi
    // gradi di tremolio), e riparte sotto 65 (isteresi, review 2 WI-4a: senza,
    // con la canna sul limite il tremolio alternava fermo e libero a ogni tick).
    // Col manubrio lo yaw viene dalle posizioni dei palmi: niente blocco.
    // Anche con le maniglie a mani vicine lo yaw viene dalle due mani (tenuto
    // fermo da TurretHandles con i controller quasi verticali): niente blocco.
    const bool bar = g_turretBarOut.weight > 0.0f || g_turretHandlesOut.weight > 0.0f;
    bool holdChanged = false;
    bool yawHeld = false;
    if (bar) {
        holdChanged = g_turretYawHold.held;
        g_turretYawHold.Reset();
    } else {
        yawHeld = g_turretYawHold.Step(g_aimDeltaPitch.load() * (360.0f / 65536.0f), &holdChanged);
    }
    if (holdChanged) {
        char hb[180];
        std::snprintf(hb, sizeof(hb), "TURRET: yaw %s (barrel %+.1f deg from level, held above %.0f, free below %.0f).",
                      yawHeld ? "held, barrel near vertical" : "free again", g_aimDeltaPitch.load() * (360.0f / 65536.0f),
                      vr::pure::kTurretYawHoldPitchDeg, vr::pure::kTurretYawFreePitchDeg);
        vr::log::Line(hb);
    }
    // Bersaglio nel mondo (unita'): yaw del PlayerController + errore avvolto
    // (WI-1), beccheggio assoluto. Limiti nel telaio del supporto (WI-4c).
    const int32_t errRaw = static_cast<int16_t>(static_cast<uint16_t>(g_aimDeltaYaw.load() & 0xFFFF));
    int32_t tgtYaw = yawBefore + errRaw, tgtPitch = g_aimDeltaPitch.load();
    int32_t mount[3] = {0, 0, 0};
    const bool mountOk = TurretMountRotation(mount);
    // Limiti dalle chiavi Avanzate (turretground* / turretflight*, review WI-4c
    // round 1): quelli a terra non sono misurati (la M2E2 non si e' potuta
    // salire), cosi' il visore li corregge senza ricompilare.
    vr::pure::TurretLimits lim;
    {
        const int m = g_flightGunHeld.load() ? 1 : 0;
        lim.yawRightDeg = vr::control::g_turretLimitDeg[m][0].load();
        lim.yawLeftDeg = vr::control::g_turretLimitDeg[m][1].load();
        lim.pitchUpDeg = vr::control::g_turretLimitDeg[m][2].load();
        lim.pitchDownDeg = vr::control::g_turretLimitDeg[m][3].load();
    }
    float localYaw = 0.0f, localPitch = 0.0f;
    bool clamped = false;
    if (vr::control::g_turretLimits.load() && mountOk && yawRead) {
        int32_t cp = tgtPitch, cy = tgtYaw;
        clamped = vr::pure::ClampAimInMount(tgtPitch, tgtYaw, mount, lim, &cp, &cy, &localYaw, &localPitch);
        tgtPitch = cp;
        tgtYaw = cy;
    }
    // Colpetto sulle due mani quando la mira arriva al limite: uno solo, di
    // nuovo solo dopo 0,5 s ad almeno 2 gradi dentro i limiti (review WI-4c
    // round 1: col supporto che si inclina in virata vibrava fino a ogni 0,6 s).
    if (vr::control::g_turretLimits.load() && mountOk && yawRead &&
        g_turretBuzz.Step(clamped, vr::pure::TurretLimitMarginDeg(localYaw, localPitch, lim), nowS)) {
        vr::input::PulseHand(0, 30);
        vr::input::PulseHand(1, 30);
        static int buzzLines = 0;
        if (buzzLines++ < 40) {
            char zb[200];
            std::snprintf(zb, sizeof(zb), "TURRET: limit buzz (aim in the mount yaw %+.1f pitch %+.1f deg; limits +%.0f/-%.0f up %.0f down %.0f).",
                          localYaw, localPitch, lim.yawRightDeg, lim.yawLeftDeg, lim.pitchUpDeg, lim.pitchDownDeg);
            vr::log::Line(zb);
        }
    }
    const int32_t errYaw = static_cast<int16_t>(static_cast<uint16_t>((tgtYaw - yawBefore) & 0xFFFF));
    int32_t dy = yawRead && !yawHeld ? vr::pure::TurretYawStep(errYaw, dtT) : 0;
    bool latchReleased = false;
    dy = g_turretLatch.Filter(dy, errYaw, nowS, &latchReleased);
    if (latchReleased) vr::log::Line("TURRET: limit latch off (the aim came back).");
    // Solo il PlayerController (niente g_bodyYaw ne' g_ownYawUnits): la vista
    // di questo tick resta sul corpo letto con la testa; il gioco porta il
    // passo nel corpo al tick dopo e TurretCompensation sposta il riferimento
    // della testa di quanto eseguito, nello stesso tick (WI-1).
    int32_t pcAfter = yawBefore + dy;
    if (yawRead && dy != 0 && !SafeCopy(pc + 0x64, &pcAfter, 4)) pcAfter = yawBefore;
    book.pending = yawRead;
    book.step = pcAfter - yawBefore;
    book.err = errYaw;
    book.pcAfter = pcAfter;
    book.pc = pcTurret;
    book.baseValid = PawnBaseYaw(&book.baseYaw, &book.base);
    int32_t newPitch = static_cast<int32_t>(pitchNow);
    int16_t err = 0;
    if (pitchRead) {
        err = static_cast<int16_t>(tgtPitch - static_cast<int32_t>(pitchNow));
        newPitch = static_cast<int32_t>(pitchNow) + static_cast<int32_t>(err * gain);
        SafeCopy(pc + 0x60, &newPitch, 4);
    }
    // Anticipo della vista: corpo cambiato dopo la posa della testa di questo
    // tick (con la scrittura solo sul PlayerController resta 0; la riga VIEW
    // ha "bodyhs" per controllarlo anche nei disegni degli occhi).
    const int32_t lead = static_cast<int16_t>(g_bodyYaw.load() - g_bodyYawAtHeadSample);
    // Due righe al secondo, piu' una riga in ogni tick con un passo o un
    // anticipo della vista oltre 1 grado o un rifiuto oltre 0,1 gradi (al piu'
    // 16 in piu' ogni mezzo secondo).
    constexpr int32_t kOneDeg = 182, kTenthDeg = 18;  // unita' Unreal
    const bool change = std::abs(dy) > kOneDeg || std::abs(lead) > kOneDeg || std::abs(book.refused) > kTenthDeg;
    static int extraLines = 0;
    ULONGLONG nowL = GetTickCount64();
    const bool periodic = nowL - lastLog >= 500;
    if (periodic || (change && extraLines < 16)) {
        if (periodic) {
            lastLog = nowL;
            extraLines = 0;
        } else {
            ++extraLines;
        }
        const float u2d = 360.0f / 65536.0f;
        int16_t pcPitch = static_cast<int16_t>(newPitch & 0xFFFF);
        // Fattore della base per la mira (PlayerController +0xBB0, non
        // verificato): solo diagnostica; la compensazione usa 1 (misurato in
        // elicottero) o 0 con la base che segue il passo ("base term").
        float baseFactor = 0.0f;
        SafeCopy(&baseFactor, pc + 0xBB0, 4);
        // Base del Pawn (review 2 WI-1): nome, classe e Physics (7 =
        // PHYS_Interpolating, la sola che il gioco porta), cosi' la prima corsa
        // sulla M2E2 dice se la base e' la torretta, il pavimento o un mezzo.
        void* baseObj = book.baseValid ? reinterpret_cast<void*>(book.base) : nullptr;
        const std::string baseName = baseObj ? vr::script::ObjectName(baseObj) : std::string("none");
        const std::string baseClass = baseObj ? vr::script::ClassName(baseObj) : std::string("-");
        int32_t gaP = 0, gaY = 0;
        const bool ga = TurretGameAim(&gaP, &gaY);
        const vr::pure::HandlebarOut& hb = g_turretBarOut;
        // Campi del WI-4c in fondo (i parser dei collaudi di prima leggono l'inizio).
        char b[1500];
        std::snprintf(b, sizeof(b),
                      "TURRET: grip %s | target yaw %+.2f pitch %+.2f deg (from the body) | PC yaw %.2f pitch %+.2f deg | "
                      "step yaw %+.3f pitch %+.3f deg (gain %.3f, dt %.1f ms) | refused %+.3f deg (moved back %+.3f, base included) | "
                      "base yaw %.2f delta %+.3f deg | view lead %+.3f deg (body turned after the head sample) | logged %s | "
                      "executed %+.3f deg, base factor %.2f, view body vs PC at the tick start %+.3f deg, base term %.0f | "
                      "base %.60s (%.60s) physics %d%s | turretgrip %s, handlebar w %.2f %s (palms %.1f cm, %.1f deg, slope %.1f) "
                      "bar yaw %+.2f pitch %+.2f | mount %s yaw %.2f pitch %+.2f roll %+.2f, aim in mount yaw %+.2f pitch %+.2f, "
                      "limits +%.0f/-%.0f up %.0f down %.0f %s, clamped %d, clamped target yaw %.2f pitch %+.2f, latch %d | "
                      "game aim yaw %.2f pitch %+.2f%s",
                      g_turretBarOut.weight > 0.0f ? "handlebar" : (g_turretHandlesOut.weight > 0.0f ? "two-hand handles" : "one-hand"), static_cast<int16_t>(g_aimDeltaYaw.load() & 0xFFFF) * u2d, g_aimDeltaPitch.load() * u2d,
                      static_cast<float>(static_cast<uint16_t>(pcAfter & 0xFFFF)) * u2d, pcPitch * u2d, book.step * u2d,
                      err * gain * u2d, gain, dtT * 1000.0f, book.refused * u2d, -book.moved * u2d,
                      static_cast<float>(static_cast<uint16_t>(book.baseYaw & 0xFFFF)) * u2d, book.baseDelta * u2d, lead * u2d,
                      periodic ? "2 Hz" : "on change", book.executed * u2d, baseFactor, book.bodyVsPc * u2d, book.baseFactor,
                      baseName.c_str(), baseClass.c_str(), book.basePhysics, book.baseIsTurret ? ", the turret itself" : "",
                      g_turretGripNow == 1 ? "gunstock" : "normal", hb.weight, hb.why, hb.sepM * 100.0f, hb.alignDeg, hb.slopeDeg,
                      hb.barYaw * 57.2957795f, hb.barPitch * 57.2957795f, mountOk ? "ok" : "unread",
                      static_cast<float>(static_cast<uint16_t>(mount[1] & 0xFFFF)) * u2d, static_cast<int16_t>(mount[0] & 0xFFFF) * u2d,
                      static_cast<int16_t>(mount[2] & 0xFFFF) * u2d, localYaw, localPitch, lim.yawRightDeg, lim.yawLeftDeg,
                      lim.pitchUpDeg, lim.pitchDownDeg, vr::control::g_turretLimits.load() ? "on" : "off", static_cast<int>(clamped),
                      static_cast<float>(static_cast<uint16_t>(tgtYaw & 0xFFFF)) * u2d, static_cast<int16_t>(tgtPitch & 0xFFFF) * u2d,
                      g_turretLatch.dir, static_cast<float>(static_cast<uint16_t>(gaY & 0xFFFF)) * u2d,
                      static_cast<int16_t>(gaP & 0xFFFF) * u2d, ga ? "" : " (unread)");
        if (g_turretHandsDiag.valid) {
            const TurretHandsDiag& dg = g_turretHandsDiag;
            const size_t bl = std::strlen(b);
            std::snprintf(b + bl, sizeof(b) - bl,
                          " | hands: gun frame ypr (%.1f, %.1f, %.1f) gun hand raw ypr (%.1f, %.1f, %.1f) support raw ypr %s(%.1f, %.1f, "
                          "%.1f) palm gun (%.1f, %.1f, %.1f) support (%.1f, %.1f, %.1f) cm",
                          dg.gunYpr[0], dg.gunYpr[1], dg.gunYpr[2], dg.rawG[0], dg.rawG[1], dg.rawG[2], dg.supValid ? "" : "(no) ",
                          dg.rawS[0], dg.rawS[1], dg.rawS[2], dg.palmG[0], dg.palmG[1], dg.palmG[2], dg.palmS[0], dg.palmS[1],
                          dg.palmS[2]);
        }
        vr::log::Line(b);
    }
}

void UpdateHeadTracking() {
    g_tickHmdPoseValid = false;
    if (vr::control::g_exiting) {
        g_deltaYaw = 0;
        g_headHeadingYaw = 0;  // direzione stabile (bodybacklook, esecuzioni): review WI-1
        g_deltaPitch = 0;
        g_deltaRoll = 0;
        for (auto& v : g_headOffset) v = 0.0f;
        return;
    }
    static int frame = 0;
    // Ogni 2 s (non ogni 120 frame: senza visore il gioco rende a oltre
    // 1000 fps e il log si riempiva di 13 dump al secondo, 21/10).
    static ULONGLONG lastDump = 0;
    ULONGLONG nowDump = GetTickCount64();
    bool shouldLog = (nowDump - lastDump >= 2000);
    if (shouldLog) lastDump = nowDump;
    ++frame;
    g_roomStepValid = false;  // vale per il tick in cui il passo e' fatto (L8)
    {
        // "roomstep <unita'>": passo diagnostico lungo l'avanti del corpo,
        // anche in modalita' sintetica (verifica di L8 con camlog).
        float req = vr::control::g_roomStepRequest.exchange(0.0f);
        char* camS = static_cast<char*>(g_cameraPtr.load());
        void* pawnS = HasCameraSignature(camS) ? PlayerPawnIfViewTarget(camS) : nullptr;
        if (req != 0.0f && pawnS) {
            float b = g_bodyYaw.load() * (6.28318530718f / 65536.0f), moved[2] = {0.0f, 0.0f};
            bool ok = RoomStepMove(pawnS, req * std::cos(b), req * std::sin(b), moved);
            bool raw = vr::control::g_roomStepRaw.load();
            if (raw) g_roomStepValid = false;
            char buf[128];
            std::snprintf(buf, sizeof(buf), "ROOMSTEP: asked %.1f units, pawn moved (%.2f, %.2f)%s%s.", req, moved[0], moved[1],
                          raw ? ", base not corrected" : "", ok ? "" : ", MoveSmooth call failed");
            vr::log::Line(buf);
        }
    }

    ResolveCameraClassNames();
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
        TwoHandReset();  // mira del gioco: niente presa a due mani (WI-4b)
        vr::control::g_turretHoldNow = -1;  // ne' presa della torretta (WI-4c)
        vr::control::g_turretGripsAreHandles = false;
        g_deltaYaw = 0;
        g_headHeadingYaw = 0;
        g_deltaPitch = 0;
        g_deltaRoll = 0;
        for (auto& v : g_headOffset) v = 0.0f;
        return;
    }

    TurretGripBegin();  // presa della torretta in questo tick (WI-4c), prima dei telai delle mani

    if (mode == vr::control::kModeSynthetic) {
        constexpr float kTwoPi = 6.28318530718f;
        constexpr float kDegToRad = 0.01745329252f;
        // Tempo dal contatore ad alta risoluzione (25/10): GetTickCount64 avanza
        // a passi di ~16 ms e l'oscillazione finta andava a scatti (sorgente dei collaudi del tremolio).
        float t = static_cast<float>(std::fmod(ProfMs() * 0.001, 3600.0));
        float period = vr::control::g_synthPeriodSec;
        float s = period > 0.0f ? std::sin(kTwoPi * t / period) : 1.0f;  // 0 = spostamento fisso
        g_deltaYaw = RadiansToUnreal(s * vr::control::g_synthYawAmpDeg * kDegToRad);
        g_deltaPitch = RadiansToUnreal(s * vr::control::g_synthPitchAmpDeg * kDegToRad);
        g_deltaRoll = 0;
        for (int i = 0; i < 3; ++i) g_headOffset[i] = s * vr::control::g_synthPosCm[i] * 0.01f * vr::control::g_posScale;
        // "synthheight <m>": altezza della testa in piedi come col visore ("height auto").
        if (vr::control::g_synthHeightM > 0.0f)
            vr::control::g_eyeHeightAuto = EyeHeightAutoFromStandingM(vr::control::g_synthHeightM);
        ClampCabinOffset();  // R-B2
        g_aimDeltaYaw = RadiansToUnreal(vr::control::g_synthAimYawDeg * kDegToRad);
        g_aimDeltaPitch = RadiansToUnreal(vr::control::g_synthAimPitchDeg * kDegToRad);
        g_aimValid = true;
        // Mani finte gia' nel riferimento del corpo, attorno al centro.
        g_trackRefYaw = 0;
        for (auto& v : g_trackOriginOff) v = 0.0f;
        for (int i = 0; i < 3; ++i)
            g_handOffset[i] = vr::control::g_synthHandCm[i] * 0.01f * vr::control::g_posScale;
        g_handRoll = 0;
        g_handValid = true;
        // "synthctrl": matrice raw del controller, stesso calcolo della mira del visore (telaio dell'arma compreso).
        if (vr::control::g_synthCtrlOn[0]) {
            ApplyGunControllerAim(SynthControllerMatrix(0));
            // Rotazione attorno al palmo (WI-4a): la matrice finta ha l'origine
            // in 0, quindi il polso dell'arma e' lo spostamento dall'origine del
            // controller; nel riferimento del corpo come col visore.
            const float sy = std::sin(g_hmdYawReference), cy = std::cos(g_hmdYawReference), sc = vr::control::g_posScale;
            const float* w = g_gunWristPos;
            g_handOffset[0] = g_handOffset[0] + (w[0] * sy - w[2] * cy) * sc;
            g_handOffset[1] = g_handOffset[1] + (w[0] * cy + w[2] * sy) * sc;
            g_handOffset[2] = g_handOffset[2] + w[1] * sc;
        }
        {
            float left[3];
            bool haveLeft = false;
            // synthleftjump: salto del tracciamento finto (collaudi della voce 1), una volta all'ora data.
            if (const unsigned long long at = vr::control::g_synthJumpAt; at && GetTickCount64() >= at) {
                vr::control::g_synthJumpAt = 0;
                for (int i = 0; i < 3; ++i) vr::control::g_synthLeftCm[i] = vr::control::g_synthLeftCm[i] + vr::control::g_synthJumpCm[i];
                vr::log::Line("SYNTH: left hand jumped (tracking jump test)");
            }
            for (int i = 0; i < 3; ++i) {
                left[i] = vr::control::g_synthLeftCm[i] * 0.01f * vr::control::g_posScale;
                if (vr::control::g_synthLeftCm[i] != 0.0f) haveLeft = true;
            }
            // synthleftvel: moto continuo (lanci nei collaudi); alla fine la
            // posizione raggiunta resta in g_synthLeftCm.
            ULONGLONG nowMs = GetTickCount64(), v0 = vr::control::g_synthLeftVelStart, v1 = vr::control::g_synthLeftVelUntil;
            // synthsmooth on: tempo trascorso dal contatore ad alta risoluzione
            // (GetTickCount64 avanza a passi di ~16 ms: la mano finta andava a
            // scatti di 2,3 cm a 1,5 m/s; collaudi del peso, 25/10).
            auto smoothElapsed = [](ULONGLONG start, ULONGLONG until, ULONGLONG* seen, double* qpc0, float* el) {
                if (start != *seen) {
                    *seen = start;
                    *qpc0 = ProfMs();
                }
                const double dur = static_cast<double>(until - start), e = ProfMs() - *qpc0;
                *el = static_cast<float>((e < dur ? e : dur) * 0.001);
                return e >= dur;
            };
            static ULONGLONG seenVel = 0, seenRot = 0;
            static double qpcVel = 0.0, qpcRot = 0.0;
            if (v1 > v0) {
                float el = static_cast<float>((nowMs < v1 ? nowMs : v1) - v0) * 0.001f;
                bool done = nowMs >= v1;
                if (vr::control::g_synthSmooth) done = smoothElapsed(v0, v1, &seenVel, &qpcVel, &el);
                for (int i = 0; i < 3; ++i) left[i] += vr::control::g_synthLeftVelCm[i] * el * 0.01f * vr::control::g_posScale;
                if (done) {
                    for (int i = 0; i < 3; ++i)
                        vr::control::g_synthLeftCm[i] = vr::control::g_synthLeftCm[i] + vr::control::g_synthLeftVelCm[i] * el;
                    vr::control::g_synthLeftVelUntil = 0;
                    vr::control::g_synthLeftVelStart = 0;
                }
            }
            // synthleftramp (collaudi del lancio, voce 4): velocita' da 0 a v1 in t1,
            // poi a v2 in t2, lungo una direzione, col contatore ad alta
            // risoluzione; alla fine la posizione raggiunta resta in
            // g_synthLeftCm. Con "release" il grip finto si apre alla fine e la
            // mano va avanti a v2 per altri 50 ms, come una mano vera che non si
            // ferma di colpo mentre apre le dita (il rilascio arriva al tick
            // dopo: senza la coda l'ultimo campione era la mano ferma).
            {
                static unsigned seenRamp = 0;
                static double rampT0 = 0.0;
                static bool rampOn = false, rampReleased = false;
                const unsigned seq = vr::control::g_synthRampSeq;
                if (seq != seenRamp) {
                    seenRamp = seq;
                    rampT0 = ProfMs();
                    rampOn = true;
                    rampReleased = false;
                }
                if (rampOn) {
                    const double v1 = vr::control::g_synthRamp[3], t1 = vr::control::g_synthRamp[4] * 0.001,
                                 v2 = vr::control::g_synthRamp[5], t2 = vr::control::g_synthRamp[6] * 0.001;
                    const bool rel = vr::control::g_synthRampRelease;
                    const double e = (ProfMs() - rampT0) * 0.001, T = t1 + t2, tail = rel ? 0.05 : 0.0;
                    const double te = e < T ? e : T, tt = e < T ? 0.0 : (e < T + tail ? e - T : tail);
                    double dist = 0.0;  // cm
                    if (te <= t1) {
                        dist = t1 > 0.0 ? v1 * te * te / (2.0 * t1) : 0.0;
                    } else {
                        const double u = te - t1;
                        dist = v1 * t1 * 0.5 + v1 * u + (t2 > 0.0 ? (v2 - v1) * u * u / (2.0 * t2) : 0.0);
                    }
                    dist += v2 * tt;
                    for (int i = 0; i < 3; ++i) left[i] += static_cast<float>(vr::control::g_synthRamp[i] * dist * 0.01) * vr::control::g_posScale;
                    if (e >= T && rel && !rampReleased) {
                        rampReleased = true;
                        vr::control::g_gripSim[1] = false;
                    }
                    if (e >= T + tail) {
                        for (int i = 0; i < 3; ++i)
                            vr::control::g_synthLeftCm[i] = vr::control::g_synthLeftCm[i] + static_cast<float>(vr::control::g_synthRamp[i] * dist);
                        rampOn = false;
                    }
                }
            }
            for (int i = 0; i < 3; ++i) g_leftOffset[i] = left[i];
            // synthleftrotvel: rotazione continua (gradi/s); alla fine resta in g_synthLeftRotDeg.
            float rotAdd[3] = {0.0f, 0.0f, 0.0f};
            ULONGLONG r0 = vr::control::g_synthLeftRotVelStart, r1 = vr::control::g_synthLeftRotVelUntil;
            if (r1 > r0) {
                float el = static_cast<float>((nowMs < r1 ? nowMs : r1) - r0) * 0.001f;
                bool done = nowMs >= r1;
                if (vr::control::g_synthSmooth) done = smoothElapsed(r0, r1, &seenRot, &qpcRot, &el);
                for (int i = 0; i < 3; ++i) rotAdd[i] = vr::control::g_synthLeftRotVelDeg[i] * el;
                if (done) {
                    for (int i = 0; i < 3; ++i) {
                        vr::control::g_synthLeftRotDeg[i] = vr::control::g_synthLeftRotDeg[i] + rotAdd[i];
                        rotAdd[i] = 0.0f;
                    }
                    vr::control::g_synthLeftRotVelUntil = 0;
                    vr::control::g_synthLeftRotVelStart = 0;
                }
            }
            g_leftPitch = RadiansToUnreal((vr::control::g_synthLeftRotDeg[0] + rotAdd[0]) * kDegToRad);
            g_leftYaw = RadiansToUnreal((vr::control::g_synthLeftRotDeg[1] + rotAdd[1]) * kDegToRad);
            g_leftRoll = RadiansToUnreal((vr::control::g_synthLeftRotDeg[2] + rotAdd[2]) * kDegToRad);
            // synthleftrot e' gia' la direzione della mano: punta dove e' girata (come prima del WI-4a).
            g_leftAimPitch = g_leftPitch.load();
            g_leftAimYaw = g_leftYaw.load();
            if (vr::control::g_synthCtrlOn[1]) ApplyLeftControllerAngles(SynthControllerMatrix(1));  // "synthctrl left"
            // synthleftlost: buco del tracciamento finto (collaudi della voce 8).
            if (haveLeft && GetTickCount64() < vr::control::g_synthLeftLostUntil.load()) haveLeft = false;
            g_leftValid = haveLeft;
            // Presa a due mani (WI-4b) con le pose finte: lo stesso risolutore del
            // visore. Spazio del tracciamento finto (metri, origine nel centro
            // degli occhi) dal riferimento del corpo con lo yaw di riferimento,
            // come le matrici di synthctrl; senza synthctrl il telaio dell'arma e'
            // synthaim (rollio 0) col polso in synthhand.
            {
                const float sy = std::sin(g_hmdYawReference), cy = std::cos(g_hmdYawReference), sc = vr::control::g_posScale;
                auto toTrack = [&](const float* units, float out[3]) {
                    const float f = units[0] / sc, r = units[1] / sc, u = units[2] / sc;
                    out[0] = f * sy + r * cy;
                    out[1] = u;
                    out[2] = -f * cy + r * sy;
                };
                float handUnits[3], rawPos[3];
                for (int i = 0; i < 3; ++i) handUnits[i] = vr::control::g_synthHandCm[i] * 0.01f * sc;
                toTrack(handUnits, rawPos);
                float gun[3][4], rightRaw[3][4];
                const vr::pure::GunFrameParams gp = vr::control::GunFrameFor(vr::control::PhysicalHand(0), true);
                if (vr::control::g_synthCtrlOn[0]) {
                    const vr::HmdMatrix34_t rm = SynthControllerMatrix(0);
                    for (int i = 0; i < 3; ++i) {
                        for (int j = 0; j < 3; ++j) {
                            rightRaw[i][j] = rm.m[i][j];
                            gun[i][j] = g_gunFrame[i][j];
                        }
                        rightRaw[i][3] = rawPos[i];
                        gun[i][3] = rawPos[i] + g_gunFrame[i][3];
                    }
                } else {
                    vr::pure::PoseFromEuler(vr::control::g_synthAimPitchDeg * kDegToRad,
                                            g_hmdYawReference + vr::control::g_synthAimYawDeg * kDegToRad, 0.0f, gun);
                    float wristPos[3];
                    float wu[3] = {g_handOffset[0].load(), g_handOffset[1].load(), g_handOffset[2].load()};
                    toTrack(wu, wristPos);
                    for (int i = 0; i < 3; ++i) gun[i][3] = wristPos[i];
                    RawFromGunFrame(gun, gp, rightRaw);
                }
                float leftRaw[3][4] = {};
                if (vr::control::g_synthCtrlOn[1]) {
                    const vr::HmdMatrix34_t lmS = SynthControllerMatrix(1);
                    std::memcpy(leftRaw, lmS.m, sizeof(leftRaw));
                } else {
                    vr::pure::PoseFromEuler((vr::control::g_synthLeftRotDeg[0] + rotAdd[0]) * kDegToRad,
                                            g_hmdYawReference + (vr::control::g_synthLeftRotDeg[1] + rotAdd[1]) * kDegToRad,
                                            (vr::control::g_synthLeftRotDeg[2] + rotAdd[2]) * kDegToRad, leftRaw);
                }
                float leftPos[3];
                toTrack(left, leftPos);
                for (int i = 0; i < 3; ++i) leftRaw[i][3] = leftPos[i];
                static const float zeroRef[3] = {0.0f, 0.0f, 0.0f};
                float tg[3][4];
                if (TwoHandStep(gun, rightRaw, haveLeft, leftRaw, zeroRef, tg)) {
                    ApplyGunFrame(tg);
                    const float* w = g_gunWristPos;
                    g_handOffset[0] = (w[0] * sy - w[2] * cy) * sc;
                    g_handOffset[1] = (w[0] * cy + w[2] * sy) * sc;
                    g_handOffset[2] = w[1] * sc;
                }
                TurretGripStep(gun, rightRaw, haveLeft, leftRaw);  // torretta, presa NORMAL: manubrio (WI-4c)
            }
        }
        {
            // "synthcomp on" (collaudi del peso, 25/10): come la compensazione
            // della copertura e della torretta qui sotto, ogni rotazione del
            // corpo sposta della stessa quantita' un riferimento finto della
            // testa: vista e mani restano ferme nel mondo mentre il corpo gira
            // (g_trackRefYaw lo dice a props.cpp, che non deve vederci un salto).
            static int32_t synthRef = 0, lastBody = 0;
            static bool haveBody = false;
            const int32_t bodyNow = g_bodyYaw.load();
            if (!vr::control::g_synthComp) synthRef = 0;
            else if (haveBody) synthRef += static_cast<int16_t>(bodyNow - lastBody);
            lastBody = bodyNow;
            haveBody = true;
            if (synthRef) {
                const float a = -synthRef * (kTwoPi / 65536.0f), ca = std::cos(a), sa = std::sin(a);
                for (std::atomic<float>* o : {g_handOffset, g_leftOffset}) {
                    const float f = o[0].load(), r = o[1].load();
                    o[0] = f * ca - r * sa;
                    o[1] = f * sa + r * ca;
                }
                g_deltaYaw = g_deltaYaw.load() - synthRef;
                g_aimDeltaYaw = g_aimDeltaYaw.load() - synthRef;
                g_leftYaw = g_leftYaw.load() - synthRef;
                g_leftAimYaw = g_leftAimYaw.load() - synthRef;
                g_trackRefYaw = synthRef;  // gia' in unita' del corpo
            }
        }
        g_headHeadingYaw = g_deltaYaw.load();  // pose finte: niente rollio, direzione = vista
        // Torretta: con le pose finte la vista la tiene ferma "synthcomp" (ogni
        // rotazione del corpo); qui si conta solo il passo (riga TURRET).
        (void)TurretCompensation();
        g_bodyYawAtHeadSample = g_bodyYaw.load();  // diagnostica: anticipo della vista (riga TURRET, VIEW bodyhs)
        {
            // Aiuto alle esecuzioni anche con le pose finte (test senza visore).
            uint32_t ctrlPawn = 0;
            void* pc = LivePlayerController();
            if (pc && TrySafeReadUint32(static_cast<char*>(pc) + 0x1F8, &ctrlPawn) && ctrlPawn)
                ExecutionAssist(pc, reinterpret_cast<void*>(ctrlPawn));
        }
        TurretAimStep();  // come col visore (25/10: prima la torretta ignorava le pose finte)
        if (shouldLog) DumpCamera(camera);
        return;
    }

    // kModeHmd
    vr::session::Shared vrLock(true);  // durante un cambio di tipo: questo tick senza testa
    vr::IVRSystem* const vrsys = vrLock.ok() ? vr::rt::System() : nullptr;
    if (!vr::session::Ready() || !vrsys) {
        static ULONGLONG lastNotReady = 0;
        if (GetTickCount64() - lastNotReady > 60000) {
            lastNotReady = GetTickCount64();
            vr::log::Line("UpdateHeadTracking: OpenVR not ready (logged once a minute).");
        }
        vr::control::g_coverPeek = false;  // niente LT bloccato senza posa (L6)
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
        vr::control::g_coverPeek = false;  // niente LT bloccato senza posa (L6)
        return;
    }
    // Riferimenti della testa: posizione (x, z: ricentramento; y: massimo per
    // le soglie di accucciata e sporgenza) e riferimento verticale stabile
    // della vista (WI-2, metri): fissato al ricentramento voluto, all'inizio
    // della scena (visore indossato) e dalla calibrazione.
    static float posRef[3] = {};
    static float viewRefY = 0.0f;
    // Seduti riconosciuti (WI-3, "seated auto"): vr::pure::SeatedDetector.
    static vr::pure::SeatedDetector seatDet;
    // Fix 2 (review media): dopo il passaggio seduti -> in piedi per il controllo
    // di salita, il ritorno a sedere si riconosce (vr::pure::SeatedReturnCheck).
    static vr::pure::SeatedReturnCheck seatReturn;
    static bool standCheckMeasuring = false;  // misura in corso partita dal controllo di salita
    // Ricentramenti del runtime (WI-2, bug 3): prima ogni evento "zero pose /
    // area di gioco cambiata" (VD ne manda a raffica, Steam Link non si sa)
    // rifaceva il ricentramento: yaw di riferimento e posizione sulla testa di
    // adesso, quindi la vista scattava (fino a 45 gradi e 15 cm) e l'altezza
    // cambiava. Ora si guarda la posa (senza predizione: vr::pure::HeadJumpDetector):
    //  - il tick del salto la vista e' tenuta (posa riportata nello spazio di
    //    prima), riferimenti intatti;
    //  - il tick dopo: se la posa resta nello spazio nuovo (posa e velocita'
    //    continue), il salto si toglie dai riferimenti (vista ferma, altezza mai
    //    toccata) e lo conferma l'evento entro 1 s (o arrivato fino a 0,5 s
    //    prima: OpenXR); se no era un campione sbagliato o la testa vera e non
    //    si tocca nulla (review WI-2: prima un campione sbagliato restava 1 s);
    //  - senza evento entro 1 s il salto si rimette sotto un battito di ciglia
    //    (prima di scatto, mani e oggetti tenuti compresi);
    //  - un evento senza salto non fa nulla.
    // OpenXR: con poseInPreviousSpace la trasformazione esatta sostituisce
    // quella misurata.
    static vr::pure::HeadJumpDetector jumpDet;
    static vr::pure::SpaceShift jumpPending;
    static double jumpPendingSince = -1.0, rtEventT = -1.0e9;
    static uint32_t rtEventsSeen = 0;
    static bool rtEventOpen = false, putBackArmed = false;
    // Yaw dei salti tolti dai riferimenti: la vignettatura usa lo yaw della
    // testa rispetto a loro (prima ogni salto tolto la accendeva per un tick).
    static float spaceYawAcc = 0.0f;
    // Review WI-2 round 2: nel tick tenuto solo la testa tornava nello spazio
    // di prima, mentre i controller erano gia' in quello nuovo e i riferimenti
    // ancora vecchi: a ogni ricentramento vero del runtime mani e mira
    // saltavano per un tick (30-40 cm, canna 40 gradi; a due mani lo
    // stiramento del filtro, sulla torretta un passo fino a ~20 gradi; props
    // vedeva un salto delle mani). Ora:
    //  - i controller saltati con lo spazio (riportati indietro tornano vicini
    //    alla posa del tick prima: vr::pure::PoseFollowsShift) tornano anche
    //    loro nello spazio di prima per questo tick; quelli fermi (salto solo
    //    del visore) restano come sono;
    //  - posa dichiarata al compositor (submitpose): quella grezza, nello
    //    spazio del runtime, se i controller dicono che lo spazio e' cambiato
    //    (prima quella tenuta: con un ricentramento vero il compositor la
    //    leggeva nello spazio nuovo, 40 gradi di riproiezione per un frame);
    //  - riferimento delle mani per props.cpp (g_trackRefYaw /
    //    g_trackOriginOff) nel primo spazio (spaceAbsorbed: i salti assorbiti):
    //    un ricentramento assorbito non e' un salto delle mani (prima props
    //    portava gli oggetti tenuti di 40 gradi attorno all'origine mentre le
    //    mani restavano ferme); un salto rimesso (sotto il battito) si', come
    //    le mani.
    // "recenterhands 0": come prima (collaudi).
    static vr::pure::SpaceShift spaceAbsorbed;
    static float prevCtrl[vr::k_unMaxTrackedDeviceCount][3][4];
    static bool prevCtrlValid[vr::k_unMaxTrackedDeviceCount] = {};
    static int heldHandsMoved = 0, heldHandsStill = 0;
    const bool handsFix = vr::control::g_recenterHandsFix.load();
    vr::HmdMatrix34_t mHold = hmdPose.mDeviceToAbsoluteTracking;
    bool declareRaw = false;
    {
        const double nowJ = ProfMs() * 0.001;
        const uint32_t ev = vr::control::g_runtimeRecenterEvents.load();
        if (ev != rtEventsSeen) {
            rtEventsSeen = ev;
            rtEventT = nowJ;
            rtEventOpen = true;
        }
        // Posa del visore senza predizione: con 80 ms di predizione (VDXR) e un
        // tick lungo l'errore dell'estrapolazione di una testa che gira in
        // fretta sembrava un salto (review WI-2).
        vr::HmdMatrix34_t m0 = hmdPose.mDeviceToAbsoluteTracking;
        if (predict > 0.0f) {
            if (handSec <= 0.0f && handPoses[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid) {
                m0 = handPoses[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking;
            } else {
                vr::TrackedDevicePose_t zp = {};
                vrsys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.0f, &zp, 1);
                if (zp.bPoseIsValid) m0 = zp.mDeviceToAbsoluteTracking;
            }
        }
        float r0[3][3];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) r0[i][j] = m0.m[i][j];
        static float heldPos = 0.0f, heldAng = 0.0f;
        static uint32_t rejectedJumps = 0;
        static ULONGLONG lastRejectLog = 0;
        vr::pure::SpaceShift js;
        const int jr = jumpDet.Step(nowJ, r0, m0.m[0][3], m0.m[2][3], &js);
        const bool holdTick = jr == vr::pure::HeadJumpDetector::kHold;
        if (holdTick) {
            vr::pure::ShiftPose(js.Inverse(), mHold.m);  // questo tick: vista tenuta
            heldPos = jumpDet.lastPos;
            heldAng = jumpDet.lastAngDeg;
            heldHandsMoved = heldHandsStill = 0;
        }
        {
            // Controller dell'arma e di supporto: pose grezze del tick prima e,
            // nel tick tenuto, ritorno nello spazio di prima (vedi sopra).
            const vr::TrackedDeviceIndex_t roles[2] = {vrsys->GetTrackedDeviceIndexForControllerRole(vr::control::GunRole()),
                                                       vrsys->GetTrackedDeviceIndexForControllerRole(vr::control::SupportRole())};
            for (int k = 0; k < 2; ++k) {
                const vr::TrackedDeviceIndex_t d = roles[k];
                if (d == vr::k_unTrackedDeviceIndex_Hmd || d >= vr::k_unMaxTrackedDeviceCount || (k == 1 && d == roles[0])) continue;
                vr::HmdMatrix34_t& hm = handPoses[d].mDeviceToAbsoluteTracking;
                if (!handPoses[d].bPoseIsValid) {
                    prevCtrlValid[d] = false;
                    continue;
                }
                bool back = false;
                if (holdTick && handsFix && prevCtrlValid[d]) {
                    back = vr::pure::PoseFollowsShift(hm.m, prevCtrl[d], js);
                    ++(back ? heldHandsMoved : heldHandsStill);
                }
                std::memcpy(prevCtrl[d], hm.m, sizeof(prevCtrl[d]));
                prevCtrlValid[d] = true;
                if (back) vr::pure::ShiftPose(js.Inverse(), hm.m);
            }
            declareRaw = holdTick && handsFix && heldHandsMoved > 0 && heldHandsStill == 0;
        }
        if (jr == vr::pure::HeadJumpDetector::kConfirm || jr == vr::pure::HeadJumpDetector::kUnsure) {
            // kUnsure (review WI-2 round 2): tick dopo il salto oltre 150 ms,
            // le ipotesi non si distinguono: tolto come un ricentramento (la
            // vista non scatta), tenuto con l'evento del runtime entro 1 s,
            // altrimenti rimesso sotto un battito di ciglia come ogni salto
            // senza evento. Prima restava tutto: 40 gradi di scatto.
            vr::pure::RebaseReferences(js, &g_hmdYawReference, &posRef[0], &posRef[2]);
            spaceYawAcc = vr::pure::WrapPi(spaceYawAcc + js.yaw);
            spaceAbsorbed = vr::pure::SpaceShift::Then(spaceAbsorbed, js);
            // Stato tenuto nello spazio del tracciamento (review WI-4b round 1,
            // H2): filtro della linea tra i palmi e ultima rotazione della mano
            // dell'arma (presa a due mani), yaw filtrato del manubrio (WI-4c).
            // Le pose del tick tenuto erano nello spazio di prima (sopra), da
            // qui sono nel nuovo: lo stato gira con loro. Prima il filtro
            // inseguiva il salto (canna 19,3 gradi al primo tick con 40 gradi) e
            // lo scarto rimasto calava di 0,5 x 40 gradi in un tick.
            // Review round 2 (basso): solo se nel tick tenuto i controller sono
            // saltati con lo spazio (stessa prova di declareRaw). Un salto del
            // solo visore preso per un ricentramento lascia le mani dove sono:
            // girare lo stato sarebbe lo stesso transitorio del filtro, al contrario.
            if (handsFix && vr::control::g_twoHandShiftFix.load() && heldHandsMoved > 0 && heldHandsStill == 0) {
                g_thSolver.ShiftSpace(js.yaw);
                g_turretBar.ShiftYaw(js.yaw);
                g_turretHandles.Reanchor(js.yaw);  // perno di nuovo davanti alle mani, nel nuovo spazio
            }
            jumpPending = jumpPendingSince < 0.0 ? js : vr::pure::SpaceShift::Then(jumpPending, js);
            if (jumpPendingSince < 0.0) jumpPendingSince = nowJ;
            putBackArmed = false;
            char b[320];
            if (jr == vr::pure::HeadJumpDetector::kConfirm)
                std::snprintf(b, sizeof(b),
                              "RECENTER: headset pose jumped %.1f cm / %.1f deg in one tick and stayed in the new space: view kept "
                              "still (controllers in the held tick: %d moved with the space, %d did not).",
                              heldPos * 100.0f, heldAng, heldHandsMoved, heldHandsStill);
            else
                std::snprintf(b, sizeof(b),
                              "RECENTER: headset pose jumped %.1f cm / %.1f deg, next tick %.0f ms later (too late to tell): taken off "
                              "like a recenter, kept with the runtime event, else put back under a blink (controllers in the held "
                              "tick: %d moved with the space, %d did not).",
                              heldPos * 100.0f, heldAng, jumpDet.lastGapMs, heldHandsMoved, heldHandsStill);
            vr::log::Line(b);
        } else if (jr == vr::pure::HeadJumpDetector::kRejected || jumpDet.lastWhy == vr::pure::HeadJumpDetector::kWhyTilted) {
            ++rejectedJumps;
            if (GetTickCount64() - lastRejectLog > 2000) {
                lastRejectLog = GetTickCount64();
                const int why = jumpDet.lastWhy;
                char b[260];
                std::snprintf(b, sizeof(b),
                              "RECENTER: one-tick headset pose jump not taken as a recenter (%s; errors new space %.2f, old space %.2f, "
                              "head motion %.2f): nothing changed (%u so far).",
                              why == vr::pure::HeadJumpDetector::kWhyGlitch    ? "bad or repeated sample"
                              : why == vr::pure::HeadJumpDetector::kWhyMotion  ? "real head motion"
                              : why == vr::pure::HeadJumpDetector::kWhyTilted  ? "tilted, not a space turn"
                                                                               : "unclear",
                              jumpDet.lastErr[0] > 1e8f ? -1.0f : jumpDet.lastErr[0], jumpDet.lastErr[1], jumpDet.lastErr[2], rejectedJumps);
                vr::log::Line(b);
            }
        }
        if (jumpPendingSince >= 0.0 && rtEventT >= jumpPendingSince - 0.5) {
            float xm[3][4];
            const bool exact = vr::rt::Active() == vr::rt::Backend::OpenXR && vr::xr::TakeSpaceChange(xm);
            if (exact) {
                const vr::pure::SpaceShift fix =
                    vr::pure::SpaceShift::Then(jumpPending.Inverse(), vr::pure::ShiftFromPreviousSpacePose(xm));
                vr::pure::RebaseReferences(fix, &g_hmdYawReference, &posRef[0], &posRef[2]);
                spaceYawAcc = vr::pure::WrapPi(spaceYawAcc + fix.yaw);
                spaceAbsorbed = vr::pure::SpaceShift::Then(spaceAbsorbed, fix);
            }
            char b[220];
            std::snprintf(b, sizeof(b),
                          "RECENTER: runtime recenter absorbed (space turned %.1f deg, moved %.1f cm%s): view, direction and height "
                          "unchanged.",
                          jumpPending.yaw * 57.2957795f, std::sqrt(jumpPending.tx * jumpPending.tx + jumpPending.tz * jumpPending.tz) * 100.0f,
                          exact ? ", OpenXR poseInPreviousSpace" : "");
            vr::log::Line(b);
            jumpPendingSince = -1.0;
            putBackArmed = false;
            rtEventOpen = false;
        } else if (putBackArmed) {
            // Battito chiesto il tick prima: il nero c'e' gia'.
            vr::pure::RebaseReferences(jumpPending.Inverse(), &g_hmdYawReference, &posRef[0], &posRef[2]);
            spaceYawAcc = vr::pure::WrapPi(spaceYawAcc - jumpPending.yaw);
            vr::log::Line("RECENTER: headset pose jump without a runtime recenter event: shown as it is (tracking), under a blink.");
            jumpPendingSince = -1.0;
            putBackArmed = false;
        } else if (jumpPendingSince >= 0.0 && nowJ - jumpPendingSince > 1.0) {
            if (vr::control::g_fades) vr::output::Blink(0.3f);
            putBackArmed = true;  // rimesso al tick dopo, sotto il nero
        }
        if (rtEventOpen && jumpPendingSince < 0.0 && nowJ - rtEventT > 0.5) {
            vr::log::Line("RECENTER: runtime recenter event without a pose jump: ignored.");
            rtEventOpen = false;
        }
    }
    const vr::HmdMatrix34_t& m = mHold;
    // Posa dichiarata (submitpose, riga VIEW hmdq): nello spazio del runtime (vedi sopra).
    const vr::HmdMatrix34_t mDeclared = declareRaw ? hmdPose.mDeviceToAbsoluteTracking : mHold;
    g_tickHmdPose = mDeclared;
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
    // Due yaw della testa (WI-1, bug 2 del tester): hmdYaw = direzione stabile
    // (YawFromOpenVrPose, avanti miscelato con l'asse "su": non salta guardando
    // quasi in verticale) per ricentramento, corpo che segue la testa, verso
    // del movimento, posizione del corpo e aiuto alle esecuzioni; hmdYawView =
    // yaw esatto per la vista. Prima la vista usava la direzione: con la testa
    // inclinata di lato guardando in basso il mondo girava (4,5 gradi a pitch
    // -30 / rollio 10), e nessuna riproiezione lo toglie.
    float hmdYaw = YawFromOpenVrPose(m);
    float hmdYawView = vr::pure::ExactEuler(m.m).yaw;
    float hmdPitch = PitchFromOpenVrPose(m);
    // Vignettatura: lo stesso yaw della vista (se no scattava col rollio della
    // testa), senza i salti dello spazio tolti dai riferimenti.
    g_hmdYawRaw = vr::pure::WrapPi(hmdYawView - spaceYawAcc);
    // OpenVR: x destra, y su, -z avanti; posizione in metri nella colonna 3.
    float pos[3] = {m.m[0][3], m.m[1][3], m.m[2][3]};
    if (vr::control::g_recenterRequested.exchange(false)) {
        // Solo direzione e posizione orizzontale: l'orizzonte resta dritto
        // (17/10: un ricentramento fatto guardando in basso inclinava tutta la
        // vista di 50 gradi) e l'altezza di riferimento e' quella in piedi.
        g_hmdYawReference = hmdYaw;
        g_hmdPitchReference = 0.0f;
        for (int i = 0; i < 3; ++i) posRef[i] = pos[i];
        viewRefY = pos[1];
        jumpPendingSince = -1.0;  // i riferimenti ripartono da qui
        g_lastRecenterMs = ProfMs();
        seatDet.Start();  // WI-3: "seated auto" misura da qui (inizio della scena o ricentramento voluto)
        seatReturn.Disarm();
        standCheckMeasuring = false;
        putBackArmed = false;
        vr::control::g_cinemaRecenter = true;
        if (!vr::control::g_recenterSilent.exchange(true)) {  // solo il ricentramento voluto (stick, comando)
            g_blinkMs = GetTickCount64();
            vr::toast::Notify("Recentered");
        }
        vr::log::Line("Recentered: hmdYaw=" + std::to_string(hmdYaw) + " hmdPitch=" + std::to_string(hmdPitch) +
                      " pos=(" + std::to_string(pos[0]) + ", " + std::to_string(pos[1]) + ", " + std::to_string(pos[2]) + ")");
    }
    // Fine dello schermo virtuale (scena, filmato, caricamento): sotto il nero
    // di assestamento la direzione e la posizione della testa diventano il
    // nuovo riferimento, cosi' dopo la scena si guarda dove guarda Walker e
    // non si riparte spostati nella stanza ("cinemaalign off" per lasciare
    // tutto com'era; R-B5).
    {
        static uint64_t aligned = 0;
        uint64_t end = g_cinemaEndMs.load();
        if (vr::control::g_cinemaAlign && end && end != aligned && GetTickCount64() - end < 600) {
            aligned = end;
            g_hmdYawReference = hmdYaw;
            posRef[0] = pos[0];
            posRef[2] = pos[2];
            jumpPendingSince = -1.0;
            putBackArmed = false;
            vr::log::Line("Recentered after the cinema screen (cinemaalign).");
        }
    }
    // Il corpo gira attorno al centro di Walker, gli occhi stanno fwd unita'
    // davanti (piu' lo spostamento della testa): girando, gli occhi
    // scivolerebbero di lato. Si sposta il riferimento della posizione della
    // stessa quantita' perche' restino dove sono (perno sulla testa).
    //   dUnits: rotazione del corpo appena applicata (unita' Unreal);
    //   fwd: avanzamento davvero applicato (0 sui mezzi; 0 in copertura, dove
    //        il gioco tiene il Pawn girato verso il riparo e ruota solo la vista);
    //   headRotates: con lo stick lo spostamento della testa gira col corpo
    //        (il riferimento non cambia), con la testa no (riferimento gia'
    //        spostato di altrettanto: il vettore nel mondo resta uguale).
    // Va chiamata con il riferimento (g_hmdYawReference) gia' aggiornato.
    auto compensatePivot = [&](int32_t dUnits, float fwd, bool headRotates) {
        float scale = vr::control::g_posScale;
        if (!dUnits || !vr::control::g_sixDof || scale <= 0.0f) return;
        float a = dUnits * (6.28318530718f / 65536.0f);
        float hf = headRotates ? g_headOffset[0].load() : 0.0f, hr = headRotates ? g_headOffset[1].load() : 0.0f;
        float vf = fwd + hf, vrr = hr;
        float nf = vf * std::cos(a) + vrr * std::sin(a), nr = -vf * std::sin(a) + vrr * std::cos(a);
        float df = (nf - fwd) - hf, dr = nr - hr;
        float sy = std::sin(g_hmdYawReference), cy = std::cos(g_hmdYawReference);
        posRef[0] -= (df * sy + dr * cy) / scale;
        posRef[2] -= (-df * cy + dr * sy) / scale;
        // Lo scostamento della testa cambia di (df, dr) nel riferimento del corpo (gia' girato):
        // spostamento fatto dalla mod, non dalla testa (vignettatura, review del follow-up).
        const float b = g_bodyYaw.load() * (6.28318530718f / 65536.0f);
        g_headShiftWorld[0] += df * std::cos(b) - dr * std::sin(b);
        g_headShiftWorld[1] += df * std::sin(b) + dr * std::cos(b);
    };
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
        // Seduti ("seated on"): il riferimento e' l'altezza della testa al
        // ricentramento, non la massima ne' quella fissa in piedi. Prima con
        // "height 1.55" e la testa seduta a 1,2 m la vista scendeva di 35 cm
        // sotto gli occhi di Walker e il corpo con lei (gambe nel pavimento,
        // braccia stirate verso i controller: "nano deforme", 21/10).
        bool seated = vr::control::g_seated;
        if (seated) {
        } else if (pos[1] > posRef[1] && !spike) posRef[1] = pos[1];
        else if (pos[1] < posRef[1] && posRef[1] - pos[1] < 0.15f && dtR > 0.0f && dtR < 0.5f)
            posRef[1] = (std::max)(pos[1], posRef[1] - 0.01f * dtR);
        // Calibrazione (entrambi gli stick 3 s, in piedi): l'altezza di adesso
        // e' quella in piedi, salvata come "height <m>" (R-B6); con un valore
        // fisso il riferimento non insegue piu' il massimo.
        if (vr::control::g_heightCalibRequested.exchange(false) && pos[1] > 0.5f) {
            char v[16];
            std::snprintf(v, sizeof(v), "%.2f", pos[1]);
            vr::control::g_heightFixedM = pos[1];
            vr::control::SaveIniValue("height", v);
            vr::toast::Notify(std::string("Standing height ") + v + " m saved");
            vr::log::Line(std::string("HEIGHT: standing height set to ") + v + " m from the controllers and saved.");
        }
        float fixedM = vr::control::g_heightFixedM.load();
        if (fixedM > 0.0f && !seated) posRef[1] = fixedM;
        // Riferimento verticale della vista (WI-2): fisso (ricentramento,
        // inizio scena, calibrazione); seduti o con l'altezza fissata e' lo
        // stesso di posRef. Unica eccezione: preso troppo in basso (visore
        // indossato da seduti o chinati) e poi in piedi: una sola ripresa
        // (vr::pure::ViewHeightRecapture: testa oltre 15 cm sopra per 3 s,
        // mediana degli ultimi 1,5 s), applicata sotto un battito di ciglia se
        // cambia la vista. Review WI-2: prima il riferimento saliva di meta' di
        // ogni aumento per tick e mai in giu', quindi saliva da solo col rumore
        // del tracciamento (6 unita' in 30 s) e i seduti che si alzavano un
        // attimo perdevano altezza per sempre. Chinarsi o alzarsi per meno di 3 s
        // non lo cambia mai (prima il massimo e la discesa di 1 cm/s spostavano
        // la vista da sola: 4,5 unita' in 30 s).
        static vr::pure::ViewHeightRecapture recap;
        static float recapPending = 0.0f;
        if (seated || fixedM > 0.0f || viewRefY <= 0.5f) {
            viewRefY = posRef[1];
            recap.Reset();
            recapPending = 0.0f;
        } else {
            const float s = vr::control::g_posScale.load() > 0.0f ? vr::control::g_posScale.load() : 50.0f;
            if (recapPending > 0.0f) {  // battito chiesto il tick prima: il nero c'e' gia'
                char b[160];
                std::snprintf(b, sizeof(b), "HEIGHT: view reference re-captured %.3f -> %.3f m (head stayed higher for 3 s), under a blink.",
                              viewRefY, recapPending);
                vr::log::Line(b);
                viewRefY = recapPending;
                recapPending = 0.0f;
            }
            const float nr = recap.Step(nowR, pos[1], viewRefY);
            if (nr > 0.0f) {
                // Cambia la vista solo col riferimento limitato (sotto ~1,5 m).
                const float dv = vr::pure::StableEyeHeight(nr, posRef[1], s, kEyeHeightMin, kEyeHeightMax) -
                                 vr::pure::StableEyeHeight(viewRefY, posRef[1], s, kEyeHeightMin, kEyeHeightMax);
                if (std::fabs(dv) < 0.5f) {
                    viewRefY = nr;
                } else {
                    if (vr::control::g_fades) vr::output::Blink(0.3f);
                    recapPending = nr;
                }
            }
        }
    }
    // "seated auto" (WI-3, bug 5): seduti riconosciuti dalla mediana dell'altezza
    // della testa per 3 s col visore indossato, dopo l'inizio della scena, ogni
    // ricentramento voluto, il visore rimesso o "seated auto" scelto dal menu;
    // mai da solo a meta' partita. Il cambio si annuncia (avviso) e passa sotto
    // un battito di ciglia: seduti gli occhi vanno all'altezza di quelli di Walker
    // col riferimento verticale sulla testa seduta.
    {
        static bool wasPresent = false;
        const bool present = vr::session::HmdPresent();
        if ((present && !wasPresent) || vr::control::g_seatedCaptureRequested.exchange(false)) {
            seatDet.Start();
            seatReturn.Disarm();
            standCheckMeasuring = false;
        }
        wasPresent = present;
        // Fix 1 (review media): riconosciuti seduti ("seated auto") e poi in piedi
        // (avviato il gioco seduti alla scrivania): testa oltre 30 cm sopra il
        // riferimento seduto per 3 s -> si rimisura (poi blink e avviso come
        // all'avvio). Prima restavano seduti (occhi ~27 unita' sopra quelli di
        // Walker, accucciata vera spenta) fino a un ricentramento. Fix 2: salita e
        // durata dall'ini ("seatedstand", Avanzate).
        static vr::pure::SeatedStandCheck standChk;
        static float standFromRef = 0.0f;  // riferimento seduto da cui ci si e' alzati
        standChk.rise = vr::control::g_seatedStandRiseM.load();
        standChk.hold = vr::control::g_seatedStandHoldS.load();
        seatReturn.hold = standChk.hold;
        seatDet.seatedBelow = vr::control::g_seatedBelowM.load();
        const bool seatedAuto = vr::control::g_seatedMode.load() == 2 && vr::control::g_seatedDetected.load() && !seatDet.armed;
        if (standChk.Step(ProfMs() * 0.001, pos[1], posRef[1], seatedAuto && present)) {
            char b0[200];
            std::snprintf(b0, sizeof(b0), "SEATED: head at %.2f m, %.0f cm above the seated reference %.2f m for %.0f s: measuring again", pos[1],
                          (pos[1] - posRef[1]) * 100.0f, posRef[1], standChk.hold);
            vr::log::Line(b0);
            seatDet.Start();
            standCheckMeasuring = true;
            standFromRef = posRef[1];
        }
        // Fix 2 (review media): passati in piedi dal controllo qui sopra, tornando a
        // sedere (testa entro 15 cm dal vecchio riferimento seduto per la stessa
        // durata) si rimisura. Prima nulla lo rilevava: occhi ~28 unita' sotto
        // quelli di Walker (nel petto) e accucciata vera riarmata fino a un
        // ricentramento.
        const bool standingAuto = vr::control::g_seatedMode.load() == 2 && !vr::control::g_seated.load() && !seatDet.armed;
        if (seatReturn.Step(ProfMs() * 0.001, pos[1], present && standingAuto)) {
            char b0[200];
            std::snprintf(b0, sizeof(b0), "SEATED: head at %.2f m, back within %.0f cm of the seated reference %.2f m for %.0f s: measuring again",
                          pos[1], vr::pure::SeatedReturnCheck::kBack * 100.0f, seatReturn.seatedRef, seatReturn.hold);
            vr::log::Line(b0);
            seatDet.Start();
        }
        const int r = seatDet.Step(ProfMs() * 0.001, pos[1], present, vr::control::g_heightFixedM.load());
        if (r >= 0) {
            const bool was = vr::control::g_seated.load();
            vr::control::g_seatedDetected = r == 1;
            vr::control::UpdateSeatedEffective();
            const bool now = vr::control::g_seated.load();
            char b[260];
            std::snprintf(b, sizeof(b), "SEATED: head at %.2f m (median of %.0f s with the headset on) -> %s (seated %s; threshold %.2f m, margin %+.2f m)%s",
                          seatDet.lastMedian, vr::pure::SeatedDetector::kWindow, r == 1 ? "sitting" : "standing",
                          vr::control::g_seatedMode.load() == 2 ? "auto" : (vr::control::g_seatedMode.load() == 1 ? "on" : "off"),
                          seatDet.seatedBelow, seatDet.lastMedian - seatDet.seatedBelow, now != was ? ", mode changed" : "");
            vr::log::Line(b);
            const bool fromStandCheck = standCheckMeasuring;
            standCheckMeasuring = false;
            if (now) seatReturn.Disarm();
            if (now != was) {
                // Riferimento verticale sulla mediana (seduti: la testa seduta; in piedi riparte il massimo).
                posRef[1] = viewRefY = seatDet.lastMedian;
                if (vr::control::g_fades) vr::output::Blink(0.3f);
                if (!now && fromStandCheck && vr::control::g_seatedMode.load() == 2) {
                    seatReturn.Arm(standFromRef);
                    vr::toast::Notify("Standing play (you stood up) - sit down for 3 s, or recenter, to go back to seated play");
                } else {
                    vr::toast::Notify(now ? "Seated play on (sitting detected) - Basic menu: Seated play"
                                          : "Standing play (seated play off) - Basic menu: Seated play");
                }
            } else if (now && std::fabs(seatDet.lastMedian - posRef[1]) > 0.02f) {
                // Gia' seduti: il riferimento seduto e' la testa misurata ora. Prima
                // restava quello del ricentramento, anche se preso col visore tolto
                // (all'inizio della scena, sul tavolo): vista 55 cm sotto (collaudo S3).
                char r2[160];
                std::snprintf(r2, sizeof(r2), "SEATED: seated reference %.2f -> %.2f m (measured head), under a blink", posRef[1],
                              seatDet.lastMedian);
                vr::log::Line(r2);
                posRef[1] = viewRefY = seatDet.lastMedian;
                if (vr::control::g_fades) vr::output::Blink(0.3f);
            }
        }
    }
    // Altezza degli occhi per "height auto" (WI-2): dal riferimento stabile,
    // entro kEyeHeightMin..55 unita' sopra il centro del Pawn, composta con
    // posRef[1] (vr::pure::StableEyeHeight): la vista non dipende dal massimo.
    if (posRef[1] > 0.5f && viewRefY > 0.5f)
        vr::control::g_eyeHeightAuto =
            vr::pure::StableEyeHeight(viewRefY, posRef[1], vr::control::g_posScale, kEyeHeightMin, kEyeHeightMax);
    g_viewDiagRefY = viewRefY;
    // Il corpo segue la testa: l'eccesso oltre la soglia passa dalla testa al
    // corpo (rotazione del PlayerController) e il riferimento si sposta della
    // stessa quantita', cosi' la vista non cambia.
    float turnThr = vr::control::g_turnWithHeadDeg * 0.01745329252f;
    void* pcForTurn = LivePlayerController();
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
                      IsCoverLikeSpecialMove(static_cast<uint8_t>((special >> 8) & 0xFF));
        vr::control::g_inCover = inCover || specialMove;
    }
    // Rotazione con lo stick (continua o a scatti): fino a 1 m di scivolata
    // dopo un passo dal punto di ricentramento (audit 19/10). In copertura il
    // Pawn non gira (fwd 0); sui mezzi fwd e' gia' 0.
    compensatePivot(vr::control::g_stickYawUnits.exchange(0), inCover ? 0.0f : g_fwdApplied.load(), true);
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
        // Torretta (WI-1): il passo scritto al tick prima, nella parte davvero
        // eseguita dal gioco al netto della base, e' ora nel corpo (g_bodyYaw dal
        // gioco): il riferimento della testa si sposta di altrettanto nello
        // stesso tick e la vista non gira. Prima il passo entrava subito nel
        // corpo della vista e il riferimento solo al tick dopo (vista in
        // anticipo di un passo), e la parte "eseguita" comprendeva la rotazione
        // dell'elicottero.
        const int32_t turretDone = TurretCompensation();
        if (turretDone != 0) {
            g_hmdYawReference += vr::control::g_yawSign * turretDone * (6.28318530718f / 65536.0f);
            lastBodyYaw += turretDone;
        }
        // Dopo una cinematica il gioco rimette Walker nella sua direzione
        // (anche 90 gradi): quella rotazione e' voluta, non si compensa.
        static ULONGLONG cinemaSeen = 0;
        if (vr::control::g_cinemaActive || g_cinematicScene) cinemaSeen = GetTickCount64();
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
            // Seduti no (WI-3): il riferimento e' la testa seduta, e con 25 cm sotto
            // si era subito "sporti" (LT tenuto: Walker usciva dal riparo appena entrato).
            if (!vr::control::g_seated && posRef[1] > 0.5f && entry[1] > posRef[1] - 0.25f) entry[1] = posRef[1] - 0.25f;
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
    // Corpo che torna sotto la testa (follow-up 27/10, sotto): stato tra i tick.
    static vr::pure::BodyFollow bodyAlign;
    static float bodyAlignCarry = 0.0f, bodyAlignDone = 0.0f;  // unita' non ancora scritte; gradi girati
    static double bodyAlignStartT = 0.0;
    // Review 2 del follow-up: righe BODY limitate (prima 2 per allineamento, ~0,3-1 al secondo, piu' le pause):
    // una riga per allineamento, alla fine, per i primi kAlignLines; poi un riassunto ogni 30 s.
    static float bodyAlignStartOff = 0.0f;
    static bool bodyAlignPausedTurn = false;  // arma puntata durante questo allineamento
    struct AlignStats {
        int n = 0, stopped = 0, paused = 0;
        float deg = 0.0f, longDeg = 0.0f, longS = 0.0f, wallS = 0.0f;
    };
    static AlignStats alignWin;
    static int alignLines = 0;
    static double alignWinT = 0.0;
    constexpr int kAlignLines = 20;
    auto alignEnded = [&](double now, const char* why, float left) {
        const float dur = static_cast<float>(now - bodyAlignStartT);
        ++alignWin.n;
        if (*why) ++alignWin.stopped;
        if (bodyAlignPausedTurn) ++alignWin.paused;
        alignWin.deg += std::fabs(bodyAlignDone);
        if (std::fabs(bodyAlignDone) > alignWin.longDeg) {
            alignWin.longDeg = std::fabs(bodyAlignDone);
            alignWin.longS = dur;
        }
        if (alignLines >= kAlignLines) return;
        ++alignLines;
        char b[260];
        std::snprintf(b, sizeof(b), "BODY: %s under the head: %+.1f deg in %.2f s (head %+.1f deg off at the start, %+.2f left%s)%s%s%s",
                      *why ? "turn stopped" : "turned", bodyAlignDone, dur, bodyAlignStartOff, left,
                      bodyAlignPausedTurn ? "; paused by the gun" : "", *why ? ": " : ".", why, *why ? "." : "");
        vr::log::Line(b);
        if (alignLines == kAlignLines) vr::log::Line("BODY: from now on the turns under the head are only in a summary every 30 s.");
    };
    {
        const double nowS = ProfMs() * 0.001;
        if (alignWinT <= 0.0) alignWinT = nowS;
        if (nowS - alignWinT >= 30.0) {
            if (alignLines >= kAlignLines && (alignWin.n > 0 || alignWin.wallS > 0.05f)) {
                char b[300];
                std::snprintf(b, sizeof(b),
                              "BODY: turns under the head in the last %.0f s: %d (%d stopped, %d paused by the gun), %.0f deg in all, longest %.1f deg "
                              "in %.2f s; held off by a wall or low cover in front for %.1f s.",
                              nowS - alignWinT, alignWin.n, alignWin.stopped, alignWin.paused, alignWin.deg, alignWin.longDeg, alignWin.longS,
                              alignWin.wallS);
                vr::log::Line(b);
            }
            alignWin = AlignStats();
            alignWinT = nowS;
        }
    }
    // Non sulle torrette: li' il PlayerController segue il controller (sotto) e
    // girare la testa oltre la soglia lo tirava verso lo sguardo, con la
    // torretta che tremava e restava indietro (audit 20/10).
    if (turnThr > 0.0f && pcForTurn && !vr::control::g_cinemaActive && !inCover && !g_gameLockedScene &&
        !TurretAimActive()) {
        float ex = std::remainder(hmdYaw - g_hmdYawReference, 6.28318530718f);
        float thr = turnThr;
        // "bodyfacing hands" (R-B3): il busto stimato da testa (0,5), linea tra
        // le mani (0,3; la perpendicolare piu' vicina alla testa) e mano
        // dell'arma (0,2), media circolare; zona morta di 20 gradi al posto
        // della soglia della testa.
        if (vr::control::g_bodyFacingHands && vrsys) {
            vr::TrackedDeviceIndex_t ri = vrsys->GetTrackedDeviceIndexForControllerRole(vr::control::GunRole());
            vr::TrackedDeviceIndex_t li = vrsys->GetTrackedDeviceIndexForControllerRole(vr::control::SupportRole());
            bool rOk = ri < vr::k_unMaxTrackedDeviceCount && handPoses[ri].bPoseIsValid;
            bool lOk = li < vr::k_unMaxTrackedDeviceCount && handPoses[li].bPoseIsValid;
            if (rOk && lOk) {
                const vr::HmdMatrix34_t& rm = handPoses[ri].mDeviceToAbsoluteTracking;
                const vr::HmdMatrix34_t& lm = handPoses[li].mDeviceToAbsoluteTracking;
                float headY = ex;
                float gunY = std::remainder(YawFromOpenVrPose(rm) - g_hmdYawReference, 6.28318530718f);
                // Linea sinistra -> destra (orizzontale): il busto guarda
                // perpendicolarmente; delle due perpendicolari quella vicina alla testa.
                float dx = rm.m[0][3] - lm.m[0][3], dz = rm.m[2][3] - lm.m[2][3];
                float yawL = 0.0f;
                bool lineOk = dx * dx + dz * dz > 0.05f * 0.05f;
                if (lineOk) {
                    // Yaw OpenVR come YawFromOpenVrPose: avanti = -z -> atan2(-x, -z) della direzione
                    float a = std::atan2(-dx, -dz);  // direzione sinistra->destra
                    float p1 = a + 1.57079633f, p2 = a - 1.57079633f;
                    float r1 = std::remainder(p1 - g_hmdYawReference, 6.28318530718f);
                    float r2 = std::remainder(p2 - g_hmdYawReference, 6.28318530718f);
                    yawL = std::fabs(std::remainder(r1 - headY, 6.28318530718f)) <
                                   std::fabs(std::remainder(r2 - headY, 6.28318530718f))
                               ? r1
                               : r2;
                }
                float wx = 0.5f * std::cos(headY) + 0.2f * std::cos(gunY) + (lineOk ? 0.3f * std::cos(yawL) : 0.0f);
                float wy = 0.5f * std::sin(headY) + 0.2f * std::sin(gunY) + (lineOk ? 0.3f * std::sin(yawL) : 0.0f);
                ex = std::atan2(wy, wx);
                thr = 20.0f * 0.01745329252f;
            }
        }
        static double lastTurnT = 0.0;
        double nowTurn = ProfMs() * 0.001;
        float dtTurn = lastTurnT > 0.0 ? static_cast<float>(nowTurn - lastTurnT) : 0.0f;
        lastTurnT = nowTurn;
        if (std::fabs(ex) > thr) {
            float shift = ex - std::copysign(thr, ex);
            // Al massimo 180 gradi/s: uscendo dalla copertura o da una scena
            // con la testa girata il corpo scattava tutto in un frame.
            float maxStep = 3.14159265f * (dtTurn > 0.0f && dtTurn < 0.1f ? dtTurn : 0.011f);
            if (std::fabs(shift) > maxStep) shift = std::copysign(maxStep, shift);
            // Al corpo vanno unita' intere: il riferimento si sposta di quelle
            // (prima dei radianti esatti: fino a 0,25 gradi/s di deriva, L7).
            int32_t units = static_cast<int32_t>(std::lround(shift * (65536.0f / 6.28318530718f)));
            int32_t applied = vr::control::g_yawSign * units;
            if (units && AddControllerYaw(pcForTurn, applied)) {
                g_hmdYawReference += units * (6.28318530718f / 65536.0f);
                ex -= units * (6.28318530718f / 65536.0f);
                // Gli occhi (fwd davanti al centro di Walker) non devono scivolare
                // di lato mentre il corpo si allinea: ~23 cm in 0,25 s con 45 gradi (H3).
                compensatePivot(applied, g_fwdApplied.load(), false);
            }
        }
        // Il corpo torna sotto la testa (follow-up della regressione finale,
        // 27/10, vr::pure::BodyFollow): sopra si gira solo l'eccesso oltre la
        // soglia, quindi dopo uno sguardo di lato e ritorno la testa restava 45
        // gradi fuori dal corpo per sempre e guardando in basso si vedeva lo
        // spallaccio. Con lo yaw della testa fermo per "bodyalign" s il corpo
        // ruota verso la testa in rampa (al massimo "bodyalignspeed" gradi/s).
        // Stesso percorso della rotazione qui sopra (corpo e riferimento della
        // testa nello stesso tick, perno sugli occhi): la vista non si muove,
        // mani, arma e mira restano nello spazio del tracciamento. Non durante
        // le mosse speciali (scavalcamenti, corpo a corpo) ne' con lo stick.
        // Review del follow-up (27/10): (1) non quando la mira del gioco e' il corpo
        // ("aim body", o "aim hand" con i controller non tracciati: la mira e' lo yaw
        // del PlayerController, girarlo la portava verso lo sguardo senza lo stick);
        // (2) arma puntata (due mani, ADS, grilletto nell'ultimo secondo) = come la
        // testa in movimento: l'arma disegnata sta un passo di rotazione indietro
        // mentre il corpo gira (0,6 gradi a una mano, 1,7 a due).
        bodyAlign.holdSec = vr::control::g_bodyAlignHoldS.load();
        bodyAlign.maxDegS = vr::control::g_bodyAlignDegS.load();
        const bool wasFollowing = bodyAlign.following;
        const float exBefore = ex;
        // Review 2 del follow-up: solo con "aim hand" e i controller tracciati, come dicono ini e README. Con "aim body"
        // (o i controller persi) la mira del gioco e' lo yaw del PlayerController e girarlo la spostava; con "aim view"
        // girava (la mira restava coerente) ma senza ADS ne' misure: escluso anche li'.
        const bool aimHandOk = vr::control::g_aimHand && vr::control::g_aimFollowsBody && g_aimValid;
        static double lastTriggerT = -10.0;
        if (vr::control::g_rightTriggerHeld) lastTriggerT = nowTurn;
        const bool gunAimed = g_twoHandActive || vr::control::g_adsActive || nowTurn - lastTriggerT < 1.0;
        // Review 2 del follow-up: muro o riparo basso davanti agli occhi (bit di g_bodyAlignBlock, vista del tick
        // prima). La vista arretra lungo l'avanti del CORPO: girarlo sotto la testa ferma faceva scivolare di lato
        // l'arretramento gia' attivo (2 W sin(d/2)) o lo accendeva/spegneva (muro di lato a 12 unita', testa 40 gradi
        // verso il muro: ~8 unita' indietro), cioe' la vista si muoveva senza che il giocatore si muovesse.
        const bool wallBlock = vr::control::g_bodyAlignWallStop && g_bodyAlignBlock.load() != 0;
        const char* alignWhy = specialMove ? "special move"
                               : vr::control::g_stickTurning ? "stick turn"
                               : !aimHandOk                  ? "the game aim is not the hand"
                               : wallBlock                   ? "wall or low cover in front of the eyes"
                                                             : "";
        if (wallBlock && aimHandOk && std::fabs(ex) * 57.2957795f >= bodyAlign.startDeg)
            alignWin.wallS += dtTurn > 0.0f && dtTurn < 0.25f ? dtTurn : 0.0f;
        const float stepRad = bodyAlign.Step(nowTurn, dtTurn, hmdYaw, ex,
                                             !specialMove && !vr::control::g_stickTurning && aimHandOk && !wallBlock, gunAimed);
        bodyAlignCarry += stepRad * (65536.0f / 6.28318530718f);
        const int32_t aUnits = static_cast<int32_t>(bodyAlignCarry);  // verso zero: il resto al tick dopo
        if (aUnits != 0) {
            bodyAlignCarry -= static_cast<float>(aUnits);
            const int32_t aApplied = vr::control::g_yawSign * aUnits;
            if (AddControllerYaw(pcForTurn, aApplied)) {
                g_hmdYawReference += aUnits * (6.28318530718f / 65536.0f);
                ex -= aUnits * (6.28318530718f / 65536.0f);
                compensatePivot(aApplied, g_fwdApplied.load(), false);
                bodyAlignDone += aUnits * (360.0f / 65536.0f);
            }
        }
        if (!bodyAlign.following && bodyAlign.rate == 0.0f) bodyAlignCarry = 0.0f;
        if (bodyAlign.following && gunAimed) bodyAlignPausedTurn = true;
        if (bodyAlign.following != wasFollowing) {
            if (bodyAlign.following) {
                bodyAlignStartT = nowTurn;
                bodyAlignDone = 0.0f;
                bodyAlignStartOff = exBefore * 57.2957795f;
                bodyAlignPausedTurn = false;
            } else {
                alignEnded(nowTurn, alignWhy, ex * 57.2957795f);
            }
        }
    } else {
        if (bodyAlign.following)
            alignEnded(ProfMs() * 0.001,
                       vr::control::g_cinemaActive ? "scene" : (inCover ? "cover" : (TurretAimActive() ? "turret" : "turnwithhead 0 or locked scene")),
                       std::remainder(hmdYaw - g_hmdYawReference, 6.28318530718f) * 57.2957795f);
        bodyAlign.Reset();
        bodyAlignCarry = 0.0f;
    }
    // Vista: yaw esatto, unita' arrotondate (WI-1). Direzione (movimento,
    // corpo, esecuzioni): lo yaw stabile.
    g_deltaYaw = vr::control::g_yawSign *
                 vr::pure::RadiansToUnrealRounded(std::remainder(hmdYawView - g_hmdYawReference, 6.28318530718f));
    g_headHeadingYaw = vr::control::g_yawSign * RadiansToUnreal(std::remainder(hmdYaw - g_hmdYawReference, 6.28318530718f));
    vr::control::g_headYawUnits = g_headHeadingYaw.load();
    g_bodyYawAtHeadSample = g_bodyYaw.load();  // diagnostica: anticipo della vista (riga TURRET, VIEW bodyhs)
    g_deltaPitch = vr::control::g_pitchSign * vr::pure::RadiansToUnrealRounded(hmdPitch - g_hmdPitchReference);
    if (vr::control::g_sixDof) {
        // Rollio: inclinazione del vettore "right" del visore (colonna 0).
        float rightY = m.m[1][0];
        if (rightY > 1.0f) rightY = 1.0f;
        if (rightY < -1.0f) rightY = -1.0f;
        // atan2(right.y, up.y): esatto anche guardando in alto/basso (asin di
        // right.y vale solo a beccheggio 0: a 45 gradi in basso sbagliava ~6 gradi).
        g_deltaRoll = vr::control::g_rollSign * vr::pure::RadiansToUnrealRounded(std::atan2(-m.m[1][0], m.m[1][1]));
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
            if (pawnR) {
                float k = (horiz - 0.05f) / horiz;  // si lasciano 5 cm
                float mf = forward * k * scale, mr = right * k * scale;
                float b = g_bodyYaw.load() * (6.28318530718f / 65536.0f);
                float moved[2];
                if (RoomStepMove(pawnR, mf * std::cos(b) - mr * std::sin(b), mf * std::sin(b) + mr * std::cos(b), moved)) {
                    float wx = moved[0], wy = moved[1];
                    // Mondo -> riferimento del corpo (avanti, destra), poi -> metri.
                    float mfw = (wx * std::cos(b) + wy * std::sin(b)) / scale;
                    float mrw = (-wx * std::sin(b) + wy * std::cos(b)) / scale;
                    if (mfw * mfw + mrw * mrw > 1e-6f) {
                        // offset = M * (pos - posRef): togliere (mfw, mrw) dall'offset.
                        posRef[0] += mfw * sy + mrw * cy;
                        posRef[2] += -mfw * cy + mrw * sy;
                        // Lo scostamento della testa cala di quanto si e' mosso il Pawn: non e' la testa (vignettatura).
                        g_headShiftWorld[0] -= wx;
                        g_headShiftWorld[1] -= wy;
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
    ClampCabinOffset();  // R-B2: abitacolo
    for (int i = 0; i < 3; ++i) g_viewDiagPosRef[i] = posRef[i];  // diagnostica (viewlog)
    PublishHeadPose(&mDeclared);
    {
        // Riferimento degli scostamenti delle mani qui sotto (stessi posRef e
        // g_hmdYawReference): origine del tracciamento e yaw del riferimento.
        // Nel primo spazio (review WI-2 round 2): i riferimenti riportati
        // indietro dei salti assorbiti descrivono le stesse mani nel mondo, ma
        // non cambiano quando un ricentramento del runtime viene assorbito.
        float refYawT = g_hmdYawReference, prx = posRef[0], prz = posRef[2];
        if (handsFix) vr::pure::RebaseReferences(spaceAbsorbed.Inverse(), &refYawT, &prx, &prz);
        float sy = std::sin(refYawT), cy = std::cos(refYawT), scale = vr::control::g_posScale;
        g_trackOriginOff[0] = (-prx * sy + prz * cy) * scale;
        g_trackOriginOff[1] = (-prx * cy - prz * sy) * scale;
        g_trackOriginOff[2] = -posRef[1] * scale;
        g_trackRefYaw = vr::control::g_yawSign * RadiansToUnreal(refYawT);
    }
    // Controller destro: direzione di puntamento (-Z del controller), yaw
    // relativo alla direzione di ricentramento, pitch rispetto all'orizzonte.
    {
        vr::TrackedDeviceIndex_t ri = vrsys->GetTrackedDeviceIndexForControllerRole(vr::control::GunRole());
        if (ri != vr::k_unTrackedDeviceIndexInvalid && ri < vr::k_unMaxTrackedDeviceCount && handPoses[ri].bPoseIsValid) {
            const vr::HmdMatrix34_t& c = handPoses[ri].mDeviceToAbsoluteTracking;
            for (int i = 0; i < 12; ++i) vr::control::g_gunCtrlPose[i] = c.m[i / 4][i % 4];  // laser sullo schermo virtuale
            vr::control::g_gunCtrlPoseValid = true;
            if (vr::control::g_stockCalibRequest.exchange(false)) {
                // Calcio in posizione di mira, bersaglio lontano guardato al
                // centro della vista: la direzione vera di mira e' quella della
                // testa. (La linea occhio-controller non va: sul calcio la mano
                // sta all'impugnatura, ~25 cm sotto l'occhio; dava -67 gradi.)
                // WI-4a: la linea di mira (avanti della testa) nel telaio di
                // puntamento del controller, elevazione = angolo della presa
                // attiva (gunangle con i controller nudi: arma all'occhio,
                // sguardo lungo la canna; stockangle col calcio). Vale col
                // controller ruotato (prima: beccheggio della testa meno quello
                // assoluto del raw, giusto solo a rollio 0). Palmo a 15-60 cm
                // dall'occhio (prima solo "oltre 15 cm").
                // Review WI-4a (round 1): con la presa normale la misura e'
                // circolare se il giocatore allinea l'arma DISEGNATA (gia'
                // inclinata di gunangle: torna gunangle stesso, nessun danno).
                // Per questo B + Y non e' piu' nell'aiuto della presa normale
                // (menu, README): li' si usa "Gun angle". Resta attiva (punta il
                // controller come una pistola vera, guardando il bersaglio);
                // verifica nel visore.
                const int phys = vr::control::PhysicalHand(0);
                const vr::pure::GunFrameParams gp = vr::control::GunFrameFor(phys, true);
                float palm[3];
                vr::pure::PalmOf(c.m, gp.left, gp.gripFix, palm);
                float d[3] = {palm[0] - pos[0], palm[1] - pos[1], palm[2] - pos[2]};
                float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                // Presa EFFETTIVA (review WI-4c round 1): su una torretta quella di
                // "Turret grip", l'angolo che la torretta usa; prima holdmode, e con
                // turretgrip normal + holdmode gunstock B + Y riscriveva stockangle con
                // una misura a controller nudi.
                const bool stock = vr::control::EffectiveHoldMode() == 1;
                const bool onTurret = vr::control::g_turretHoldNow.load() >= 0;
                const char* key = stock ? "stockangle" : "gunangle";
                if (dist >= 0.15f && dist <= 0.60f) {
                    const float headFwd[3] = {-m.m[0][2], -m.m[1][2], -m.m[2][2]};
                    float elev = 0.0f, lat = 0.0f;
                    vr::pure::SightLineInAim(c.m, gp.aim, headFwd, &elev, &lat);
                    float angle = std::round(elev * 2.0f) / 2.0f;
                    const float lo = stock ? vr::pure::kStockAngleMin : vr::pure::kGunAngleMin;
                    const float hi = stock ? vr::pure::kStockAngleMax : vr::pure::kGunAngleMax;
                    // Esito diverso al tatto: 150 ms = calibrato, 500 ms = rifiutata
                    // (prima la stessa vibrazione anche quando falliva; audit 20/10).
                    // Rifiutata anche guardando di lato oltre 30 gradi (non lungo l'arma).
                    const bool rejected = angle < lo || angle > hi || std::fabs(lat) > 30.0f;
                    char b[320];
                    std::snprintf(b, sizeof(b),
                                  "STOCK: %s calibration (%s hold%s): sight line %+.1f deg above the pointing ray, %+.1f deg to the right "
                                  "(not applied: gunyaw), palm %.0f cm from the eyes, head pitch %.1f deg -> %s",
                                  rejected ? "REJECTED" : "accepted", stock ? "gunstock" : "normal", onTurret ? ", turret grip" : "", elev, lat,
                                  dist * 100.0f,
                                  hmdPitch * 57.2957795f, rejected ? "kept the old value" : key);
                    vr::log::Line(b);
                    char val[32];
                    std::snprintf(val, sizeof(val), "%.1f", angle);
                    if (!rejected) {
                        (stock ? vr::control::g_stockAngleDeg : vr::control::g_gunAngleDeg) = angle;
                        vr::control::SaveIniValue(key, val);
                    }
                    // WI-4b: col calcio B + Y salva anche dove sta il controller
                    // sinistro sul calcio ("stockmount", posa nel raw destro):
                    // la presa a due mani GUNSTOCK lo riconosce entro stocktol.
                    // Review round 2 (basso): anche con l'angolo rifiutato (la posa
                    // della sinistra non dipende dalla linea di mira): prima un B + Y
                    // fatto senza guardare lungo il calcio non salvava nemmeno quella.
                    bool mountSaved = false;
                    if (stock) {
                        const vr::TrackedDeviceIndex_t lq = vrsys->GetTrackedDeviceIndexForControllerRole(vr::control::SupportRole());
                        if (lq != vr::k_unTrackedDeviceIndexInvalid && lq < vr::k_unMaxTrackedDeviceCount && handPoses[lq].bPoseIsValid) {
                            const vr::HmdMatrix34_t& lmq = handPoses[lq].mDeviceToAbsoluteTracking;
                            float lp[3];
                            vr::pure::PalmOf(lmq.m, vr::control::PhysicalHand(1) == 1, gp.gripFix, lp);
                            const float pd[3] = {lp[0] - palm[0], lp[1] - palm[1], lp[2] - palm[2]};
                            const float pdist = std::sqrt(pd[0] * pd[0] + pd[1] * pd[1] + pd[2] * pd[2]);
                            char mb[200];
                            if (pdist >= 0.10f && pdist <= 0.90f) {
                                float v6[6];
                                vr::twohand::StockMountFromPoses(c.m, lmq.m, v6);
                                std::snprintf(mb, sizeof(mb), "%.1f %.1f %.1f %.1f %.1f %.1f", v6[0], v6[1], v6[2], v6[3], v6[4], v6[5]);
                                vr::control::ExecuteLine(std::string("stockmount ") + mb);
                                vr::control::SaveIniValue("stockmount", mb);
                                vr::log::Line(std::string("STOCK: left controller on the stock saved (stockmount ") + mb + ", palms " +
                                              std::to_string(static_cast<int>(pdist * 100.0f + 0.5f)) + " cm apart" +
                                              (rejected ? ", stock angle not changed" : "") + ").");
                                mountSaved = true;
                            } else {
                                std::snprintf(mb, sizeof(mb),
                                              "STOCK: left controller %.0f cm from the gun hand (10-90 cm): stockmount not saved.", pdist * 100.0f);
                                vr::log::Line(mb);
                            }
                        }
                    }
                    // Testi brevi, letti in un attimo (review WI-4b round 2). Dal round 1
                    // della review WI-4c l'avviso va a capo (fino a tre righe, ui_text.h):
                    // il limite di una riga (~38 caratteri) non c'e' piu'.
                    vr::toast::Notify(rejected ? std::string(stock ? "Angle kept: look along the gun, far" : "Not calibrated: use Basic > Gun angle")
                                               : std::string(stock ? "Gunstock calibrated: " : "Gun angle calibrated: ") + val + " degrees");
                    if (mountSaved) vr::toast::Notify("Left hand on the stock saved");
                    vr::input::PulseHand(1, rejected ? 500 : 150);
                } else {
                    char b[160];
                    std::snprintf(b, sizeof(b), "STOCK: calibration ignored, controller %.0f cm from the eyes (15-60 cm).", dist * 100.0f);
                    vr::log::Line(b);
                    vr::input::PulseHand(1, 500);
                }
            }
            ApplyGunControllerAim(c);  // mira e rollio della mano (anche "synthctrl")
            float sy = std::sin(g_hmdYawReference), cy = std::cos(g_hmdYawReference);
            float scale = vr::control::g_posScale;
            // Controller sinistro, stesso riferimento.
            vr::TrackedDeviceIndex_t li =
                vrsys->GetTrackedDeviceIndexForControllerRole(vr::control::SupportRole());
            const bool leftOk = li != vr::k_unTrackedDeviceIndexInvalid && li < vr::k_unMaxTrackedDeviceCount && handPoses[li].bPoseIsValid;
            static const vr::HmdMatrix34_t kZeroPose = {};
            const vr::HmdMatrix34_t& lm = leftOk ? handPoses[li].mDeviceToAbsoluteTracking : kZeroPose;
            if (leftOk) {
                float lx = lm.m[0][3] - posRef[0], ly = lm.m[1][3] - posRef[1], lz = lm.m[2][3] - posRef[2];
                float left[3] = {(lx * sy - lz * cy) * scale, (lx * cy + lz * sy) * scale, ly * scale};
                for (int i = 0; i < 3; ++i) g_leftOffset[i] = left[i];
                ApplyLeftControllerAngles(lm);  // anche "synthctrl left"
                g_leftValid = true;
            } else {
                // Sinistra persa: niente direzione per "movedir hand" (prima
                // restava l'ultima, M11); la presa a due mani si chiude (M12) nel
                // passo qui sotto, senza scatto dell'arma (lo scarto resta e cala col moto).
                g_leftValid = false;
                vr::control::g_leftYawValid = false;
            }
            // Presa a due mani (WI-4b): puo' girare il telaio dell'arma attorno al palmo destro.
            {
                float tg[3][4];
                if (TwoHandStep(g_gunFrame, c.m, leftOk, lm.m, posRef, tg)) ApplyGunFrame(tg);
            }
            TurretGripStep(g_gunFrame, c.m, leftOk, lm.m);  // torretta, presa NORMAL: manubrio (WI-4c)
            // Posizione del polso dell'arma (telaio ruotato attorno al palmo,
            // WI-4a; con l'angolo 0 sul raw e' l'origine del controller, come
            // prima) rispetto alla testa al ricentramento, nello stesso
            // riferimento dello spostamento 6DOF della testa.
            float dx = g_gunWristPos[0] - posRef[0], dy = g_gunWristPos[1] - posRef[1], dz = g_gunWristPos[2] - posRef[2];
            g_handOffset[0] = (dx * sy - dz * cy) * scale;
            g_handOffset[1] = (dx * cy + dz * sy) * scale;
            g_handOffset[2] = dy * scale;
            g_handValid = true;
        } else {
            g_aimValid = false;
            g_handValid = false;
            vr::control::g_leftYawValid = false;
            TwoHandReset();
        }
    }
    TurretAimStep();  // anche con le pose finte (mode synth)
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
const uintptr_t& kCalcSceneViewAddr = vr::addr::g[vr::addr::CalcSceneView];
typedef void*(__thiscall* CalcSceneView_t)(void* self, void* a, void* b, void* c, void* d, void* e);
CalcSceneView_t g_CalcSceneView_Original = nullptr;
void* g_localPlayer = nullptr;

// APlayerController::GetPlayerViewPoint(out Location, out Rotation) @
// 0x00AB83F0: wrapper C++ dell'evento UnrealScript (thiscall, ret 8),
// chiamato da CalcSceneView a 0x0075C602 per ottenere la vista.
const uintptr_t& kGetPlayerViewPointAddr = vr::addr::g[vr::addr::GetPlayerViewPoint];
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
const uintptr_t& kSingleLineCheckAddr = vr::addr::g[vr::addr::SingleLineCheck];
const uintptr_t& kGWorldAddr = vr::addr::g[vr::addr::GWorld];
// + TRACE_Pawns (0x1): la testa non entra piu' nei compagni di squadra e il
// mirino si ferma sul nemico invece che sul muro dietro di lui (playtest
// 19/10). Il Pawn di Walker e' la sorgente, quindi escluso.
constexpr DWORD kTraceFlags = 0x2286 | 0x1 | 0x10;  // + TRACE_Others: attori mossi (cabina dell'elicottero)
typedef int(__thiscall* SingleLineCheck_t)(void* world, void* hit, void* source, const float* end, const float* start,
                                           DWORD flags, const float* extent, void* light);

// Ritorna true se la linea start->end colpisce qualcosa; hitTime in [0,1].
// Le sfere invisibili delle mani (props.cpp) non sono ostacoli: ProxyCreate
// le toglie dalle trace del motore; se una viene colpita lo stesso (bit
// riaccesi) si riparte oltre (diametro 5 unita') e lo si dice nel log. Con
// la sinistra sull'astina la sfera stava sulla trace spalla -> volata di
// MuzzleStart: colpo e mirino partivano dalla mano (25/10).
bool TraceLine(void* source, const float* start, const float* end, float* hitTime) {
    const float sx = end[0] - start[0], sy = end[1] - start[1], sz = end[2] - start[2];
    const float seg2 = sx * sx + sy * sy + sz * sz;
    float a[3] = {start[0], start[1], start[2]};
    for (int k = 0; k < 3; ++k) {
        alignas(16) unsigned char hit[0x100] = {};
        void* actor = nullptr;
        __try {
            if (!kGWorldAddr || !kSingleLineCheckAddr) return false;
            void* world = *reinterpret_cast<void**>(kGWorldAddr);
            if (!world) return false;
            const float extent[3] = {0.0f, 0.0f, 0.0f};
            reinterpret_cast<SingleLineCheck_t>(kSingleLineCheckAddr)(world, hit, source, end, a, kTraceFlags, extent, nullptr);
            actor = *reinterpret_cast<void**>(hit + 4);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        if (!actor) return false;
        // Tempo ricavato dalla posizione d'impatto (+8): l'offset del campo
        // Time non e' certo, la posizione si'.
        const float* loc = reinterpret_cast<const float*>(hit + 8);
        float hx = loc[0] - start[0], hy = loc[1] - start[1], hz = loc[2] - start[2];
        float t = seg2 > 0.0f ? (hx * sx + hy * sy + hz * sz) / seg2 : 0.0f;
        if (k < 2 && seg2 > 0.0f && vr::props::IsHandSphere(actor)) {
            vr::props::HandSphereTraced(actor, "TraceLine");
            const float tn = (t > 0.0f ? t : 0.0f) + 6.0f / std::sqrt(seg2);
            if (tn >= 1.0f) return false;
            a[0] = start[0] + sx * tn;
            a[1] = start[1] + sy * tn;
            a[2] = start[2] + sz * tn;
            continue;
        }
        *hitTime = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        return true;
    }
    return false;
}

// Come TraceLine, per la vista (muro davanti e collisione della testa): un
// oggetto tenuto in mano e le sfere delle mani non sono muri
// (props::IsHeldActor). Una cassa da 1,4 m tenuta davanti alla faccia tirava
// indietro la vista e con lei le mani; l'oggetto, guidato con il peso, le
// inseguiva e la vista oscillava di ~9 cm con lui (cassa, objectweight light,
// 25/10). La sfera della mano sinistra davanti al petto stava sulla trace
// bassa (10 unita' sotto l'occhio) e la vista arretrava di 25 unita'. Si
// riprova oltre (al piu' 3 volte): un muro dietro conta ancora. Solo le trace
// della vista: mira, volata e colpi restano come sono.
// Diagnostica di "camlog": cosa ferma la trace della vista.
void LogViewTraceHit(void* actor, float t) {
    char b[200];
    std::snprintf(b, sizeof(b), "CAMLOG: view trace hit %s (%s) at t=%.2f%s", vr::script::ObjectName(actor).c_str(),
                  vr::script::ClassName(actor).c_str(), t, vr::props::IsHeldActor(actor) ? ", held or hand: ignored" : "");
    vr::log::Line(b);
}

bool TraceLineView(void* source, const float* start, const float* end, float* hitTime) {
    float a[3] = {start[0], start[1], start[2]};
    const float d[3] = {end[0] - start[0], end[1] - start[1], end[2] - start[2]};
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(len > 1e-3f)) return TraceLine(source, start, end, hitTime);
    for (int k = 0; k < 4; ++k) {
        alignas(16) unsigned char hit[0x100] = {};
        bool ok = false;
        __try {
            if (!kGWorldAddr || !kSingleLineCheckAddr) return false;
            void* world = *reinterpret_cast<void**>(kGWorldAddr);
            if (!world) return false;
            const float extent[3] = {0.0f, 0.0f, 0.0f};
            reinterpret_cast<SingleLineCheck_t>(kSingleLineCheckAddr)(world, hit, source, end, a, kTraceFlags, extent, nullptr);
            ok = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
        if (!ok) return false;
        void* actor = *reinterpret_cast<void**>(hit + 4);
        if (!actor) return false;
        const float* loc = reinterpret_cast<const float*>(hit + 8);
        const float t = ((loc[0] - start[0]) * d[0] + (loc[1] - start[1]) * d[1] + (loc[2] - start[2]) * d[2]) / (len * len);
        if (vr::control::g_camLogFrames.load() > 0) LogViewTraceHit(actor, t);
        if (vr::props::IsHandSphere(actor)) vr::props::HandSphereTraced(actor, "TraceLineView");  // non dovrebbe piu' succedere
        if (!vr::props::IsHeldActor(actor) || k == 3) {
            *hitTime = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
            return true;
        }
        if (t >= 1.0f) return false;
        // Oltre tutto l'oggetto: dove la trace esce dalla sfera dei suoi limiti
        // (componente colpito, Bounds +0xEC: origine, ..., raggio +24), almeno 1
        // unita' oltre il colpo. Un'unita' sola non bastava: la trace ripartita
        // dentro la bottiglia la colpiva di nuovo, 4 volte, e al quarto colpo
        // contava come muro (vista tirata indietro di 25 unita').
        float tn = (std::max)(0.0f, t) + 1.0f / len;
        void* comp = *reinterpret_cast<void**>(hit + 0x30);
        float bb[7];
        if (comp && SafeCopy(bb, static_cast<char*>(comp) + 0xEC, sizeof(bb)) && bb[6] > 0.0f && bb[6] < 500.0f) {
            const float tc = ((bb[0] - start[0]) * d[0] + (bb[1] - start[1]) * d[1] + (bb[2] - start[2]) * d[2]) / (len * len);
            float pd2 = 0.0f;
            for (int c = 0; c < 3; ++c) {
                const float q = start[c] + d[c] * tc - bb[c];
                pd2 += q * q;
            }
            const float half = std::sqrt((std::max)(0.0f, bb[6] * bb[6] - pd2)) / len;
            // al piu' 30 unita' per passo: un muro dentro la sfera di una cassa
            // grande resta visibile al passo dopo
            tn = (std::max)(tn, (std::min)(tc + half, (std::max)(0.0f, t) + 30.0f / len) + 1.0f / len);
        }
        tn = (std::min)(1.0f, tn);
        for (int c = 0; c < 3; ++c) a[c] = start[c] + d[c] * tn;
    }
    return false;
}

// WI-2 (25/10): trace della vista col colpo piu' vicino. TraceLine e
// TraceLineView usano kTraceFlags, che ha TRACE_StopAtAnyHit (0x200, da
// FastTrace): in MultiLineCheck (0x0071B130) con quel flag un colpo sul BSP
// salta tutto il passaggio degli attori (mesh statiche, oggetti, Pawn), quindi
// una faccia del BSP piu' lontana vinceva su una cassa piu' vicina e il
// risultato saltava tra le due mentre la trace si muoveva con la testa (muro
// davanti e collisione della testa a scatti: bug 3 del tester). Qui senza 0x200.
// Stessi oggetti saltati di TraceLineView (props::IsHeldActor: oggetti in mano
// e sfere delle mani; lo stesso passo oltre i loro limiti).
//  kViewHead: collisione della testa, Pawn compresi (la testa non entra nei
//    compagni, 19/10);
//  kViewPull: muro davanti e trace basse, senza Pawn (un compagno a 30 unita'
//    tirava indietro la vista di 16, misurato) e senza attori in movimento
//    (Velocity +0x140 oltre 10 unita'/s: oggetti fisici lanciati, porte).
//    Il filtro degli oggetti mobili e' da concordare con la fisica (merge).
// normalOut: normale della superficie colpita (FCheckResult +0x14), solo se
// unitaria e rivolta verso l'inizio della trace; se no resta a zero.
enum ViewTraceKind { kViewHead = 0, kViewPull = 1 };
double QpcMicrosPerTick() {
    static const double us = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1.0e6 / static_cast<double>(f.QuadPart);
    }();
    return us;
}
constexpr DWORD kViewTraceFlags = kTraceFlags & ~static_cast<DWORD>(0x200);
std::atomic<uint32_t> g_viewTraceMovingSkips{0};  // diagnostica (riga VIEW)
// Una passata: trace con questi flag, oggetti tenuti saltati (e per kViewPull
// gli attori in movimento). Vedi TraceViewNearest.
bool TraceViewPass(void* source, const float* start, const float* end, DWORD flags, bool skipMoving, float* hitTime,
                   float* normalOut, void** actorOut) {
    if (normalOut) normalOut[0] = normalOut[1] = normalOut[2] = 0.0f;
    if (actorOut) *actorOut = nullptr;
    float a[3] = {start[0], start[1], start[2]};
    const float d[3] = {end[0] - start[0], end[1] - start[1], end[2] - start[2]};
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(len > 1e-3f)) return false;
    for (int k = 0; k < 4; ++k) {
        alignas(16) unsigned char hit[0x100] = {};
        bool ok = false;
        LARGE_INTEGER q0 = {}, q1 = {};
        QueryPerformanceCounter(&q0);
        __try {
            if (!kGWorldAddr || !kSingleLineCheckAddr) return false;
            void* world = *reinterpret_cast<void**>(kGWorldAddr);
            if (!world) return false;
            const float extent[3] = {0.0f, 0.0f, 0.0f};
            reinterpret_cast<SingleLineCheck_t>(kSingleLineCheckAddr)(world, hit, source, end, a, flags, extent, nullptr);
            ok = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
        QueryPerformanceCounter(&q1);
        g_viewTraceUs.fetch_add(static_cast<uint64_t>((q1.QuadPart - q0.QuadPart) * QpcMicrosPerTick() + 0.5), std::memory_order_relaxed);
        g_viewTraceCalls.fetch_add(1, std::memory_order_relaxed);
        if (!ok) return false;
        void* actor = *reinterpret_cast<void**>(hit + 4);
        if (!actor) return false;
        const float* loc = reinterpret_cast<const float*>(hit + 8);
        const float t = ((loc[0] - start[0]) * d[0] + (loc[1] - start[1]) * d[1] + (loc[2] - start[2]) * d[2]) / (len * len);
        if (vr::control::g_camLogFrames.load() > 0) LogViewTraceHit(actor, t);
        bool skip = vr::props::IsHeldActor(actor) && k < 3;
        if (!skip && skipMoving && k < 3) {
            float v[3];
            if (SafeCopy(v, static_cast<char*>(actor) + 0x140, sizeof(v)) && v[0] * v[0] + v[1] * v[1] + v[2] * v[2] > 100.0f) {
                skip = true;
                g_viewTraceMovingSkips.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (!skip) {
            *hitTime = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
            if (normalOut) {
                float n[3];
                std::memcpy(n, hit + 0x14, sizeof(n));
                const float nl = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
                const float facing = n[0] * d[0] + n[1] * d[1] + n[2] * d[2];
                if (nl > 0.81f && nl < 1.21f && facing < 0.0f) {
                    const float s = 1.0f / std::sqrt(nl);
                    for (int c = 0; c < 3; ++c) normalOut[c] = n[c] * s;
                }
            }
            if (actorOut) *actorOut = actor;
            return true;
        }
        if (t >= 1.0f) return false;
        // Oltre l'oggetto saltato, come in TraceLineView.
        float tn = (std::max)(0.0f, t) + 1.0f / len;
        void* comp = *reinterpret_cast<void**>(hit + 0x30);
        float bb[7];
        if (comp && SafeCopy(bb, static_cast<char*>(comp) + 0xEC, sizeof(bb)) && bb[6] > 0.0f && bb[6] < 500.0f) {
            const float tc = ((bb[0] - start[0]) * d[0] + (bb[1] - start[1]) * d[1] + (bb[2] - start[2]) * d[2]) / (len * len);
            float pd2 = 0.0f;
            for (int c = 0; c < 3; ++c) {
                const float q = start[c] + d[c] * tc - bb[c];
                pd2 += q * q;
            }
            const float half = std::sqrt((std::max)(0.0f, bb[6] * bb[6] - pd2)) / len;
            tn = (std::max)(tn, (std::min)(tc + half, (std::max)(0.0f, t) + 30.0f / len) + 1.0f / len);
        }
        tn = (std::min)(1.0f, tn);
        for (int c = 0; c < 3; ++c) a[c] = start[c] + d[c] * tn;
    }
    return false;
}

// kViewPull: una passata senza 0x200, senza Pawn, senza attori in movimento.
// kViewHead ("headtrace", review WI-2): 1 = una passata senza 0x200 coi Pawn
// (TRACE_Pawns 0x1): colpisce compagni, mesh statiche e BSP, il piu' vicino;
// 0 = la piu' vicina di due passate, senza 0x200 e senza Pawn, e con i flag di
// prima (kTraceFlags, coi Pawn ma con 0x200: un compagno davanti a un muro
// del BSP si saltava); 2 = le due passate coi Pawn nella prima.
// La passata senza Pawn del 25/10 ("senza 0x200 nessun colpo sul compagno")
// toglieva lei 0x1: la misura era delle trace basse (review WI-2).
bool TraceViewNearest(void* source, const float* start, const float* end, float* hitTime, int kind, float* normalOut = nullptr,
                      void** actorOut = nullptr) {
    const DWORD noPawns = kViewTraceFlags & ~static_cast<DWORD>(0x1);
    const int modeAll = vr::control::g_headTraceMode.load();
    // "headtrace +4" (solo collaudi prima/dopo): muro davanti coi flag di TraceLineView (0x200, Pawn).
    if (kind == kViewPull)
        return (modeAll & 4) ? TraceViewPass(source, start, end, kTraceFlags, false, hitTime, normalOut, actorOut)
                             : TraceViewPass(source, start, end, noPawns, true, hitTime, normalOut, actorOut);
    const int mode = modeAll & 3;
    if (mode == 1) return TraceViewPass(source, start, end, kViewTraceFlags, false, hitTime, normalOut, actorOut);
    float t1 = 2.0f, t2 = 2.0f, n2[3];
    void* a2 = nullptr;
    const bool h1 = TraceViewPass(source, start, end, mode == 2 ? kViewTraceFlags : noPawns, false, &t1, normalOut, actorOut);
    const bool h2 = TraceViewPass(source, start, end, kTraceFlags, false, &t2, n2, &a2);
    if (!h1 && !h2) return false;
    if (h2 && (!h1 || t2 < t1 - 1e-4f)) {
        *hitTime = t2;
        if (normalOut) std::memcpy(normalOut, n2, sizeof(n2));
        if (actorOut) *actorOut = a2;
    } else {
        *hitTime = t1;
    }
    return true;
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
const uintptr_t& kGObjectsAddr = vr::addr::g[vr::addr::GObjects];
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

void* LivePlayerController() {
    uint32_t cls = g_playerControllerCls;
    char* cam = static_cast<char*>(g_cameraPtr.load());
    if (HasCameraSignature(cam)) {
        void* pc = cam - 0x77C;
        if (!cls || StillAlive(pc, cls)) return pc;
    }
    void* pc = g_playerControllerPtr;
    if (pc && cls && StillAlive(pc, cls)) return pc;
    return nullptr;
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
uint8_t g_gunSaved[24];                           // Translation+Rotation originali del componente
uint32_t g_gunSavedFlags = 0;                     // bit Abs* originali
float g_gunEyeBase[3];                            // posizione del corpo dell'occhio sinistro
bool g_gunEyeBaseValid = false;

// enable: salva (prima volta) e scrive posa e bit assoluti; disable: rimette i
// valori originali (prima si azzeravano, che e' quasi sempre ma non per forza
// la posa del gioco).
bool SafeSetGunPose(void* comp, bool enable, const float* pos, const int32_t* rot) {
    __try {
        char* c = static_cast<char*>(comp);
        uint32_t& flags = *reinterpret_cast<uint32_t*>(c + kOffCompFlags);
        if (enable) {
            if (comp != g_gunComp) {
                std::memcpy(g_gunSaved, c + kOffCompTranslation, 24);
                g_gunSavedFlags = flags & (kAbsTranslation | kAbsRotation);
            }
            std::memcpy(c + kOffCompTranslation, pos, 12);
            std::memcpy(c + kOffCompRotation, rot, 12);
            flags |= kAbsTranslation | kAbsRotation;
        } else {
            flags = (flags & ~(kAbsTranslation | kAbsRotation)) | g_gunSavedFlags;
            std::memcpy(c + kOffCompTranslation, g_gunSaved, 24);
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
    bool pistol = false;    // pistole e Uzi: una mano, caricatore nell'impugnatura, presa a due mani avvolgente
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
        g_flightGunHeld = g_weaponInfo.cls.find("OnFlight") != std::string::npos;
        // Caricatore per tipo d'arma (rispetto a magwell, tarato sui fucili):
        // pistole e Uzi nell'impugnatura sotto la mano, P90 sopra la canna,
        // fucili a tamburo/pompa piu' avanti.
        const std::string& c = g_weaponInfo.cls;
        g_weaponInfo.pistol = c.find("DesertEagle") != std::string::npos || c.find("M9A1") != std::string::npos ||
                              c.find("Python") != std::string::npos || c.find("MicroUzi") != std::string::npos ||
                              c.find("Pistol") != std::string::npos;
        g_weaponShort = g_weaponInfo.pistol;
        float mf = 0.0f, md = 0.0f;
        if (g_weaponInfo.pistol) {
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

void UpdateMagazineVisual(void* comp);
// Vecchio aimpitch calibrato migrato a "holdmode gunstock" (review WI-4a,
// round 1): due avvisi, una volta, dopo 10 s di gioco. Prima di ogni ritorno
// di UpdateGunInHand (con le braccia guidate esce subito).
// Review WI-4b round 1 (M-e): col calcio la presa a due mani riconosce la
// sinistra sul calcio solo dalla posa salvata da B + Y ("stockmount"); chi
// arrivava dalla migrazione (o non l'aveva rifatto) aveva "stockmount none" e
// nessuno glielo diceva: dispersione 1,5x invece di 0,5x, niente sinistra
// disegnata sull'arma, niente vibrazione. Ora l'avviso lo dice, e con il calcio
// senza posa salvata un suggerimento una volta per sessione (dopo 20 s di gioco
// con il controller sinistro acceso).
void HoldMigrationNoticeTick() {
    static ULONGLONG since = 0, stockSince = 0, resetSince = 0;
    static bool stockHintShown = false;
    const bool migration = vr::control::g_holdMigrationNotice.load();
    const bool reset = vr::control::g_settingsResetNotice.load();
    float mpos[3], mrot[3][4];
    const bool stockHint = !stockHintShown && vr::control::g_holdMode.load() == 1 && vr::control::g_twoHand.load() &&
                           !vr::control::StockMount(mpos, mrot) && g_leftValid.load();
    if (!migration && !stockHint && !reset) {
        stockSince = 0;
        return;
    }
    if (reset) {
        // 0.10.0: ini rimesso ai predefiniti (control.cpp LoadConfigFile), dopo 10 s di gioco.
        char* camR = static_cast<char*>(g_cameraPtr.load());
        void* pawnR = HasCameraSignature(camR) ? PlayerPawnIfViewTarget(camR) : nullptr;
        const bool playing = !g_gameLockedScene && !vr::control::g_cinemaActive && pawnR && PawnAlive(pawnR);
        const ULONGLONG nowR = GetTickCount64();
        if (!playing) {
            resetSince = 0;
        } else if (!resetSince) {
            resetSince = nowR;
        } else if (nowR - resetSince > 10000 && vr::control::g_settingsResetNotice.exchange(false)) {
            vr::toast::Notify(vr::ui::kToastSettingsReset);
            vr::log::Line("CONFIG: settings reset notice shown.");
        }
        if (!migration && !stockHint) return;
    }
    char* cam = static_cast<char*>(g_cameraPtr.load());
    void* pawn = HasCameraSignature(cam) ? PlayerPawnIfViewTarget(cam) : nullptr;
    const bool gameplay = !g_gameLockedScene && !vr::control::g_cinemaActive && pawn && PawnAlive(pawn);
    if (!gameplay) {
        since = stockSince = 0;
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (migration) {
        if (!since) {
            since = now;
        } else if (now - since > 10000 && vr::control::g_holdMigrationNotice.exchange(false)) {
            // Review WI-4b round 2 (basso): prima si leggeva "On the stock: hold
            // B + Y 1 s with both ..." e lo scopo non si vedeva; B + Y col calcio
            // misura anche l'angolo: guardare lungo il calcio un bersaglio lontano.
            // Dal round 1 della review WI-4c gli avvisi vanno a capo (fino a tre
            // righe, ui_text.h); dal round 2 il suggerimento del calcio e' un
            // avviso solo, non due mezze frasi da 3,5 s l'una (testi in ui_text.h,
            // misurati nei test).
            vr::toast::Notify(vr::ui::kToastMigrationKept + vr::pure::FormatConfigNumber(vr::control::g_stockAngleDeg.load(), 1) +
                              " deg");
            vr::toast::Notify(vr::ui::kToastMigrationNoStock);
            vr::toast::Notify(vr::ui::kToastStockHint);
            vr::log::Line("AIM: migration notice shown (old aimpitch calibration -> holdmode gunstock; B + Y saves the left hand on the stock).");
            stockHintShown = true;
        }
        return;
    }
    if (!stockSince) {
        stockSince = now;
    } else if (now - stockSince > 20000) {
        stockHintShown = true;
        vr::toast::Notify(vr::ui::kToastStockHint);
        vr::log::Line("AIM: stock hint shown (gunstock hold, no stockmount saved: the left hand counts only on the bare barrel).");
    }
}

void UpdateGunInHand(const float* base, int32_t bodyYaw) {
    HoldMigrationNoticeTick();
    char* cam = static_cast<char*>(g_cameraPtr.load());
    void* pawn = HasCameraSignature(cam) ? PlayerPawnIfViewTarget(cam) : nullptr;
    // Stesse esclusioni delle braccia (UpdateArms): scene scriptate e mosse
    // animate dal gioco (corda, zipline, scavalcare), schermo virtuale, Walker
    // morto. Prima le braccia venivano rilasciate all'animazione ma il fucile
    // restava inchiodato al controller: "corpo deforme" sulla corda (21/10).
    bool gameplay = !g_gameLockedScene && !vr::control::g_cinemaActive && pawn && PawnAlive(pawn);
    // Suggerimenti sui controller nelle prime sessioni (R-U2). Prima del ritorno
    // per le braccia guidate (review WI-4c round 2): prima stavano dopo, e con le
    // braccia (il default) giravano solo sulle torrette, dove le braccia si
    // rilasciano; a piedi mai. Non in pausa. Sulla torretta la serie dell'arma
    // aspetta (turret: -1 a piedi, 0 NORMAL, 1 GUNSTOCK, 2 mirata con lo stick).
    {
        const int hold = vr::control::g_turretHoldNow.load();
        const int turret = g_onTurret.load() ? (hold >= 0 ? hold : 2) : -1;
        // Situazioni degli oggetti (props.cpp, ultimo tick) per i suggerimenti contestuali.
        vr::toast::HintsTick(gameplay && !vr::control::g_gamePaused, turret, vr::props::HintSituations());
    }
    if (g_armsActive) {
        ReleaseGun();
        return;
    }
    bool hand = vr::control::g_aimHand && vr::control::g_aimFollowsBody && g_aimValid && g_handValid;
    // Solo in prima persona: in terza persona la mano sarebbe davanti alla camera.
    bool fp = vr::control::g_firstPerson;
    void* comp = (vr::control::g_gunInHand && hand && fp && gameplay) ? CurrentGunComponent(pawn) : nullptr;
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
    float of = (vr::control::g_gunOffsetCm[0] + cls[0]) * k,
          orr = (vr::control::g_gunOffsetCm[1] + cls[1]) * k * (vr::control::g_leftHanded ? -1.0f : 1.0f),  // M17
          ou = (vr::control::g_gunOffsetCm[2] + cls[2]) * k;
    for (int i = 0; i < 3; ++i) pos[i] += gf[i] * of + gr[i] * orr + gu[i] * ou;
    if (SafeSetGunPose(comp, true, pos, rot)) {
        g_gunComp = comp;
        g_gunCompCls = ObjClass(comp);
    } else {
        g_gunComp = nullptr;
    }
}

// Ricarica fisica: con il caricatore estratto o in mano l'osso "Magazine"
// dell'arma viene nascosto (HideBoneByName, come la testa), rimesso
// all'inserimento o al cambio d'arma (R-W7 passo 1; i modelli hanno l'osso
// "Magazine": M4, AA12, Desert Eagle, SCAR, M9A1; Milkor no).
void UpdateMagazineVisual(void* comp) {
    static void* lastComp = nullptr;
    static uint32_t lastCls = 0;
    static bool hidden = false;
    bool want = vr::input::MagazineRemoved();
    if (comp != lastComp) {
        if (hidden && lastComp && StillAlive(lastComp, lastCls)) {
            struct {
                int32_t name[2];
                uint8_t physBodyOption;
                uint8_t pad[3];
            } p = {{vr::script::NameIndex("Magazine"), 0}, 0, {0, 0, 0}};
            if (p.name[0] >= 0) vr::script::CallFunction(lastComp, "UnHideBoneByName", &p);
        }
        lastComp = comp;
        lastCls = ObjClass(comp);
        hidden = false;
    }
    if (!comp || want == hidden) return;
    struct {
        int32_t name[2];
        uint8_t physBodyOption;
        uint8_t pad[3];
    } p = {{vr::script::NameIndex("Magazine"), 0}, 0, {0, 0, 0}};
    if (p.name[0] < 0) return;
    if (vr::script::CallFunction(comp, want ? "HideBoneByName" : "UnHideBoneByName", &p)) {
        hidden = want;
        vr::log::Line(want ? "MAGAZINE: bone hidden (magazine out)" : "MAGAZINE: bone shown");
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
// Mancini ("hand left"): il lato 0 (arma) e' il braccio SINISTRO di Walker e
// il lato 1 (supporto) il destro; l'arma viene riagganciata all'osso Hand_Left.
// Niente mesh specchiata: l'IK del motore converte gli spazi con atomi a
// quaternione, che una riflessione rompe (22/10, Ghidra 0x00583610 /
// GetBoneQuaternion a caso). Nessuna opzione a parte: segue "hand".
inline bool ArmsSwapped() { return vr::control::g_leftHanded; }
inline bool WalkerRightArm(int side) { return (side == 0) != ArmsSwapped(); }
inline const char* HandBone(int side) { return WalkerRightArm(side) ? "Hand_Right" : "Hand_Left"; }
inline const char* WeaponBone(int side) { return WalkerRightArm(side) ? "Weapon_Right" : "Weapon_Left"; }
inline const char* IkHelperBone(int side) { return WalkerRightArm(side) ? "IK_Hand_R" : "IK_Hand_L"; }
inline const char* ShoulderBone(int side) { return WalkerRightArm(side) ? "Arm_Right" : "Arm_Left"; }
// Mancini: dopo il riaggancio dell'arma la mano sinistra resta all'animazione
// per questi frame e la relazione mano/arma si misura all'ultimo, ferma; poi
// non si rimisura piu' (misurarla con la mano guidata faceva ruotare la
// relazione di ~10 gradi a ogni misura, spirale, 22/10).
int g_relaxFrames = 0;
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

void ResetSkelFix();
void RestoreGameRecoil();
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
    // Voce 5: nessuna mano disegnata (l'HUD al polso torna opaco, la misura
    // della sinistra torna libera; revisione 26/10).
    vr::props::RenderHandNone(1);
    vr::control::g_supportHandSepCm = 0.0f;
    // Review WI-4c round 2: con le braccia rilasciate (torretta, scena, morte)
    // la sinistra non e' sull'arma. Prima restava l'ultimo valore a piedi: sulla
    // torretta i gesti vedevano "left grip on the gun" a ogni grip sinistro (LB mai
    // permesso, niente lancio, m_twoHandT rinfrescato di continuo).
    g_leftOnGun = false;
    vr::control::g_leftOnGun = false;
    ResetSkelFix();       // L9
    RestoreGameRecoil();  // L11: il rinculo del gioco torna com'era
}

bool AcquireArms(void* mesh) {
    ReleaseArms();
    std::string names;
    for (int i = 0; i < ArmRig::kCount; ++i) {
        std::string n = i < 4 ? vr::control::ArmControlName(ArmsSwapped() ? (i ^ 1) : i) : (i == 4 ? "IKBone_RightHand" : "IKBone_LeftHand");
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
    float mirror = vr::control::PhysicalHand(side) == 0 ? 1.0f : -1.0f;  // laterale per mano fisica (M17)
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
    if (side == 0 && ArmsSwapped() && g_relaxFrames > 0) {
        // Mano dell'arma a riposo (animazione) mentre si misura la relazione.
        static const float zero3[3] = {0, 0, 0};
        SafeCopy(static_cast<char*>(limb) + kOffCtrlStrength, zero3, 4);
        SafeCopy(static_cast<char*>(limb) + kOffCtrlStrengthTarget, zero3, 4);
        if (rotc) {
            SafeCopy(static_cast<char*>(rotc) + kOffCtrlStrength, zero3, 4);
            SafeCopy(static_cast<char*>(rotc) + kOffCtrlStrengthTarget, zero3, 4);
        }
        return;
    }
    float mirror = vr::control::PhysicalHand(side) == 0 ? 1.0f : -1.0f;  // laterale per mano fisica (M17)
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
        struct {
            int32_t name[2];
            int32_t space;
            float ret[3];
        } bl = {{vr::script::NameIndex(HandBone(side)), 0}, 0, {0, 0, 0}};
        if (g_rig.mesh && vr::script::CallFunction(g_rig.mesh, "GetBoneLocation", &bl)) {
            float dx = bl.ret[0] - pos[0], dy = bl.ret[1] - pos[1], dz = bl.ret[2] - pos[2];
            snprintf(buf, sizeof(buf), "ARMS %s: actual hand world=(%.1f, %.1f, %.1f), %.1f units from the wanted one",
                     side == 0 ? "right" : "left", bl.ret[0], bl.ret[1], bl.ret[2], std::sqrt(dx * dx + dy * dy + dz * dz));
            vr::log::Line(buf);
        }
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
    static bool cachedSwap = false;
    if (mesh != cachedMesh || cachedSwap != ArmsSwapped()) {
        cachedSwap = ArmsSwapped();
        cachedMesh = mesh;
        idxR = idxL = -1;
        for (int i = 0; i < 256 && (idxR < 0 || idxL < 0); ++i) {
            struct {
                int32_t index;
                int32_t ret[2];
            } parms = {i, {0, 0}};
            if (!vr::script::CallFunction(mesh, "GetBoneName", &parms) || parms.ret[0] <= 0) break;
            std::string n = vr::script::NameFromIndex(parms.ret[0]);
            if (n == IkHelperBone(0)) idxR = i;
            if (n == IkHelperBone(1)) idxL = i;
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
    float d2 = pos[0] * pos[0] + pos[1] * pos[1] + pos[2] * pos[2];
    if (vr::control::g_fireLog) {
        // Diagnostica ("firelog on"): la distanza tra le mani dell'animazione
        // quando cambia di 2 unita' (pistole: in quali animazioni la sinistra
        // sta sull'arma?).
        static float lastD = -1.0f;
        float dn = std::sqrt(d2);
        if (std::fabs(dn - lastD) > 2.0f) {
            lastD = dn;
            char b[160];
            snprintf(b, sizeof(b), "ARMS grip relation: left hand %.1f units from the right in the animation, rel=(%.1f %.1f %.1f)%s",
                     dn, pos[0], pos[1], pos[2], dn < 26.0f ? "" : " (no grip)");
            vr::log::Line(b);
        }
    }
    return d2 < 26.0f * 26.0f;
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
    } hp = {{vr::script::NameIndex(ShoulderBone(side)), 0}, 0, {0, 0, 0}};
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
    return BoneAxes(mesh, WeaponBone(side), w);
}

// Mancini: l'arma, che il gioco aggancia all'osso Weapon_Right, passa a
// Weapon_Left riscrivendo il nome dell'osso nella voce Attachments della mesh
// (FAttachment: Component, BoneName, RelativeLocation/Rotation/Scale, 48
// byte); l'array si trova cercando la voce con il componente dell'arma. Il
// motore rilegge il nome a ogni aggiornamento dei figli. Torna indietro con
// "hand right". Ritorna true quando ha cambiato aggancio (relazione da rimisurare).
bool UpdateGunAttachment(void* mesh, void* gunComp, void* pawn) {
    static void* lastComp = nullptr;
    static bool lastLeft = false;
    static uint32_t knownOff = 0x348;  // USkeletalMeshComponent.Attachments (visto dal vivo 22/10; se cambia si cerca)
    static int32_t origName = -1;  // nome originale della voce (osso o socket destro)
    bool wantLeft = ArmsSwapped();
    if (!mesh || !gunComp) return false;
    if (gunComp == lastComp && wantLeft == lastLeft) return false;
    char* m = static_cast<char*>(mesh);
    // Voce Attachments con Component == arma: qualsiasi nome (il gioco puo'
    // agganciare a un socket, non all'osso Weapon_Right).
    char* entry = nullptr;
    uint32_t foundOff = 0, foundIdx = 0;
    for (int pass = 0; pass < 2 && !entry; ++pass)
    for (uint32_t off = pass == 0 ? knownOff : 0x100; off < 0xC00 && !entry; off += 4) {
        uint32_t data = 0, num = 0;
        if (!TrySafeReadUint32(m + off, &data) || !TrySafeReadUint32(m + off + 4, &num) || !data || num == 0 || num > 32) {
            if (pass == 0) break;
            continue;
        }
        // Passo della voce non fissato (FAttachment qui e' 52 byte, non 48: la
        // voce dell'arma stava a 312 = 6*52, 22/10): si cerca il puntatore a
        // passi di 4 byte, seguito da un FName valido.
        for (uint32_t q = 0; q < num * 64; q += 4) {
            char* e = reinterpret_cast<char*>(data) + q;
            uint32_t comp = 0;
            int32_t bn = 0;
            if (!TrySafeReadUint32(e, &comp)) break;
            if (comp != reinterpret_cast<uintptr_t>(gunComp)) continue;
            if (!TrySafeReadUint32(e + 4, reinterpret_cast<uint32_t*>(&bn)) || bn <= 0 || bn > 200000) continue;
            entry = e;
            foundOff = off;
            foundIdx = q;
            break;
        }
        if (pass == 0) break;
    }
    lastComp = gunComp;
    lastLeft = wantLeft;
    if (!entry) {
        static bool logged = false;
        if (!logged && wantLeft) {
            logged = true;
            vr::log::Line("ARMS: weapon attachment entry not found in the pawn mesh (0x100..0xC00): no arm swap for the gun.");
            // Diagnostica: l'arma e' un attore basato sul Pawn (SetBase con osso)?
            // Actor.Base +0xAC, poi BaseBoneName / BaseSkelComponent nei dintorni.
            uint32_t weapon = 0;
            if (pawn && TrySafeReadUint32(static_cast<char*>(pawn) + kOffPawnCurrentWeapon, &weapon) && weapon) {
                std::string line = "ARMS: weapon actor fields:";
                for (uint32_t o = 0xA8; o <= 0xD0; o += 4) {
                    uint32_t v = 0;
                    if (!TrySafeReadUint32(reinterpret_cast<char*>(weapon) + o, &v)) continue;
                    char b[96];
                    std::string nm = (v > 0 && v < 200000) ? vr::script::NameFromIndex(static_cast<int32_t>(v)) : std::string();
                    std::snprintf(b, sizeof(b), " +0x%X=%08X%s%s%s", o, v, nm.empty() ? "" : " (name ", nm.c_str(), nm.empty() ? "" : ")");
                    line += b;
                }
                vr::log::Line(line);
                // Il componente mesh dell'arma: a chi e' agganciato? (UActorComponent.Owner +0x?? non noto: si logga il puntatore del Pawn e della mesh)
                char b2[120];
                std::snprintf(b2, sizeof(b2), "ARMS: pawn %p, pawn mesh %p, weapon actor %p, weapon mesh comp %p", pawn, mesh,
                              reinterpret_cast<void*>(weapon), gunComp);
                vr::log::Line(b2);
                // Dove sta il puntatore del componente arma? In ogni TArray della
                // mesh del Pawn (blocchi fino a num*64 byte) e nei campi diretti.
                std::string hits = "ARMS: gun comp pointer found in pawn mesh:";
                for (uint32_t o = 0x0; o < 0xC00; o += 4) {
                    uint32_t v = 0;
                    if (!TrySafeReadUint32(m + o, &v)) continue;
                    if (v == reinterpret_cast<uintptr_t>(gunComp)) {
                        char b[40];
                        std::snprintf(b, sizeof(b), " direct+0x%X", o);
                        hits += b;
                        continue;
                    }
                    uint32_t num = 0;
                    if (!v || !TrySafeReadUint32(m + o + 4, &num) || num == 0 || num > 64) continue;
                    for (uint32_t q = 0; q < num * 64; q += 4) {
                        uint32_t w = 0;
                        if (!TrySafeReadUint32(reinterpret_cast<char*>(v) + q, &w)) break;
                        if (w == reinterpret_cast<uintptr_t>(gunComp)) {
                            int32_t nm = 0;
                            TrySafeReadUint32(reinterpret_cast<char*>(v) + q + 4, reinterpret_cast<uint32_t*>(&nm));
                            char b[120];
                            std::snprintf(b, sizeof(b), " array+0x%X[num %u] at byte %u (next name %s)", o, num, q,
                                          (nm > 0 && nm < 200000) ? vr::script::NameFromIndex(nm).c_str() : "?");
                            hits += b;
                        }
                    }
                }
                vr::log::Line(hits);
                // Nomi (FName) nell'attore arma attorno al componente mesh (+0x2C0..+0x330).
                std::string names = "ARMS: weapon actor names near the mesh comp:";
                for (uint32_t o = 0x2C0; o <= 0x330; o += 4) {
                    uint32_t v = 0;
                    if (!TrySafeReadUint32(reinterpret_cast<char*>(weapon) + o, &v) || v == 0 || v > 200000) continue;
                    std::string nm = vr::script::NameFromIndex(static_cast<int32_t>(v));
                    if (nm.empty() || nm == "None") continue;
                    char b[80];
                    std::snprintf(b, sizeof(b), " +0x%X=%s", o, nm.c_str());
                    names += b;
                }
                vr::log::Line(names);
            }
        }
        return false;
    }
    knownOff = foundOff;
    int32_t cur = 0;
    TrySafeReadUint32(entry + 4, reinterpret_cast<uint32_t*>(&cur));
    std::string curName = vr::script::NameFromIndex(cur);
    if (origName < 0 || (!wantLeft && cur != origName)) {
        // Prima volta (o nuova arma): il nome di adesso e' quello del gioco.
        if (origName < 0) {
            origName = cur;
            vr::log::Line("ARMS: weapon attached by the game to \"" + curName + "\" (attachments at mesh+0x" +
                          std::to_string(foundOff) + ", entry " + std::to_string(foundIdx) + ").");
        }
    }
    int32_t target = origName;
    std::string targetName = vr::script::NameFromIndex(origName);
    if (wantLeft) {
        // Nome speculare: Right -> Left, _R -> _L.
        std::string leftName = targetName;
        // Weapon_Left non e' rigido rispetto a Hand_Left (la relazione mano/arma
        // misurata ruotava di ~10 gradi a ogni misura, 22/10): l'arma va
        // direttamente sull'osso della mano sinistra.
        if (targetName == "Weapon_Right") leftName = "Hand_Left";
        size_t q = leftName == targetName ? leftName.find("Right") : std::string::npos;
        if (q != std::string::npos) leftName.replace(q, 5, "Left");
        else if (leftName.size() > 2 && leftName.compare(leftName.size() - 2, 2, "_R") == 0) leftName.replace(leftName.size() - 2, 2, "_L");
        else if (leftName.size() > 1 && leftName.back() == 'R') leftName.back() = 'L';
        int32_t leftIdx = leftName != targetName ? vr::script::NameIndex(leftName) : -1;
        // Esiste come osso o come socket?
        bool exists = false;
        if (leftIdx >= 0) {
            for (int i = 0; i < 256 && !exists; ++i) {
                struct {
                    int32_t index;
                    int32_t ret[2];
                } parms = {i, {0, 0}};
                if (!vr::script::CallFunction(mesh, "GetBoneName", &parms) || parms.ret[0] <= 0) break;
                if (parms.ret[0] == leftIdx) exists = true;
            }
            if (!exists) {
                struct {
                    int32_t name[2];
                    uint32_t ret;
                } sp = {{leftIdx, 0}, 0};
                if (vr::script::CallFunction(mesh, "GetSocketByName", &sp) && sp.ret) exists = true;
            }
        }
        if (exists) {
            target = leftIdx;
            targetName = leftName;
        } else {
            static bool logged = false;
            if (!logged) vr::log::Line("ARMS: no left-hand counterpart for \"" + targetName + "\" (tried \"" + leftName + "\"): gun stays in the right hand.");
            logged = true;
            return false;
        }
    }
    if (cur == target) return false;
    SafeCopy(entry + 4, &target, 4);
    vr::log::Line("ARMS: weapon re-attached from \"" + curName + "\" to \"" + targetName + "\".");
    return true;
}

void MeasureHandInWeapon(void* mesh, int side, void* gunComp) {
    float h[3][3], w[3][3];
    const char* hand = HandBone(side);
    const char* weap = side == 0 && gunComp ? "weapon mesh" : WeaponBone(side);
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

// Componente ancora attaccato a quel Pawn (UActorComponent.Owner +0x4C). WI-3:
// al caricamento di un livello GetBoneLocation su una mesh in demolizione
// ("None..SkeletalMeshComponent") ha sollevato un'eccezione, e CallFunction
// disattiva quella funzione per tutta la sessione (bodylog, armdebug, misure
// delle braccia). Le chiamate di ogni tick passano da qui prima.
// Anche attaccato (UActorComponent.bAttached, bit 0 di +0x50: lo stesso test di
// BeginDeferredUpdateTransform 0x00905AB0), nessuno dei due in demolizione
// (RF_PendingKill, bit 0x20000000 della parte alta di ObjectFlags +0xC) e con le
// ossa gia' composte (SpaceBases +0x274 non vuoto): l'eccezione e' tornata al
// primo tick dopo il caricamento, sulla mesh nuova non ancora in posa
// (GetBoneLocation legge SpaceBases senza controllo).
bool MeshOwnedBy(void* mesh, void* pawn) {
    uint32_t owner = 0, attached = 0, mf = 0, pf = 0, sbData = 0, sbNum = 0, skel = 0;
    return mesh && pawn && TrySafeReadUint32(static_cast<char*>(mesh) + 0x4C, &owner) && owner == reinterpret_cast<uintptr_t>(pawn) &&
           TrySafeReadUint32(static_cast<char*>(mesh) + 0x50, &attached) && (attached & 1) &&
           TrySafeReadUint32(static_cast<char*>(mesh) + 0xC, &mf) && !(mf & 0x20000000) &&
           TrySafeReadUint32(static_cast<char*>(pawn) + 0xC, &pf) && !(pf & 0x20000000) &&
           TrySafeReadUint32(static_cast<char*>(mesh) + 0x274, &sbData) && sbData &&
           TrySafeReadUint32(static_cast<char*>(mesh) + 0x278, &sbNum) && sbNum >= 100 && sbNum <= 1024 &&
           TrySafeReadUint32(static_cast<char*>(mesh) + 0x20C, &skel) && skel;  // SkeletalMesh (come HideBone 0x0064E290)
}

// --- "fpbody arms" (WI-3): busto, gambe e attrezzatura nascosti nel solo rendering ---
//
// HideBone non serve: azzera la scala in LocalAtoms e nasconde anche le ossa
// figlie (spina -> spalle -> braccia). Qui invece, per la sola mesh di Walker, si
// aggancia l'aggiornamento dei dati di rendering della mesh: FSkeletalMeshObject::
// Update (MeshObject a +0x26C, vtable +0x10, chiamato alla fine di
// USkeletalMeshComponent::UpdateTransform 0x00C33FD0 con (LOD, componente,
// morph attivi)), che copia sul thread di gioco le ossa composte (SpaceBases
// +0x274, FBoneAtom da 32 byte) per il thread di rendering. Prima della chiamata
// le ossa del busto, delle gambe e dell'attrezzatura vanno a scala 0 in Spine3,
// dopo si rimettono come erano. Collisione, GetBoneLocation, IK, props, colpi dei
// nemici e tutto il codice della mod vedono le ossa vere. (Prima versione: hook
// su tutto UpdateTransform, ma la stessa funzione spinge le matrici delle ossa
// anche nei corpi fisici: con "arms" gambe e collo non si colpivano piu',
// hittest -40/-16..-4/+28.) Le spalle (Shoulder_*) restano: chiuse sull'attacco
// del braccio facevano un cono della manica guardando in basso.
typedef void(__thiscall* MeshObjectUpdate_t)(void* self, int lod, void* comp, void* morphs);
MeshObjectUpdate_t g_MeshObjectUpdate_Original = nullptr;
void* g_meshObjectUpdateTarget = nullptr;
std::atomic<void*> g_armsOnlyMesh{nullptr};
constexpr uintptr_t kOffMeshObject = 0x26C;  // USkeletalMeshComponent.MeshObject (FSkeletalMeshObject*)
struct ArmsCollapse {
    void* mesh = nullptr;
    std::vector<int> bones;    // ossa da chiudere
    std::vector<int> anchors;  // punto di confine di ciascuna
    uint32_t calls = 0, applied = 0;
};
ArmsCollapse g_armsCollapse;  // thread di gioco

void ArmsCollapseResolve(void* mesh) {
    ArmsCollapse& a = g_armsCollapse;
    a = ArmsCollapse();
    a.mesh = mesh;
    std::vector<std::string> names;
    for (int i = 0; i < 400; ++i) {
        struct {
            int32_t index;
            int32_t ret[2];
        } bn = {i, {0, 0}};
        if (!vr::script::CallFunction(mesh, "GetBoneName", &bn) || bn.ret[0] <= 0) break;
        names.push_back(vr::script::NameFromIndex(bn.ret[0]));
    }
    auto idx = [&](const char* n) {
        for (size_t i = 0; i < names.size(); ++i)
            if (names[i] == n) return static_cast<int>(i);
        return -1;
    };
    const int spine3 = idx("Spine3");
    if (spine3 < 0) {
        vr::log::Line("BODY: fpbody arms not available (Spine3 bone not found), full body kept");
        return;
    }
    for (size_t i = 0; i < names.size(); ++i) {
        const std::string& n = names[i];
        if (n == "Root" || n == "Hips" || n.rfind("Spine", 0) == 0 || n == "Neck" || n == "head" || n.rfind("FACE_", 0) == 0 ||
            n.rfind("Leg_", 0) == 0 || n.rfind("Knee", 0) == 0 || n.rfind("Ankle_", 0) == 0 || n.rfind("Ball_", 0) == 0 ||
            n.rfind("SEC_", 0) == 0) {
            a.bones.push_back(static_cast<int>(i));
            a.anchors.push_back(spine3);
        }
    }
    vr::log::Line("BODY: fpbody arms: " + std::to_string(a.bones.size()) + " of " + std::to_string(names.size()) +
                  " bones collapsed in rendering only (torso, legs, gear)");
}

// --- Collo nel solo rendering (WI-3 fix 2, review media) --------------------
//
// HideBoneByName("Neck") (scala 0 nelle LocalAtoms: anche i corpi fisici, testa
// non colpibile, decisione dell'utente) chiude sul perno del Neck tutti i vertici
// pesati sul collo: la sciarpa si strappa in fili (1,15-1,45 m a -60/-85) e resta
// l'apertura del colletto con l'interno rosa (1,85 m a -85, accucciati col
// tasto). Qui, solo per i dati del rendering (stesso hook di "fpbody arms"), il
// Neck e FACE_THRO tornano a scala vr::control::g_neckSize ("necksize", 1 = collo
// intero) sul loro perno vero (la rotazione e la traslazione in SpaceBases sono
// quelle vere, solo la scala e' 0), e testa + ossa del viso si chiudono in un
// punto sulla cima del collo (posizione della testa nello scheletro di
// riferimento): il collo resta chiuso da un cappuccio, la sciarpa non si
// deforma. Colpi, fisica, GetBoneLocation e IK vedono ancora le ossa nascoste.
// Walker (26/10): Neck figlio di Spine3, figli head (3,76 unita' lungo l'asse
// del collo) e FACE_THRO; 34 ossa FACE_* sotto head.
// Scala del collo nel rendering: vr::control::g_neckSize ("necksize", 0 = come HideBone: collo chiuso nel perno).
struct NeckRender {
    void* mesh = nullptr;
    int neck = -1, thro = -1;
    std::vector<int> headBones;  // head e discendenti (chiusi nel punto della testa)
    float headLocal[3] = {0.0f, 0.0f, 0.0f}, throLocal[3] = {0.0f, 0.0f, 0.0f};
    bool ok = false;
    uint32_t applied = 0;
};
NeckRender g_neckRender;  // thread di gioco
std::atomic<void*> g_neckRenderMesh{nullptr};

// Traslazione di riferimento (USkeletalMesh.RefSkeleton +0x88, FMeshBone da 80
// byte: nome +0, BonePos.Position +32, ParentIndex +64; letto il 26/10 dalla
// copia B, controllato sui nomi).
bool RefBoneLocal(void* mesh, int idx, int32_t nameIdx, float out[3], int* parent) {
    uint32_t skel = 0, data = 0, num = 0;
    int32_t nm = 0;
    if (!TrySafeReadUint32(static_cast<char*>(mesh) + 0x20C, &skel) || !skel ||
        !TrySafeReadUint32(reinterpret_cast<char*>(skel) + 0x88, &data) || !data ||
        !TrySafeReadUint32(reinterpret_cast<char*>(skel) + 0x8C, &num) || idx < 0 || static_cast<uint32_t>(idx) >= num || num > 1024)
        return false;
    char* e = reinterpret_cast<char*>(data) + idx * 80;
    if (!SafeCopy(&nm, e, 4) || nm != nameIdx || !SafeCopy(out, e + 32, 12) || !SafeCopy(parent, e + 64, 4)) return false;
    const float l = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    return l > 0.5f && l < 15.0f;
}

void NeckRenderResolve(void* mesh) {
    NeckRender& nr = g_neckRender;
    nr = NeckRender();
    nr.mesh = mesh;
    std::vector<std::string> names;
    for (int i = 0; i < 400; ++i) {
        struct {
            int32_t index;
            int32_t ret[2];
        } bn = {i, {0, 0}};
        if (!vr::script::CallFunction(mesh, "GetBoneName", &bn) || bn.ret[0] <= 0) break;
        names.push_back(vr::script::NameFromIndex(bn.ret[0]));
    }
    int head = -1;
    for (size_t i = 0; i < names.size(); ++i) {
        if (names[i] == "Neck") nr.neck = static_cast<int>(i);
        else if (names[i] == "head") head = static_cast<int>(i);
        else if (names[i] == "FACE_THRO") nr.thro = static_cast<int>(i);
    }
    int pHead = -1, pThro = -1;
    const bool okHead = nr.neck >= 0 && head >= 0 && RefBoneLocal(mesh, head, vr::script::NameIndex("head"), nr.headLocal, &pHead) && pHead == nr.neck;
    const bool okThro = nr.thro < 0 || (RefBoneLocal(mesh, nr.thro, vr::script::NameIndex("FACE_THRO"), nr.throLocal, &pThro) && pThro == nr.neck);
    if (!okHead || !okThro) {
        vr::log::Line("BODY: neck render: skeleton not as expected (Neck/head/FACE_THRO), neck left hidden by HideBone");
        return;
    }
    nr.headBones.push_back(head);
    for (size_t i = 0; i < names.size(); ++i)
        if (names[i].rfind("FACE_", 0) == 0 && static_cast<int>(i) != nr.thro) nr.headBones.push_back(static_cast<int>(i));
    nr.ok = true;
    char b[220];
    std::snprintf(b, sizeof(b), "BODY: neck render: Neck %d, FACE_THRO %d, head + %u face bones closed at the head point (%.2f, %.2f, %.2f) of the Neck",
                  nr.neck, nr.thro, static_cast<unsigned>(nr.headBones.size() - 1), nr.headLocal[0], nr.headLocal[1], nr.headLocal[2]);
    vr::log::Line(b);
}

using vr::pure::QuatRotateVec;

void __fastcall Hooked_MeshObjectUpdate(void* self, void* /*edx*/, int lod, void* comp, void* morphs) {
    ArmsCollapse& a = g_armsCollapse;
    NeckRender& nr = g_neckRender;
    const bool doNeck = comp == g_neckRenderMesh.load(std::memory_order_relaxed) && comp == nr.mesh && nr.ok;
    const bool doArms = comp == g_armsOnlyMesh.load(std::memory_order_relaxed) && comp == a.mesh && !a.bones.empty();
    if (!doNeck && !doArms) {
        g_MeshObjectUpdate_Original(self, lod, comp, morphs);
        return;
    }
    uint32_t data = 0, num = 0;
    char* m = static_cast<char*>(comp);
    if (!TrySafeReadUint32(m + kOffMeshSpaceBases, &data) || !TrySafeReadUint32(m + kOffMeshSpaceBases + 4, &num) || !data ||
        num == 0 || num > 1024) {
        g_MeshObjectUpdate_Original(self, lod, comp, morphs);
        return;
    }
    char* sbN = reinterpret_cast<char*>(data);
    // Collo (prima delle braccia: con "arms" collo e testa sono comunque chiusi in Spine3).
    static std::vector<uint8_t> neckSaved;
    static std::vector<int> neckIdx;
    neckIdx.clear();
    if (doNeck) {
        neckIdx.push_back(nr.neck);
        if (nr.thro >= 0) neckIdx.push_back(nr.thro);
        neckIdx.insert(neckIdx.end(), nr.headBones.begin(), nr.headBones.end());
        bool okN = true;
        for (int i : neckIdx)
            if (static_cast<uint32_t>(i) >= num) okN = false;
        neckSaved.resize(neckIdx.size() * kBoneAtomSize);
        for (size_t i = 0; i < neckIdx.size() && okN; ++i) okN = SafeCopy(&neckSaved[i * kBoneAtomSize], sbN + neckIdx[i] * kBoneAtomSize, kBoneAtomSize);
        float neckAtom[8];
        if (okN) std::memcpy(neckAtom, &neckSaved[0], sizeof(neckAtom));
        // Solo se HideBone l'ha davvero chiuso (scala 0) e il quaternione e' sano.
        const float qn = okN ? neckAtom[0] * neckAtom[0] + neckAtom[1] * neckAtom[1] + neckAtom[2] * neckAtom[2] + neckAtom[3] * neckAtom[3] : 0.0f;
        if (!okN || neckAtom[7] != 0.0f || !(qn > 0.9f && qn < 1.1f)) {
            neckIdx.clear();
        } else {
            const float s = (std::max)(0.0f, (std::min)(1.0f, vr::control::g_neckSize.load()));
            float headPt[3], v[3], r[3];
            for (int k = 0; k < 3; ++k) v[k] = s * nr.headLocal[k];
            QuatRotateVec(neckAtom, v, r);
            for (int k = 0; k < 3; ++k) headPt[k] = neckAtom[4 + k] + r[k];
            float at[8];
            std::memcpy(at, neckAtom, sizeof(at));
            at[7] = s;
            SafeCopy(sbN + nr.neck * kBoneAtomSize, at, sizeof(at));
            if (nr.thro >= 0) {
                std::memcpy(at, &neckSaved[kBoneAtomSize], sizeof(at));  // rotazione vera di FACE_THRO
                for (int k = 0; k < 3; ++k) v[k] = s * nr.throLocal[k];
                QuatRotateVec(neckAtom, v, r);
                for (int k = 0; k < 3; ++k) at[4 + k] = neckAtom[4 + k] + r[k];
                at[7] = s;
                SafeCopy(sbN + nr.thro * kBoneAtomSize, at, sizeof(at));
            }
            const size_t firstHead = nr.thro >= 0 ? 2 : 1;
            for (size_t i = firstHead; i < neckIdx.size(); ++i) {
                std::memcpy(at, &neckSaved[i * kBoneAtomSize], sizeof(at));
                at[4] = headPt[0];
                at[5] = headPt[1];
                at[6] = headPt[2];
                at[7] = 0.0f;
                SafeCopy(sbN + neckIdx[i] * kBoneAtomSize, at, sizeof(at));
            }
            ++nr.applied;
        }
    }
    auto restoreNeck = [&]() {
        uint32_t d2 = 0;
        if (!neckIdx.empty() && TrySafeReadUint32(m + kOffMeshSpaceBases, &d2) && d2 == data)
            for (size_t i = 0; i < neckIdx.size(); ++i) SafeCopy(sbN + neckIdx[i] * kBoneAtomSize, &neckSaved[i * kBoneAtomSize], kBoneAtomSize);
    };
    if (!doArms) {
        g_MeshObjectUpdate_Original(self, lod, comp, morphs);
        restoreNeck();
        return;
    }
    ++a.calls;
    // Copia delle ossa toccate e dei punti di confine (letti prima di cambiare nulla).
    static std::vector<uint8_t> saved;
    static std::vector<float> anchorPos;
    const size_t n = a.bones.size();
    saved.resize(n * kBoneAtomSize);
    anchorPos.resize(n * 3);
    char* sb = reinterpret_cast<char*>(data);
    bool ok = true;
    for (size_t i = 0; i < n && ok; ++i) {
        if (static_cast<uint32_t>(a.bones[i]) >= num || static_cast<uint32_t>(a.anchors[i]) >= num) ok = false;
        else ok = SafeCopy(&saved[i * kBoneAtomSize], sb + a.bones[i] * kBoneAtomSize, kBoneAtomSize) &&
                  SafeCopy(&anchorPos[i * 3], sb + a.anchors[i] * kBoneAtomSize + 16, 12);
    }
    if (!ok) {
        g_MeshObjectUpdate_Original(self, lod, comp, morphs);
        restoreNeck();
        return;
    }
    for (size_t i = 0; i < n; ++i) {
        float atom[8];
        std::memcpy(atom, &saved[i * kBoneAtomSize], sizeof(atom));
        atom[4] = anchorPos[i * 3];
        atom[5] = anchorPos[i * 3 + 1];
        atom[6] = anchorPos[i * 3 + 2];
        atom[7] = 0.0f;  // scala
        SafeCopy(sb + a.bones[i] * kBoneAtomSize, atom, sizeof(atom));
    }
    ++a.applied;
    g_MeshObjectUpdate_Original(self, lod, comp, morphs);
    // Rimesse com'erano (stesso array: l'originale non lo rialloca), prima le
    // braccia (tornano ai valori del collo), poi il collo.
    uint32_t data2 = 0;
    if (TrySafeReadUint32(m + kOffMeshSpaceBases, &data2) && data2 == data)
        for (size_t i = 0; i < n; ++i) SafeCopy(sb + a.bones[i] * kBoneAtomSize, &saved[i * kBoneAtomSize], kBoneAtomSize);
    restoreNeck();
}

// Hook dell'aggiornamento del rendering della mesh (una volta, alla prima mesh di Walker).
bool EnsureMeshObjectUpdateHook(void* mesh) {
    if (!g_meshObjectUpdateTarget) {
        uint32_t mo = 0, vt = 0, fn = 0;
        if (!TrySafeReadUint32(static_cast<char*>(mesh) + kOffMeshObject, &mo) || !mo || !TrySafeReadUint32(reinterpret_cast<void*>(mo), &vt) ||
            !vt || !TrySafeReadUint32(reinterpret_cast<char*>(vt) + 0x10, &fn) || !fn)
            return false;
        g_meshObjectUpdateTarget = reinterpret_cast<void*>(fn);
        const bool ok = MH_CreateHook(g_meshObjectUpdateTarget, reinterpret_cast<void*>(&Hooked_MeshObjectUpdate),
                                      reinterpret_cast<void**>(&g_MeshObjectUpdate_Original)) == MH_OK &&
                        MH_EnableHook(g_meshObjectUpdateTarget) == MH_OK;
        char b[200];
        std::snprintf(b, sizeof(b), "BODY: skeletal mesh render update (MeshObject 0x%08X, vtable 0x%08X +0x10) at 0x%08X: hook %s", mo, vt, fn,
                      ok ? "on" : "FAILED");
        vr::log::Line(b);
        if (!ok) g_MeshObjectUpdate_Original = nullptr;
    }
    return g_MeshObjectUpdate_Original != nullptr;
}

// Collo nel solo rendering (fix 2): mesh di Walker in prima persona con la testa nascosta.
void UpdateNeckRenderMesh(void* mesh, void* pawn) {
    if (!mesh || vr::control::g_neckSize.load() <= 0.0f) {
        g_neckRenderMesh = nullptr;
        return;
    }
    if (!EnsureMeshObjectUpdateHook(mesh)) return;
    if (g_neckRender.mesh != mesh) {
        if (!MeshOwnedBy(mesh, pawn)) return;
        NeckRenderResolve(mesh);
    }
    g_neckRenderMesh = mesh;
}

// Chiamata dal corpo in prima persona (thread di gioco): hook e ossa alla prima
// mesh di Walker vista con "fpbody arms"; mesh attiva solo in quel modo.
void UpdateArmsOnlyMesh(void* mesh, void* pawn, bool want) {
    if (!want || !mesh) {
        g_armsOnlyMesh = nullptr;
        return;
    }
    if (!EnsureMeshObjectUpdateHook(mesh)) return;
    if (g_armsCollapse.mesh != mesh) {
        if (!MeshOwnedBy(mesh, pawn)) return;
        ArmsCollapseResolve(mesh);
    }
    g_armsOnlyMesh = mesh;
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
// Ancora del collo (WI-3): centro degli occhi e rotazione della testa rispetto
// al Pawn dell'ultimo tick disegnato (GetPlayerViewPoint, occhio destro), nel
// riferimento del Pawn di quel momento (avanti, destra, su; radianti FRotator).
struct AnchorEye {
    bool valid = false;
    void* pawn = nullptr;
    float eye[3] = {0.0f, 0.0f, 0.0f};
    float pitch = 0.0f, yaw = 0.0f, roll = 0.0f;
    double t = 0.0;
};
AnchorEye g_anchorEye;  // thread di gioco
// Diagnostica (bodylog): bersaglio e spostamento della mesh, Neck medio (riferimento del Pawn, mesh non spostata);
// fix 1: bersaglio dell'ancora prima di anello e portata, avanzamento per la portata, arretramento per l'anello.
struct BodyAnchorDiag {
    bool anchored;
    float target[2], cur[2], neck[3];
    float uncapped[2], reach, clear;
};
BodyAnchorDiag g_bodyAnchorDiag = {false, {0, 0}, {0, 0}, {0, 0, 0}, {0, 0}, 0.0f, 0.0f};
// Anello del collo (fix 1): apertura del colletto con testa e Neck nascosti,
// approssimata da un anello attorno al Neck (raggio, altezza sopra il Neck,
// margine; unita'). Vedi vr::pure::NeckRingClearance; anche nella riga BODY (ring).
// Raggio 4: con 6 (provato, 1,85 m) a -85 gradi e testa girata di 120 il corpo
// arretrava di 11 unita' lungo lo sguardo, cioe' di lato al corpo, e si vedeva
// la schiena di Walker; con 4 resta solo il bordo della sciarpa in fondo.
// Fix 2: il raggio si regola dall'ini ("neckring", Avanzate; predefinito 4).
constexpr float kNeckRingH = 1.0f, kNeckRingMargin = 1.0f;
inline float NeckRingR() { return vr::control::g_neckRingR.load(); }
// Portata del braccio dell'arma (fix 1), misurata dopo l'IK (UpdateSkelPose):
// avanzamento della mesh che serve (vr::pure::ReachForwardNeed) con lo
// spostamento della mesh di quel momento.
struct ReachNeed {
    bool valid = false;
    // armLen: portata (fix 2: lunghezza di riposo del braccio, senza armstretch); posedLen: braccio come disegnato
    float need = 0.0f, shiftX = 0.0f, armLen = 0.0f, dist = 0.0f, posedLen = 0.0f;
    double t = 0.0;
};
ReachNeed g_reachNeed;  // thread di gioco
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
        // Gia' accucciato (checkpoint caricato accucciato, WI-3): in piedi e' 20
        // unita' piu' alta (capsula 45 / 25); prima G restava 0 fino al primo
        // "in piedi" e poi la vista saltava di 20.
        uint32_t pf = 0;
        if (TrySafeReadUint32(static_cast<char*>(pawn) + 0x228, &pf) && (pf & 0x8)) standingH = h + 20.0f;
    }
    if (h > standingH) standingH = h;
    return standingH - h;
}

// Vista dell'accucciata (WI-3, bug 4): dal tick (UpdateCrouchView) per la vista
// (GetPlayerViewPoint) e l'abbassamento del corpo. g_crouchCompOn: la discesa
// del gioco si toglie dalla vista (crouchview auto, o real in piedi);
// g_crouchViewDrop: abbassamento V (unita', >= 0) per le accucciate non fatte
// col corpo. Pubblicato anche per la fisica (merge: escludere V dalla velocita'
// delle mani, come un passo della testa).
std::atomic<bool> g_crouchCompOn{true};
std::atomic<float> g_crouchViewDrop{0.0f};
std::atomic<float> g_crouchViewTarget{0.0f};      // V a rampa finita (fix 1: l'abbassamento del corpo rimette solo V - questo)
std::atomic<bool> g_crouchButtonFlag{false};      // m_wantsToCrouch acceso non dall'accucciata vera (fix 1: tasto o script)
std::atomic<bool> g_crouchOwnedByReal{false};  // accucciata del gioco chiesta dall'accucciata vera
std::atomic<float> g_crouchViewSpeed{0.0f};     // |dV/dt| (unita'/s), per la vignettatura

// Accucciata vera (physicalcrouch, 23/10): abbassandosi nella stanza Walker
// passa alla postura accucciata del gioco (YAnimtreeManager.PlayPosture con
// YPTID_Crouch): gambe piegate e bacino giu' con le animazioni Crc_* del gioco,
// invece della mesh in piedi spinta nel pavimento. La postura resta con sparo,
// ricarica, mira, granate e camminando; la camera del gioco non cambia (Pawn+40)
// e la capsula nemmeno. Prima la mesh scendeva di tutta la discesa della testa.
constexpr uintptr_t kOffPawnAnimtreeManager = 0xB30;  // YGamePawn.m_animtreeManager
constexpr uintptr_t kOffAtmCurrentPosture = 0x98;      // YAnimtreeManager.m_currentPosture (YE_PostureID)
constexpr uint8_t kPostureStand = 1, kPostureCrouch = 2;
// Testa di Walker nello spazio della mesh (unita' sopra l'origine della mesh,
// -1000 = non letta) e postura attuale: per tenere gli occhi fuori dal corpo
// (UpdateFirstPersonBody).
std::atomic<float> g_meshHeadUp{-1000.0f};
std::atomic<int> g_walkerPosture{0};
// Quanto la postura accucciata ha portato avanti la testa (unita', ~14): il
// corpo arretra di altrettanto, se no il collo (testa nascosta) finiva davanti
// agli occhi e si vedeva il corpo decapitato (utente 23/10).
std::atomic<float> g_crouchPoseFwd{0.0f};

bool PlayPosture(void* atm, uint8_t posture) {
    struct {
        uint8_t postureID;
        uint8_t pad[3];
        float blendInTime;
        uint32_t forcePosture;
        uint32_t disablePostureTransition;
        uint32_t ret;
    } parms = {posture, {0, 0, 0}, 0.0f, 0, 0, 0};
    return vr::script::CallFunction(atm, "PlayPosture", &parms) && parms.ret;
}

// Accucciata vera del gioco, come il tasto del pad (24/10, utente: "risulti
// basso solo per l'animazione, nei passaggi bassi non passi"):
// YPlayerController.GamePad_ToggleCrouch inverte m_wantsToCrouch (PC+0x5E4
// bit 0x20000) e il gioco fa tutto il resto: Pawn.bIsCrouched, capsula
// 45 -> 25 (passa sotto gli ostacoli bassi), YGamePawn.m_crouched, postura
// accucciata e velocita' da accucciato. PlayPosture da sola cambiava solo
// l'animazione.
constexpr uintptr_t kOffPcFlags5E4 = 0x5E4;
constexpr uint32_t kPcWantsToCrouch = 0x20000;  // YPlayerController.m_wantsToCrouch

bool GameWantsCrouch(void* pc) {
    uint32_t f = 0;
    return pc && TrySafeReadUint32(static_cast<char*>(pc) + kOffPcFlags5E4, &f) && (f & kPcWantsToCrouch);
}

bool ToggleGameCrouch(void* pc) {
    char parms[16] = {};
    return pc && vr::script::CallFunction(pc, "GamePad_ToggleCrouch", parms);
}

// Testa di Walker nello spazio della mesh (X = avanti dell'attore, -Y = su,
// Z = destra): non dipende dallo spostamento della mesh fatto qui sotto,
// niente retroazione.
bool MeshHeadPos(void* mesh, float* fwd, float* up) {
    struct {
        int32_t name[2];
        int32_t space;
        float ret[3];
    } hp = {{vr::script::NameIndex("head"), 0}, 1, {0, 0, 0}};
    if (hp.name[0] < 0 || !vr::script::CallFunction(mesh, "GetBoneLocation", &hp)) return false;
    *fwd = hp.ret[0];
    *up = -hp.ret[1];
    return true;
}

void UpdatePhysicalCrouch(void* pc, void* pawn) {
    static void* ownPawn = nullptr;
    // byUs: postura accucciata messa da noi senza l'accucciata del gioco (ripiego);
    // byUsGame: accucciata del gioco (GamePad_ToggleCrouch) chiesta da noi.
    static bool byUs = false, byUsGame = false, gameRecrouched = false, prevGameCrouch = false;
    // WI-3: accucciata (o rialzata) del gioco non nostra (tasto, script, riparo):
    // finche' il giocatore non torna in piedi davvero con Walker in piedi,
    // l'accucciata vera non la riprende e non la ribalta. Prima, accucciati col
    // tasto e poi davvero, rialzandosi Walker si rialzava; e con un riferimento
    // in piedi vecchio (seduti in modo "in piedi") il tasto per alzarsi veniva
    // annullato entro 0,3 s (riaccucciato da noi).
    static bool externalOwn = false;
    static double lastReq = 0.0, lastCrouchT = 0.0, stoodUpT = 0.0, lastT = 0.0, lastToggleT = -10.0;
    static float standFwd = 0.0f, fwdApplied = 0.0f;
    static bool haveStandFwd = false;
    g_meshHeadUp = -1000.0f;
    g_walkerPosture = 0;
    g_crouchOwnedByReal = false;
    g_crouchButtonFlag = false;
    if (!pawn || !PawnAlive(pawn)) {
        ownPawn = nullptr;
        g_crouchPoseFwd = fwdApplied = 0.0f;
        return;
    }
    if (pawn != ownPawn) {
        ownPawn = pawn;
        byUs = byUsGame = gameRecrouched = haveStandFwd = externalOwn = false;
        prevGameCrouch = GameWantsCrouch(pc);
        fwdApplied = 0.0f;
    }
    uint32_t atm = 0, mesh = 0;
    uint8_t posture = 0;
    if (!TrySafeReadUint32(static_cast<char*>(pawn) + kOffPawnAnimtreeManager, &atm) || !atm ||
        !SafeCopy(&posture, reinterpret_cast<char*>(atm) + kOffAtmCurrentPosture, 1)) {
        g_crouchPoseFwd = fwdApplied = 0.0f;
        return;
    }
    const double now = ProfMs() * 0.001;
    float dt = lastT > 0.0 ? static_cast<float>(now - lastT) : 0.0f;
    lastT = now;
    float head = 0.0f, headFwd = 0.0f;
    bool haveHead = TrySafeReadUint32(static_cast<char*>(pawn) + kOffPawnMesh, &mesh) && mesh &&
                    MeshOwnedBy(reinterpret_cast<void*>(mesh), pawn) && MeshHeadPos(reinterpret_cast<void*>(mesh), &headFwd, &head);
    if (haveHead) g_meshHeadUp = head;
    g_walkerPosture = posture;
    if (posture == kPostureCrouch) lastCrouchT = now;
    // Testa di Walker in piedi: posizione avanti media (in piedi ondeggia di
    // 1-2 unita'). Non durante le mosse animate (scavalcare, corda) ne' nel
    // secondo in cui si rialza.
    if (haveHead && posture == kPostureStand && !vr::control::g_inCover && !vr::control::g_gameLocked &&
        now - lastCrouchT >= 1.0) {
        standFwd = haveStandFwd ? standFwd + 0.05f * (headFwd - standFwd) : headFwd;
        haveStandFwd = true;
    }
    // Durante la postura accucciata e la transizione per rialzarsi (1 s).
    float fwdTarget = 0.0f;
    if (haveHead && haveStandFwd && !vr::control::g_inCover && now - lastCrouchT < 1.0) {
        float f = headFwd - standFwd;
        fwdTarget = f < 0.0f ? 0.0f : (f > 30.0f ? 30.0f : f);
    }
    // Al massimo 80 unita'/s: entrando in copertura da accucciati (il riparo
    // azzera la correzione) il corpo non salta avanti in un frame.
    float maxStep = 80.0f * (dt > 0.0f && dt < 0.1f ? dt : 0.011f);
    fwdApplied += (std::max)(-maxStep, (std::min)(maxStep, fwdTarget - fwdApplied));
    g_crouchPoseFwd = fwdApplied;
    // Accucciata del gioco cambiata non da noi (tasto del pad, script, uscita
    // da una copertura): non e' piu' nostra, la si lascia com'e' finche' non
    // ci si accuccia davvero (allora la riprendiamo) o si ripreme il tasto.
    const bool gameCrouch = GameWantsCrouch(pc);
    if (gameCrouch != prevGameCrouch) {
        if (now - lastToggleT > 0.5) {
            byUsGame = false;
            externalOwn = true;
            vr::log::Line(gameCrouch ? "CROUCH: the game crouched Walker (button or script), left as it is"
                                     : "CROUCH: the game stood Walker up");
        }
        prevGameCrouch = gameCrouch;
    }
    g_crouchOwnedByReal = byUsGame && gameCrouch;
    // Fix 1 (review media): per la vista dell'accucciata (CrouchView) il tasto e
    // gli script si riconoscono da m_wantsToCrouch acceso non da noi; la
    // copertura non lo tocca.
    g_crouchButtonFlag = gameCrouch && !byUsGame;
    uint32_t pawnFlags = 0;
    const bool pawnCrouched = TrySafeReadUint32(static_cast<char*>(pawn) + 0x228, &pawnFlags) && (pawnFlags & 0x8);  // bIsCrouched
    bool allowed = vr::control::g_physicalCrouch && vr::control::g_sixDof && !vr::control::g_seated &&
                   !vr::control::g_cinemaActive && !vr::control::g_gameLocked && !vr::control::g_inCover &&
                   PawnOnFirmGround(pawn) && (posture == kPostureStand || posture == kPostureCrouch);
    if (!allowed) {
        // Spenta dal menu con Walker accucciato da noi: si rialza. Fix 2: anche
        // passando ai seduti (la misura di "seated auto" dura 3 s: tornando a
        // sedere da in piedi l'accucciata vera l'aveva gia' accucciato, e da
        // seduti nessuno lo rialzava piu').
        if ((!vr::control::g_physicalCrouch || vr::control::g_seated) && now - lastReq > 0.3) {
            if (byUsGame && gameCrouch) {
                lastReq = lastToggleT = now;
                if (ToggleGameCrouch(pc))
                    vr::log::Line(vr::control::g_seated ? "CROUCH: seated play on, Walker stands (our game crouch undone)"
                                                        : "CROUCH: real crouch turned off, Walker stands (our game crouch undone)");
            } else if (byUs && posture == kPostureCrouch) {
                lastReq = now;
                PlayPosture(reinterpret_cast<void*>(atm), kPostureStand);
            }
            byUsGame = false;
        }
        byUs = false;
        return;
    }
    // 30 cm sotto l'altezza in piedi per accucciarsi, sopra i 20 cm per
    // rialzarsi (isteresi: niente su e giu' guardando a terra).
    float drop = -g_headOffset[2].load(), scale = vr::control::g_posScale;
    bool down = drop > (posture == kPostureCrouch ? 0.20f : 0.30f) * scale;
    if (down) gameRecrouched = false;  // la prossima accucciata vera riprende il controllo
    if (externalOwn) {
        // Esclusiva del tasto / script (WI-3): si libera solo con giocatore e Walker in piedi.
        if (!down && !gameCrouch && posture == kPostureStand) {
            externalOwn = false;
            vr::log::Line("CROUCH: you and Walker are standing, the real crouch is armed again");
        }
        return;
    }
    if (now - lastReq < 0.3) return;
    if (down && !gameCrouch && posture == kPostureStand) {
        lastReq = now;
        if (now - lastToggleT > 1.5) {
            // Prima l'accucciata vera del gioco; se dopo 0,3 s non e' partita
            // (il gioco la vieta qui) si ripiega sulla sola postura.
            lastToggleT = now;
            if (ToggleGameCrouch(pc)) {
                byUsGame = true;
                prevGameCrouch = GameWantsCrouch(pc);
                vr::log::Line("CROUCH: you crouched " + std::to_string(static_cast<int>(drop / scale * 100.0f)) +
                              " cm, Walker crouches (game crouch)");
            }
        } else if (PlayPosture(reinterpret_cast<void*>(atm), kPostureCrouch)) {
            byUs = true;
            vr::log::Line("CROUCH: the game refused its crouch here, Walker only bends (pose)");
        }
    } else if (!down && gameCrouch) {
        if (byUsGame) {
            lastReq = lastToggleT = now;
            if (ToggleGameCrouch(pc)) {
                byUsGame = byUs = false;
                stoodUpT = now;
                prevGameCrouch = GameWantsCrouch(pc);
                vr::log::Line("CROUCH: you stood up, Walker stands (game crouch)");
            }
        }
    } else if (!down && posture == kPostureCrouch && !gameRecrouched && !pawnCrouched) {
        // Accucciato dal gioco subito dopo averlo rialzato (soffitto basso,
        // scena): non si insiste fino alla prossima accucciata vera.
        if (!byUs && now - stoodUpT < 2.0) {
            gameRecrouched = true;
            vr::log::Line("CROUCH: the game keeps Walker crouched, left as it is");
            return;
        }
        lastReq = now;
        if (PlayPosture(reinterpret_cast<void*>(atm), kPostureStand)) {
            byUs = false;
            stoodUpT = now;
            vr::log::Line("CROUCH: you stood up, Walker stands");
        }
    }
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

// Abbassamento del corpo (23/10): l'altezza della mesh la compone il sistema
// di traslazione di YGamePawn (vedi la NOTA in UpdateFirstPersonBody), quindi
// si toglie da m_meshTranslationOffset, che il gioco applica entro un tick
// (misurato: -10 -> LocalToWorld -10 al tick dopo). Si tiene solo la propria
// parte: se il gioco riscrive il campo, il nuovo valore e' la sua base.
constexpr uintptr_t kOffPawnMeshTranslationOffset = 0x1574;  // YGamePawn.m_meshTranslationOffset
struct MeshLowering {
    void* pawn;
    float applied;  // unita' tolte all'offset del gioco
    float written;  // valore scritto nel campo
    float ramp;     // abbassamento voluto, a velocita' limitata
} g_meshLower = {nullptr, 0.0f, 0.0f, 0.0f};
// Diagnostica (bodylog, WI-3): occhio, testa di Walker, distanza in piedi e bersaglio dell'abbassamento.
float g_lowerDiag[4] = {0.0f, 0.0f, 0.0f, 0.0f};

void SetMeshLowering(void* pawn, float lower) {
    if (pawn != g_meshLower.pawn) g_meshLower = {pawn, 0.0f, 0.0f, 0.0f};
    float cur = 0.0f;
    if (!SafeCopy(&cur, static_cast<char*>(pawn) + kOffPawnMeshTranslationOffset, 4)) return;
    if (g_meshLower.applied != 0.0f && std::fabs(cur - g_meshLower.written) > 0.01f) g_meshLower.applied = 0.0f;
    if (lower == 0.0f && g_meshLower.applied == 0.0f) return;
    float v = cur + g_meshLower.applied - lower;
    if (SafeCopy(static_cast<char*>(pawn) + kOffPawnMeshTranslationOffset, &v, 4)) {
        g_meshLower.written = v;
        g_meshLower.applied = lower;
    }
}

// Occhi fuori dal corpo (23/10): inginocchiandosi, accucciandosi piu' della
// postura di Walker o chinandosi prima che si accucci, gli occhi arrivavano
// al collo e oltre (testa e collo nascosti: si vedeva l'interno del corpo,
// utente). Il corpo scende quanto serve perche' la testa di Walker resti
// almeno 10 unita' sotto gli occhi, o quanto da dritti in piedi se e' meno
// (giocatori bassi): dritti in piedi non cambia nulla. Non in copertura, sui
// mezzi o nelle mosse animate.
void UpdateMeshLowering(void* pawn, bool onMount) {
    static double lastT = 0.0;
    static float gapStand = 0.0f;
    static bool haveGapStand = false;
    static void* gapPawn = nullptr;
    const double now = ProfMs() * 0.001;
    float dt = lastT > 0.0 ? static_cast<float>(now - lastT) : 0.0f;
    lastT = now;
    if (pawn != gapPawn) {
        gapPawn = pawn;
        haveGapStand = false;
        if (g_meshLower.pawn != pawn) g_meshLower.ramp = 0.0f;
    }
    float target = 0.0f, headUp = g_meshHeadUp.load();
    float l2w[16], pl[3];
    if (!onMount && !vr::control::g_inCover && !vr::control::g_gameLocked && headUp > -999.0f && g_fpHeadMesh &&
        SafeCopy(l2w, static_cast<char*>(g_fpHeadMesh) + kOffCompLocalToWorld, sizeof(l2w)) &&
        SafeCopy(pl, static_cast<char*>(pawn) + kOffsetLocationX, 12)) {
        // Testa di Walker e occhi sopra il centro del Pawn, senza il nostro abbassamento.
        float mine = g_meshLower.pawn == pawn ? g_meshLower.applied : 0.0f;
        float headZ = l2w[14] - pl[2] + mine + headUp;
        // Occhi come nella vista (WI-3): con la discesa del gioco tolta (comp) e
        // l'abbassamento V delle accucciate non fatte col corpo. Prima senza: col
        // gioco accucciato gli occhi risultavano 20 unita' piu' bassi e il corpo
        // scendeva troppo accucciandosi davvero.
        float eyeZ = vr::control::g_fpEyeHeight + ViewHeightExtra() + g_headOffset[2].load() +
                     (g_crouchCompOn.load() ? GameCrouchDrop(pawn) : 0.0f) - g_crouchViewDrop.load();
        // Occhio vero dell'ultimo tick disegnato (WI-3), se c'e': la formula sopra
        // non conta smorzamento, collisione e muro (guardando giu' a -85 gradi da
        // seduti con "seated off" l'occhio finiva 4,5 unita' sotto il Neck di
        // Walker mentre la formula lo dava sopra, e il corpo non scendeva).
        // Fix 1 (review alta 1): portato a testa dritta sul perno del collo
        // (vr::pure::NeckPivotEyeZ). Chinare la testa abbassa l'occhio di ~10 unita'
        // ma lo porta avanti, davanti alla gola: il corpo non deve scendere. Prima
        // a -60/-85 scendeva di 5-9 unita' e da sopra si vedeva l'apertura del
        // collo nella meta' bassa della vista. Si abbassa solo se scende il collo
        // del giocatore (inginocchiarsi, accucciarsi, chinarsi col busto).
        const AnchorEye& aeL = g_anchorEye;
        const bool aeFresh = aeL.valid && aeL.pawn == pawn && now - aeL.t < 0.25;
        if (aeFresh && !(vr::control::g_wi3TestMask.load() & 2)) {
            const float sc = vr::control::g_posScale.load() > 0.0f ? vr::control::g_posScale.load() : 50.0f;
            eyeZ = (vr::control::g_wi3TestMask.load() & 4)
                       ? aeL.eye[2]  // collaudo: occhio vero senza il perno (come la build WI-3)
                       : vr::pure::NeckPivotEyeZ(aeL.eye[2], aeL.pitch, aeL.yaw, aeL.roll, vr::control::g_neckModelCm[0].load() * 0.01f * sc,
                                                 vr::control::g_neckModelCm[1].load() * 0.01f * sc);
        }
        // Dell'abbassamento V della vista (accucciata del tasto) si rimette solo
        // la parte di rampa non ancora finita: rialzandosi la vista risale in
        // crouchtime mentre Walker e' gia' in piedi (il corpo scendeva di 9-11
        // unita' per quella rampa), ma a rampa finita l'occhio vero vale. Fix 1
        // (review media): prima si rimetteva tutto V, e col tasto piu'
        // un'accucciata vera di 40 cm l'occhio stava 3 unita' sotto il collo di
        // Walker (crouch_wi3 caso D) senza che il corpo scendesse.
        eyeZ += (std::max)(0.0f, g_crouchViewDrop.load() - g_crouchViewTarget.load());
        float gap = eyeZ - headZ;
        // Distanza da dritti in piedi (entro 3 cm): dipende dall'altezza del giocatore.
        // WI-3: solo con la testa circa dritta (entro 20 gradi): guardando in basso
        // per qualche secondo il riferimento in piedi scende di 1 cm/s verso la
        // testa, la condizione sopra tornava vera e la distanza imparata andava a
        // zero (1,64 m: 6,6 -> -0,3 unita'), cosi' il corpo non scendeva piu' e a
        // -85 gradi gli occhi finivano nel colletto.
        const bool headLevel = !aeFresh || std::fabs(aeL.pitch) < 0.35f;
        if (g_walkerPosture == kPostureStand && g_headOffset[2].load() > -1.5f && headLevel) {
            gapStand = haveGapStand ? gapStand + 0.02f * (gap - gapStand) : gap;
            haveGapStand = true;
        }
        if (haveGapStand) {
            float t = (std::min)(10.0f, gapStand) - gap;
            target = t < 0.0f ? 0.0f : (t > 30.0f ? 30.0f : t);
        }
        g_lowerDiag[0] = eyeZ;
        g_lowerDiag[1] = headZ;
        g_lowerDiag[2] = haveGapStand ? gapStand : -99.0f;
        g_lowerDiag[3] = target;
    }
    // Al massimo 120 unita'/s (accucciandosi in fretta la testa scende a ~50).
    float maxStep = 120.0f * (dt > 0.0f && dt < 0.1f ? dt : 0.011f);
    g_meshLower.ramp += (std::max)(-maxStep, (std::min)(maxStep, target - g_meshLower.ramp));
    SetMeshLowering(pawn, g_meshLower.ramp);
}

void UpdateFirstPersonBody(void* pawn) {
    bool want = pawn && vr::control::g_firstPerson && !vr::control::g_cinemaActive && PawnAlive(pawn);
    uint32_t mesh = 0;
    if (want && !TrySafeReadUint32(static_cast<char*>(pawn) + kOffPawnMesh, &mesh)) want = false;
    if (g_fpPawn && (!want || pawn != g_fpPawn)) {
        if (StillAlive(g_fpPawn, g_fpPawnCls)) SetCylinders(g_fpPawn, g_fpSavedCylinders);
        // Corpo all'altezza del gioco (mai su Walker morto: ragdoll, crash 19/10).
        if (g_meshLower.pawn == g_fpPawn && StillAlive(g_fpPawn, g_fpPawnCls) && PawnAlive(g_fpPawn)) {
            SetMeshLowering(g_fpPawn, 0.0f);
            g_meshLower.ramp = 0.0f;
        }
        if (g_fpHeadMesh && StillAlive(g_fpHeadMesh, g_fpHeadMeshCls)) {
            SetHeadHidden(g_fpHeadMesh, false);
            float xyz[3] = {g_fpSavedMeshX, g_fpSavedMeshY, g_fpSavedMeshZ};
            SafeCopy(static_cast<char*>(g_fpHeadMesh) + kOffMeshTranslationX, xyz, 12);
        }
        g_fpPawn = nullptr;
        g_fpHeadMesh = nullptr;
    }
    if (!want) {
        UpdateArmsOnlyMesh(nullptr, nullptr, false);
        UpdateNeckRenderMesh(nullptr, nullptr);
        return;
    }
    if (!g_fpPawn) {
        if (!SafeCopy(g_fpSavedCylinders, static_cast<char*>(pawn) + kOffPawnInvisCylinders, 32)) return;
        g_fpPawn = pawn;
        g_fpPawnCls = ObjClass(pawn);
        vr::script::ClearObjectFaults();  // Walker nuovo: niente esclusioni ereditate da oggetti morti (fix 1)
        vr::log::Line(std::string("BODY: first person, Walker opaque and head hidden, body on the neck anchor (fpbody ") +
                      (vr::control::g_fpBodyMode.load() == 1 ? "arms" : "full") + ").");
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
        char bm[120];
        std::snprintf(bm, sizeof(bm), "BODY: Walker's mesh %p (pawn %p, posed %d)", g_fpHeadMesh, pawn, (int)MeshOwnedBy(g_fpHeadMesh, pawn));
        vr::log::Line(bm);
    }
    // "fpbody arms" (WI-3): busto e gambe nascosti nel solo rendering (UpdateTransform).
    UpdateArmsOnlyMesh(g_fpHeadMesh, pawn, g_fpHeadMesh && vr::control::g_fpBodyMode.load() == 1);
    // Collo e sciarpa interi nel rendering, testa chiusa sulla cima del collo (fix 2).
    UpdateNeckRenderMesh(g_fpHeadMesh, pawn);
    if (g_fpHeadMesh) {
        // Ancora del collo (WI-3, bug 5: "body too prominent", "ready for full VR").
        // Prima il corpo seguiva lo spostamento del visore 1:1 (girando o chinando
        // la testa gli occhi si muovono di 3-8 unita' attorno al collo e il petto
        // veniva trascinato sotto lo sguardo), 10 unita' avanti (bodyback -10) e
        // con correzioni a soglia (guardando in basso oltre 30 gradi, testa girata
        // oltre 35, postura accucciata). Ora il perno del collo del giocatore
        // (occhio del tick prima meno la testa ruotata applicata a "neckmodel",
        // vr::pure::NeckAnchorShift) porta il punto corrispondente di Walker: a
        // testa dritta i suoi occhi stanno sui tuoi, girando o chinando la testa il
        // corpo resta fermo, sporgendosi (anche seduti) ti segue. Il Neck di Walker
        // e' la media (0,5 s; 0,12 s nel secondo dopo un cambio di postura), non
        // l'oscillazione della camminata; al massimo 200 unita'/s e 60 di distanza.
        const double nowB = ProfMs() * 0.001;
        static double lastB = 0.0, postureT = -10.0;
        static float cur[2] = {0.0f, 0.0f}, neckF[3] = {0.0f, 0.0f, 0.0f}, standZ = 0.0f, standNeck[2] = {0.0f, 0.0f};
        static bool haveNeck = false, haveStandZ = false, haveStandNeck = false;
        static int lastPosture = 0;
        static void* anchorPawn = nullptr;
        const float dtB = lastB > 0.0 ? static_cast<float>(nowB - lastB) : 0.0f;
        lastB = nowB;
        if (pawn != anchorPawn) {
            anchorPawn = pawn;
            haveNeck = haveStandZ = haveStandNeck = false;
            cur[0] = cur[1] = 0.0f;
        }
        const bool onMount = !PawnOnFirmGround(pawn) || g_pawnSpecialAnim;
        uint32_t coverB = 0;
        const bool inCoverB = TrySafeReadUint32(static_cast<char*>(pawn) + 0x498, &coverB) && (coverB & 0xFF) != 0;
        // Neck di Walker nel riferimento del Pawn con la mesh non spostata: spazio
        // della mesh (X avanti, -Y su, Z destra) piu' la traslazione salvata; in
        // altezza dal LocalToWorld della mesh senza il nostro abbassamento.
        struct {
            int32_t name[2];
            int32_t space;
            float ret[3];
        } np = {{vr::script::NameIndex("Neck"), 0}, 1, {0, 0, 0}};
        float l2w[16], pl[3];
        if (np.name[0] >= 0 && MeshOwnedBy(g_fpHeadMesh, pawn) && vr::script::CallFunction(g_fpHeadMesh, "GetBoneLocation", &np) &&
            SafeCopy(l2w, static_cast<char*>(g_fpHeadMesh) + kOffCompLocalToWorld, sizeof(l2w)) &&
            SafeCopy(pl, static_cast<char*>(pawn) + kOffsetLocationX, 12)) {
            const float mine = g_meshLower.pawn == pawn ? g_meshLower.applied : 0.0f;
            const float n[3] = {g_fpSavedMeshX + np.ret[0], g_fpSavedMeshY + np.ret[2], l2w[14] - pl[2] + mine - np.ret[1]};
            const int posture = g_walkerPosture.load();
            if (posture != lastPosture) {
                lastPosture = posture;
                postureT = nowB;
            }
            const float tau = nowB - postureT < 1.0 ? 0.12f : 0.5f;
            if (!haveNeck || !(dtB > 0.0f && dtB < 0.5f)) {
                std::memcpy(neckF, n, sizeof(neckF));
                haveNeck = true;
            } else {
                const float k = 1.0f - std::exp(-dtB / tau);
                for (int i = 0; i < 3; ++i) neckF[i] += (n[i] - neckF[i]) * k;
            }
            // Neck in piedi per l'ancora: media lenta (3 s). Il Neck dal vivo si
            // sposta di qualche unita' a ogni nuova mira (la posa di mira del gioco
            // gira la schiena): seguirlo spostava la mesh per ~0,5 s dopo ogni cambio
            // di mira e l'arma disegnata in mano risultava girata fino a 1-3 gradi al
            // primo colpo (gunframe WI-4a: 15/18 pose, errore 1,3 gradi; con la
            // posizione fissa 18/18 entro 0,04). Il Neck dal vivo vale solo nel
            // secondo dopo un cambio di postura (accucciata).
            if (posture == kPostureStand && !onMount) {
                if (!haveStandNeck || !(dtB > 0.0f && dtB < 0.5f)) {
                    standNeck[0] = n[0];
                    standNeck[1] = n[1];
                    haveStandNeck = true;
                } else {
                    const float k3 = 1.0f - std::exp(-dtB / 3.0f);
                    standNeck[0] += (n[0] - standNeck[0]) * k3;
                    standNeck[1] += (n[1] - standNeck[1]) * k3;
                }
            }
            // Altezza del Neck in piedi (occhi dei seduti, ViewHeightExtra): media su ~1 s.
            if (posture == kPostureStand && !onMount && !inCoverB && !vr::control::g_gameLocked && nowB - postureT > 1.0 &&
                n[2] > 15.0f && n[2] < 45.0f) {
                standZ = haveStandZ && dtB > 0.0f && dtB < 0.5f ? standZ + (n[2] - standZ) * (1.0f - std::exp(-dtB / 1.0f)) : n[2];
                haveStandZ = true;
                g_walkerNeckStandZ = standZ;
            }
        }
        float target[2] = {0.0f, 0.0f}, uncapped[2] = {0.0f, 0.0f}, clearance = 0.0f;
        static float reachAssist = 0.0f;
        static double reachHoldT = 0.0;
        bool anchored = false;
        const AnchorEye& ae = g_anchorEye;
        if (!onMount && haveNeck && ae.valid && ae.pawn == pawn && nowB - ae.t < 0.25) {
            const float sc = vr::control::g_posScale.load() > 0.0f ? vr::control::g_posScale.load() : 50.0f;
            const float nf = vr::control::g_neckModelCm[0].load() * 0.01f * sc, nu = vr::control::g_neckModelCm[1].load() * 0.01f * sc;
            float eye[3] = {ae.eye[0], ae.eye[1], ae.eye[2]};
            float pitch = ae.pitch, yaw = ae.yaw, roll = ae.roll;
            if (!vr::control::g_bodyFollowsHead) {  // "bodyfollow off" (collaudi): corpo fermo sotto gli occhi del gioco
                eye[0] = g_fwdApplied.load();
                eye[1] = 0.0f;
                pitch = yaw = roll = 0.0f;
            }
            const bool useStand = haveStandNeck && lastPosture == kPostureStand && nowB - postureT > 1.0;
            const float* neckBase = useStand ? standNeck : neckF;
            vr::pure::NeckAnchorShift(eye, pitch, yaw, roll, nf, nu, neckBase, target);
            uncapped[0] = target[0];
            uncapped[1] = target[1];
            constexpr float kAnchorOld = 10.0f;  // posizione fissa di prima (bodyback -10)
            // Anello del collo fuori dalla vista (fix 1, review alta 1): per i
            // giocatori alti (occhi 10-20 unita' sopra il collo di Walker) guardando
            // in basso il corpo arretra lungo lo sguardo quanto basta perche'
            // l'apertura del colletto resti sotto il bordo inferiore del campo.
            if (vr::control::g_bodyFollowsHead && !(vr::control::g_wi3TestMask.load() & 8)) {
                float alphaDown = 0.8727f;  // 50 gradi, se la proiezione non e' nota
                vr::output::EyeProjection pjB;
                if (vr::output::OffCenterProjection(0, &pjB) && pjB.m11 > 0.0f) alphaDown = std::atan((1.0f + pjB.m21) / pjB.m11);
                const float mineB = g_meshLower.pawn == pawn ? g_meshLower.applied : 0.0f;
                const float neckAt[3] = {neckBase[0] + target[0], neckBase[1] + target[1], neckF[2] - mineB};
                clearance = vr::pure::NeckRingClearance(eye, pitch, yaw, alphaDown, neckAt, NeckRingR(), kNeckRingH, kNeckRingMargin);
                target[0] -= clearance * std::cos(yaw);
                target[1] -= clearance * std::sin(yaw);
            }
            // Portata del braccio dell'arma (fix 1, review alta 2): prima un limite
            // fisso (mai piu' di 4 unita' dietro +10) legava l'ancora in 81 pose su
            // 90 anche col fucile (avanti fermo a +6, chinarsi all'indietro non
            // spostava il corpo). Ora il corpo va avanti solo quanto serve perche'
            // il bersaglio della mano dell'arma resti 2 unita' dentro la lunghezza
            // del braccio (g_reachNeed, misurato dopo l'IK), mai oltre la posizione
            // di prima. Con la pistola la posa del gioco porta il Neck ~11 unita'
            // avanti e senza questo il braccio finiva teso al limite (arma girata
            // fino a 1,4 gradi, gunframe WI-4a 15/18 pose).
            if (vr::control::g_wi3TestMask.load() & 8) {
                // collaudo A/B: il limite fisso della build WI-3
                if (target[0] < kAnchorOld - 4.0f) target[0] = kAnchorOld - 4.0f;
            } else {
                // Fix 2 (review media): spinta voluta = spinta della mesh di adesso
                // (rispetto al bersaglio senza spinta, bodyback compreso) + margine
                // con segno: a mano vicina il margine e' negativo e la spinta scende
                // (dopo 0,5 s, 15 unita'/s). Prima il bisogno era >= 0 e la spinta,
                // salita una volta, restava (pistola: 5,4 unita' col braccio vicino).
                const ReachNeed rn = g_reachNeed;
                float want = 0.0f;
                if (rn.valid && nowB - rn.t < 0.3) want = rn.need + (rn.shiftX + vr::control::g_bodyBack.load() - target[0]);
                want = (std::max)(0.0f, (std::min)(want, (std::max)(0.0f, kAnchorOld - target[0])));
                if (want >= reachAssist) {
                    reachAssist = want;
                    reachHoldT = nowB;
                } else if (nowB - reachHoldT > 0.5) {
                    reachAssist = (std::max)(want, reachAssist - 15.0f * (dtB > 0.0f && dtB < 0.1f ? dtB : 0.011f));
                }
                target[0] += (std::min)(reachAssist, (std::max)(0.0f, kAnchorOld - target[0]));
            }
            target[0] -= vr::control::g_bodyBack.load();  // regolazione fine (unita' indietro, predefinito 0)
            // In copertura la posa del gioco e' contro il riparo: al massimo 10 unita'.
            const float tl = std::sqrt(target[0] * target[0] + target[1] * target[1]);
            if (inCoverB && tl > 10.0f) {
                target[0] *= 10.0f / tl;
                target[1] *= 10.0f / tl;
            }
            anchored = true;
        }
        // Su un mezzo (elicottero: il corpo finiva fuori dalla porta) e nelle mosse
        // animate (corda, scavalcare) il corpo resta dove lo mette il gioco.
        if (!anchored) reachAssist = 0.0f;
        if (onMount) {
            cur[0] = cur[1] = 0.0f;
        } else if (vr::control::g_wi3TestMask.load() & 1) {
            cur[0] = 10.0f;  // collaudo: posizione fissa di prima (bodyback -10)
            cur[1] = 0.0f;
        } else if (anchored) {
            // Banda morta di 0,5 unita': fermi, la mesh non insegue i decimi.
            const float e0 = target[0] - cur[0], e1 = target[1] - cur[1];
            if (e0 * e0 + e1 * e1 > 0.25f) vr::pure::NeckAnchorStep(cur, target, dtB, 200.0f, 60.0f);
        }
        float xy[2] = {g_fpSavedMeshX + cur[0], g_fpSavedMeshY + cur[1]};
        SafeCopy(static_cast<char*>(g_fpHeadMesh) + kOffMeshTranslationX, xy, 8);
        g_bodyAnchorDiag = {anchored, {target[0], target[1]}, {cur[0], cur[1]}, {neckF[0], neckF[1], neckF[2]},
                            {uncapped[0], uncapped[1]}, anchored ? reachAssist : 0.0f, clearance};
        // NOTA (misurato il 23/10): Translation.Z scritta qui non si vede (il sistema
        // di traslazione di YGamePawn la riscrive a ogni tick); l'altezza passa da
        // m_meshTranslationOffset (UpdateMeshLowering). Il rialzo del busto con
        // "crouchview real" scritto in Z era quindi senza effetto: tolto (WI-3).
        UpdateMeshLowering(pawn, onMount);
    }
}

// Vista dell'accucciata (WI-3, bug 4), una volta per tick prima dei due occhi:
// vr::pure::CrouchView decide se la discesa del gioco si toglie dalla vista e di
// quanto abbassarla per le accucciate non fatte col corpo (tasto, script, riparo
// da seduti), con la rampa di crouchtime.
void UpdateCrouchView(void* pawn) {
    static vr::pure::CrouchView cv;
    static double lastT = 0.0;
    static int lastSource = -1;
    const double now = ProfMs() * 0.001;
    vr::pure::CrouchViewIn in;
    in.dt = lastT > 0.0 ? static_cast<float>(now - lastT) : 0.0f;
    lastT = now;
    in.mode = vr::control::g_crouchViewMode.load();
    in.seated = vr::control::g_seated.load();
    in.active = pawn && vr::control::g_firstPerson && !vr::control::g_cinemaActive && PawnAlive(pawn) && PawnOnFirmGround(pawn);
    in.G = pawn ? GameCrouchDrop(pawn) : 0.0f;
    in.ownedByReal = g_crouchOwnedByReal.load();
    in.realDrop = vr::control::g_sixDof ? (std::max)(0.0f, -g_headOffset[2].load()) : 0.0f;
    in.standUp = 0.20f * vr::control::g_posScale.load();
    // Copertura letta dal Pawn (g_inCover e' aggiornato solo col visore): stato
    // di copertura (+0x498), mosse 19/20 (entrata e uscita, precedono lo stato),
    // mira dal riparo (+0x5F0 bit 0x800), sporgersi col corpo (g_coverPeek).
    // Fix 1 (review media): la sorgente la decide m_wantsToCrouch (tasto o
    // script, g_crouchButtonFlag da UpdatePhysicalCrouch); la copertura vista da
    // poco (1,5 s) vale solo da riparo a riparo senza tasto (rientrando nel
    // riparo dopo aver mirato lo stato arriva dopo la capsula: regress WI-3).
    uint32_t cover = 0, special = 0, aimFlags = 0;
    static double coverSeenMs = -1.0e9, coverEdgeMs = -1.0e9;
    static bool prevCoverNow = false;
    // Mossa speciale letta sempre (fix 2: prima solo se lo stato di copertura era 0).
    const bool haveSpecial = pawn && TrySafeReadUint32(static_cast<char*>(pawn) + 0x490, &special);
    const uint8_t specialMove = haveSpecial ? static_cast<uint8_t>((special >> 8) & 0xFF) : 0;
    const bool coverNow = pawn && ((TrySafeReadUint32(static_cast<char*>(pawn) + 0x498, &cover) && (cover & 0xFF) != 0) ||
                                   IsCoverLikeSpecialMove(specialMove) ||
                                   (TrySafeReadUint32(static_cast<char*>(pawn) + 0x5F0, &aimFlags) && (aimFlags & 0x800)) ||
                                   vr::control::g_coverPeek.load());
    // Fix 2 (review alta): colpi subiti, inciampi e scatti abbassano la capsula
    // senza che il giocatore faccia nulla: mai un'accucciata "script" (CrouchView).
    in.specialMove = specialMove;
    if (coverNow) coverSeenMs = ProfMs();
    if (coverNow != prevCoverNow) coverEdgeMs = ProfMs();
    prevCoverNow = coverNow;
    in.inCover = coverNow;
    in.coverRecent = coverNow || ProfMs() - coverSeenMs < 1500.0;
    in.coverEdge = ProfMs() - coverEdgeMs < 300.0;
    in.buttonCrouch = g_crouchButtonFlag.load();
    in.recentRecenter = ProfMs() - g_lastRecenterMs.load() < 500.0;
    in.rampTime = vr::control::g_crouchTime.load();
    const vr::pure::CrouchViewOut o = cv.Step(in);
    // Fix 2: capsula giu' per una mossa speciale, vista ferma (una riga per discesa).
    static bool specialDropLogged = false;
    if (in.active && in.mode == 0 && in.G > 2.0f && o.source == 0 && specialMove != 0 && !coverNow) {
        if (!specialDropLogged) {
            char b[200];
            std::snprintf(b, sizeof(b), "CROUCHVIEW: game drop %.1f during special move %u (%s), view kept still (not a crouch)", in.G,
                          static_cast<unsigned>(specialMove), SpecialMoveName(specialMove));
            vr::log::Line(b);
            specialDropLogged = true;
        }
    } else if (in.G <= 2.0f) {
        specialDropLogged = false;
    }
    // Fix 2 (review bassa): tasto + riparo, Walker alzato per mirare fuori.
    static bool lastPopup = false;
    if (cv.popup != lastPopup) {
        char b[200];
        std::snprintf(b, sizeof(b), "CROUCHVIEW: button crouch in cover, Walker %s (game drop %.1f): view kept, down %.1f units", cv.popup ? "up to aim out" : "back down",
                      in.G, o.v);
        vr::log::Line(b);
        lastPopup = cv.popup;
    }
    g_crouchCompOn = in.mode == 0 || (in.mode == 1 && !in.seated);
    const float prevV = g_crouchViewDrop.load();
    g_crouchViewSpeed = in.dt > 0.0f && in.dt < 0.25f && !o.blink ? std::fabs(o.v - prevV) / in.dt : 0.0f;
    g_crouchViewDrop = o.v;
    g_crouchViewTarget = in.mode == 0 && in.active ? cv.target : 0.0f;
    if (o.blink && vr::control::g_fades) vr::output::Blink(0.2f);
    if (o.source != lastSource && (o.source != 0 || lastSource > 0)) {
        char b[400];
        std::snprintf(b, sizeof(b),
                      "CROUCHVIEW: %s, view down %.1f units in %.2f s (real head %.0f cm, game drop %.1f, mesh lowered %.1f, %s, mode %s; "
                      "cover state %u move %u aim %d peek %d recent %d edge %d; wantsToCrouch not by us %d)",
                      vr::pure::CrouchView::Name(o.source), cv.target, cv.rampDur, in.realDrop / (std::max)(1.0f, in.standUp) * 20.0f,
                      in.G, g_meshLower.applied, in.seated ? "seated" : "standing",
                      in.mode == 0 ? "auto" : (in.mode == 1 ? "real" : "game"), cover & 0xFF, (special >> 8) & 0xFF,
                      (int)((aimFlags & 0x800) != 0), (int)vr::control::g_coverPeek.load(), (int)in.coverRecent, (int)in.coverEdge,
                      (int)in.buttonCrouch);
        vr::log::Line(b);
    }
    lastSource = o.source;
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
// Valori originali per arma (L11: azzerati e mai rimessi; servono anche per
// derivare il contraccolpo dai dati del gioco, R-W4).
struct SavedRecoil {
    uint32_t cls;
    uint8_t bytes[24];
};
std::map<void*, SavedRecoil> g_savedRecoil;
void RestoreGameRecoil() {
    for (auto& kv : g_savedRecoil)
        if (StillAlive(kv.first, kv.second.cls))
            SafeCopy(static_cast<char*>(kv.first) + kOffWeaponRecoilRange, kv.second.bytes, sizeof(kv.second.bytes));
    g_savedRecoil.clear();
}
void DisableGameRecoil(void* weapon) {
    if (!weapon) return;
    static const uint8_t zero[24] = {};
    if (!g_savedRecoil.count(weapon)) {
        SavedRecoil s = {ObjClass(weapon), {}};
        if (SafeCopy(s.bytes, static_cast<char*>(weapon) + kOffWeaponRecoilRange, sizeof(s.bytes)) &&
            std::memcmp(s.bytes, zero, sizeof(zero)) != 0) {
            g_savedRecoil[weapon] = s;
            // Valori del gioco nel log (R-W4: da qui il contraccolpo per classe).
            float f[6];
            std::memcpy(f, s.bytes, sizeof(f));
            char b[200];
            std::snprintf(b, sizeof(b), "RECOIL: %s game range (%.3f %.3f %.3f %.3f %.3f %.3f)",
                          vr::script::ClassName(weapon).c_str(), f[0], f[1], f[2], f[3], f[4], f[5]);
            vr::log::Line(b);
        }
    }
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
    ScanTimer scanTimer;  // L12
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
        if (vr::props::IsThrownBackGrenade(obj)) continue;  // voce 8: granata nemica rilanciata dalla mano
        std::string cls = vr::script::ClassName(obj);
        if (cls.find("Grenade") == std::string::npos && cls.find("grenade") == std::string::npos) continue;
        if (cls.find("Milkor") != std::string::npos) continue;  // lanciagranate: ci pensa RedirectNewProjectiles
        float v[3];
        if (!SafeCopy(v, static_cast<char*>(obj) + kOffActorVelocity, 12)) continue;
        float speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (speed < 100.0f) continue;  // ancora in mano
        // Forza del lancio dalla mano (R-W6): velocita' media della mano / 4 m/s,
        // tra 0,4 e 1,3 della velocita' del gioco (tetto: niente lanci da stadio).
        float hand = vr::control::g_throwSpeed.load();
        float k = hand > 0.0f ? hand / 4.0f : 1.0f;
        k = k < 0.4f ? 0.4f : (k > 1.3f ? 1.3f : k);
        float nv[3] = {dir[0] * speed * k, dir[1] * speed * k, dir[2] * speed * k};
        SafeCopy(static_cast<char*>(obj) + kOffActorVelocity, nv, 12);
        g_projRedirected.push_back(obj);
        if (g_projRedirected.size() > 64) g_projRedirected.erase(g_projRedirected.begin());
        handled = stamp;
        char buf[240];
        snprintf(buf, sizeof(buf), "GRENADE: %s thrown along the hand (%.2f, %.2f, %.2f), speed %.0f x %.2f (hand %.1f m/s)",
                 cls.c_str(), dir[0], dir[1], dir[2], speed, k, hand);
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
    ScanTimer scanTimer;  // L12
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
    // Proiettili vivi visti in questa scansione: alla fine la lista di quelli
    // gia' reindirizzati tiene solo questi. Un proiettile nuovo allo stesso
    // indirizzo di uno distrutto non veniva piu' reindirizzato (M18).
    std::vector<void*> seen;
    for (uint32_t i = 0; i < num; ++i) {
        uint32_t o = 0, inst = 0;
        if (!TrySafeReadUint32(reinterpret_cast<void*>(data + 4 * i), &o) || !o) continue;
        void* obj = reinterpret_cast<void*>(o);
        if (!IsProjectile(obj)) continue;
        if (std::find(g_projRedirected.begin(), g_projRedirected.end(), obj) != g_projRedirected.end()) seen.push_back(obj);
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
        if (vr::props::IsThrownBackGrenade(obj)) continue;  // voce 8: granata nemica rilanciata dalla mano
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
        seen.push_back(obj);
    }
    // Solo i proiettili ancora vivi restano "gia' reindirizzati" (M18).
    g_projRedirected.erase(std::remove_if(g_projRedirected.begin(), g_projRedirected.end(),
                                          [&](void* p) { return std::find(seen.begin(), seen.end(), p) == seen.end(); }),
                           g_projRedirected.end());
}

// Arma in mano = postazione fissa (torretta, minigun dell'elicottero): la mira
// col controller ruota la torretta (UpdateHeadTracking), non la camera.
std::atomic<bool> g_onTurret{false};
bool TurretAimActive() {
    return g_onTurret.load() && vr::control::g_turretAimHand && vr::control::g_aimHand && !vr::control::g_cinemaActive;
}
bool OnStationaryWeapon() { return g_onTurret.load(); }  // presa a due mani: veto (WI-4b)

// "twohandinfo" (25/10; WI-4b): una riga TWOHAND. Inizio come prima (i
// collaudi la leggono): synthleft che mette il palmo sinistro sulla presa
// disegnata (cm avanti, destra, su dal centro degli occhi, nel corpo; con la
// rotazione attuale della sinistra), la mano sinistra disegnata e la sua
// distanza dalla presa disegnata, la mano destra (punto del polso). Poi il
// risolutore della presa a due mani: distanza del palmo sinistro dall'astina,
// presa, modo, veto, correzione dell'arma, palmi e astina in cm del corpo.
void LogTwoHandInfo(bool canPose, bool canGrip, const float* gPos, const float* center, int32_t bodyYaw, float dist, bool gripOn) {
    const float scale = vr::control::g_posScale;
    const vr::twohand::Output& o = g_thOut;
    const vr::twohand::Solver& sv = g_thSolver;
    char tail[1100];
    std::snprintf(tail, sizeof(tail),
                  " | solver: mode %s, engaged %d by %s, veto %s, correction %.2f deg (two-hand target %.2f), left palm %.1f cm from "
                  "the barrel, %.1f along | palms R %.1f %.1f %.1f L %.1f %.1f %.1f cm | barrel grip target %.1f %.1f %.1f cm, segment "
                  "%.1f..%.1f cm (%s) | stock mount %.1f cm %.1f deg | left valid %d | left palm from the barrel of the drawn gun %.2f cm"
                  " | distance weight %.3f, near the gun %d (%.1f cm), fading %d, held cant %.1f deg, drawn left hand shift %.2f %.2f %.2f cm"
                  " | drawn gun grip at synthleft %.1f %.1f %.1f cm"
                  " | departure %.2f deg (gun hand %.2f), hands apart %.2f deg, past the barrel end %.1f cm, limit %s, regrip needed %d",
                  vr::control::g_holdMode == 1 ? "gunstock" : "normal", (int)o.engaged, o.engaged ? (sv.byGrip ? "grip" : "hold") : "-",
                  g_thVeto ? vr::twohand::VetoName(g_thVeto) : "none", o.corrDeg, o.targetDeg, o.dist < 1e8f ? o.dist * 100.0f : -1.0f,
                  o.along * 100.0f, g_thDiagPalmCm[0][0], g_thDiagPalmCm[0][1], g_thDiagPalmCm[0][2], g_thDiagPalmCm[1][0],
                  g_thDiagPalmCm[1][1], g_thDiagPalmCm[1][2], g_thDiagGuardCm[0], g_thDiagGuardCm[1], g_thDiagGuardCm[2],
                  g_thPub.guard.tMin * 100.0f, g_thPub.guard.tMax * 100.0f,
                  g_thPub.guard.valid ? (g_thPub.measured ? (g_thPub.guard.shortGun ? "cup" : "drawn grip") : "typical model") : "none",
                  o.mountCm, o.mountDeg, (int)g_thLeftValid, o.distOut < 1e8f ? o.distOut * 100.0f : -1.0f, o.weight,
                  (int)vr::control::g_leftNearBarrel.load(), o.nearDist < 1e8f ? o.nearDist * 100.0f : -1.0f, (int)o.fading,
                  sv.cant * 57.2957795f, o.drawShift[0] * 100.0f, o.drawShift[1] * 100.0f, o.drawShift[2] * 100.0f,
                  g_thDiagSynthLeftDrawn[0], g_thDiagSynthLeftDrawn[1], g_thDiagSynthLeftDrawn[2], o.departDeg, o.turnDeg, o.apartDeg,
                  o.endCm, o.limit[0] ? o.limit : "-", (int)sv.needRegrip);
    const float weight = o.engaged ? (sv.w < 1.0f ? vr::twohand::SmoothStep01(sv.w) : 1.0f) : 0.0f;
    char b[1900];
    if (!canPose || scale <= 0.0f) {
        std::snprintf(b, sizeof(b), "TWOHAND: no support grip on this weapon now (grip %s, short weapon %d, two-hand aim %d).%s",
                      canGrip ? "allowed" : "not allowed", (int)g_weaponShort.load(), (int)g_twoHandActive.load(), tail);
        vr::log::Line(b);
        return;
    }
    (void)gPos;
    (void)center;
    (void)bodyYaw;
    const float k = 100.0f / scale;
    std::snprintf(b, sizeof(b),
                  "TWOHAND: drawn grip at synthleft %.1f %.1f %.1f cm | left hand %.1f %.1f %.1f cm, %.1f cm from the grip "
                  "(left hand on the gun %d, same flag as the two-hand aim) | right hand %.1f %.1f %.1f cm | aim test: %.1f cm "
                  "along the barrel, %.1f cm from the barrel line, engage %.0f / release %.0f cm, two-hand aim %d weight %.2f, short weapon %d%s",
                  g_thDiagSynthLeft[0], g_thDiagSynthLeft[1], g_thDiagSynthLeft[2], g_leftOffset[0].load() * k, g_leftOffset[1].load() * k,
                  g_leftOffset[2].load() * k, dist * k, (int)gripOn, g_handOffset[0].load() * k, g_handOffset[1].load() * k,
                  g_handOffset[2].load() * k, o.along * 100.0f, o.dist < 1e8f ? o.dist * 100.0f : -1.0f, sv.p.engageM * 100.0f,
                  sv.p.releaseM * 100.0f, (int)g_twoHandActive.load(), weight, (int)g_weaponShort.load(), tail);
    vr::log::Line(b);
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
        // Voce 5: niente mano disegnata fuori dal controller (scena bloccata,
        // postazione fissa, morte): stato e distanza a zero (revisione 26/10).
        vr::props::RenderHandNone(1);
        vr::control::g_supportHandSepCm = 0.0f;
        g_handWorldValid[0] = g_handWorldValid[1] = false;  // niente mani: gli oggetti in mano cadono
        return;
    }
    {
        // Mesh nuova, o la stessa su cui l'aggancio e' fallito: si riprova
        // ogni secondo (L10: prima mai piu', finche' le braccia non venivano
        // spente e riaccese), senza loggare a ogni tentativo.
        static ULONGLONG failedAt = 0;
        static void* failedMesh = nullptr;
        bool retry = failedMesh == reinterpret_cast<void*>(mesh) && !g_rig.ctrl[0] && GetTickCount64() - failedAt > 1000;
        // Cambio di mano dominante (armswap): i controlli IK vanno ripresi con i nomi scambiati.
        static bool rigSwapped = false;
        bool swapChanged = rigSwapped != ArmsSwapped();
        if (swapChanged) {
            rigSwapped = ArmsSwapped();
            vr::log::Line(std::string("ARMS: dominant hand changed, IK controls re-acquired (") + (rigSwapped ? "left-handed, arms swapped" : "right-handed") + ").");
        }
        if ((reinterpret_cast<void*>(mesh) != g_rig.mesh || retry || swapChanged) && !AcquireArms(reinterpret_cast<void*>(mesh))) {
            ReleaseArms();
            g_rig.mesh = reinterpret_cast<void*>(mesh);
            if (failedMesh != reinterpret_cast<void*>(mesh)) vr::log::Line("ARMS: IK controls not found on this mesh, retrying every second.");
            failedMesh = reinterpret_cast<void*>(mesh);
            failedAt = GetTickCount64();
            return;
        }
        if (g_rig.ctrl[0]) failedMesh = nullptr;
    }
    if (!g_rig.ctrl[0]) return;
    if (!StillAlive(g_rig.mesh, g_rig.meshCls) || !StillAlive(g_rig.ctrl[0], g_rig.ctrlCls[0])) {
        // Oggetti distrutti (livello scaricato): si abbandonano senza scriverci.
        for (auto& c : g_rig.ctrl) c = nullptr;
        g_rig.mesh = nullptr;
        g_armsActive = false;
        return;
    }
    // WI-3 fix 1: al primo tick dopo un caricamento la mesh puo' essere ancora senza
    // ossa o di un altro Pawn: niente interrogazioni delle ossa in questo tick.
    if (!MeshOwnedBy(g_rig.mesh, pawn)) return;
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
    if (vr::control::g_armBonesRequested.exchange(false)) {
        LogBones(g_rig.mesh);
        // Diagnostica: vtable dei controlli (limb e single bone) per trovare
        // CalculateNewBoneTransforms in Ghidra (corpo specchiato, 22/10).
        for (int ci : {0, 2}) {
            if (!g_rig.ctrl[ci]) continue;
            uint32_t vt = 0;
            if (!TrySafeReadUint32(g_rig.ctrl[ci], &vt) || !vt) continue;
            std::string line = std::string("VTABLE ") + (ci == 0 ? "limb" : "single") + " @" + std::to_string(vt) + ":";
            for (int k = 0; k < 110; ++k) {
                uint32_t fn = 0;
                if (!TrySafeReadUint32(reinterpret_cast<void*>(vt + 4 * k), &fn)) break;
                char h[16];
                std::snprintf(h, sizeof(h), " %x", fn);
                line += h;
            }
            vr::log::Line(line);
        }
    }
    {
        // Modello dell'arma in mano (indipendente dalla posa dell'arma): osso
        // del caricatore durante la ricarica fisica (R-W7) e "weaponbones".
        char* camB = static_cast<char*>(g_cameraPtr.load());
        void* pawnB = HasCameraSignature(camB) ? PlayerPawnIfViewTarget(camB) : nullptr;
        void* gc = pawnB && PawnAlive(pawnB) ? CurrentGunComponent(pawnB) : nullptr;
        UpdateMagazineVisual(gc);
        if (vr::control::g_weaponBonesRequested.exchange(false)) {
            if (gc) {
                vr::log::Line("BONES: weapon " + g_weaponInfo.cls);
                LogBones(gc);
            } else {
                vr::log::Line("BONES: no weapon mesh in hand.");
            }
        }
    }
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
    if (UpdateGunAttachment(g_rig.mesh, gunComp, pawn)) {
        measureDue[0] = true;
        if (ArmsSwapped()) {
            g_relaxFrames = 20;
            g_handInWeaponValid[0] = false;
        }
    }
    if (ArmsSwapped() && g_relaxFrames > 0) {
        --g_relaxFrames;
        measureDue[0] = (g_relaxFrames == 0);  // misura solo all'ultimo frame di riposo
    }
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
        } hp = {{vr::script::NameIndex(HandBone(0)), 0}, 0, {0, 0, 0}};
        float gm[16];
        if (hp.name[0] >= 0 && vr::script::CallFunction(g_rig.mesh, "GetBoneLocation", &hp) &&
            SafeCopy(gm, static_cast<char*>(gunComp) + kOffCompLocalToWorld, sizeof(gm))) {
            float dx = gm[12] - hp.ret[0], dy = gm[13] - hp.ret[1], dz = gm[14] - hp.ret[2];
            gunInHand = dx * dx + dy * dy + dz * dz < 20.0f * 20.0f;
            if (!gunInHand && vr::control::g_fireLog) {
                static ULONGLONG lastLog = 0;
                if (GetTickCount64() - lastLog > 2000) {
                    lastLog = GetTickCount64();
                    char b[120];
                    snprintf(b, sizeof(b), "ARMS: gun mesh origin %.0f units from the hand bone: no grip measurements.",
                             std::sqrt(dx * dx + dy * dy + dz * dz));
                    vr::log::Line(b);
                }
            }
        }
    }
    if (++remeasure >= 30) {
        remeasure = 0;
        if (gunInHand && !ArmsSwapped()) measureDue[0] = true;  // mancini: relazione misurata una volta, a mano ferma
    }
    for (int side = 0; side < 2; ++side)
        if ((measureDue[side] || (!g_handInWeaponValid[side] && !(side == 0 && ArmsSwapped() && g_relaxFrames > 0)) || debugNow) &&
            (side == 1 || gunInHand || !gunComp) &&
            // Voce 5 ("solidhands"): con la sinistra disegnata fuori dal
            // controller le sue ossa non sono la posa del controller: niente
            // misura (entrerebbe in HandTarget(1)); con off resta 0, come prima.
            // Oltre 1 mm (revisione 26/10): con full e la mano vuota la sfera
            // da' quasi sempre qualche centesimo di mm (zona morta), e con "> 0"
            // la relazione della sinistra non si rimisurava mai.
            !(side == 1 && vr::control::g_supportHandSepCm.load() > 0.1f)) {
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
    // Review 2 del follow-up (27/10): quando UpdatePawnFacing gira il Pawn in questo Draw (corpo che segue la testa,
    // torna sotto la testa, stick della mod: lo yaw del PlayerController, a passi di almeno 32 unita'), l'arma
    // disegnata al tick dopo stava avanti di quel passo (misurato, GUNLOG per tick: errore della canna al Draw N+1 =
    // passo del Pawn al Draw N meno l'anticipo usato al Draw N; 0,62-0,75 gradi a 72 fps col corpo che gira), e con
    // "muzzleaim" colpi e mirino seguono la canna. Si converte con lo yaw del Pawn piu' quel passo (yaw attuale + ultimo
    // passo). Prima prova (yaw del PlayerController dopo la rotazione del tick): sbagliata, 0,97 gradi. "armslead off" =
    // come prima (collaudi).
    // f3 (27/10): l'anticipo e' sostituito dalla LocalToWorld corretta in UpdateSkelPoseThunk (esatta per ogni
    // passo, anche oltre 5 gradi nei tick lunghi) e dalle ossa rimesse girate in KeepGoodBones; qui si converte con
    // la rotazione del Pawn di adesso e si annota il riferimento delle mani rispetto a quella.
    // f4 (review del f3): se lo scatto della LocalToWorld non vale (posa del gioco che gira la mesh rispetto
    // all'attore), UpdateSkelPoseThunk non corregge nulla: si torna all'anticipo della review 2 (yaw attuale +
    // passo di UpdatePawnFacing, non oltre 5 gradi), invece di restare un passo indietro.
    {
        const MeshL2WSnap& sn = g_meshL2WSnap;
        const bool snapOk = sn.valid && sn.mesh == g_rig.mesh && sn.pawn == pawn;
        int32_t lead = vr::control::g_armsLead && !snapOk ? g_pawnFacingStep.load() : 0;
        if (std::abs(lead) > 910) lead = 0;
        actorRot[1] += lead;
        if (lead != 0) g_armsLeadUsed = lead;
    }
    {
        // Riferimento delle mani nel mondo (come HandTarget: centro + Rz(bodyYaw) x origine, yaw bodyYaw - riferimento).
        const float b = bodyYaw * (6.28318530718f / 65536.0f), cb = std::cos(b), sb = std::sin(b);
        const float org[3] = {g_trackOriginOff[0].load(), g_trackOriginOff[1].load(), g_trackOriginOff[2].load()};
        g_armsFrameW.org[0] = center[0] + org[0] * cb - org[1] * sb;
        g_armsFrameW.org[1] = center[1] + org[0] * sb + org[1] * cb;
        g_armsFrameW.org[2] = center[2] + org[2];
        g_armsFrameW.yaw = static_cast<int32_t>(bodyYaw - g_trackRefYaw.load()) * (6.28318530718f / 65536.0f);
        g_armsFrameValid = true;
        if (!vr::control::g_armsLead) g_armsLeadUsed = 0;
        if (vr::control::g_gunLogFrames.load() > 0) {
            ++g_skelPoseDiag.arms;
            g_skelPoseDiag.armsYaw = static_cast<int16_t>(actorRot[1] & 0xFFFF) * (360.0f / 65536.0f);
            g_skelPoseDiag.armsMesh = CompL2WYawDeg(g_rig.mesh);
        }
    }
    float af[3], ar[3], au[3];
    RotatorToAxes(actorRot[0], actorRot[1], actorRot[2], af, ar, au);
    bool debug = vr::control::g_armDebugFrames.exchange(0) > 0;
    float rOff[3] = {g_handOffset[0], g_handOffset[1], g_handOffset[2]};
    float rPos[3], rAxes[3][3];
    // Riferimento delle mani per gli oggetti tenuti (props.cpp: un giro a
    // scatti li porta con se'); non cambia nulla per l'arma. Con lo yaw del
    // riferimento della testa: le rotazioni del gioco compensate (copertura,
    // torretta) non sono salti.
    {
        const float org[3] = {g_trackOriginOff[0].load(), g_trackOriginOff[1].load(), g_trackOriginOff[2].load()};
        vr::props::SetHandFrame(center, bodyYaw, g_bodyYaw.load(), g_trackRefYaw.load(), org);
    }
    HandTarget(0, center, bodyYaw, rOff, g_bodyPitch.load() + g_aimDeltaPitch.load(), g_bodyYaw.load() + g_aimDeltaYaw.load(),
               g_handRoll.load(), rPos, rAxes);
    WriteHand(0, rPos, rAxes, actorLoc, af, ar, au, scale, debug);
    for (int i = 0; i < 3; ++i) g_handWorld[0][i] = rPos[i];
    std::memcpy(g_handAxesWorld[0], rAxes, sizeof(rAxes));
    g_handWorldValid[0] = true;
    g_handWorldValid[1] = false;
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
        if (leftCtrl) {
            HandTarget(1, center, bodyYaw, lOff, g_bodyPitch.load() + g_leftPitch.load(), g_bodyYaw.load() + g_leftYaw.load(),
                       g_leftRoll.load(), lPos, lAxes);
            for (int i = 0; i < 3; ++i) g_handWorld[1][i] = lPos[i];
            std::memcpy(g_handAxesWorld[1], lAxes, sizeof(lAxes));
            g_handWorldValid[1] = true;
            {
                // Direzione in cui punta la mano di supporto (ordini alla squadra,
                // guanti gravitazionali): il raggio di puntamento del controller,
                // come il laser del menu (WI-4a; prima il -Z del raw ruotato di
                // aimpitch: 39,4 gradi sopra il raggio col predefinito 0).
                float sf[3], sr[3], su[3];
                RotatorToAxes(g_bodyPitch.load() + g_leftAimPitch.load(), g_bodyYaw.load() + g_leftAimYaw.load(), 0, sf, sr, su);
                for (int i = 0; i < 3; ++i) vr::control::g_supportFwd[i] = sf[i];
                vr::control::g_supportFwdValid = true;
            }
        }
        // Relazione della presa fissata per arma: la prima si accetta quando
        // resta ferma 0,3 s fuori dalle mosse animate del gioco; poi solo
        // relazioni ferme entro 3 unita' dalla PRIMA. Le animazioni del gioco
        // che spostano IK_Hand_L (ricarica, corsa, lancio, copertura) non
        // trascinano via la mano dall'astina; per uscire dalla presa si usa
        // l'ultima relazione valida, senza scatti (audit 19/10).
        static void* relGun = nullptr;
        static bool relValid = false, haveLastRel = false, farLogged = false, relProvisional = false;
        static float cPos[3], cRot[3][3], candPos[3], firstPos[3];
        static double candSince = 0.0, relGunSince = 0.0, lastSpecialMs = 0.0;
        float relPos[3], relRot[3][3], gPos[3], gAxes[3][3];
        if (gunComp != relGun) {
            relGun = gunComp;
            relValid = false;
            candSince = 0.0;
            farLogged = false;
            relProvisional = false;
            relGunSince = gripNow;
        }
        bool fresh = gunInHand && SupportGripRelation(g_rig.mesh, relPos, relRot);
        if (fresh) {
            auto near3 = [](const float* a, const float* b, float lim) {
                float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
                return x * x + y * y + z * z < lim * lim;
            };
            // WI-4b: la relazione e' anche l'astina della presa a due mani (mira).
            // Prima, accettata, seguiva ogni variazione sotto 3 unita': durante le
            // raffiche l'animazione la spostava a passi piccoli e la presa finiva
            // 7 cm piu' indietro (collaudo 26/10: rel da 16 a 12 unita' avanti).
            // Vale solo una relazione ferma 0,3 s (entro 1,5 unita') e mai mentre
            // la sinistra tiene l'arma: con la presa a due mani l'astina non si
            // muove con l'animazione dello sparo.
            // Review WI-4b round 1 (M-b): dopo la prima, una relazione nuova
            // conta solo entro 3 unita' dalla PRIMA accettata (prima qualsiasi
            // posa ferma 0,3 s la sostituiva: la ricarica con la sinistra al
            // caricatore, una posa in copertura diventavano l'astina della mira);
            // la prima non durante una mossa animata del gioco (m_specialMove).
            uint8_t sm = 0;
            const bool special = SafeCopy(&sm, static_cast<char*>(pawn) + kOffPawnSpecialMove, 1) && sm != 0;
            if (special) lastSpecialMs = gripNow;
            // Review WI-4b round 2 (basso): la prima relazione e' "provvisoria"
            // se presa in copertura (+0x498), entro 1 s dal cambio d'arma (fine
            // dell'estrazione), entro 0,5 s da una mossa animata (scatto, entrata
            // in copertura) o durante la ricarica fisica: una posa di quei
            // momenti ferma 0,3 s restava l'astina della mira fino al cambio
            // d'arma. Una relazione ferma 1 s fuori da quei momenti la sostituisce
            // una volta (se dista piu' di 3 unita'; se no la conferma).
            uint32_t coverState = 0;
            const bool inCover = TrySafeReadUint32(static_cast<char*>(pawn) + 0x498, &coverState) && (coverState & 0xFF) != 0;
            const bool settled = !special && !inCover && (vr::control::g_leftHandBusy.load() & 1) == 0 &&
                                 gripNow - lastSpecialMs > 500.0 && gripNow - relGunSince > 1000.0;
            if (candSince > 0.0 && near3(relPos, candPos, 1.5f)) {
                const bool still = gripNow - candSince > 300.0 && !g_twoHandActive.load();
                if (still && !relValid && !special) {
                    relValid = true;
                    relProvisional = !settled;
                    std::memcpy(firstPos, relPos, sizeof(firstPos));
                    std::memcpy(cPos, relPos, sizeof(cPos));
                    std::memcpy(cRot, relRot, sizeof(cRot));
                    if (relProvisional) {
                        char b[200];
                        snprintf(b, sizeof(b),
                                 "ARMS: grip relation (%.1f %.1f %.1f) taken as provisional (%s): a still pose outside those moments "
                                 "replaces it once.",
                                 relPos[0], relPos[1], relPos[2],
                                 inCover ? "in cover" : (gripNow - relGunSince <= 1000.0 ? "weapon just drawn" : "right after a special move"));
                        vr::log::Line(b);
                    }
                } else if (still && relValid && near3(relPos, firstPos, 3.0f)) {
                    std::memcpy(cPos, relPos, sizeof(cPos));
                    std::memcpy(cRot, relRot, sizeof(cRot));
                    if (settled) relProvisional = false;
                } else if (relValid && relProvisional && settled && !g_twoHandActive.load() && gripNow - candSince > 1000.0) {
                    char b[220];
                    snprintf(b, sizeof(b),
                             "ARMS: provisional grip relation (%.1f %.1f %.1f) replaced by (%.1f %.1f %.1f), still for 1 s outside "
                             "cover, special moves and the draw.",
                             firstPos[0], firstPos[1], firstPos[2], relPos[0], relPos[1], relPos[2]);
                    vr::log::Line(b);
                    relProvisional = false;
                    std::memcpy(firstPos, relPos, sizeof(firstPos));
                    std::memcpy(cPos, relPos, sizeof(cPos));
                    std::memcpy(cRot, relRot, sizeof(cRot));
                } else if (relValid && !farLogged && gripNow - candSince > 3000.0 && !near3(relPos, firstPos, 3.0f)) {
                    farLogged = true;
                    char b[220];
                    snprintf(b, sizeof(b),
                             "ARMS: grip relation (%.1f %.1f %.1f) still for 3 s but %.1f units from the first one (%.1f %.1f %.1f): "
                             "the first stays until the weapon changes.",
                             relPos[0], relPos[1], relPos[2],
                             std::sqrt((relPos[0] - firstPos[0]) * (relPos[0] - firstPos[0]) + (relPos[1] - firstPos[1]) * (relPos[1] - firstPos[1]) +
                                       (relPos[2] - firstPos[2]) * (relPos[2] - firstPos[2])),
                             firstPos[0], firstPos[1], firstPos[2]);
                    vr::log::Line(b);
                }
            } else {
                candSince = gripNow;
                std::memcpy(candPos, relPos, sizeof(candPos));
            }
        }
        // Pistole e Uzi: nessuna animazione a due mani da misurare (IK_Hand_L
        // a 30+ unita'). Presa avvolgente sintetica: la sinistra accanto alla
        // destra, palmi uno verso l'altro (stesso orientamento della destra con
        // Y e Z invertiti: e' cosi' che le due mani stanno coi controller
        // affiancati), e sta attaccata finche' il controller sinistro e'
        // entro leftgrip. Prima la sinistra attraversava la pistola (22/10).
        bool synthCup = false;
        if (!relValid && gunInHand && g_weaponShort.load() && !ArmsSwapped()) {
            cPos[0] = vr::control::g_cupRel[0];
            cPos[1] = vr::control::g_cupRel[1];
            cPos[2] = vr::control::g_cupRel[2];
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) cRot[r][c] = (r == c) ? (r == 0 ? 1.0f : -1.0f) : 0.0f;
            synthCup = true;
        }
        if (relValid || synthCup) haveLastRel = true;
        // Con un oggetto di scena in mano la sinistra non va sull'arma.
        bool canGrip = vr::control::g_leftGrip && gunInHand && (relValid || synthCup) && !ArmsSwapped() &&
                       !vr::control::g_propHeld[1];
        bool canPose = canGrip || (gripBlend > 0.0f && haveLastRel);
        // Astina della presa a due mani (WI-4b) per UpdateHeadTracking: dalla
        // presa disegnata (relazione accettata, senza lo scorrimento per la
        // portata), come palmo sinistro voluto rispetto al palmo destro. Mancini:
        // nessuna presa disegnata (la sinistra disegnata non va sull'arma):
        // modello tipico specchiato, solo per la mira.
        {
            TwoHandPub pub;
            pub.gunInHand = gunInHand;
            pub.tick = g_thTicks;
            const int physGun = vr::control::PhysicalHand(0);
            const vr::pure::GunFrameParams gp = vr::control::GunFrameFor(physGun, true);
            const bool shortGun = g_weaponShort.load();
            float q[3] = {0.0f, 0.0f, 0.0f};
            bool haveQ = false;
            if (relValid || synthCup) {
                // Relazione negli assi dell'osso della mano (unita') -> assi dell'arma
                // (g_handInWeapon) -> assi del controller (armrot) = avanti, destra, su.
                float qw[3];
                for (int j = 0; j < 3; ++j) {
                    qw[j] = 0.0f;
                    for (int kk = 0; kk < 3; ++kk)
                        qw[j] += cPos[kk] * (g_handInWeaponValid[0] ? g_handInWeapon[0][kk][j] : (kk == j ? 1.0f : 0.0f));
                }
                const float mirror = physGun == 0 ? 1.0f : -1.0f, d2u = 65536.0f / 360.0f;
                float of[3], orr[3], ou[3];
                RotatorToAxes(static_cast<int32_t>(vr::control::g_armRotDeg[0] * d2u),
                              static_cast<int32_t>(vr::control::g_armRotDeg[1] * d2u * mirror),
                              static_cast<int32_t>(vr::control::g_armRotDeg[2] * d2u * mirror), of, orr, ou);
                const float toM = scale / vr::control::g_posScale;
                for (int i = 0; i < 3; ++i) q[i] = (qw[0] * of[i] + qw[1] * orr[i] + qw[2] * ou[i]) * toM;
                haveQ = true;
                pub.measured = true;
            } else if (gunInHand && ArmsSwapped()) {
                // Presa tipica dei fucili (16,2 / -3,1 / -5,6 unita', TECHNICAL.md) e coppa, specchiate.
                q[0] = shortGun ? 0.01f : 0.331f;
                q[1] = shortGun ? 0.09f : 0.096f;
                q[2] = shortGun ? -0.05f : 0.046f;
                haveQ = true;
            }
            if (haveQ) {
                float gir[3][4];
                vr::pure::GunRotInRaw(gp, gir);
                const float armOffM[3] = {vr::control::g_armOffsetCm[0] * 0.01f, vr::control::g_armOffsetCm[1] * 0.01f,
                                          vr::control::g_armOffsetCm[2] * 0.01f};
                float t[3];
                vr::twohand::PalmTargetFromDrawnGrip(q, gir, gp.left, gp.gripFix, armOffM, t);
                const float off[3] = {vr::control::g_twoHandGuardCm[0] * 0.01f, vr::control::g_twoHandGuardCm[1] * 0.01f,
                                      vr::control::g_twoHandGuardCm[2] * 0.01f};
                pub.guard = vr::twohand::MakeGuard(t[0] + off[0], t[1] + off[1], t[2] + off[2], shortGun);
                // Per TwoHandStep: l'astina con la rotazione attuale della sinistra (M4).
                pub.haveQ = true;
                pub.shortGun = shortGun;
                pub.gunLeft = gp.left;
                pub.gripFix = gp.gripFix;
                for (int i = 0; i < 3; ++i) {
                    pub.q[i] = q[i];
                    pub.armOffM[i] = armOffM[i];
                    pub.offM[i] = off[i];
                }
            }
            std::lock_guard<std::mutex> lock(g_thPubMutex);
            g_thPub = pub;
        }
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
        // Spostamento della sinistra disegnata dell'ultimo tick nella presa:
        // resta mentre la mano lascia l'arma (gripBlend scende), cosi' esce dal
        // punto del modello senza scatto. Review round 2 (basso): prima tornava
        // a 0 al primo tick fuori dalla presa con gripBlend ancora ~0,9: la mano
        // saltava di ~96% dello spostamento (2-4 cm col polso destro ruotato).
        static float lastDrawShift[3] = {0.0f, 0.0f, 0.0f};
        if (g_twoHandActive.load()) {
            for (int i = 0; i < 3; ++i) lastDrawShift[i] = g_thDrawShift[i].load();
        } else if (gripBlend <= 0.0f || !leftCtrl) {
            lastDrawShift[0] = lastDrawShift[1] = lastDrawShift[2] = 0.0f;
        }
        if (canPose && leftCtrl && (g_twoHandActive.load() || gripBlend > 0.0f)) {
            // Review WI-4b round 1 (M3): nella presa la sinistra disegnata va sul
            // modello dell'astina (inclinazione fissata alla presa), dove sta il
            // palmo vero: col polso destro ruotato dopo la presa la presa
            // disegnata gira attorno alla canna (2-4 cm a 20-30 gradi), il modello no.
            const float sh[3] = {lastDrawShift[0], lastDrawShift[1], lastDrawShift[2]};
            if (sh[0] != 0.0f || sh[1] != 0.0f || sh[2] != 0.0f) {
                float cf[3], cr[3], cu[3];
                RotatorToAxes(g_bodyPitch.load() + g_aimDeltaPitch.load(), g_bodyYaw.load() + g_aimDeltaYaw.load(), g_handRoll.load(), cf, cr,
                              cu);
                const float k = vr::control::g_posScale;
                for (int i = 0; i < 3; ++i) gPos[i] += (sh[0] * cf[i] + sh[1] * cr[i] + sh[2] * cu[i]) * k;
            }
        }
        if (vr::control::g_twoHandInfoRequest.exchange(false)) LogTwoHandInfo(canPose, canGrip, gPos, center, bodyYaw, dist, gripOn);
        // WI-4b: la sinistra disegnata va sull'arma quando la tiene la presa a
        // due mani (risolutore in UpdateHeadTracking: palmo entro twohandgrip cm
        // dall'astina col grip o fermo li', rilascio oltre twohandrelease), la
        // stessa prova di mira, dispersione e vibrazione. Prima un'altra prova
        // (20 cm dalla presa, leftgrip <cm>) che con la mira non era d'accordo.
        // Senza controller sinistro la sinistra tiene l'arma come prima.
        bool on = canGrip && (!leftCtrl || g_twoHandActive.load());
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
            vr::props::RenderHandNone(1);
            vr::control::g_supportHandSepCm = 0.0f;
        } else {
            // Voce 5 del piano ("solidhands"): la mano libera disegnata segue
            // la fisica (sull'oggetto tenuto, sul punto afferrato della
            // portiera, con "full" sulla sfera della mano vuota), solo fuori
            // dall'arma (gripBlend 0) e col controller. g_handWorld[1] resta il
            // bersaglio della fisica; con solidhands off la posa e' lPos, identica.
            float wPos[3], wAxes[3][3];
            float sep = 0.0f;
            if (gripBlend == 0.0f && leftCtrl) {
                float sh[3] = {0, 0, 0}, reach = 0.0f;
                const bool haveReach = vr::control::g_solidHands.load() > 0 && ArmReach(g_rig.mesh, 1, sh, &reach);
                sep = vr::props::RenderHandPose(1, lPos, lAxes, haveReach ? sh : nullptr, haveReach ? reach * 0.97f : 0.0f, wPos, wAxes);
                // Polso scritto oltre la portata intera: le ossa non ci arrivano,
                // niente misura del palmo (props.cpp MeasurePalm; revisione 26/10).
                const float dx = wPos[0] - sh[0], dy = wPos[1] - sh[1], dz = wPos[2] - sh[2];
                vr::props::SetHandBeyondReach(1, haveReach && dx * dx + dy * dy + dz * dz > reach * reach);
            } else {
                vr::props::RenderHandNone(1);
                std::memcpy(wPos, lPos, sizeof(wPos));
                std::memcpy(wAxes, lAxes, sizeof(wAxes));
            }
            vr::control::g_supportHandSepCm = sep;
            WriteHand(1, wPos, wAxes, actorLoc, af, ar, au, scale, debug);
            // Diagnostica WI-4b (campo lrel della riga VIEW): sinistra disegnata
            // meno destra disegnata. Dal merge (27/10) la sinistra disegnata e'
            // wPos (con solidhands puo' stare sull'oggetto tenuto, non su lPos).
            for (int i = 0; i < 3; ++i) g_diagLeftRel[i] = wPos[i] - rPos[i];
            leftReleased = false;
            g_leftDriven = true;
        }
    } else {
        gripOn = false;
        gripBlend = 0.0f;
        g_leftDriven = false;
        vr::props::RenderHandNone(1);
        vr::control::g_supportHandSepCm = 0.0f;
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

// --- Diagnostica della vista (25/10, collaudi senza visore dei bug del tester) ---
//
// Valori di un occhio alla fine di GetPlayerViewPoint, per viewlog / bodylog e
// per la riga JITTER del runtime finto.
// Diagnostica WI-2 della riga VIEW (occhio corrente): sorgente dell'arretramento
// (1 trace agli occhi, 2 trace basse), trace basse agganciate, correzione della
// collisione della testa (unita'), modo della vista (a terra), base del collo,
// offset del passaggio di modo, riferimento verticale della vista (m).
struct ViewDiagWi2 {
    int pullSrc = 0, latch = 0, viewFirm = 1, neck = 0;
    float corr[3] = {0, 0, 0}, blend[3] = {0, 0, 0};
};
ViewDiagWi2 g_viewDiagWi2;
struct ViewDiag {
    int eye;
    const float* loc;    // posizione finale dell'occhio
    const int32_t* rot;  // rotazione finale (FRotator)
    int32_t bodyYawTick, bodyPitchTick, bodyYaw;  // corpo del tick, corpo per lo spostamento della testa
    float head[3];       // spostamento della testa usato (avanti, destra, su; unita')
    const float* base;   // base della vista (centro della testa, senza la testa)
    float wallPull, headT;
    const float* eyeOff;
};

// "viewlog N": riga VIEW per occhio e tick, in forma chiave=valore per tools/viewcheck.py.
void LogViewLine(const ViewDiag& d) {
    char* camL = static_cast<char*>(g_cameraPtr.load());
    void* pawnL = HasCameraSignature(camL) ? PlayerPawnIfViewTarget(camL) : nullptr;
    float pl[3] = {0, 0, 0};
    int32_t pyaw = 0, baseYaw = 0, pcYaw = 0, pcPitch = 0;
    if (pawnL) {
        SafeCopy(pl, static_cast<char*>(pawnL) + 0x54, 12);
        SafeCopy(&pyaw, static_cast<char*>(pawnL) + 0x64, 4);
        uint32_t base = 0;  // Base (+0xAC): elicottero, mezzo, piattaforma
        if (TrySafeReadUint32(static_cast<char*>(pawnL) + 0xAC, &base) && base)
            SafeCopy(&baseYaw, reinterpret_cast<char*>(base) + 0x64, 4);
    }
    if (void* pcL = LivePlayerController()) {
        SafeCopy(&pcPitch, static_cast<char*>(pcL) + 0x60, 4);
        SafeCopy(&pcYaw, static_cast<char*>(pcL) + 0x64, 4);
    }
    float q[4] = {1, 0, 0, 0}, hp[3] = {0, 0, 0};
    bool poseValid = g_headSnap.valid && g_headSnap.poseValid;
    if (poseValid) {
        vr::pure::QuatFromPose(g_headSnap.pose.m, q);
        for (int i = 0; i < 3; ++i) hp[i] = g_headSnap.pose.m[i][3];
    }
    char b[2300];
    std::snprintf(b, sizeof(b),
                  "VIEW: tick=%u eye=%d t=%.3f mode=%d loc=(%.3f,%.3f,%.3f) rot=(%d,%d,%d) body=%d bodyp=%d bodyofs=%d "
                  "head=(%d,%d,%d) ho=(%.3f,%.3f,%.3f) ref=%.6f posref=(%.4f,%.4f,%.4f) hmd=%d hmdq=(%.6f,%.6f,%.6f,%.6f) "
                  "hmdp=(%.4f,%.4f,%.4f) sample=%.3f base=(%.3f,%.3f,%.3f) pawn=(%.3f,%.3f,%.3f) pawnyaw=%d fp=%.2f "
                  "eyeh=%.3f eyeauto=%.3f wall=%.3f headt=%.4f lean=(%.3f,%.3f) eyeoff=(%.3f,%.3f,%.3f) firm=%d turret=%d "
                  "crouch=%.3f seated=%d pc=(%d,%d) baseyaw=%d fpyaw=%d fpfwd=%.2f bodyhs=%d forced=%.3f "
                  "pullsrc=%d latch=%d hc=(%.3f,%.3f,%.3f) vfirm=%d neck=%d blend=(%.3f,%.3f,%.3f) vref=%.4f movskip=%u "
                  "planes=%d trus=%llu trn=%llu hand=(%.3f,%.3f,%.3f) lhand=(%.3f,%.3f,%.3f) aim=(%d,%d) th=%d thb=%.3f tref=%d "
                  "torg=(%.3f,%.3f,%.3f) lrel=(%.3f,%.3f,%.3f) hcside=%u forcedold=%.3f rstep=%u ablk=%d ppawn=%d",
                  g_stereoDraws.load(), d.eye, ProfMs(), vr::control::g_mode.load(), d.loc[0], d.loc[1], d.loc[2], d.rot[0], d.rot[1],
                  d.rot[2], d.bodyYawTick, d.bodyPitchTick, d.bodyYaw, HeadYaw(), HeadPitch(), HeadRoll(), d.head[0], d.head[1],
                  d.head[2], g_hmdYawReference, g_viewDiagPosRef[0], g_viewDiagPosRef[1], g_viewDiagPosRef[2], (int)poseValid, q[0],
                  q[1], q[2], q[3], hp[0], hp[1], hp[2], g_headSnap.valid ? g_headSnap.sampleMs : 0.0, d.base[0], d.base[1], d.base[2],
                  pl[0], pl[1], pl[2], pyaw, vr::control::g_fpForward.load(), ViewHeightExtra(), vr::control::g_eyeHeightAuto.load(),
                  d.wallPull, d.headT, g_coverLean[0].load(), g_coverLean[1].load(), d.eyeOff[0], d.eyeOff[1], d.eyeOff[2],
                  (int)PawnOnFirmGround(pawnL), (int)g_onTurret.load(), pawnL ? GameCrouchDrop(pawnL) : 0.0f,
                  (int)vr::control::g_seated.load(), pcPitch, pcYaw, baseYaw, g_fpYawApplied.load(), g_fwdApplied.load(),
                  g_bodyYawAtHeadSample, vr::control::g_forcedMotion.load(), g_viewDiagWi2.pullSrc, g_viewDiagWi2.latch,
                  g_viewDiagWi2.corr[0], g_viewDiagWi2.corr[1], g_viewDiagWi2.corr[2], g_viewDiagWi2.viewFirm, g_viewDiagWi2.neck,
                  g_viewDiagWi2.blend[0], g_viewDiagWi2.blend[1], g_viewDiagWi2.blend[2], g_viewDiagRefY,
                  g_viewTraceMovingSkips.load(), g_headPlanes.load(), static_cast<unsigned long long>(g_viewTraceUs.load()),
                  static_cast<unsigned long long>(g_viewTraceCalls.load()), g_handOffset[0].load(), g_handOffset[1].load(),
                  g_handOffset[2].load(), g_leftOffset[0].load(), g_leftOffset[1].load(), g_leftOffset[2].load(), g_aimDeltaYaw.load(),
                  g_aimDeltaPitch.load(), (int)g_twoHandActive.load(), g_thSolver.engaged ? vr::twohand::SmoothStep01(g_thSolver.w) : 0.0f, g_trackRefYaw.load(), g_trackOriginOff[0].load(),
                  g_trackOriginOff[1].load(), g_trackOriginOff[2].load(), g_diagLeftRel[0].load(), g_diagLeftRel[1].load(),
                  g_diagLeftRel[2].load(), g_headPushSideTicks.load(), g_forcedMotionOld.load(), g_roomSteps.load(),
                  g_bodyAlignBlock.load(), g_headPlanesPawn.load());
    vr::log::Line(b);
}

// "bodylog N [ossa]": riga BODY per tick (dopo l'occhio destro): occhi E e
// ossa di Walker nel riferimento del Pawn (avanti, destra, su), angolo sotto
// e di lato rispetto allo sguardo e se l'osso sta nel frustum (tangenti della
// proiezione dell'occhio sinistro, o FOV del gioco senza visore), e lo stato
// del corpo (mesh, abbassamento, accucciata, postura).
void LogBodyLine(const float* eyeCenter, const int32_t* rot) {
    char* camB = static_cast<char*>(g_cameraPtr.load());
    void* pawn = HasCameraSignature(camB) ? PlayerPawnIfViewTarget(camB) : nullptr;
    uint32_t mesh = 0;
    if (!pawn || !TrySafeReadUint32(static_cast<char*>(pawn) + kOffPawnMesh, &mesh) || !mesh ||
        !MeshOwnedBy(reinterpret_cast<void*>(mesh), pawn))
        return;
    float pl[3] = {0, 0, 0};
    int32_t pyaw = 0;
    SafeCopy(pl, static_cast<char*>(pawn) + 0x54, 12);
    SafeCopy(&pyaw, static_cast<char*>(pawn) + 0x64, 4);
    const float py = pyaw * (6.28318530718f / 65536.0f), cp = std::cos(py), sp = std::sin(py);
    auto pawnFrame = [&](const float* w, float* o) {
        const float dx = w[0] - pl[0], dy = w[1] - pl[1];
        o[0] = dx * cp + dy * sp;
        o[1] = -dx * sp + dy * cp;
        o[2] = w[2] - pl[2];
    };
    float vf[3], vrt[3], vu[3];
    RotatorToAxes(rot[0], rot[1], rot[2], vf, vrt, vu);
    // Tangenti: proiezione decentrata dell'occhio sinistro se nota, altrimenti FOV del gioco.
    float tl, tr, tu, td;
    vr::output::EyeProjection pj;
    if (vr::output::OffCenterProjection(0, &pj)) {
        tl = (-1.0f - pj.m20) / pj.m00;
        tr = (1.0f - pj.m20) / pj.m00;
        td = (-1.0f - pj.m21) / pj.m11;
        tu = (1.0f - pj.m21) / pj.m11;
    } else {
        float fov = vr::control::g_fovOverride > 0.0f ? vr::control::g_fovOverride.load() : 90.0f;
        float th = std::tan(fov * 0.5f * 0.01745329252f);
        float aspect = g_backBufferW && g_backBufferH ? static_cast<float>(g_backBufferW) / g_backBufferH : 1.0f;
        tl = -th;
        tr = th;
        tu = th / aspect;
        td = -th / aspect;
    }
    float ep[3];
    pawnFrame(eyeCenter, ep);
    std::string line;
    char b[256];
    std::snprintf(b, sizeof(b), "BODY: tick=%u eye=(%.2f,%.2f,%.2f) gaze=(%.2f,%.2f) pawnyaw=%.2f body=%.2f tan=(%.3f,%.3f,%.3f,%.3f)",
                  g_stereoDraws.load(), ep[0], ep[1], ep[2], static_cast<int16_t>(rot[0] & 0xFFFF) * (360.0f / 65536.0f),
                  static_cast<int16_t>(rot[1] - pyaw) * (360.0f / 65536.0f),
                  static_cast<float>(static_cast<uint16_t>(pyaw & 0xFFFF)) * (360.0f / 65536.0f),
                  static_cast<int16_t>(g_bodyYaw.load() - pyaw) * (360.0f / 65536.0f), tl, tr, tu, td);
    line = b;
    std::istringstream bones(vr::control::BodyLogBones());
    std::string name;
    while (bones >> name) {
        struct {
            int32_t name[2];
            int32_t space;
            float ret[3];
        } bp = {{vr::script::NameIndex(name.c_str()), 0}, 0, {0, 0, 0}};
        if (bp.name[0] < 0 || !vr::script::CallFunction(reinterpret_cast<void*>(mesh), "GetBoneLocation", &bp)) {
            line += " | " + name + "=missing";
            continue;
        }
        float bf[3];
        pawnFrame(bp.ret, bf);
        const float d[3] = {bp.ret[0] - eyeCenter[0], bp.ret[1] - eyeCenter[1], bp.ret[2] - eyeCenter[2]};
        const float df = d[0] * vf[0] + d[1] * vf[1] + d[2] * vf[2];
        const float dr = d[0] * vrt[0] + d[1] * vrt[1] + d[2] * vrt[2];
        const float du = d[0] * vu[0] + d[1] * vu[1] + d[2] * vu[2];
        const bool in = df > 1.0f && dr / df >= tl && dr / df <= tr && du / df >= td && du / df <= tu;
        std::snprintf(b, sizeof(b), " | %s=(%.2f,%.2f,%.2f) v=%.1f h=%.1f in=%d", name.c_str(), bf[0], bf[1], bf[2],
                      std::atan2(du, df) * 57.2957795f, std::atan2(dr, df) * 57.2957795f, (int)in);
        line += b;
    }
    // Fix 1 (review alta 1): anello del colletto attorno al Neck (8 punti a
    // NeckRingR(), kNeckRingH sopra) nel frustum: deve essere 0 guardando in basso.
    int ringIn = -1;
    {
        struct {
            int32_t name[2];
            int32_t space;
            float ret[3];
        } np = {{vr::script::NameIndex("Neck"), 0}, 0, {0, 0, 0}};
        if (np.name[0] >= 0 && vr::script::CallFunction(reinterpret_cast<void*>(mesh), "GetBoneLocation", &np)) {
            ringIn = 0;
            for (int k = 0; k < 8; ++k) {
                const float a = k * 0.785398163f;
                const float q[3] = {np.ret[0] + NeckRingR() * std::cos(a), np.ret[1] + NeckRingR() * std::sin(a), np.ret[2] + kNeckRingH};
                const float d[3] = {q[0] - eyeCenter[0], q[1] - eyeCenter[1], q[2] - eyeCenter[2]};
                const float df = d[0] * vf[0] + d[1] * vf[1] + d[2] * vf[2];
                const float dr = d[0] * vrt[0] + d[1] * vrt[1] + d[2] * vrt[2];
                const float du = d[0] * vu[0] + d[1] * vu[1] + d[2] * vu[2];
                ringIn += df > 1.0f && dr / df >= tl && dr / df <= tr && du / df >= td && du / df <= tu;
            }
        }
    }
    float mt[3] = {0, 0, 0}, mto = 0.0f;
    if (g_fpHeadMesh) SafeCopy(mt, static_cast<char*>(g_fpHeadMesh) + kOffMeshTranslationX, 12);
    SafeCopy(&mto, static_cast<char*>(pawn) + kOffPawnMeshTranslationOffset, 4);
    std::snprintf(b, sizeof(b), " | mesh=(%.2f,%.2f,%.2f) meshofs=%.2f lowered=%.2f crouch=%.2f posture=%d firm=%d fpbody=%d seated=%d",
                  mt[0], mt[1], mt[2], mto, g_meshLower.pawn == pawn ? g_meshLower.applied : 0.0f, GameCrouchDrop(pawn),
                  g_walkerPosture.load(), (int)PawnOnFirmGround(pawn), (int)vr::control::g_fpBody.load(), (int)vr::control::g_seated.load());
    line += b;
    // WI-3: ancora del collo (bersaglio e spostamento della mesh, Neck medio con la
    // mesh non spostata), Neck atteso N = Neck medio + spostamento, vista
    // dell'accucciata, modi.
    const BodyAnchorDiag& ad = g_bodyAnchorDiag;
    char ba[512];
    std::snprintf(ba, sizeof(ba),
                  " | anchor=%d target=(%.2f,%.2f) shift=(%.2f,%.2f) neckavg=(%.2f,%.2f,%.2f) neckexp=(%.2f,%.2f) crouchcomp=%d crouchv=%.2f "
                  "seatedmode=%d seateddet=%d fpbodymode=%d neckstand=%.2f eyesrc=(%.2f,%.2f,%.2f) lowerdiag=(%.2f,%.2f,%.2f,%.2f)",
                  (int)ad.anchored, ad.target[0], ad.target[1], ad.cur[0], ad.cur[1], ad.neck[0], ad.neck[1], ad.neck[2],
                  ad.neck[0] + ad.cur[0], ad.neck[1] + ad.cur[1], (int)g_crouchCompOn.load(), g_crouchViewDrop.load(),
                  vr::control::g_seatedMode.load(), (int)vr::control::g_seatedDetected.load(), vr::control::g_fpBodyMode.load(),
                  g_walkerNeckStandZ.load(), g_anchorEye.eye[0], g_anchorEye.eye[1], g_anchorEye.eye[2], g_lowerDiag[0], g_lowerDiag[1],
                  g_lowerDiag[2], g_lowerDiag[3]);
    line += ba;
    // Fix 1: bersaglio dell'ancora senza anello e portata, avanzamento per la portata
    // (misura: avanzamento che serve, distanza spalla -> bersaglio, braccio), arretramento
    // per l'anello, punti dell'anello nel frustum, modello del collo usato.
    const ReachNeed& rnB = g_reachNeed;
    std::snprintf(ba, sizeof(ba),
                  " | uncapped=(%.2f,%.2f) reach=%.2f clear=%.2f reachneed=(%.2f,%.2f,%.2f,%d) ring=%d neckmodel=(%.2f,%.2f) crouchvt=%.2f"
                  " armposed=%.2f necksize=%.2f neckrender=%u",
                  ad.uncapped[0], ad.uncapped[1], ad.reach, ad.clear, rnB.need, rnB.dist, rnB.armLen,
                  (int)(rnB.valid && ProfMs() * 0.001 - rnB.t < 0.3), ringIn, vr::control::g_neckModelCm[0].load(),
                  vr::control::g_neckModelCm[1].load(), g_crouchViewTarget.load(), rnB.posedLen, vr::control::g_neckSize.load(),
                  g_neckRender.applied);
    line += ba;
    vr::log::Line(line);
}

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
        // Camera del gioco (controller non tracciati): fatta col corpo di prima
        // delle rotazioni della mod di questo tick (corpo che segue la testa o
        // torna sotto: AddControllerYaw), mentre il riferimento della testa
        // si e' gia' spostato; senza questa parte la vista girava di un passo
        // per tick (0,6 gradi a 40 gradi/s, follow-up 27/10). Review del
        // follow-up: limitata a +-10 gradi (piu' di un tick di rotazione) invece
        // di sparire oltre 30 (salto), e usata anche per la posizione (bodyYaw).
        int32_t ownSinceCamera = 0;
        if (!hand && vr::control::g_aimFollowsBody && vr::control::g_camOwnYaw) {
            ownSinceCamera = static_cast<int16_t>(static_cast<uint16_t>(bodyYawT - g_bodyYawAtCamera.load()));
            const int32_t kOwnMax = 1820;  // 10 gradi
            ownSinceCamera = ownSinceCamera > kOwnMax ? kOwnMax : (ownSinceCamera < -kOwnMax ? -kOwnMax : ownSinceCamera);
        }
        int32_t bodyYaw = hand ? bodyYawT
                               : outRot[1] - (vr::control::g_aimFollowsBody ? 0 : HeadYaw()) + ownSinceCamera;
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
            outRot[1] += HeadYaw() + ownSinceCamera;  // rotazioni della mod dopo la camera (sopra)
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
            // Modo della vista di questo tick (WI-2): a terra (anche a una
            // torretta fissa) o su un mezzo. Letto sull'occhio sinistro e
            // riusato dal destro.
            static bool tickViewFirm = true;
            char* camV = static_cast<char*>(g_cameraPtr.load());
            void* pawnV = HasCameraSignature(camV) ? PlayerPawnIfViewTarget(camV) : nullptr;
            if (g_drawEye != 1) tickViewFirm = ViewOnFirmGround(pawnV);
            const bool viewFirm = tickViewFirm;
            // Su un mezzo la cabina non blocca i trace (collisione della testa
            // cieca): la testa sale al massimo 6 unita' sopra quella di Walker,
            // altrimenti alzandosi si usciva dal tetto dell'elicottero (19/10).
            if (pawnV && !viewFirm && u > 6.0f) u = 6.0f;
            // Spostamento dell'occhio di questo Draw: tolto qui e rimesso alla
            // fine, cosi' smorzamento dell'altezza e collisione lavorano sul
            // centro della testa. Prima l'occhio destro prendeva l'altezza
            // smorzata dell'occhio sinistro (testa inclinata: occhi alla
            // stessa altezza, disparita' verticale) e la collisione accorciava
            // la distanza tra gli occhi vicino ai muri (audit 19/10).
            float eyeOff[3] = {0.0f, 0.0f, 0.0f};
            if (g_drawEye == 0 || g_drawEye == 1) std::memcpy(eyeOff, g_eyeOffsetApplied[g_drawEye.load()], sizeof(eyeOff));
            float rawLoc[3] = {outLoc[0], outLoc[1], outLoc[2]};  // dal gioco (diagnostica camlog)
            // Altezza della testa vera sopra quella fissa del gioco ("height
            // auto"): aggiunta dopo lo smorzamento dell'altezza (WI-2). Prima
            // era nella base smorzata mentre la testa (u) no: quando il
            // riferimento verticale cambiava, extra e u si muovevano in versi
            // opposti nello stesso tick e la vista ondeggiava per 0,12 s.
            float extraZ = ViewHeightExtra();
            float base[3] = {outLoc[0] - eyeOff[0], outLoc[1] - eyeOff[1], outLoc[2] - eyeOff[2]};
            if (g_roomStepValid) {  // il POV del gioco precede il passo di questo tick (L8)
                base[0] += g_roomStepWorld[0];
                base[1] += g_roomStepWorld[1];
            }
            // Perno degli occhi (review WI-2, bug 2 causa F): ApplyFirstPerson mette gli occhi fp
            // avanti lungo lo yaw del Pawn al momento della copia della camera, un tick prima della
            // rotazione del corpo di questo tick (stick, corpo che segue la testa, scatti), mentre
            // la compensazione del perno (compensatePivot) conta gia' il corpo nuovo. Misurato
            // (26/10, viewlog): girando con lo stick a 150 gradi/s l'occhio saltava di 0,55 unita'
            // all'inizio e alla fine del giro, a ogni scatto di 30 gradi di 7,8 unita' per un tick.
            // Si porta l'avanzamento sullo yaw del Pawn di adesso (lo stesso che userebbe
            // ApplyFirstPerson: anche in copertura gli occhi seguono il Pawn, 18/10).
            float fpYawView = g_fpYawApplied.load() * (6.28318530718f / 65536.0f);
            {
                const float fwdA = g_fwdApplied.load();
                int32_t pawnYawNow = 0;
                if (vr::control::g_fpPivotFix && fwdA > 0.5f && pawnV && SafeCopy(&pawnYawNow, static_cast<char*>(pawnV) + 0x64, 4)) {
                    const float yNow = pawnYawNow * (6.28318530718f / 65536.0f);
                    const float dFp = vr::pure::WrapPi(yNow - fpYawView);
                    if (std::fabs(dFp) > 1e-5f && std::fabs(dFp) < 1.6f) {
                        base[0] += fwdA * (std::cos(yNow) - std::cos(fpYawView));
                        base[1] += fwdA * (std::sin(yNow) - std::sin(fpYawView));
                        fpYawView = yNow;
                    }
                }
            }
            // Su un mezzo (elicottero, camion) Walker e' seduto o inginocchiato:
            // l'altezza fissa sopra il Pawn metteva la vista nel tetto
            // dell'elicottero o sopra (19/10, nel visore). Si usa l'altezza della
            // sua testa: base del collo + 12 unita' (testa e collo sono nascosti
            // con scala 0, quindi "head" coincide con l'attacco del collo).
            bool neckBase = false;  // base presa dal collo di Walker in questo tick (diagnostica, passaggio di modo)
            {
                void* pawnM = pawnV;
                uint32_t meshM = 0;
                if (pawnM && !viewFirm && TrySafeReadUint32(static_cast<char*>(pawnM) + kOffPawnMesh, &meshM) && meshM) {
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
                    if (hp.name[0] >= 0 && sp.name[0] >= 0 && MeshOwnedBy(reinterpret_cast<void*>(meshM), pawnM) &&
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
                            static double lastRelT = 0.0, boardSince = 0.0, offSince = -1.0;
                            float yr = pyaw * (6.28318530718f / 65536.0f), cy = std::cos(yr), sy = std::sin(yr);
                            float d[3] = {eye[0] - pl[0], eye[1] - pl[1], eye[2] - pl[2]};
                            float rel[3] = {d[0] * cy + d[1] * sy, -d[0] * sy + d[1] * cy, d[2]};
                            double nowR = ProfMs() * 0.001;
                            float dtR = lastRelT > 0.0 ? static_cast<float>(nowR - lastRelT) : 0.0f;
                            if (g_drawEye != 1) lastRelT = nowR;
                            bool sane = rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2] < 90.0f * 90.0f;
                            if (sane && g_drawEye != 1) {
                                if (!haveRel || dtR > 0.5f) {
                                    std::memcpy(relF, rel, sizeof(relF));
                                    haveRel = true;
                                    boardSince = nowR;
                                    offSince = -1.0;
                                } else if (nowR - boardSince < 1.0) {
                                    float k = 1.0f - std::exp(-dtR / 1.0f);
                                    for (int i = 0; i < 3; ++i) relF[i] += (rel[i] - relF[i]) * k;
                                } else {
                                    // WI-2: dopo 1 s a bordo la posizione resta ferma rispetto
                                    // al Pawn (prima seguiva l'animazione di Walker su ~1 s:
                                    // la vista si spostava sparando o girando la torretta). Si
                                    // riallinea solo se resta oltre 10 unita' per oltre 1 s
                                    // (cambio di posa vero), con lo stesso filtro.
                                    const float e0 = rel[0] - relF[0], e1 = rel[1] - relF[1], e2 = rel[2] - relF[2];
                                    if (e0 * e0 + e1 * e1 + e2 * e2 > 100.0f) {
                                        if (offSince < 0.0) offSince = nowR;
                                        if (nowR - offSince > 1.0) boardSince = nowR;
                                    } else {
                                        offSince = -1.0;
                                    }
                                }
                            }
                            if (haveRel) {
                                base[0] = pl[0] + relF[0] * cy - relF[1] * sy;
                                base[1] = pl[1] + relF[0] * sy + relF[1] * cy;
                                base[2] = pl[2] + relF[2];
                                neckBase = true;
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
            // WI-3 ("crouchview auto", predefinito): la discesa del gioco si toglie
            // sempre (anche seduti: prima scattava di 20 unita' in un tick) e le
            // accucciate non fatte col corpo (tasto, script, riparo da seduti)
            // abbassano la vista di V in crouchtime s (UpdateCrouchView). Prima il
            // tasto non cambiava l'altezza della vista (tester: "no noticeable
            // change in camera height").
            // V si toglie dopo lo smorzamento dell'altezza (e' gia' una rampa: con
            // eyesmooth sopra arrivava in 0,7 s invece di crouchtime); la discesa
            // del gioco e il suo annullamento restano insieme nella parte smorzata.
            float crouchV = 0.0f;
            if (g_crouchCompOn.load() && pawnV && viewFirm) {  // sui mezzi come il gioco
                base[2] += GameCrouchDrop(pawnV);
                crouchV = g_crouchViewDrop.load();
            }
            // Extra dell'altezza: dell'occhio sinistro anche per il destro; niente
            // con la base del collo (e' gia' l'altezza della testa di Walker).
            {
                static float tickExtra = 0.0f;
                if (g_drawEye != 1) tickExtra = neckBase ? 0.0f : extraZ;
                extraZ = tickExtra;
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
                void* pawnK = g_neckFollowScene && HasCameraSignature(camK) ? PlayerPawnIfViewTarget(camK) : nullptr;
                uint32_t meshK = 0;
                if (pawnK && g_drawEye != 1 && TrySafeReadUint32(static_cast<char*>(pawnK) + kOffPawnMesh, &meshK) && meshK) {
                    struct {
                        int32_t name[2];
                        int32_t space;
                        float ret[3];
                    } hp = {{vr::script::NameIndex("Neck"), 0}, 0, {0, 0, 0}};
                    if (hp.name[0] >= 0 && MeshOwnedBy(reinterpret_cast<void*>(meshK), pawnK) &&
                        vr::script::CallFunction(reinterpret_cast<void*>(meshK), "GetBoneLocation", &hp)) {
                        float d = base[2] + extraZ - (hp.ret[2] + 12.0f);  // occhi ~12 unita' sopra l'attacco del collo
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
                if (pawnV && !viewFirm) tau = 0.0f;
                if (lastT == 0.0 || tau <= 0.0f || std::fabs(base[2] - smoothZ) > 80.0f || now - lastT > 0.5) {
                    smoothZ = base[2];
                } else {
                    float a = 1.0f - std::exp(-static_cast<float>(now - lastT) / tau);
                    smoothZ += (base[2] - smoothZ) * a;
                }
                lastT = now;
                base[2] = smoothZ;
            }
            // Smorzata solo l'altezza del gioco (gradini, accucciata del Pawn):
            // l'altezza della testa vera va subito, come lo spostamento della testa.
            base[2] += extraZ;
            {
                static float tickV = 0.0f;  // dell'occhio sinistro anche per il destro
                if (g_drawEye != 1) tickV = crouchV;
                base[2] -= tickV;
            }
            // Passaggio terra <-> mezzo (WI-2): la differenza tra il modo di
            // prima e quello nuovo (fp 15 -> 0, base del collo, limite della
            // testa, smorzamento, accucciata) si toglie in 0,3 s invece che in
            // un tick. Occhio (senza muro e collisione) rispetto al Pawn, nel
            // riferimento del corpo; al cambio di modo l'offset parte dalla
            // differenza tra l'ultimo tick e questo.
            float blendOff[3] = {0.0f, 0.0f, 0.0f};
            {
                static vr::pure::ModeBlend blend;
                static float lastRel[3] = {0, 0, 0}, tickOff[3] = {0, 0, 0};
                static int lastKey = -1;
                static double lastBT = 0.0;
                float pl[3];
                if (g_drawEye != 1) {
                    if (pawnV && SafeCopy(pl, static_cast<char*>(pawnV) + 0x54, 12)) {
                        const float cb = std::cos(b), sb = std::sin(b);
                        const float eye[3] = {base[0] + f * cb - r * sb, base[1] + f * sb + r * cb, base[2] + u};
                        const float dw[3] = {eye[0] - pl[0], eye[1] - pl[1], eye[2] - pl[2]};
                        const float rel[3] = {dw[0] * cb + dw[1] * sb, -dw[0] * sb + dw[1] * cb, dw[2]};
                        const int key = (viewFirm ? 1 : 0) | (neckBase ? 2 : 0) | (g_fwdApplied.load() > 0.5f ? 4 : 0);
                        const double nowB = ProfMs() * 0.001;
                        const float dtB = lastBT > 0.0 ? static_cast<float>(nowB - lastBT) : 0.0f;
                        lastBT = nowB;
                        if (lastKey >= 0 && key != lastKey && dtB > 0.0f && dtB < 0.5f) {
                            float delta[3] = {lastRel[0] - rel[0], lastRel[1] - rel[1], lastRel[2] - rel[2]};
                            // Oltre 60 unita' non e' un cambio di modo (teletrasporto, scena).
                            if (delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2] < 3600.0f) {
                                blend.Start(delta);
                                char bl[200];
                                std::snprintf(bl, sizeof(bl), "VIEWMODE: %s -> %s (neck base %d -> %d, fp %d), step (%.1f, %.1f, %.1f) eased over %.1f s",
                                              (lastKey & 1) ? "ground" : "vehicle", viewFirm ? "ground" : "vehicle", (lastKey >> 1) & 1,
                                              (int)neckBase, (key >> 2) & 1, delta[0], delta[1], delta[2], vr::pure::ModeBlend::kTime);
                                vr::log::Line(bl);
                            }
                        }
                        lastKey = key;
                        float o[3];
                        blend.Step(dtB, o);
                        std::memcpy(tickOff, o, sizeof(tickOff));
                        for (int i = 0; i < 3; ++i) lastRel[i] = rel[i] + o[i];
                    } else {
                        lastKey = -1;
                        tickOff[0] = tickOff[1] = tickOff[2] = 0.0f;
                    }
                }
                const float cb = std::cos(b), sb = std::sin(b);
                blendOff[0] = tickOff[0] * cb - tickOff[1] * sb;
                blendOff[1] = tickOff[0] * sb + tickOff[1] * cb;
                blendOff[2] = tickOff[2];
                for (int i = 0; i < 3; ++i) base[i] += blendOff[i];
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
            // WI-2 (25/10): diagnostica della riga VIEW (sorgente: 1 trace agli
            // occhi, 2 trace basse) e direzione dell'arretramento (avanti del corpo).
            static int pullSrc = 0;
            static float pullDir[2] = {1.0f, 0.0f};
            static vr::pure::LowProbeLatch lowLatch;
            if (g_drawEye != 1) {
                float want = 0.0f, hard = 0.0f;
                pullSrc = 0;
                // Solo la componente lungo l'avanti del corpo (yaw con cui
                // ApplyFirstPerson ha messo gli occhi fp avanti): la sporgenza dal
                // riparo (coverlean, di lato) e il passo nella stanza non si
                // tirano indietro. Prima la direzione era base - centro, sporgenza
                // compresa: l'arretramento andava in diagonale e mangiava la sporgenza.
                const float fyaw = fpYawView;  // yaw dell'avanzamento fp di questa vista (corretto sopra)
                float fx = std::cos(fyaw), fy = std::sin(fyaw);
                float fl = (base[0] - origin[0]) * fx + (base[1] - origin[1]) * fy;
                pullDir[0] = fx;
                pullDir[1] = fy;
                if (fromPawn && fl > 1.0f) {
                    const float kWallGap = 12.0f, kMaxBehind = 10.0f, kHardGap = 4.0f;
                    float probe = fl + kWallGap;
                    float end[3] = {origin[0] + fx * probe, origin[1] + fy * probe, origin[2]};
                    void* self = pawnV;
                    float hitTime;
                    bool hit = TraceViewNearest(self, origin, end, &hitTime, kViewPull);
                    if (hit) {
                        want = fl - (hitTime * probe - kWallGap);  // fino a fl + kWallGap
                        hard = fl - (hitTime * probe - kHardGap);  // occhi mai a meno di 4 unita' dal muro
                        pullSrc |= 1;
                    }
                    // Dietro un riparo basso (cassa del cap. 1, cp01, tester
                    // 22/10) gli occhi stanno appena sopra il bordo: il trace
                    // all'altezza degli occhi passa sopra e la vista restava
                    // fp unita' avanti, sopra la cassa ("almost in the crate").
                    // Si controlla anche 10 e 22 unita' sotto l'occhio vero
                    // (base + altezza della testa u): un bordo all'altezza del
                    // mento, tenuto a kLowGap (piu' di un muro: con 12 il
                    // coperchio riempiva meta' della vista). Non solo "in
                    // copertura": accucciati dietro la cassa il gioco non segna
                    // la copertura. Un parapetto alla vita, in piedi, resta ~40
                    // unita' sotto e non conta.
                    // WI-2: con isteresi (vr::pure::LowProbeLatch): agganciate, si
                    // controlla 4 unita' piu' in basso e l'arretramento resta 0,4 s
                    // dopo l'ultimo colpo; prima il bordo entrava e usciva con
                    // l'ondeggiare della testa e la vista pompava di 20 unita'.
                    static double lastPullT = 0.0;
                    const double nowT = ProfMs() * 0.001;
                    const float dt = static_cast<float>(nowT - lastPullT);
                    lastPullT = nowT;
                    const float kLowGap = 20.0f, lowProbe = fl + kLowGap;
                    float lowWant = 0.0f;
                    bool lowHit = false;
                    for (float drop : {10.0f, 22.0f}) {
                        float z = origin[2] + u - drop - lowLatch.Drop();
                        float lo[3] = {origin[0], origin[1], z};
                        float loEnd[3] = {origin[0] + fx * lowProbe, origin[1] + fy * lowProbe, z};
                        float lt;
                        if (TraceViewNearest(self, lo, loEnd, &lt, kViewPull)) {
                            lowWant = (std::max)(lowWant, fl - (lt * lowProbe - kLowGap));
                            lowHit = true;
                        }
                    }
                    const float lowHeld = lowLatch.Step(lowHit, lowWant, dt > 0.0f && dt < 0.5f ? dt : 0.0f);
                    if (lowLatch.engaged) {
                        want = (std::max)(want, lowHeld);
                        hit = true;
                        pullSrc |= 2;
                    }
                    if (hit) {
                        if (want > fl) {
                            // Dietro il centro: solo se alle spalle c'e' spazio.
                            float maxBack = kMaxBehind;
                            float bend[3] = {origin[0] - fx * (kMaxBehind + 8.0f), origin[1] - fy * (kMaxBehind + 8.0f),
                                             origin[2]};
                            float bt;
                            if (TraceViewNearest(self, origin, bend, &bt, kViewPull)) maxBack = bt * (kMaxBehind + 8.0f) - 8.0f;
                            maxBack = maxBack < 0.0f ? 0.0f : maxBack;
                            if (want > fl + maxBack) want = fl + maxBack;
                            if (hard > want) hard = want;
                        }
                        if (want < 0.0f) want = 0.0f;
                    }
                    // Allontanandosi dal muro la vista torna avanti in ~0,2 s;
                    // avvicinandosi arretra al massimo a 150 unita'/s (prima di
                    // colpo: girandosi vicino a un angolo la vista saltava
                    // indietro di 25 unita' in un frame), ma subito quanto basta
                    // perche' gli occhi non entrino nel muro (WI-2: prima per
                    // qualche tick stavano dentro, e la collisione della testa
                    // accorciava tutta la vista).
                    want = vr::pure::WallPullStep(wallPull, want, hard, dt);
                    base[0] -= fx * want;
                    base[1] -= fy * want;
                    // Review 2 del follow-up (27/10): il corpo che torna sotto la testa (UpdateHeadTracking) si
                    // ferma con l'arretramento attivo (girare il corpo ne gira la direzione: con W unita' la vista
                    // scivolava di lato di 2 W sin(d/2)) o previsto col corpo girato verso la testa: stesse trace
                    // (occhi fl + 12, mento fl + 20) lungo lo yaw della testa, 4 unita' di margine. Prima un muro di
                    // lato a 12 unita' con la testa girata di 40 gradi verso di lui accendeva l'arretramento a
                    // meta' rotazione (~8 unita' indietro, la testa ferma).
                    int blk = (want > 0.5f ? 1 : 0) | (lowLatch.engaged ? 2 : 0);
                    const int32_t headRel = g_headHeadingYaw.load();
                    if (!blk && vr::control::g_bodyAlignWallStop && vr::control::g_bodyAlignHoldS.load() > 0.0f && std::abs(headRel) >= 91) {
                        const float hyaw = static_cast<float>(bodyYaw + headRel) * (6.28318530718f / 65536.0f);
                        const float hx = std::cos(hyaw), hy = std::sin(hyaw), kMargin = 4.0f;
                        const float hp = fl + kWallGap + kMargin, hlp = fl + kLowGap + kMargin;
                        float hend[3] = {origin[0] + hx * hp, origin[1] + hy * hp, origin[2]};
                        float ht;
                        if (TraceViewNearest(self, origin, hend, &ht, kViewPull)) blk |= 4;
                        for (float drop : {10.0f, 22.0f}) {
                            if (blk) break;
                            const float z = origin[2] + u - drop;
                            float lo[3] = {origin[0], origin[1], z};
                            float loEnd[3] = {origin[0] + hx * hlp, origin[1] + hy * hlp, z};
                            if (TraceViewNearest(self, lo, loEnd, &ht, kViewPull)) blk |= 8;
                        }
                    }
                    g_bodyAlignBlock.store(blk, std::memory_order_relaxed);
                } else {
                    lowLatch = vr::pure::LowProbeLatch();
                    g_bodyAlignBlock.store(0, std::memory_order_relaxed);
                }
                wallPull = want;
                g_wallPull.store(want, std::memory_order_relaxed);
            } else if (wallPull > 0.0f) {
                base[0] -= pullDir[0] * wallPull;
                base[1] -= pullDir[1] * wallPull;
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
            // WI-2: la collisione toglie solo la parte dello spostamento della
            // testa che entra nell'ostacolo (normale del piano colpito,
            // vr::pure::ClampIntoPlane): la testa scivola lungo il muro 1:1 e la
            // base (15 unita' avanti, arretrata dal muro) non si accorcia. Prima
            // t scalava tutto il vettore dal centro di Walker agli occhi. Senza
            // normale valida, o con un secondo ostacolo (spigolo), come prima (t).
            static float tickCorr[3] = {0.0f, 0.0f, 0.0f}, smoothCorr[3] = {0.0f, 0.0f, 0.0f};
            static bool tickHitPawn = false;  // colpo della testa su un Pawn da solo in questo tick (HeadPushStep)
            if (g_drawEye != 1) tickHitPawn = false;
            float corr[3] = {0.0f, 0.0f, 0.0f};
            if (g_drawEye == 1) {
                t = tickT;
                std::memcpy(corr, tickCorr, sizeof(corr));
            } else if (vr::control::g_headCollision && len > 0.5f) {
                const float margin = 8.0f;
                // Trace lunghe 2 margini oltre l'occhio (review WI-2): un muro si trova
                // gia' prima che l'occhio entri nel margine anche con la trace obliqua
                // (fino a 60 gradi dalla normale), quindi il piano entra senza scatto.
                // Prima la trace arrivava a 8 unita' oltre l'occhio lungo la linea dal
                // centro di Walker: un muro di lato entrava a 5-6 unita' e l'occhio
                // saltava a 8 (5,6 unita' misurate nell'angolo della stanza).
                const float reach = 2.0f * margin;
                float ext[3] = {origin[0] + dx / len * (len + reach), origin[1] + dy / len * (len + reach),
                                origin[2] + dz / len * (len + reach)};
                float hitTime, n[3];
                void* self = pawnV;
                g_headPlanes = 0;
                g_headPlanesPawn = 0;
                void* headHitActor = nullptr;
                tickHitPawn = false;
                if (TraceViewNearest(self, origin, ext, &hitTime, kViewHead, n, &headHitActor)) {
                    bool planeOk = false;
                    if (n[0] != 0.0f || n[1] != 0.0f || n[2] != 0.0f) {
                        const float hitLoc[3] = {origin[0] + (ext[0] - origin[0]) * hitTime, origin[1] + (ext[1] - origin[1]) * hitTime,
                                                 origin[2] + (ext[2] - origin[2]) * hitTime};
                        float t2[3] = {target[0], target[1], target[2]};
                        // Limite: il centro di Walker (inizio della trace, sempre libero),
                        // non la base: con un compagno davanti (niente arretramento per i
                        // Pawn) la base e' dentro di lui e l'occhio va a 8 unita' fuori.
                        // Piani colpiti: il primo, poi quelli che la trace di controllo
                        // trova (spigolo, angolo della stanza: review WI-2, prima col
                        // secondo ostacolo si tornava a t e di lato il guadagno era 0,79).
                        float planeHit[3][3], planeN[3][3];
                        int planes = 1;
                        // f3 (27/10): ogni piano usato viene da un Pawn? Prima solo il primo colpo, e con un
                        // secondo piano (la trace di controllo sulla stessa capsula del compagno, d26) il cambio
                        // di lato arrivava di colpo (5,14 unita' in un tick). Un piano da un muro, un oggetto o
                        // il BSP (attore nullo o non Pawn) = nessun passaggio dolce, come prima.
                        static void* pawnCacheActor[2] = {nullptr, nullptr};
                        static bool pawnCacheIs[2] = {false, false};
                        static int pawnCacheNext = 0;
                        auto isPawnHit = [](void* actor) {
                            if (!actor) return false;
                            for (int c = 0; c < 2; ++c)
                                if (pawnCacheActor[c] == actor) return pawnCacheIs[c];
                            const bool is = vr::script::IsA(actor, "Pawn");
                            pawnCacheActor[pawnCacheNext] = actor;
                            pawnCacheIs[pawnCacheNext] = is;
                            pawnCacheNext ^= 1;
                            return is;
                        };
                        bool allPawn = isPawnHit(headHitActor);
                        std::memcpy(planeHit[0], hitLoc, sizeof(hitLoc));
                        std::memcpy(planeN[0], n, sizeof(planeN[0]));
                        vr::pure::ClampIntoPlanes(origin, t2, planeHit, planeN, planes, margin);
                        // Il punto corretto dev'essere libero dal centro e fuori dal margine
                        // di ogni superficie che la trace di controllo incontra (spigoli,
                        // angoli); un piano gia' usato o oltre il margine va bene.
                        for (int it = 0; it < 3; ++it) {
                            const float ex2[3] = {t2[0] - origin[0], t2[1] - origin[1], t2[2] - origin[2]};
                            const float l2 = std::sqrt(ex2[0] * ex2[0] + ex2[1] * ex2[1] + ex2[2] * ex2[2]);
                            if (!(l2 > 0.5f)) {
                                planeOk = true;
                                break;
                            }
                            const float e2[3] = {origin[0] + ex2[0] / l2 * (l2 + reach), origin[1] + ex2[1] / l2 * (l2 + reach),
                                                 origin[2] + ex2[2] / l2 * (l2 + reach)};
                            float h2, n2[3];
                            void* a2 = nullptr;
                            if (!TraceViewNearest(self, origin, e2, &h2, kViewHead, n2, &a2)) {
                                planeOk = true;
                                break;
                            }
                            if (n2[0] == 0.0f && n2[1] == 0.0f && n2[2] == 0.0f) break;
                            float hit2[3], dist2 = 0.0f;
                            for (int i = 0; i < 3; ++i) {
                                hit2[i] = origin[i] + (e2[i] - origin[i]) * h2;
                                dist2 += (t2[i] - hit2[i]) * n2[i];
                            }
                            // Colpo prima del punto corretto: la linea dal centro non e' libera.
                            const bool beforePoint = h2 * (l2 + reach) < l2 - 0.01f;
                            if (!beforePoint && dist2 >= margin - 0.01f) {
                                planeOk = true;
                                break;
                            }
                            bool same = false;
                            for (int k = 0; k < planes && !same; ++k) {
                                const float nd = planeN[k][0] * n2[0] + planeN[k][1] * n2[1] + planeN[k][2] * n2[2];
                                const float off = (hit2[0] - planeHit[k][0]) * planeN[k][0] + (hit2[1] - planeHit[k][1]) * planeN[k][1] +
                                                  (hit2[2] - planeHit[k][2]) * planeN[k][2];
                                same = nd > 0.995f && std::fabs(off) < 0.5f;
                            }
                            if (same && !beforePoint) {
                                planeOk = true;  // gia' al meglio (centro di Walker piu' vicino del margine)
                                break;
                            }
                            if (same || planes >= 3) break;
                            std::memcpy(planeHit[planes], hit2, sizeof(hit2));
                            std::memcpy(planeN[planes], n2, sizeof(n2));
                            ++planes;
                            allPawn = allPawn && isPawnHit(a2);
                            vr::pure::ClampIntoPlanes(origin, t2, planeHit, planeN, planes, margin);
                        }
                        if (planeOk) {
                            g_headPlanes = planes;
                            // Solo Pawn (compagno, anche due piani sulla sua capsula): il cambio di lato della
                            // spinta puo' essere dolce (HeadPushStep). Un muro tra i piani: subito.
                            tickHitPawn = allPawn;
                            g_headPlanesPawn = allPawn ? 1 : 0;
                            for (int i = 0; i < 3; ++i) corr[i] = t2[i] - target[i];
                            if (corr[0] * corr[0] + corr[1] * corr[1] + corr[2] * corr[2] > 1e-4f)
                                g_headBlocked.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    if (!planeOk) {
                        g_headPlanes = -1;  // come prima: t
                        float allowed = hitTime * (len + reach) - margin;
                        t = allowed <= 0.0f ? 0.0f : (allowed < len ? allowed / len : 1.0f);
                        if (t < 1.0f) g_headBlocked.fetch_add(1, std::memory_order_relaxed);
                    }
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
                // Spinta della testa (vr::pure::HeadPushStep, follow-up 27/10): subito
                // solo se cresce dallo stesso lato (muri, spigoli: mai attraverso);
                // un cambio di lato (compagno che passa attraverso la testa: la normale
                // gira in un tick, l'occhio saltava di 7 unita' di lato) va verso la
                // nuova con tau 0,1 s e al massimo 40 unita'/s; il rilascio come prima.
                // Review del follow-up: dolce solo su un Pawn (muri, spigoli, oggetti: subito
                // come prima), residuo sotto 0,5 unita' = nessuna spinta, occhio mai piu' di
                // 6 unita' (margine - 2) dietro la spinta nuova.
                static bool pushChanging = false;
                const float cn2 = corr[0] * corr[0] + corr[1] * corr[1] + corr[2] * corr[2];
                const float sn2 = smoothCorr[0] * smoothCorr[0] + smoothCorr[1] * smoothCorr[1] + smoothCorr[2] * smoothCorr[2];
                const float dot2 = corr[0] * smoothCorr[0] + corr[1] * smoothCorr[1] + corr[2] * smoothCorr[2];
                const bool otherSide = cn2 > 1e-6f && sn2 > 0.25f && dot2 < 0.5f * std::sqrt(cn2 * sn2);
                const bool wasChanging = pushChanging;
                vr::pure::HeadPushParams hpp;
                hpp.maxInside = 8.0f - 2.0f;  // margine della trace qui sopra (8) - 2
                const int pushKind = vr::pure::HeadPushStep(smoothCorr, corr, dtt, &pushChanging, hpp, tickHitPawn);
                if (pushKind == 2 && otherSide) g_headPushSideTicks.fetch_add(1, std::memory_order_relaxed);
                if (pushChanging && !wasChanging) {
                    static ULONGLONG lastSideLog = 0;
                    if (GetTickCount64() - lastSideLog > 1000) {
                        lastSideLog = GetTickCount64();
                        char b[200];
                        const float ang = std::acos((std::max)(-1.0f, (std::min)(1.0f, dot2 / std::sqrt((std::max)(1e-12f, cn2 * sn2))))) * 57.2957795f;
                        std::snprintf(b, sizeof(b), "VIEW: head push changes side on a pawn (%.0f deg, %.2f -> %.2f units), eased at most 40 units/s.",
                                      ang, std::sqrt(sn2), std::sqrt(cn2));
                        vr::log::Line(b);
                    }
                }
                std::memcpy(corr, smoothCorr, sizeof(corr));
                std::memcpy(tickCorr, corr, sizeof(corr));
            }
            for (int i = 0; i < 3; ++i) target[i] += corr[i];
            dx = target[0] - origin[0];
            dy = target[1] - origin[1];
            dz = target[2] - origin[2];
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
                // WI-1 (25/10): confronto delle velocita', non delle distanze per
                // tick. Prima in elicottero ogni tick lungo (20-83 ms) a 3000
                // unita'/s sembrava un salto: nero di 0,3 s fino a 7 volte al
                // secondo (log del tester 23/10; 11 in 12 s col runtime finto).
                // Su un mezzo conta solo il teletrasporto (oltre 2000 unita').
                static vr::pure::ViewJumpDetector jump;
                static double lastJumpT = 0.0;
                static float prevSpeed = 0.0f;
                const double nowJ = ProfMs() * 0.001;
                const float dtJ = lastJumpT > 0.0 ? static_cast<float>(nowJ - lastJumpT) : 0.0f;
                lastJumpT = nowJ;
                char* camJ = static_cast<char*>(g_cameraPtr.load());
                void* pawnJ = HasCameraSignature(camJ) ? PlayerPawnIfViewTarget(camJ) : nullptr;
                const bool mountJ = pawnJ && !viewFirm;  // WI-2: torretta fissa a terra = a terra
                float speedJ = 0.0f;
                const int jumpKind = dtJ > 0.0f && dtJ < 1.0f ? jump.Step(dj, dtJ, mountJ, &speedJ)
                                                               : (dj > 2000.0f ? vr::pure::ViewJumpDetector::kTeleport : 0);
                // Subito dopo lo schermo virtuale il nero e' gia' tenuto: il salto lo prolunga.
                bool settling = GetTickCount64() - g_cinemaEndMs.load() < 1500;
                if (dj > 80.0f && settling) g_settleJumpMs = GetTickCount64();
                // Teletrasporto (fine di un tratto in elicottero: 17700 unita'
                // e 0,6 s dopo la scena): nero tenuto e scena subito sullo schermo.
                if (dj > 2000.0f) g_teleportMs = GetTickCount64();
                if (jumpKind != vr::pure::ViewJumpDetector::kNone && vr::control::g_fades && !g_gameLockedScene && !settling) {
                    vr::output::Blink(0.3f);
                    char b[200];
                    snprintf(b, sizeof(b), "FADE: view jumped %.0f units in a tick (%.1f ms, %.0f units/s, previous %.0f units/s)%s",
                             dj, dtJ * 1000.0f, speedJ, prevSpeed, mountJ ? ", on a vehicle" : "");
                    vr::log::Line(b);
                }
                prevSpeed = speedJ;
                // Movimento forzato per la vignettatura: base della vista (senza
                // testa) che si muove senza lo stick (veicoli, spinte del gioco)
                // e rotazioni del corpo non fatte dal giocatore.
                // Movimento NON fatto dal giocatore = movimento della vista meno
                // quello della testa vera (M20): prima la base si muoveva anche
                // per i passi nella stanza (il Pawn insegue la testa) e il corpo
                // girava per l'allineamento con la testa o in copertura, e la
                // vignettatura scattava senza motivo.
                {
                    static float lastView[2] = {0, 0}, lastHeadW[2] = {0, 0}, lastPov[2] = {0, 0};
                    static int32_t lastViewYaw = 0;
                    static float lastHmdYaw = 0.0f;
                    static double lastT = 0.0;
                    double now = ProfMs() * 0.001;
                    float dt = lastT > 0.0 ? static_cast<float>(now - lastT) : 0.0f;
                    lastT = now;
                    float bAng = g_bodyYaw.load() * (6.28318530718f / 65536.0f);
                    float hf = g_headOffset[0].load(), hr = g_headOffset[1].load();
                    float headW[2] = {hf * std::cos(bAng) - hr * std::sin(bAng), hf * std::sin(bAng) + hr * std::cos(bAng)};
                    // Calcolo di prima (diagnostica, VIEW forcedold).
                    const float mxOld = (outLoc[0] - lastView[0]) - (headW[0] - lastHeadW[0]);
                    const float myOld = (outLoc[1] - lastView[1]) - (headW[1] - lastHeadW[1]);
                    const float bdOld = std::sqrt(mxOld * mxOld + myOld * myOld);
                    // Review del follow-up (27/10), misurato (VIEW per tick, corpo che torna
                    // sotto la testa): qui outLoc e' ancora il POV del gioco (l'occhio finale
                    // si scrive dopo), non la vista. Un passo room-scale (il corpo che gira
                    // attorno agli occhi fa crescere lo scostamento della testa fino a 15 cm,
                    // e il Pawn la segue di ~5 unita' in un tick) dava: nel tick del passo lo
                    // scostamento che cala di S (meno il suo calo = +S), nel tick dopo il POV
                    // del gioco che si sposta di S: 2 tick a 360 unita'/s, vignettatura piena
                    // (forced 1,0) a ogni corpo che tornava sotto la testa. Ora: movimento
                    // della base = POV del gioco (senza lo spostamento dell'occhio, col passo
                    // di questo tick come la vista, L8) piu' gli spostamenti dello
                    // scostamento della testa fatti dalla mod (passo room-scale, perno degli
                    // occhi: g_headShiftWorld). La testa vera non entra: il POV non la contiene
                    // (prima il suo movimento veloce contava come forzato).
                    const float povX = outLoc[0] - eyeOff[0] + (g_roomStepValid ? g_roomStepWorld[0] : 0.0f);
                    const float povY = outLoc[1] - eyeOff[1] + (g_roomStepValid ? g_roomStepWorld[1] : 0.0f);
                    float mx = (povX - lastPov[0]) + g_headShiftWorld[0];
                    float my = (povY - lastPov[1]) + g_headShiftWorld[1];
                    g_headShiftWorld[0] = g_headShiftWorld[1] = 0.0f;
                    lastPov[0] = povX;
                    lastPov[1] = povY;
                    float bd = std::sqrt(mx * mx + my * my);
                    float hmdYawNow = g_hmdYawRaw.load();
                    float dHmd = std::remainder(hmdYawNow - lastHmdYaw, 6.28318530718f) * (65536.0f / 6.28318530718f);
                    float dView = static_cast<float>(static_cast<int16_t>(outRot[1] - lastViewYaw));
                    float dyaw = std::fabs(dView - vr::control::g_yawSign * dHmd) * (360.0f / 65536.0f);
                    float forced = 0.0f, forcedOld = 0.0f;
                    if (dt > 0.0f && dt < 0.2f && bdOld < 200.0f && vr::control::g_motionAmount < 0.1f)
                        forcedOld = (bdOld / dt - 40.0f) / 250.0f;
                    if (dt > 0.0f && dt < 0.2f && bd < 200.0f) {
                        float speed = bd / dt;  // unita'/s
                        if (vr::control::g_motionAmount < 0.1f) forced = (std::max)(forced, (speed - 40.0f) / 250.0f);
                        // Rampa della vista con l'accucciata del tasto (WI-3): movimento
                        // verticale non fatto dal giocatore, vignettatura (al massimo ~0,6).
                        forced = (std::max)(forced, g_crouchViewSpeed.load() / 150.0f);
                        if (!vr::control::g_stickTurning) forced = (std::max)(forced, (dyaw / dt - 20.0f) / 120.0f);
                    }
                    vr::control::g_forcedMotion = forced < 0.0f ? 0.0f : (forced > 1.0f ? 1.0f : forced);
                    // Diagnostica (VIEW forcedold): solo la parte di posizione, col calcolo di prima.
                    g_forcedMotionOld = forcedOld < 0.0f ? 0.0f : (forcedOld > 1.0f ? 1.0f : forcedOld);
                    lastView[0] = outLoc[0];
                    lastView[1] = outLoc[1];
                    lastHeadW[0] = headW[0];
                    lastHeadW[1] = headW[1];
                    lastViewYaw = outRot[1];
                    lastHmdYaw = hmdYawNow;
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
            // Diagnostica (25/10): viewlog, bodylog e la riga JITTER del runtime finto.
            {
                static float eye0Loc[3] = {0, 0, 0};
                if (g_drawEye == 0) std::memcpy(eye0Loc, outLoc, sizeof(eye0Loc));
                if (vr::rt::Active() == vr::rt::Backend::Fake && g_drawEye >= 0)
                    vr::fake::NoteView(g_stereoDraws.load(), g_drawEye, outRot, bodyYawT, g_hmdYawReference);
                // Solo i disegni stereo (occhio 0 e 1), contati sull'occhio destro:
                // le chiamate del gioco fuori dallo stereo (occhio -1) consumavano N
                // prima del tempo (viewlog 700 dava ~500 tick, review 25/10).
                const int eyeV = g_drawEye.load();
                if (vr::control::g_viewLogTicks.load() > 0 && (eyeV == 0 || eyeV == 1)) {
                    g_viewDiagWi2.pullSrc = pullSrc;
                    g_viewDiagWi2.latch = lowLatch.engaged ? 1 : 0;
                    std::memcpy(g_viewDiagWi2.corr, corr, sizeof(corr));
                    g_viewDiagWi2.viewFirm = viewFirm ? 1 : 0;
                    g_viewDiagWi2.neck = neckBase ? 1 : 0;
                    std::memcpy(g_viewDiagWi2.blend, blendOff, sizeof(blendOff));
                    ViewDiag vd = {eyeV, outLoc, outRot, bodyYawT, bodyPitchT, bodyYaw, {f, r, u}, base, wallPull, t, eyeOff};
                    LogViewLine(vd);
                    if (eyeV == 1) vr::control::g_viewLogTicks.fetch_sub(1);
                }
                if (vr::control::g_bodyLogTicks.load() > 0 && eyeV == 1) {
                    vr::control::g_bodyLogTicks.fetch_sub(1);
                    float center[3];
                    for (int i = 0; i < 3; ++i) center[i] = 0.5f * (outLoc[i] + eye0Loc[i]);
                    LogBodyLine(center, outRot);
                }
                // Ancora del collo (WI-3): occhio finale (muro e collisione compresi)
                // e testa rispetto al Pawn per il prossimo tick. Col cannocchiale la
                // vista segue la canna: si usa la testa.
                if (eyeV == 1 && pawnV) {
                    float pl[3];
                    int32_t pyaw = 0;
                    if (SafeCopy(pl, static_cast<char*>(pawnV) + 0x54, 12) && SafeCopy(&pyaw, static_cast<char*>(pawnV) + 0x64, 4)) {
                        const float k = 6.28318530718f / 65536.0f, py = pyaw * k, cp = std::cos(py), sp = std::sin(py);
                        float c[3];
                        for (int i = 0; i < 3; ++i) c[i] = 0.5f * (outLoc[i] + eye0Loc[i]);
                        const float dx = c[0] - pl[0], dy = c[1] - pl[1];
                        AnchorEye& ae = g_anchorEye;
                        ae.eye[0] = dx * cp + dy * sp;
                        ae.eye[1] = -dx * sp + dy * cp;
                        ae.eye[2] = c[2] - pl[2];
                        const bool scopeView = g_scopeActive && g_muzzleAimValid;
                        const int32_t rp = scopeView ? HeadPitch() : outRot[0];
                        const int32_t ry = (scopeView ? bodyYawT + HeadYaw() : outRot[1]) - pyaw;
                        const int32_t rr = scopeView ? HeadRoll() : outRot[2];
                        ae.pitch = static_cast<int16_t>(rp & 0xFFFF) * k;
                        ae.yaw = static_cast<int16_t>(ry & 0xFFFF) * k;
                        ae.roll = static_cast<int16_t>(rr & 0xFFFF) * k;
                        ae.pawn = pawnV;
                        ae.t = ProfMs() * 0.001;
                        ae.valid = true;
                    }
                }
            }
            // Il rendering usa questa origine (testa 6DOF compresa): va nella
            // lista delle origini per occhio, altrimenti col visore vero (testa
            // spostata) nessun frame viene riconosciuto e il visore non riceve
            // immagini (17/10, prima prova del 6DOF reale).
            if (g_drawEye >= 0) RecordEyeOrigin(outLoc, g_drawEye);
            UpdateAimMarker(base, outLoc, outRot);
            UpdateArms(base, bodyYaw);
            if (g_drawEye != 0) {
                // Oggetti di scena in mano: subito dopo le mani di questo frame
                // (prima, nel Draw, usavano quelle del frame precedente: 5 cm di
                // distacco a 90 Hz muovendo la mano in fretta).
                char* camP = static_cast<char*>(g_cameraPtr.load());
                vr::props::SetViewOrigin(outLoc);  // testa: lo scudo colpito vola via lontano da li'
                vr::props::Tick(HasCameraSignature(camP) ? PlayerPawnIfViewTarget(camP) : nullptr, g_handWorld, g_handAxesWorld,
                                g_handWorldValid);
            }
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
                    char b[800];
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
                    const bool rigOk = g_rig.mesh && MeshOwnedBy(g_rig.mesh, pawnG);
                    if (rigOk && hp.name[0] >= 0) vr::script::CallFunction(g_rig.mesh, "GetBoneLocation", &hp);
                    st[4] = hp.ret[0] - outLoc[0];
                    st[5] = hp.ret[1] - outLoc[1];
                    // Mano sinistra rispetto alla destra (presa sull'astina).
                    struct {
                        int32_t name[2];
                        int32_t space;
                        float ret[3];
                    } lp = {{vr::script::NameIndex("Hand_Left"), 0}, 0, {0, 0, 0}};
                    if (rigOk && lp.name[0] >= 0) vr::script::CallFunction(g_rig.mesh, "GetBoneLocation", &lp);
                    int32_t pawnYaw = 0;
                    SafeCopy(&pawnYaw, static_cast<char*>(pawnG) + 0x64, 4);
                    st[1] = static_cast<float>(pawnYaw & 0xFFFF) * (360.0f / 65536.0f);
                    st[3] = static_cast<float>(g_bodyYaw.load() & 0xFFFF) * (360.0f / 65536.0f);
                    snprintf(b, sizeof(b),
                             "GUNLOG: t=%.1f eye %d view (%.2f, %.2f, %.2f) barrel-view (%.2f, %.2f, %.2f) pawn (%.2f, %.2f, %.2f) "
                             "arms=%d flags=%08X strength %.2f %.2f %.2f %.2f %.2f %.2f lh (%.2f, %.2f, %.2f) axis (%.5f, %.5f, %.5f) "
                             "lead %d marker %d (%.2f, %.2f, %.2f)",
                             ProfMs(), g_drawEye.load(), outLoc[0], outLoc[1], outLoc[2], mo[0] - outLoc[0],
                             mo[1] - outLoc[1], mo[2] - outLoc[2], al[0], al[1], al[2], (int)g_armsActive.load(), gunFlags,
                             st[0], st[1], st[2], st[3], st[4], st[5], lp.ret[0] - hp.ret[0], lp.ret[1] - hp.ret[1],
                             lp.ret[2] - hp.ret[2], md[0], md[1], md[2], g_armsLeadUsed.load(), (int)g_muzzleAimValid.load(),
                             g_muzzleTarget[0], g_muzzleTarget[1], g_muzzleTarget[2]);
                    // Voce 5 (regressione dell'arma con solidhands): asse e origine
                    // della canna, bersaglio della mano dell'arma e mirino a piena
                    // precisione, piu' la relazione mano/arma (rimisurata ogni 30 frame).
                    const size_t bl = std::strlen(b);
                    snprintf(b + bl, sizeof(b) - bl,
                             " | axis (%.6f, %.6f, %.6f) origin (%.3f, %.3f, %.3f) rhand (%.3f, %.3f, %.3f) marker (%.1f, %.1f, %.1f)"
                             " hiw %.6f %.6f %.6f",
                             md[0], md[1], md[2], mo[0], mo[1], mo[2], g_handWorld[0][0], g_handWorld[0][1], g_handWorld[0][2],
                             g_muzzleTarget[0], g_muzzleTarget[1], g_muzzleTarget[2], g_handInWeapon[0][0][0], g_handInWeapon[0][0][1],
                             g_handInWeapon[0][1][2]);
                    vr::log::Line(b);
                    // Diagnostica f3: yaw della mesh disegnata ora, yaw di conversione dell'ultimo UpdateArms e
                    // chiamate di UpdateSkelPose (prima > dopo, yaw del Pawn) da questa riga GUNLOG alla precedente.
                    SkelPoseDiag& sd = g_skelPoseDiag;
                    int o = std::snprintf(b, sizeof(b),
                                          "SKELDIAG: t=%.1f mesh %.3f pawn %.3f armsyaw %.3f armsmesh %.3f facing %d arms %d restored %d rot %.3f "
                                          "hmove %.3f seam %.3f trunk %.3f snap %d rshift %d n %d",
                                          ProfMs(), CompL2WYawDeg(g_rig.mesh), st[1], sd.armsYaw, sd.armsMesh, sd.facing, sd.arms, sd.restored,
                                          sd.restoreDeg, sd.restoreHand, sd.restoreSeam, TrunkYawRelDeg(g_rig.mesh), (int)g_meshL2WSnap.valid, sd.roomShift, sd.n);
                    for (int k = 0; k < (sd.n < 4 ? sd.n : 4) && o > 0 && o < static_cast<int>(sizeof(b)) - 80; ++k)
                        o += std::snprintf(b + o, sizeof(b) - o, " [%p %.3f>%.3f a%.3f fix %.3f/%.2f]", sd.ret[k], sd.pre[k], sd.post[k],
                                           sd.actor[k], sd.fixDeg[k], sd.fixUnits[k]);
                    vr::log::Line(b);
                    sd.n = sd.facing = sd.arms = sd.restored = sd.roomShift = 0;
                    sd.restoreDeg = sd.restoreHand = sd.restoreSeam = 0.0f;
                    for (int k = 0; k < 4; ++k) sd.fixDeg[k] = sd.fixUnits[k] = 0.0f;
                }
            }
        }
    }
};

// Yaw (gradi) dell'asse X della LocalToWorld di un componente (diagnostica f3).
float CompL2WYawDeg(const void* comp) {
    float m[3];
    if (!comp || !SafeCopy(m, static_cast<const char*>(comp) + kOffCompLocalToWorld, sizeof(m))) return -999.0f;
    return std::atan2(m[1], m[0]) * 57.2957795f;
}

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
// Come TraceLine (stessi flag, stesso tempo dalla posizione d'impatto), in
// piu' l'attore colpito. Diagnostica della riga SHOT (review 25/10): la
// partenza dei colpi a 1-6 unita' dall'asse con l'impatto sulla partenza "on
// KActorSpawnable" (prova col runtime finto) faceva pensare alla sfera della
// mano (props.cpp, ProxyCreate), che questa trace non salta.
bool TraceLineActor(void* source, const float* start, const float* end, float* hitTime, void** actor) {
    *actor = nullptr;
    __try {
        if (!kGWorldAddr || !kSingleLineCheckAddr) return false;
        void* world = *reinterpret_cast<void**>(kGWorldAddr);
        if (!world) return false;
        alignas(16) unsigned char hit[0x100] = {};
        const float extent[3] = {0.0f, 0.0f, 0.0f};
        reinterpret_cast<SingleLineCheck_t>(kSingleLineCheckAddr)(world, hit, source, end, start, kTraceFlags, extent, nullptr);
        void* a = *reinterpret_cast<void**>(hit + 4);
        if (!a) return false;
        const float* loc = reinterpret_cast<const float*>(hit + 8);
        float sx = end[0] - start[0], sy = end[1] - start[1], sz = end[2] - start[2];
        float hx = loc[0] - start[0], hy = loc[1] - start[1], hz = loc[2] - start[2];
        float seg2 = sx * sx + sy * sy + sz * sz;
        float t = seg2 > 0.0f ? (hx * sx + hy * sy + hz * sz) / seg2 : 0.0f;
        *hitTime = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        *actor = a;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// Diagnostica (riga SHOT): attore su cui la trace spalla -> volata dell'ultima
// MuzzleStart ha fermato la partenza (nullptr = nessun ostacolo). La trace e
// il suo esito restano quelli di prima: solo il nome finisce nel log.
void* g_muzzleClipActor = nullptr;
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
    g_muzzleClipActor = nullptr;
    // Il mirino passa di qui PRIMA di UpdateArms: al primo tick di un livello
    // nuovo g_rig.mesh e' ancora la mesh di Walker del livello vecchio,
    // liberata (crash al cambio di livello del 26/10).
    if (!g_rig.mesh) return;
    if (!StillAlive(g_rig.mesh, g_rig.meshCls)) {
        static void* logged = nullptr;
        if (logged != g_rig.mesh) {
            logged = g_rig.mesh;
            char b[140];
            std::snprintf(b, sizeof(b), "ARMS: arm mesh %p of the previous level is gone: aim trace without it (stale pointer kept out)", g_rig.mesh);
            vr::log::Line(b);
        }
        return;
    }
    // Mesh viva ma non ancora quella del Pawn (o non posata/attaccata) al primo
    // disegno dopo un caricamento: niente trace dalla spalla (WI-3 fix 1). Dopo
    // StillAlive: MeshOwnedBy legge i campi della mesh (merge 27/10).
    if (!MeshOwnedBy(g_rig.mesh, pawn)) return;
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
    void* clip = nullptr;
    if (sp.name[0] >= 0 && vr::script::CallFunction(g_rig.mesh, "GetBoneLocation", &sp) &&
        TraceLineActor(pawn, sp.ret, unpushed, &hit, &clip) && hit < 1.0f) {
        g_muzzleClipActor = clip;
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
        it->laserVis = g_laserVisible[eye];
        it->hlState = g_hlState[eye];
        for (int i = 0; i < 4; ++i) it->hlNdc[i] = g_hlNdc[eye][i];
        for (int i = 0; i < 2; ++i) {
            it->markerNdc[i] = g_markerNdc[eye][i];
            it->targetNdc[i] = g_targetNdc[eye][i];
            it->laserNdc[i] = g_laserNdc[eye][i];
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
        g_hlState[eye] = 0;
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
        // Torretta (WI-4c): la mira vera del gioco (YGamePawn +0x14A8, da cui
        // UpdateTurretAiming punta la torretta), non la mano: al limite il
        // mirino si ferma dove vanno i colpi.
        float af[3], ar[3], au[3];
        int32_t taP = 0, taY = 0;
        if (TurretAimActive() && TurretGameAim(&taP, &taY))
            RotatorToAxes(taP, taY, 0, af, ar, au);
        else
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
    {
        // Oggetto evidenziato: gli 8 angoli del suo box proiettati in
        // quest'occhio; il rettangolo che li contiene (nessun angolo dietro).
        float mn[3], mx[3];
        int st = 0;
        bool ok = vr::props::Highlight(mn, mx, &st);
        float r[4] = {1e9f, 1e9f, -1e9f, -1e9f};
        float fovT = vr::control::g_fovOverride > 0.0f ? vr::control::g_fovOverride.load() : 90.0f;
        float th2 = std::tan(fovT * 0.5f / 57.2957795f);
        UINT w2 = g_backBufferW, h2 = g_backBufferH;
        float tv2 = (w2 && h2) ? th2 * h2 / w2 : th2;
        for (int k = 0; ok && k < 8; ++k) {
            float c[3] = {(k & 1) ? mx[0] : mn[0], (k & 2) ? mx[1] : mn[1], (k & 4) ? mx[2] : mn[2]};
            float tv[3] = {c[0] - eyeLoc[0], c[1] - eyeLoc[1], c[2] - eyeLoc[2]};
            float tz = tv[0] * vf[0] + tv[1] * vf[1] + tv[2] * vf[2];
            if (tz < 2.0f) {
                ok = false;
                break;
            }
            float tx = tv[0] * vrt[0] + tv[1] * vrt[1] + tv[2] * vrt[2];
            float ty = tv[0] * vu[0] + tv[1] * vu[1] + tv[2] * vu[2];
            float nx, ny;
            TanToNdc(eye, g_drawOffCenter, tx / tz, ty / tz, th2, tv2, &nx, &ny);
            r[0] = (std::min)(r[0], nx);
            r[1] = (std::min)(r[1], ny);
            r[2] = (std::max)(r[2], nx);
            r[3] = (std::max)(r[3], ny);
        }
        if (ok)
            for (int i = 0; i < 4; ++i) g_hlNdc[eye][i] = r[i];
        g_hlState[eye] = ok ? st : 0;
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
    // Laser: partenza 25 unita' davanti alla canna lungo il raggio (la canna
    // stessa e' troppo vicina all'occhio per essere proiettata bene).
    {
        float dirv[3] = {p[0] - aimOrigin[0], p[1] - aimOrigin[1], p[2] - aimOrigin[2]};
        float dl = std::sqrt(dirv[0] * dirv[0] + dirv[1] * dirv[1] + dirv[2] * dirv[2]);
        bool lvis = false;
        if (dl > 30.0f) {
            float s0[3] = {aimOrigin[0] + dirv[0] / dl * 25.0f - eyeLoc[0], aimOrigin[1] + dirv[1] / dl * 25.0f - eyeLoc[1],
                           aimOrigin[2] + dirv[2] / dl * 25.0f - eyeLoc[2]};
            float z0 = s0[0] * vf[0] + s0[1] * vf[1] + s0[2] * vf[2];
            if (z0 > 1.0f) {
                float x0 = s0[0] * vrt[0] + s0[1] * vrt[1] + s0[2] * vrt[2];
                float y0 = s0[0] * vu[0] + s0[1] * vu[1] + s0[2] * vu[2];
                float lx, ly;
                TanToNdc(eye, g_drawOffCenter, x0 / z0, y0 / z0, th, tv, &lx, &ly);
                g_laserNdc[eye][0] = lx;
                g_laserNdc[eye][1] = ly;
                lvis = true;
            }
        }
        g_laserVisible[eye] = lvis && vr::control::g_laser && !vr::control::g_adsActive;
    }
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

// Diagnostica "hittest <z>" (23/10): linea orizzontale a estensione zero, come
// un colpo istantaneo, da 2 m davanti a Walker verso di lui a <z> unita' sopra
// il centro del Pawn. I colpi istantanei prendono la mesh (physics asset,
// BlockZeroExtent), non la capsula (solo BlockNonZeroExtent): dice se con
// Walker accucciato (physicalcrouch) il colpo gli passa sopra.
// FCheckResult: Actor +4, Location +8, Component +0x30, BoneName +0x34.
bool TraceHitActor(void* source, const float* start, const float* end, void** actor, float* loc, int32_t* bone) {
    __try {
        if (!kGWorldAddr || !kSingleLineCheckAddr) return false;
        void* world = *reinterpret_cast<void**>(kGWorldAddr);
        if (!world) return false;
        alignas(16) unsigned char hit[0x100] = {};
        const float extent[3] = {0.0f, 0.0f, 0.0f};
        reinterpret_cast<SingleLineCheck_t>(kSingleLineCheckAddr)(world, hit, source, end, start, kTraceFlags, extent, nullptr);
        *actor = *reinterpret_cast<void**>(hit + 4);
        std::memcpy(loc, hit + 8, 12);
        std::memcpy(bone, hit + 0x34, 4);
        return *actor != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void RunHitTest(void* pawn) {
    float z = vr::control::g_hitTestZ.exchange(-1000.0f);
    if (z <= -999.0f || !pawn) return;
    float loc[3];
    int32_t yaw = 0;
    uint32_t worldInfo = 0;
    if (!SafeCopy(loc, static_cast<char*>(pawn) + kOffActorLocation, 12) ||
        !SafeCopy(&yaw, static_cast<char*>(pawn) + kOffActorRotation + 4, 4) ||
        !TrySafeReadUint32(static_cast<char*>(pawn) + kOffActorWorldInfo, &worldInfo) || !worldInfo)
        return;
    float a = yaw * (6.28318530718f / 65536.0f), fx = std::cos(a), fy = std::sin(a);
    float start[3] = {loc[0] + fx * 200.0f, loc[1] + fy * 200.0f, loc[2] + z};
    float end[3] = {loc[0] - fx * 30.0f, loc[1] - fy * 30.0f, loc[2] + z};
    void* actor = nullptr;
    float at[3] = {};
    int32_t bone = 0;
    // Sorgente WorldInfo: col Pawn (o il Controller, suo Owner) Walker sarebbe escluso.
    bool hit = TraceHitActor(reinterpret_cast<void*>(worldInfo), start, end, &actor, at, &bone);
    std::string what = hit ? vr::script::ObjectName(actor) : std::string("nothing");
    if (hit && bone > 0) what += " bone " + vr::script::NameFromIndex(bone);
    char b[240];
    std::snprintf(b, sizeof(b), "HITTEST: line %+.0f units above Walker's centre -> %s", z, what.c_str());
    if (hit) std::snprintf(b + std::strlen(b), sizeof(b) - std::strlen(b), " (impact %+.1f)", at[2] - loc[2]);
    vr::log::Line(b);
}

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
    {
        // YPlayerController.CheckExecute (bytecode letto il 22/10):
        //   if (m_playerCam.Rotation.Pitch > m_executionMaxCamPitchAngle) return false;
        // cioe' esecuzione solo guardando piu' in basso del limite (+0xB90,
        // -1000 = -5,5 gradi; "Look down and press B"). La versione precedente
        // lo portava a -16384 (-90 gradi) col ragionamento invertito: bisognava
        // guardare esattamente in verticale e B dava solo pugni (tester 22/10,
        // nemico a terra visto dall'alto). In VR la testa non deve decidere:
        // limite al massimo, decide il nemico giustiziabile.
        int32_t maxPitch = 0;
        if (TrySafeReadUint32(static_cast<char*>(pc) + 0xB90, reinterpret_cast<uint32_t*>(&maxPitch)) && maxPitch != 0x7FFFFFFF) {
            int32_t v = 0x7FFFFFFF;
            if (SafeCopy(static_cast<char*>(pc) + 0xB90, &v, 4))
                vr::log::Line("EXECUTE: camera pitch limit for executions " + std::to_string(maxPitch) + " -> none (any head pitch).");
        }
    }
    {
        // Riquadro di rilevamento dell'esecuzione: deciso dai valori a ogni
        // tick, non una volta per Pawn (il gioco puo' rimetterlo; L16).
        float cur[6] = {};
        if (SafeCopy(cur, static_cast<char*>(pawn) + kOffPawnExecDetection, sizeof(cur)) && cur[1] > 0.0f &&
            cur[1] < 400.0f && (cur[0] > -80.0f || cur[1] < 120.0f || cur[2] > -110.0f || cur[3] < 110.0f || cur[4] > -120.0f)) {
            float wide[6] = {(std::min)(cur[0], -80.0f), (std::max)(cur[1], 120.0f), (std::min)(cur[2], -110.0f),
                             (std::max)(cur[3], 110.0f), (std::min)(cur[4], -120.0f), cur[5]};
            if (SafeCopy(static_cast<char*>(pawn) + kOffPawnExecDetection, wide, sizeof(wide))) {
                char b[200];
                snprintf(b, sizeof(b), "EXECUTE: detection box widened, forward %.0f..%.0f side %.0f..%.0f height %.0f..%.0f",
                         wide[0], wide[1], wide[2], wide[3], wide[4], wide[5]);
                vr::log::Line(b);
            }
        }
    }
    static uint64_t handled = 0;
    uint64_t intent = vr::control::g_meleeIntentMs;
    if (intent == handled || GetTickCount64() - intent > 400) return;
    handled = intent;
    if (vr::control::g_cinemaActive || g_gameLockedScene || TurretAimActive() || vr::control::g_gamePaused) return;
    int32_t myHealth = 0;
    float me[3];
    uint32_t worldInfo = 0, p = 0;
    if (!TrySafeReadUint32(static_cast<char*>(pawn) + kOffPawnHealth, reinterpret_cast<uint32_t*>(&myHealth)) ||
        myHealth <= 0 || !SafeCopy(me, static_cast<char*>(pawn) + 0x54, 12) ||
        !TrySafeReadUint32(static_cast<char*>(pawn) + kOffActorWorldInfo, &worldInfo) || !worldInfo ||
        !TrySafeReadUint32(reinterpret_cast<char*>(worldInfo) + kOffWorldInfoPawnList, &p))
        return;
    // Candidati: nemici a terra entro 2 m e entro 60 gradi da dove si guarda
    // (prima il piu' vicino in qualunque direzione, anche alle spalle: il
    // corpo scattava fino a 180 gradi, M4). Un nemico in piedi entro 1,5 m e
    // 45 gradi davanti ha la precedenza: il pugno e' per lui, niente giro.
    char* best = nullptr;
    float bestD = 200.0f, bestLoc[3] = {};
    bool standingInFront = false;
    const int32_t viewYaw = g_bodyYaw.load() + g_headHeadingYaw.load();  // direzione stabile (WI-1)
    for (int guard = 0; p && guard < 512; ++guard) {
        char* other = reinterpret_cast<char*>(p);
        uint32_t next = 0, flags = 0;
        TrySafeReadUint32(other + kOffPawnNextPawn, &next);
        int32_t health = 0;
        float loc[3];
        if (other != pawn && TrySafeReadUint32(other + kOffPawnHealth, reinterpret_cast<uint32_t*>(&health)) &&
            health > 0 && TrySafeReadUint32(other + kOffPawnDbnoFlags, &flags) && SafeCopy(loc, other + 0x54, 12) &&
            std::fabs(loc[2] - me[2]) < 150.0f) {
            float d = std::sqrt((loc[0] - me[0]) * (loc[0] - me[0]) + (loc[1] - me[1]) * (loc[1] - me[1]));
            if (d >= 200.0f) {
                p = next;
                continue;
            }
            int32_t rel = static_cast<int16_t>(RadiansToUnreal(std::atan2(loc[1] - me[1], loc[0] - me[0])) - viewYaw);
            // Giustiziabile per YGamePawn.CheckExecute (script, letto dal
            // bytecode 22/10): m_deathDBNOState (+0x48A) 1..3 oppure
            // m_canBeExecutedInDBNO sul PlayerReplicationInfo (+0x36C ->
            // +0x2D4 bit 0x20). I flag di knock-down (+0x5FC) non bastano al
            // gioco, ma restano per girare il corpo.
            uint8_t dbnoState = 0;
            uint32_t pri = 0, priFlags = 0;
            SafeCopy(&dbnoState, other + 0x48A, 1);
            bool executable = (dbnoState >= 1 && dbnoState <= 3) ||
                              (TrySafeReadUint32(other + 0x36C, &pri) && pri &&
                               TrySafeReadUint32(reinterpret_cast<char*>(pri) + 0x2D4, &priFlags) && (priFlags & 0x20));
            bool downed = (flags & kDbnoBits) != 0 || executable;
            if (!downed) {
                if (d < 150.0f && std::abs(rel) <= 65536 / 8 && !IsFriendOrCivilian(vr::script::ClassName(other)))
                    standingInFront = true;
            } else if (d < bestD && std::abs(rel) <= 65536 / 6 && !IsFriendOrCivilian(vr::script::ClassName(other))) {
                // Gia' preso da un'esecuzione o da un soccorso: non e' un bersaglio.
                struct {
                    uint32_t ret;
                } busy = {0};
                if (vr::control::g_headSampleOnDraw && vr::script::CallFunction(other, "IsBeingExecutedOrRevived", &busy) &&
                    (busy.ret & 1)) {
                    p = next;
                    continue;
                }
                bestD = d;
                best = other;
                std::memcpy(bestLoc, loc, sizeof(loc));
            }
        }
        p = next;
    }
    if (standingInFront) {
        if (best) vr::log::Line("EXECUTE: B with a standing enemy in front: no turn toward the downed one");
        return;
    }
    if (!best) {
        // Nessun nemico a terra riconosciuto: chi c'e' vicino (22/10, primo
        // nemico a terra dopo il container del cap. 1: solo pugni).
        uint32_t q = 0;
        TrySafeReadUint32(reinterpret_cast<char*>(worldInfo) + kOffWorldInfoPawnList, &q);
        std::string nearby;
        for (int guard = 0; q && guard < 512; ++guard) {
            char* other = reinterpret_cast<char*>(q);
            uint32_t next = 0, flags = 0;
            int32_t health = 0;
            float loc[3];
            TrySafeReadUint32(other + kOffPawnNextPawn, &next);
            if (other != pawn && SafeCopy(loc, other + 0x54, 12)) {
                float d = std::sqrt((loc[0] - me[0]) * (loc[0] - me[0]) + (loc[1] - me[1]) * (loc[1] - me[1]));
                if (d < 400.0f) {
                    TrySafeReadUint32(other + kOffPawnHealth, reinterpret_cast<uint32_t*>(&health));
                    TrySafeReadUint32(other + kOffPawnDbnoFlags, &flags);
                    int32_t rel = static_cast<int16_t>(RadiansToUnreal(std::atan2(loc[1] - me[1], loc[0] - me[0])) - viewYaw);
                    char b[200];
                    snprintf(b, sizeof(b), " %s d=%.0f dz=%.0f rel=%.0f deg health=%d flags5FC=%08X;",
                             vr::script::ClassName(other).c_str(), d, loc[2] - me[2], rel * 360.0f / 65536.0f, health, flags);
                    nearby += b;
                }
            }
            q = next;
        }
        vr::log::Line("EXECUTE: B pressed, no downed enemy recognised; pawns within 4 m:" + (nearby.empty() ? std::string(" none") : nearby));
        return;
    }
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
    // CheckExecute rifiuta l'esecuzione con Walker in copertura (IsInCover):
    // in VR ci si china sul nemico a terra e il gesto "abbassarsi vicino a un
    // riparo" mette Walker in copertura, poi B da' solo pugni (22/10). Si esce
    // dalla copertura senza animazione (LeaveCoverInternal(true), script)
    // prima che B arrivi al gioco.
    {
        uint32_t coverNow = 0;
        if (TrySafeReadUint32(static_cast<char*>(pawn) + 0x498, &coverNow) && (coverNow & 0xFF)) {
            struct {
                uint32_t noLeaveCoverSpecialMove;
                uint32_t ret;
            } lp = {1, 0};
            bool ok = vr::script::CallFunction(pc, "LeaveCoverInternal", &lp);
            vr::log::Line(std::string("EXECUTE: Walker was in cover, left it for the execution (") +
                          (ok ? ((lp.ret & 1) ? "done" : "refused by the game") : "call failed") + ")");
        }
    }
    // Condizioni di CheckExecute (YPlayerController / YGamePawn, script):
    // restrizione m_execution, pitch della camera, stato DBNO del nemico,
    // m_canBeExecutedInDBNO sul suo PlayerReplicationInfo (+0x2D4 bit 0x20),
    // Walker in copertura o in una mossa speciale. Il 22/10 (tester e nel
    // visore) il nemico a terra era riconosciuto ma il gioco dava solo pugni.
    uint32_t restr = 0, pri = 0, priFlags = 0, cover = 0, special = 0, flags5FC = 0;
    uint8_t dbnoState = 0;
    int32_t maxPitch = 0;
    TrySafeReadUint32(static_cast<char*>(pc) + 0xC30, &restr);
    TrySafeReadUint32(static_cast<char*>(pc) + 0xB90, reinterpret_cast<uint32_t*>(&maxPitch));
    SafeCopy(&dbnoState, best + 0x48A, 1);
    TrySafeReadUint32(best + kOffPawnDbnoFlags, &flags5FC);
    bool priOk = TrySafeReadUint32(best + 0x36C, &pri) && pri &&
                 TrySafeReadUint32(reinterpret_cast<char*>(pri) + 0x2D4, &priFlags);
    TrySafeReadUint32(static_cast<char*>(pawn) + 0x498, &cover);
    TrySafeReadUint32(static_cast<char*>(pawn) + 0x490, &special);
    char b[400];
    snprintf(b, sizeof(b),
             "EXECUTE: B near %s %.0f units away (dz %.0f), body turned %.0f degrees toward it | execution %s, cam pitch "
             "%.0f deg (limit %d), DBNO state %u flags5FC %08X, canBeExecutedInDBNO %s, Walker cover %u special move %u",
             vr::script::ClassName(best).c_str(), bestD, bestLoc[2] - me[2], delta * 360.0f / 65536.0f,
             (restr & 0x10000000) ? "FORBIDDEN by the game" : "allowed",
             static_cast<float>(static_cast<int16_t>(g_bodyPitch.load() + g_deltaPitch.load())) * 360.0f / 65536.0f, maxPitch,
             dbnoState, flags5FC, priOk ? ((priFlags & 0x20) ? "yes" : "NO") : "no PRI", cover & 0xFF, (special >> 8) & 0xFF);
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
// Convalida del bersaglio (YPlayerController vtable +0x550, 0x008389D0,
// Ghidra 22/10): il candidato +0xEA0 viene confermato in +0xE98 solo dopo
// due tracce asincrone di visibilita'; se cambia, contatori e tracce
// ripartono. Il gioco la chiama ogni frame col proprio candidato (centro
// dello schermo) e la mod con il suo: con due candidati diversi (ordine con
// la mano di supporto) la convalida non finiva mai ("not taken by the
// game", log 22/10). Con un bersaglio della mod, le chiamate del gioco con
// un altro candidato si scartano.
const uintptr_t& kSelectMarkableAddr = vr::addr::g[vr::addr::SelectMarkable];
typedef void(__thiscall* SelectMarkable_t)(void* pc, void* markable, float screenDist);
SelectMarkable_t g_SelectMarkable_Original = nullptr;
std::atomic<void*> g_squadModPick{nullptr};
void __fastcall Hooked_SelectMarkable(void* pc, void* /*edx*/, void* markable, float screenDist) {
    void* pick = g_squadModPick.load();
    if (vr::control::g_squadValidateLog > 0) {
        // Diagnostica ("squadpick"): ogni chiamata, con il candidato prima e dopo.
        vr::control::g_squadValidateLog.fetch_sub(1);
        uint32_t before = 0, after = 0, sel = 0;
        TrySafeReadUint32(static_cast<char*>(pc) + 0xEA0, &before);
        bool dropped = pick && markable != pick;
        if (!dropped) g_SelectMarkable_Original(pc, markable, screenDist);
        TrySafeReadUint32(static_cast<char*>(pc) + 0xEA0, &after);
        TrySafeReadUint32(static_cast<char*>(pc) + 0xE98, &sel);
        char b[300];
        snprintf(b, sizeof(b), "SQUAD validate: %s dist=%.1f (%s) candidate before=%s after=%s selected=%s",
                 markable ? vr::script::ClassName(markable).c_str() : "none", screenDist,
                 dropped ? "dropped" : (markable == pick ? "mod" : "game"),
                 before ? vr::script::ClassName(reinterpret_cast<void*>(before)).c_str() : "none",
                 after ? vr::script::ClassName(reinterpret_cast<void*>(after)).c_str() : "none",
                 sel ? vr::script::ClassName(reinterpret_cast<void*>(sel)).c_str() : "none");
        vr::log::Line(b);
        return;
    }
    if (pick && markable != pick) {
        static void* loggedFor = nullptr;
        if (loggedFor != pick) {
            loggedFor = pick;
            vr::log::Line("SQUAD: game's own candidate " + (markable ? vr::script::ClassName(markable) : std::string("none")) +
                          " ignored while the mod points at " + vr::script::ClassName(pick) + ".");
        }
        return;
    }
    g_SelectMarkable_Original(pc, markable, screenDist);
}

// Bersagli degli ordini che non sono Pawn: YCheapShooter (nemici semplificati
// che sparano da lontano, es. il cecchino sul cartellone del capitolo 1, che
// il tutorial chiede di far abbattere: 18/10, "non riesco a dare l'ordine").
// La lista dei Pawn non li contiene: si cercano in GObjects (classe e
// sottoclassi), ogni 2 s mentre il tasto squadra e' tenuto.
constexpr uint32_t kOffCheapShooterHealth = 0x264;  // YCheapShooter.m_health (float)
// Censimento delle interazioni fisiche del livello (fase 0 del piano, R-T4,
// "interactlog on"): ogni 2 s gli attori YTrigger_*Interaction (torrette,
// corde, zipline) in GObjects, con posizione, distanza da Walker, icona
// (GetCurrentIconPosition) e abilitazione (IsEnabledForTacView) nel log.
// Diagnostica: serve a costruire la tabella {tipo, posizione, comando} per
// le fasi successive (presa per usare).
// Mani nel mondo (bersagli IK scritti da UpdateArms): fase 1, prossimita'.
float g_handWorld[2][3] = {{0, 0, 0}, {0, 0, 0}};
bool g_handWorldValid[2] = {false, false};
float g_handAxesWorld[2][3][3] = {};  // assi della mano (righe X, Y, Z), per gli oggetti tenuti (props.cpp)

struct Interaction {
    void* obj;
    uint32_t cls;
    std::string name;
    float loc[3];
    float grab[3];  // punto da afferrare: inizio della zipline, ingresso della torretta, se letti; altrimenti loc
    bool pickup = false;  // classe con "Pickup" (armi a terra della voce 6: forse le prende la mano, vr::props::WeaponForHand)
};
std::vector<Interaction> g_interactions;

void InteractionScan(void* pawn) {
    if (!pawn) return;
    static ULONGLONG nextScan = 0;
    if (GetTickCount64() < nextScan) return;
    nextScan = GetTickCount64() + 2000;
    ScanTimer scanTimer;  // L12
    g_interactions.clear();
    uint32_t data = 0, num = 0;
    if (!TrySafeReadUint32(reinterpret_cast<void*>(kGObjectsAddr), &data) ||
        !TrySafeReadUint32(reinterpret_cast<void*>(kGObjectsAddr + 4), &num) || !data || num > 2000000)
        return;
    float pl[3] = {0, 0, 0};
    SafeCopy(pl, static_cast<char*>(pawn) + 0x54, 12);
    bool log = vr::control::g_interactLog;
    for (uint32_t i = 0; i < num; ++i) {
        uint32_t o = 0;
        if (!TrySafeReadUint32(reinterpret_cast<void*>(data + 4 * i), &o) || !o) continue;
        void* obj = reinterpret_cast<void*>(o);
        std::string cls = vr::script::ClassName(obj);
        // Trigger delle interazioni e oggetti da raccogliere (fase 4: armi e
        // munizioni a terra, classi con "Pickup"): stessa zona di presa.
        bool trig = cls.find("Interaction") != std::string::npos && cls.rfind("YTrigger_", 0) == 0;
        bool pickup = cls.find("Pickup") != std::string::npos && cls.rfind("Y", 0) == 0;
        if (!trig && !pickup) continue;
        if (cls == "Class") continue;
        // Armi lasciate dai nemici (voce 6): restano nella lista; quelle che la
        // mano sinistra prende da sola le salta InteractionAffordance, per
        // quella mano e solo quando la presa della mano funziona davvero
        // (revisione: prima uscivano sempre con weaponpickup acceso, anche con
        // grabprops spento, per la destra e per le armi senza PhysicsAsset o
        // temporanee che la mano non prende: il grip non faceva piu' niente).
        std::string name = vr::script::ObjectName(obj);
        if (name.rfind("Default__", 0) == 0) continue;  // oggetti predefiniti delle classi
        Interaction it;
        it.obj = obj;
        it.cls = ObjClass(obj);
        it.name = cls;
        it.pickup = pickup;
        if (!SafeCopy(it.loc, static_cast<char*>(obj) + 0x54, 12)) continue;
        std::memcpy(it.grab, it.loc, sizeof(it.grab));
        float d = std::sqrt((it.loc[0] - pl[0]) * (it.loc[0] - pl[0]) + (it.loc[1] - pl[1]) * (it.loc[1] - pl[1]) +
                            (it.loc[2] - pl[2]) * (it.loc[2] - pl[2]));
        // Punto di presa dalle native con parametro out (exec stub in Ghidra:
        // GetRopeStart(out vector) -> bool, GetEntryPoint(out vector, out
        // rotator) -> bool); solo per i trigger vicini.
        if (d < 1500.0f) {
            if (cls == "YTrigger_ZiplineInteraction") {
                struct {
                    float start[3];
                    uint32_t ret;
                } p = {{0, 0, 0}, 0};
                if (vr::script::CallFunction(obj, "GetRopeStart", &p) && p.ret) std::memcpy(it.grab, p.start, sizeof(it.grab));
            } else if (cls == "YTrigger_TurretInteraction") {
                struct {
                    float loc[3];
                    int32_t rot[3];
                    uint32_t ret;
                } p = {{0, 0, 0}, {0, 0, 0}, 0};
                if (vr::script::CallFunction(obj, "GetEntryPoint", &p) && p.ret) std::memcpy(it.grab, p.loc, sizeof(it.grab));
            }
        }
        // GetCurrentIconPosition e IsEnabledForTacView (proposti dalla review)
        // sollevano un'eccezione dentro il gioco sulle istanze dei trigger
        // (componente nullo in +0x238; 21/10): non si usano.
        if (log) {
            char b[300];
            std::snprintf(b, sizeof(b), "INTERACT: %s %s at (%.0f, %.0f, %.0f) dist %.0f grab (%.0f, %.0f, %.0f)", cls.c_str(),
                          name.c_str(), it.loc[0], it.loc[1], it.loc[2], d, it.grab[0], it.grab[1], it.grab[2]);
            vr::log::Line(b);
        }
        g_interactions.push_back(it);
        if (g_interactions.size() >= 64) break;
    }
    if (log && g_interactions.empty()) vr::log::Line("INTERACT: no YTrigger_*Interaction actors in this level.");
}

// Fase 1 (affordance): una mano entro 25 cm dal punto di presa di
// un'interazione -> impulso aptico, piu' fitto avvicinandosi; una riga nel
// log all'ingresso nella zona (le fasi successive usano la stessa zona per
// "grip = usa").
void InteractionAffordance() {
    static ULONGLONG lastPulse[2] = {0, 0};
    static bool inZone[2] = {false, false};
    float scale = vr::control::g_posScale;
    if (scale <= 0.0f || g_interactions.empty()) return;
    float zone = 0.25f * scale;
    for (int h = 0; h < 2; ++h) {
        if (!g_handWorldValid[h]) {
            inZone[h] = false;
            vr::control::g_interactZone[h] = false;
            continue;
        }
        float best = zone;
        const Interaction* nearest = nullptr;
        for (const Interaction& it : g_interactions) {
            float dx = it.grab[0] - g_handWorld[h][0], dy = it.grab[1] - g_handWorld[h][1], dz = it.grab[2] - g_handWorld[h][2];
            float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            // Arma a terra che questa mano prende da sola (voce 6): non e' un
            // tasto A (l'animazione della raccolta del gioco insieme alla presa).
            // Ne' un'arma tornata nel pool: dopo il passaggio alla destra resta
            // dove l'ha presa DoPickup, sulla mano dell'arma, e la faceva
            // vibrare ogni 0,3 s (collaudo della revisione, 27/10).
            if (d < best && it.pickup && (vr::props::WeaponPickupGone(it.obj) || vr::props::WeaponForHand(it.obj, h))) continue;
            if (d < best) {
                best = d;
                nearest = &it;
            }
        }
        if (!nearest) {
            inZone[h] = false;
            vr::control::g_interactZone[h] = false;
            continue;
        }
        vr::control::g_interactZone[h] = true;
        if (!inZone[h]) {
            inZone[h] = true;
            vr::log::Line(std::string("INTERACT: ") + (h ? "left" : "right") + " hand at " + nearest->name + " (" +
                          std::to_string(static_cast<int>(best / scale * 100.0f)) + " cm)");
        }
        ULONGLONG now = GetTickCount64();
        unsigned period = best < 0.10f * scale ? 120 : 300;
        if (now - lastPulse[h] >= period) {
            lastPulse[h] = now;
            vr::input::PulseHand(h ? 0 : 1, 40);  // PulseHand: 0 sinistra, 1 destra
        }
    }
}

std::vector<void*> CheapShooters() {
    ScanTimer scanTimer;  // L12
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
                // Non l'oggetto predefinito della classe: sta all'origine della
                // mappa e risultava il "cheap shooter" piu' vicino (22/10, log
                // del tester al cartellone: sempre 30-60 gradi dalla canna).
                if (vr::script::ObjectName(reinterpret_cast<void*>(o)).rfind("Default__", 0) != 0)
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

// Segnaposto tattici del livello (YTacticalMarker): bersagli degli ordini che
// non sono nemici, es. lo stordimento del tutorial del cap. 1 (cp01-cp02:
// m_contextCommand YCTX_StunEnemies = 18 mentre l'attacco e' vietato).
// Bytecode del 22/10: GetContextCommandFor di un nemico da' 0 con l'attacco
// vietato, il comando viene solo dal marker. Stessa scansione di CheapShooters.
constexpr uintptr_t kOffMarkerFlags = 0x210;    // m_enabled 0x1
constexpr uintptr_t kOffMarkerCommand = 0x21C;  // m_contextCommand (YE_ContextCommand)
std::vector<void*> TacticalMarkers() {
    static uint32_t cls = 0;
    static std::vector<void*> cache;
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
                vr::script::ObjectName(reinterpret_cast<void*>(o)) == "YTacticalMarker" &&
                vr::script::ClassName(reinterpret_cast<void*>(o)) == "Class")
                cls = o;
        }
        if (!cls) {
            nextScan = GetTickCount64() + 60000;
            return cache;
        }
    }
    for (uint32_t i = 0; i < num; ++i) {
        uint32_t o = 0;
        if (!TrySafeReadUint32(reinterpret_cast<void*>(data + 4 * i), &o) || !o) continue;
        uint32_t c = ObjClass(reinterpret_cast<void*>(o));
        for (int depth = 0; c && depth < 4; ++depth) {
            if (c == cls) {
                if (vr::script::ObjectName(reinterpret_cast<void*>(o)).rfind("Default__", 0) != 0)
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

bool MarkerUsable(void* m) {
    uint32_t flags = 0;
    uint8_t cmd = 0;
    return TrySafeReadUint32(static_cast<char*>(m) + kOffMarkerFlags, &flags) && (flags & 1) &&
           SafeCopy(&cmd, static_cast<char*>(m) + kOffMarkerCommand, 1) && cmd != 0 && cmd != 7 && cmd != 8;
}

// Dati per l'HUD al polso (R-U3): solo in gioco, in prima persona, Walker vivo.
void PublishWristData(char* cam) {
    void* pawn = HasCameraSignature(cam) ? PlayerPawnIfViewTarget(cam) : nullptr;
    bool show = pawn && vr::control::g_firstPerson && !vr::control::g_cinemaActive && !vr::control::g_gamePaused &&
                !vr::menu::IsOpen() && PawnAlive(pawn);
    int32_t health = -1;
    if (pawn) TrySafeReadUint32(static_cast<char*>(pawn) + 0x304, reinterpret_cast<uint32_t*>(&health));
    vr::wrist::SetData(show, vr::control::g_weaponAmmo[1].load(), vr::control::g_weaponAmmo[0].load(), health, g_weaponInfo.cls);
}

std::string g_squadClosestCls;  // candidato piu' vicino al raggio durante la pressione (diagnostica)
float g_squadClosestDeg = 999.0f;

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
            // Ordini bloccati dal gioco (YS_ControlRestrictions in +0xC30: bit
            // impostato = azione vietata; m_squadControl 0x8000). Al cap. 1
            // restano vietati in alcune fasi: il tester premeva il grip senza
            // alcuna risposta (22/10). Lo si dice nel visore.
            uint32_t restr = 0, squadObj = 0;
            int squadCmd = -1;
            uint8_t sc = 0;
            if (HasCameraSignature(cam) && TrySafeReadUint32(cam - 0x77C + 0xE7C, &squadObj) && squadObj &&
                SafeCopy(&sc, reinterpret_cast<char*>(squadObj) + 0x2E0, 1))
                squadCmd = sc;
            if (HasCameraSignature(cam) && TrySafeReadUint32(cam - 0x77C + 0xC30, &restr) && (restr & 0x8000)) {
                vr::toast::Notify("Squad orders are not available yet at this point of the game");
                vr::log::Line("SQUAD: orders locked by the game here (control restrictions " + std::to_string(restr) + ").");
            }
            snprintf(b, sizeof(b),
                     "SQUAD: button pressed (head, restrictions %08X, squad doing command %d, pawn=%d, aim hand=%d, arms=%d, barrel aim=%d, cheap "
                     "shooters=%u); game target %s",
                     restr, squadCmd, pawn != nullptr, (int)vr::control::g_aimHand.load(),
                     (int)g_armsActive.load(), (int)g_muzzleAimValid.load(), (unsigned)CheapShooters().size(),
                     sel ? vr::script::ClassName(reinterpret_cast<void*>(sel)).c_str() : "none");
        } else {
            snprintf(b, sizeof(b), "SQUAD: button released; mod target %s, game target %s; closest candidate %s at %.1f deg",
                     everPicked ? vr::script::ClassName(everPicked).c_str() : "none",
                     sel ? vr::script::ClassName(reinterpret_cast<void*>(sel)).c_str() : "none",
                     g_squadClosestCls.empty() ? "none" : g_squadClosestCls.c_str(), g_squadClosestDeg);
            g_squadClosestCls.clear();
            g_squadClosestDeg = 999.0f;
        }
        vr::log::Line(b);
    }
    static void* squadPickTarget = nullptr;
    static int squadPickFrames = 0;
    // Diagnostica "squadpick <classe>": convalida chiamata dal mod su un Pawn
    // vivo di quella classe, senza tasto; le righe "SQUAD validate" dicono se
    // il candidato viene accettato.
    {
        std::string req = vr::control::TakeSquadPickRequest();
        if (!req.empty() && pawn && HasCameraSignature(cam)) {
            uint32_t wi = 0, pp = 0;
            void* found = nullptr;
            if (TrySafeReadUint32(static_cast<char*>(pawn) + kOffActorWorldInfo, &wi) && wi &&
                TrySafeReadUint32(reinterpret_cast<char*>(wi) + kOffWorldInfoPawnList, &pp)) {
                for (int guard = 0; pp && guard < 512 && !found; ++guard) {
                    char* other = reinterpret_cast<char*>(pp);
                    int32_t hp = 0;
                    std::string cn = vr::script::ClassName(other);
                    for (char& ch : cn) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
                    if (other != pawn && TrySafeReadUint32(other + kOffPawnHealth, reinterpret_cast<uint32_t*>(&hp)) && hp > 0 &&
                        cn.find(req) != std::string::npos)  // i comandi arrivano in minuscolo
                        found = other;
                    TrySafeReadUint32(other + kOffPawnNextPawn, &pp);
                }
            }
            if (found && g_SelectMarkable_Original) {
                vr::log::Line("SQUAD: squadpick -> " + vr::script::ClassName(found) + " (held for 60 frames)");
                squadPickTarget = found;
                squadPickFrames = 60;
                vr::control::g_squadValidateLog = 6;
            } else {
                vr::log::Line("SQUAD: squadpick: no live pawn matching \"" + req + "\".");
            }
        }
        // Emulazione del tasto tenuto: il nostro candidato a ogni frame, quelli
        // del gioco scartati; alla fine lo stato della convalida.
        if (squadPickFrames > 0 && HasCameraSignature(cam)) {
            char* pc = cam - 0x77C;
            g_squadModPick = squadPickTarget;
            Hooked_SelectMarkable(pc, nullptr, squadPickTarget, 0.0f);
            if (--squadPickFrames == 0) {
                uint32_t sel = 0, cand = 0, st = 0;
                uint8_t ci = 0, cf = 0;
                TrySafeReadUint32(pc + 0xE98, &sel);
                TrySafeReadUint32(pc + 0xEA0, &cand);
                TrySafeReadUint32(pc + 0x5E8, &st);
                SafeCopy(&ci, pc + 0x74C, 1);
                SafeCopy(&cf, pc + 0x74D, 1);
                char b[260];
                snprintf(b, sizeof(b), "SQUAD: squadpick result after 60 frames: selected=%s candidate=%s visible=%d checkIndex=%u failCount=%u",
                         sel ? vr::script::ClassName(reinterpret_cast<void*>(sel)).c_str() : "none",
                         cand ? vr::script::ClassName(reinterpret_cast<void*>(cand)).c_str() : "none", (st >> 31) & 1, ci, cf);
                vr::log::Line(b);
                g_squadModPick = nullptr;
                squadPickTarget = nullptr;
            }
        }
    }
    // m_squadCommandModeOn non si accende tenendo il tasto (la selezione del
    // gioco gira sempre, per l'HUD): conta il tasto squadra passato al gioco.
    // Attacco vietato dal gioco (m_squadCommandAttack 0x200000) ma ordini
    // permessi: e' un altro comando del tutorial (stordimento a cp01). La
    // convalida (0x008389D0) esce subito solo se il comando che la squadra
    // sta eseguendo (pc+0xE7C YAIPlayerSquad, +0x2E0) e' un attacco (7/8):
    // i nemici restano bersagli validi (letto con Ghidra il 22/10).
    uint32_t restrNow = 0;
    bool readRestr = HasCameraSignature(cam) && TrySafeReadUint32(cam - 0x77C + 0xC30, &restrNow);
    bool attackLocked = readRestr && (restrNow & 0x200000);
    if (!pawn || !held || (readRestr && (restrNow & 0x8000))) {
        lastPick = nullptr;
        g_squadModPick = nullptr;
        return;
    }
    // Raggio dagli occhi verso il punto in cui arriva la canna (il mirino
    // verde): lo stesso della camera logica del gioco. La visibilita' si
    // controlla dagli occhi, come fa il gioco: la canna all'altezza del fianco
    // dietro una copertura "vede" solo la copertura.
    // Solo lo sguardo (22/10, richiesta: canna e mano di supporto davano
    // bersagli diversi da quello che il giocatore guardava). Il gioco sceglie
    // allo stesso modo (centro della vista); la mod serve per i bersagli che
    // il gioco da solo non convalida (cecchino sul cartellone, 18/10).
    float eo[3];
    std::memcpy(eo, cam + 0x10, sizeof(eo));  // occhi
    float rayO[1][3], rayD[1][3];
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
    float hr[3], hu[3];
    RotatorToAxes(g_lastViewRot[0], g_lastViewRot[1], 0, rayD[0], hr, hu);
    std::memcpy(rayO[0], eo, sizeof(eo));
    const uint32_t pawnList = p;
    for (int pass = 0; pass < 1 && !best; ++pass) {
        const float* dir = rayD[pass];
        const float* o = rayO[pass];
        // 7 gradi sui Pawn: la testa punta meno precisa della canna (3 gradi,
        // ereditati dalla mira con l'arma: allo stordente del cap. 1 il nemico
        // era a 4-11 gradi e la mod non lo sceglieva, 22/10). I nemici
        // semplificati (cecchino sul cartellone) sono bersagli scriptati per
        // gli ordini: 10 gradi.
        const float tolPawn = 0.1228f, tolCheap = 0.1763f;
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
                float v[3] = {loc[0] - o[0], loc[1] - o[1], loc[2] - o[2]};
                float along = v[0] * dir[0] + v[1] * dir[1] + v[2] * dir[2];
                if (!friendOrCivil && along > 50.0f) {
                    float perp[3] = {v[0] - dir[0] * along, v[1] - dir[1] * along, v[2] - dir[2] * along};
                    float off = std::sqrt(perp[0] * perp[0] + perp[1] * perp[1] + perp[2] * perp[2]);
                    // Tolleranza: 7 gradi, e almeno ~45 unita' (un corpo) da vicino.
                    float tol = std::fmax(along * tolPawn, 45.0f);
                    float score = off / along;
                    if (pass == 0 && std::atan2(off, along) * 57.2957795f < g_squadClosestDeg) {
                        g_squadClosestDeg = std::atan2(off, along) * 57.2957795f;
                        g_squadClosestCls = cls;
                    }
                    float hitTime = 1.0f;
                    // Traccia solo i candidati vicini al raggio (L13: prima ogni
                    // Pawn del livello, due passate, due volte per tick).
                    bool blocked = off < 1.5f * tol && TraceLine(pawn, o, loc, &hitTime) &&
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
            float v[3] = {loc[0] - o[0], loc[1] - o[1], loc[2] - o[2]};
            float along = v[0] * dir[0] + v[1] * dir[1] + v[2] * dir[2];
            if (along <= 50.0f) continue;
            float perp[3] = {v[0] - dir[0] * along, v[1] - dir[1] * along, v[2] - dir[2] * along};
            float off = std::sqrt(perp[0] * perp[0] + perp[1] * perp[1] + perp[2] * perp[2]);
            float tol = std::fmax(along * tolCheap, 60.0f);  // 10 gradi: bersagli piccoli e lontani
            if (pass == 0 && std::atan2(off, along) * 57.2957795f < g_squadClosestDeg) {
                g_squadClosestDeg = std::atan2(off, along) * 57.2957795f;
                g_squadClosestCls = vr::script::ClassName(cs) + " (cheap shooter)";
            }
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
    (void)attackLocked;  // i nemici restano: la convalida la fa il gioco (vedi Hooked_SelectMarkable)
    // Compagni a terra (Adams, Lugo): l'ordine di cura (GetHealCommand,
    // bytecode 22/10: m_isDownButNotOut e cura non vietata, 0x800000) ha la
    // precedenza sui nemici vicini allo sguardo. Prima i compagni erano
    // esclusi e con un nemico accanto la mod sceglieva il nemico.
    if (!(readRestr && (restrNow & 0x800000))) {
        uint32_t q = pawnList;
        void* healBest = nullptr;
        float healScore = 1e9f;
        for (int guard = 0; q && guard < 512; ++guard) {
            char* other = reinterpret_cast<char*>(q);
            uint32_t next = 0, flags = 0;
            float loc[3];
            TrySafeReadUint32(other + kOffPawnNextPawn, &next);
            if (other != pawn && TrySafeReadUint32(other + kOffPawnDbnoFlags, &flags) && (flags & 0x40000) &&
                SafeCopy(loc, other + 0x54, 12)) {
                std::string cls = vr::script::ClassName(other);
                if (cls.find("Adams") != std::string::npos || cls.find("Lugo") != std::string::npos) {
                    float v[3] = {loc[0] - eo[0], loc[1] - eo[1], loc[2] - eo[2]};
                    float along = v[0] * rayD[0][0] + v[1] * rayD[0][1] + v[2] * rayD[0][2];
                    if (along > 30.0f) {
                        float perp[3] = {v[0] - rayD[0][0] * along, v[1] - rayD[0][1] * along, v[2] - rayD[0][2] * along};
                        float off = std::sqrt(perp[0] * perp[0] + perp[1] * perp[1] + perp[2] * perp[2]);
                        if (off < std::fmax(along * 0.1405f, 60.0f) && off / along < healScore) {
                            healBest = other;
                            healScore = off / along;
                        }
                    }
                }
            }
            q = next;
        }
        if (healBest) {
            best = healBest;
            bestScore = healScore;
        }
    }
    if (!best) {
        // Segnaposto tattici lungo lo sguardo (10 gradi, almeno 80 unita':
        // sono aree, non corpi). Visibilita' non controllata: li piazza il livello.
        for (void* m : TacticalMarkers()) {
            float loc[3];
            if (!MarkerUsable(m) || !SafeCopy(loc, static_cast<char*>(m) + 0x54, 12)) continue;
            float v[3] = {loc[0] - eo[0], loc[1] - eo[1], loc[2] - eo[2]};
            float along = v[0] * rayD[0][0] + v[1] * rayD[0][1] + v[2] * rayD[0][2];
            if (along <= 50.0f) continue;
            float perp[3] = {v[0] - rayD[0][0] * along, v[1] - rayD[0][1] * along, v[2] - rayD[0][2] * along};
            float off = std::sqrt(perp[0] * perp[0] + perp[1] * perp[1] + perp[2] * perp[2]);
            float tol = std::fmax(along * 0.1763f, 80.0f);
            if (m == lastPick && off < 1.5f * tol) lastPickScore = off / along;
            if (off < tol && off / along < bestScore) {
                best = m;
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
        std::string lcls = vr::script::ClassName(lastPick);
        if (lcls.find("CheapShooter") != std::string::npos)
            alive = SafeCopy(&fhp, static_cast<char*>(lastPick) + kOffCheapShooterHealth, 4) && fhp > 0.0f;
        else if (lcls.find("TacticalMarker") != std::string::npos)
            alive = MarkerUsable(lastPick);
        else
            alive = TrySafeReadUint32(static_cast<char*>(lastPick) + kOffPawnHealth, reinterpret_cast<uint32_t*>(&hp)) &&
                    hp > 0;
        if (alive) best = lastPick;
    }
    if (!best) {
        lastPick = nullptr;
        g_squadModPick = nullptr;
        return;
    }
    struct {
        void* markable;
        float screenDist;
    } parms = {best, 0.0f};
    g_squadModPick = best;
    // Il gioco convalida la scelta in modo asincrono (traccia di visibilita'):
    // l'esito si controlla qualche frame dopo, non subito.
    static void* pending = nullptr;
    static int pendingFrames = 0;
    if (pending && ++pendingFrames >= 10) {
        uint32_t sel = 0;
        TrySafeReadUint32(cam - 0x77C + kOffPcSelectedMarkable, &sel);
        std::string why;
        if (reinterpret_cast<void*>(sel) != pending) {
            // Perche' il gioco non l'ha preso? Campi della convalida (FUN_008389d0,
            // Ghidra 22/10): flag di stato +0xC30/+0xC34, bit 31 di +0x5E8
            // (visibile), contatori +0x74C/+0x74D (check index / fail count),
            // candidato +0xEA0, miglior distanza a schermo +0xE94.
            char* pc = cam - 0x77C;
            uint32_t f0 = 0, f1 = 0, st = 0, cand = 0, bufm = 0;
            uint8_t ci = 0, cf = 0;
            float bestD = 0.0f;
            TrySafeReadUint32(pc + 0xC30, &f0);
            TrySafeReadUint32(pc + 0xC34, &f1);
            TrySafeReadUint32(pc + 0x5E8, &st);
            TrySafeReadUint32(pc + 0xEA0, &cand);
            TrySafeReadUint32(pc + 0xE9C, &bufm);
            SafeCopy(&ci, pc + 0x74C, 1);
            SafeCopy(&cf, pc + 0x74D, 1);
            SafeCopy(&bestD, pc + 0xE94, 4);
            char b[260];
            snprintf(b, sizeof(b), " [flags C30=%08X C34=%08X visible=%d checkIndex=%u failCount=%u bestDist=%.3g candidate=%s buffer=%s]",
                     f0, f1, (st >> 31) & 1, ci, cf, bestD,
                     cand ? vr::script::ClassName(reinterpret_cast<void*>(cand)).c_str() : "none",
                     bufm ? vr::script::ClassName(reinterpret_cast<void*>(bufm)).c_str() : "none");
            why = b;
        }
        vr::log::Line("SQUAD: command target along the view: " + vr::script::ClassName(pending) +
                      (reinterpret_cast<void*>(sel) == pending ? " (selected)" : " (not taken by the game)") + why);
        pending = nullptr;
    }
    // Chiamata diretta all'implementazione (vtable +0x550): via ProcessEvent
    // la funzione nativa non veniva mai eseguita (22/10: candidato +0xEA0
    // sempre "none" dopo la nostra chiamata; i "selected" lungo la canna
    // erano la convalida del gioco sul suo stesso candidato).
    if (g_SelectMarkable_Original) g_SelectMarkable_Original(cam - 0x77C, best, 0.0f);
    else vr::script::CallFunction(cam - 0x77C, "SetAsCurrentSelectedMarkable", &parms);
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
const uintptr_t& kCalcWeaponFireAddr = vr::addr::g[vr::addr::CalcWeaponFire];

// UCanvas::execProject (0x00A15D90): quante volte per tick l'HUD proietta
// punti del mondo sulla tela (M19: quei punti sono gia' nella proiezione
// decentrata dell'occhio e il rimappaggio della tela li sposta una seconda
// volta; prima di correggere serve sapere chi lo usa). Contati in PROFILE.
const uintptr_t& kCanvasProjectAddr = vr::addr::g[vr::addr::CanvasProject];
std::atomic<uint32_t> g_canvasProjectCalls{0};
typedef void(__thiscall* ExecCanvasProject_t)(void* self, void* stack, void* result);
ExecCanvasProject_t g_ExecCanvasProject_Original = nullptr;
class CanvasProjectThunk {
public:
    void Detour(void* stack, void* result) {
        g_canvasProjectCalls.fetch_add(1, std::memory_order_relaxed);
        g_ExecCanvasProject_Original(this, stack, result);
    }
};
void* CanvasProjectThunkAddress() {
    union {
        void (CanvasProjectThunk::*mfp)(void*, void*);
        void* fp;
    } c;
    c.mfp = &CanvasProjectThunk::Detour;
    return c.fp;
}

// Dimensione del viewport vista dai menu (UIScene di UE3, non Scaleform):
// UGameViewportClient::GetViewportSize (0x00B45070, __thiscall(this, float*
// out[2])) e UUIScreenObject::GetViewportSize (0x00574390, -> UBOOL, la
// dimensione memorizzata nella scena). Con lo schermo virtuale attivo i menu,
// scalati per l'altezza della finestra quasi quadrata (2400x2500), erano
// larghi 4444 px e tagliati a destra (21/10): si dichiara un'altezza 16:9
// (larghezza * 9/16), cosi' il menu si dispone per la larghezza.
const uintptr_t& kViewportSizeAddr = vr::addr::g[vr::addr::ViewportSize];
const uintptr_t& kUiViewportSizeAddr = vr::addr::g[vr::addr::UiViewportSize];
typedef void(__thiscall* ViewportSize_t)(void* self, float* out);
typedef int(__thiscall* UiViewportSize_t)(void* self, float* out);
ViewportSize_t g_ViewportSize_Original = nullptr;
UiViewportSize_t g_UiViewportSize_Original = nullptr;
std::atomic<uint32_t> g_uiSizeClamped{0};
inline void ClampUiSize(float* out) {
    static std::atomic<int> logged{0};
    if (!out) return;
    if (logged.load() < 4) {
        int n = logged.fetch_add(1);
        if (n < 4) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "UISIZE: getter -> %.0f x %.0f (cinema %d, clamp %d)", out[0], out[1],
                          vr::control::g_cinemaActive ? 1 : 0, vr::control::g_ui169 ? 1 : 0);
            vr::log::Line(buf);
        }
    }
    if (!vr::control::g_ui169 || !vr::control::g_cinemaActive || !(out[0] > 0.0f) || !(out[1] > 0.0f)) return;
    float h169 = out[0] * 9.0f / 16.0f;
    if (out[1] > h169 + 1.0f) {
        out[1] = h169;
        g_uiSizeClamped.fetch_add(1, std::memory_order_relaxed);
    }
}
class ViewportSizeThunk {
public:
    void Detour(float* out) {
        g_ViewportSize_Original(this, out);
        ClampUiSize(out);
    }
};
class UiViewportSizeThunk {
public:
    int Detour(float* out) {
        int r = g_UiViewportSize_Original(this, out);
        if (r) ClampUiSize(out);
        return r;
    }
};
// UGameUISceneClient::GetViewportSize(const UUIScene*, FVector2D& out)
// (0x00F539E0, slot +0x150 della vtable del scene client, chiamata da
// UUIScene::UpdateViewportSize e dagli aggiornamenti delle scene attive):
// e' la vera sorgente della dimensione con cui i menu si dispongono.
const uintptr_t& kSceneViewportSizeAddr = vr::addr::g[vr::addr::SceneViewportSize];
typedef int(__thiscall* SceneViewportSize_t)(void* self, void* scene, float* out);
SceneViewportSize_t g_SceneViewportSize_Original = nullptr;
class SceneViewportSizeThunk {
public:
    int Detour(void* scene, float* out) {
        int r = g_SceneViewportSize_Original(this, scene, out);
        if (r) ClampUiSize(out);
        return r;
    }
};
void* SceneViewportSizeThunkAddress() {
    union { int (SceneViewportSizeThunk::*mfp)(void*, float*); void* fp; } c;
    c.mfp = &SceneViewportSizeThunk::Detour;
    return c.fp;
}
void* ViewportSizeThunkAddress() {
    union { void (ViewportSizeThunk::*mfp)(float*); void* fp; } c;
    c.mfp = &ViewportSizeThunk::Detour;
    return c.fp;
}
void* UiViewportSizeThunkAddress() {
    union { int (UiViewportSizeThunk::*mfp)(float*); void* fp; } c;
    c.mfp = &UiViewportSizeThunk::Detour;
    return c.fp;
}

// Filtro dei crash del motore: LONG __cdecl(EXCEPTION_POINTERS*) (vedi l'installazione).
const uintptr_t& kEngineCrashFilterAddr = vr::addr::g[vr::addr::EngineCrashFilter];
typedef LONG(__cdecl* EngineCrashFilter_t)(EXCEPTION_POINTERS* info);
EngineCrashFilter_t g_EngineCrashFilter_Original = nullptr;
LONG __cdecl Hooked_EngineCrashFilter(EXCEPTION_POINTERS* info) {
    vr::log::ReportCrash(info);
    return g_EngineCrashFilter_Original ? g_EngineCrashFilter_Original(info) : EXCEPTION_EXECUTE_HANDLER;
}
struct FVec3 {
    float x, y, z;
};
typedef void(__thiscall* CalcWeaponFire_t)(void* self, FVec3 start, FVec3 end, void* impact);
bool GunBarrel(void* pawn, float* origin, float* axis);
CalcWeaponFire_t g_CalcWeaponFire_Original = nullptr;

// Base ortonormale attorno a una direzione (destra e su rispetto alla
// verticale del mondo), per riportare la deviazione del cono da una
// direzione all'altra.
void DirBasis(const float* f, float* r, float* u) {
    float up[3] = {0.0f, 0.0f, 1.0f};
    if (std::fabs(f[2]) > 0.99f) up[0] = 1.0f, up[2] = 0.0f;
    r[0] = up[1] * f[2] - up[2] * f[1];
    r[1] = up[2] * f[0] - up[0] * f[2];
    r[2] = up[0] * f[1] - up[1] * f[0];
    float n = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (n < 1e-6f) n = 1.0f;
    for (int i = 0; i < 3; ++i) r[i] /= n;
    u[0] = f[1] * r[2] - f[2] * r[1];
    u[1] = f[2] * r[0] - f[0] * r[2];
    u[2] = f[0] * r[1] - f[1] * r[0];
}

// Ultimo colpo corretto (thread di gioco): per riconoscere le chiamate
// successive dello stesso colpo (passaggio attraverso un ostacolo).
struct LastShot {
    ULONGLONG ms = 0;
    FVec3 inStart{}, inEnd{};
    FVec3 outEnd{};
    bool valid = false;
};
LastShot g_lastShot;
// YGamePawn: fine nominale del raggio (punto trovato dal raggio della camera,
// +0x140c) usata dal gioco prima del cono (FUN_0093f510, Ghidra).
constexpr uintptr_t kOffPawnAimTargetEnd = 0x140C;

// Chi chiama CalcWeaponFire (revisione della voce 3). Le chiamate dirette
// nell'exe (cercate nei byte, 26/10) sono tre: 0x0093FEA2 nel ciclo dei colpi
// istantanei (FUN_0093F510, ogni YWeapon), 0x00F63029 nei fucili a pompa
// (FUN_00F62930), 0x00C2727C in AYWeapon_execCalcWeaponFire (gli script).
// Solo le prime due sono proiettili; il ritorno e' l'indirizzo dopo la call
// (il detour di MinHook entra con un salto: [esp] e' quello del chiamante).
const uintptr_t& kCwfRetInstant = vr::addr::g[vr::addr::CwfRetInstant];
const uintptr_t& kCwfRetShotgun = vr::addr::g[vr::addr::CwfRetShotgun];
const uintptr_t& kCwfRetScript = vr::addr::g[vr::addr::CwfRetScript];

void __fastcall Hooked_CalcWeaponFire(void* self, void* /*edx*/, FVec3 start, FVec3 end, void* impact) {
    const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    char* cam = static_cast<char*>(g_cameraPtr.load());
    void* pawn = HasCameraSignature(cam) ? PlayerPawnIfViewTarget(cam) : nullptr;
    uint32_t weapon = 0;
    bool mine = pawn && TrySafeReadUint32(static_cast<char*>(pawn) + kOffPawnCurrentWeapon, &weapon) &&
                reinterpret_cast<void*>(weapon) == self;
    FVec3 origEnd = end, origStart = start;
    float bo[3], bd[3];
    float coneDeg = 0.0f;
    bool passThrough = false;
    bool muzzleTraced = false;      // diagnostica: MuzzleStart chiamata per questo colpo
    void* muzzleClip = nullptr;     // diagnostica: ostacolo tra spalla e volata (riga SHOT)
    float muzzlePen = 0.0f;
    ULONGLONG nowMs = GetTickCount64();
    if (mine && vr::control::g_muzzleAim && g_muzzleAimValid && g_armsActive && GunBarrel(pawn, bo, bd)) {
        float range = std::sqrt((end.x - start.x) * (end.x - start.x) + (end.y - start.y) * (end.y - start.y) +
                                (end.z - start.z) * (end.z - start.z));
        // Stesso colpo, chiamata successiva (il gioco riparte da poco oltre
        // l'ostacolo attraversabile, con lo stesso End): l'inizio resta quello
        // avanzato (sta sul nostro raggio) e la fine quella gia' corretta.
        // Prima si ripartiva dalla volata: stesso attore colpito, fine del
        // ciclo, niente attraversamento (H2).
        bool sameEnd = g_lastShot.valid && nowMs - g_lastShot.ms < 100 && end.x == g_lastShot.inEnd.x &&
                       end.y == g_lastShot.inEnd.y && end.z == g_lastShot.inEnd.z;
        bool sameStart = sameEnd && start.x == g_lastShot.inStart.x && start.y == g_lastShot.inStart.y &&
                         start.z == g_lastShot.inStart.z;
        if (sameEnd && !sameStart) {
            passThrough = true;
            end = g_lastShot.outEnd;
        } else if (range > 1.0f) {
            // Il raggio del colpo diventa quello della canna, lo stesso del
            // mirino: il gioco lo fa partire 25-30 unita' fuori dall'asse
            // (17/10) e verso il bersaglio passava da un'altra parte. Origine =
            // punto dell'asse della canna piu' vicino all'origine del gioco.
            // Il cono di dispersione del gioco (applicato a End prima di
            // questa chiamata: FUN_0093f510 -> FUN_00f60300, lerp(min, max,
            // accuracy) attorno a End-Start) viene misurato come rotazione tra
            // la direzione nominale (verso il punto del raggio della camera,
            // Pawn+0x140c) e quella ricevuta, e riportato sull'asse della
            // canna, scalato dalla postura: prima andava perso del tutto
            // ("spread" non faceva nulla, pallini su una riga sola).
            float dir[3] = {bd[0], bd[1], bd[2]};
            float scale = vr::control::g_spreadScale.load();
            float nomEnd[3];
            if (scale > 0.0f && SafeCopy(nomEnd, static_cast<char*>(pawn) + kOffPawnAimTargetEnd, 12)) {
                float nom[3] = {nomEnd[0] - start.x, nomEnd[1] - start.y, nomEnd[2] - start.z};
                float act[3] = {end.x - start.x, end.y - start.y, end.z - start.z};
                float nn = std::sqrt(nom[0] * nom[0] + nom[1] * nom[1] + nom[2] * nom[2]);
                float na = std::sqrt(act[0] * act[0] + act[1] * act[1] + act[2] * act[2]);
                if (nn > 1.0f && na > 1.0f && std::isfinite(nn)) {
                    for (int i = 0; i < 3; ++i) nom[i] /= nn, act[i] /= na;
                    float r1[3], u1[3], r2[3], u2[3];
                    DirBasis(nom, r1, u1);
                    DirBasis(bd, r2, u2);
                    // Postura (R-W1, regolabile: "spread on <scala> <ads> <due mani>
                    // <una mano>"): tacche all'occhio 0.1 (mirini veri = quasi
                    // laser, prova nel visore 21/10), due mani 0.5, arma lunga
                    // con una mano sola 1.5.
                    float k = scale;
                    if (vr::control::g_adsActive) k *= vr::control::g_spreadAds;
                    else if (g_twoHandActive) k *= vr::control::g_spreadTwoHand;
                    bool pistol = CurrentWeaponInfo(self).pistol;
                    if (!g_twoHandActive && !vr::control::g_adsActive && !pistol) k *= vr::control::g_spreadOneHand;
                    float b = (act[0] * r1[0] + act[1] * r1[1] + act[2] * r1[2]) * k;
                    float c = (act[0] * u1[0] + act[1] * u1[1] + act[2] * u1[2]) * k;
                    float lat2 = b * b + c * c;
                    if (lat2 > 0.9999f) {
                        float s = std::sqrt(0.9999f / lat2);
                        b *= s, c *= s, lat2 = 0.9999f;
                    }
                    float a = std::sqrt(1.0f - lat2);
                    for (int i = 0; i < 3; ++i) dir[i] = a * bd[i] + b * r2[i] + c * u2[i];
                    coneDeg = std::acos(std::fmax(-1.0f, std::fmin(1.0f, a))) * 57.2957795f;
                }
            }
            float ms[3];
            MuzzleStart(pawn, bo, bd, ms);
            muzzleTraced = true;
            muzzleClip = g_muzzleClipActor;
            muzzlePen = g_muzzlePenetration;
            start = {ms[0], ms[1], ms[2]};
            end = {start.x + dir[0] * range, start.y + dir[1] * range, start.z + dir[2] * range};
            g_lastShot.ms = nowMs;
            g_lastShot.inStart = origStart;
            g_lastShot.inEnd = origEnd;
            g_lastShot.outEnd = end;
            g_lastShot.valid = true;
        }
    }
    g_CalcWeaponFire_Original(self, start, end, impact);
    if (!mine && pawn && impact) {
        // Colpi delle altre armi (nemici, squadra): oggetti tenuti come scudi
        // (props.cpp OnOtherShot, voce 3 dei combattimenti). Con propshield off
        // un colpo su un oggetto tenuto rifa' la trace con gli oggetti tenuti
        // fuori: il colpo prosegue come se l'oggetto non ci fosse.
        // Una chiamata da script (controllo di tiro, non un proiettile) non
        // conta; un chiamante sconosciuto si tratta come un proiettile.
        const float st3[3] = {start.x, start.y, start.z}, en3[3] = {end.x, end.y, end.z};
        const bool bullet = caller != kCwfRetScript;
        const int sh = vr::props::OnOtherShot(self, pawn, st3, en3, impact, bullet);
        if (sh == 2) {
            uint32_t saved[2] = {0, 0};
            vr::props::HeldTraceable(false, saved);
            g_CalcWeaponFire_Original(self, start, end, impact);
            vr::props::HeldTraceable(true, saved);
        }
        if (vr::control::g_fireLog) {
            // Diagnostica ("firelog on"): anche i colpi delle altre armi, con chi spara e cosa prendono.
            float hit[3] = {0, 0, 0};
            SafeCopy(hit, static_cast<char*>(impact) + 4, 12);
            uint32_t hitActor = 0, instig = 0;
            TrySafeReadUint32(impact, &hitActor);
            TrySafeReadUint32(static_cast<char*>(self) + 0xE8, &instig);
            char b[420];
            char via[40];
            if (caller == kCwfRetInstant) snprintf(via, sizeof(via), "instant fire");
            else if (caller == kCwfRetShotgun) snprintf(via, sizeof(via), "shotgun");
            else if (caller == kCwfRetScript) snprintf(via, sizeof(via), "script call, not a bullet");
            else snprintf(via, sizeof(via), "caller 0x%08X", static_cast<unsigned>(caller));
            snprintf(b, sizeof(b), "SHOT (other): %s of %s from (%.1f, %.1f, %.1f) to (%.1f, %.1f, %.1f), impact (%.1f, %.1f, %.1f) on %s [%s]%s",
                     vr::script::ClassName(self).c_str(), instig ? vr::script::ClassName(reinterpret_cast<void*>(instig)).c_str() : "none",
                     start.x, start.y, start.z, end.x, end.y, end.z, hit[0], hit[1], hit[2],
                     hitActor ? vr::script::ClassName(reinterpret_cast<void*>(hitActor)).c_str() : "none", via,
                     sh == 1 ? " (redirected to the held object)" : (sh == 2 ? " (passed through the held object: propshield off)" : ""));
            vr::log::Line(b);
        }
    }
    if (mine && impact) {
        // La trace del colpo e' del gioco: le sfere delle mani le restano
        // fuori solo coi loro bit spenti (props.cpp, ProxyCreate). Se il colpo
        // ne prende una lo stesso, lo si dice e i bit si rispengono.
        uint32_t hitA = 0;
        if (TrySafeReadUint32(impact, &hitA) && hitA && vr::props::IsHandSphere(reinterpret_cast<void*>(hitA)))
            vr::props::HandSphereTraced(reinterpret_cast<void*>(hitA), "the shot");
    }
    if (mine && impact) {
        // Oggetti di scena colpiti (props.cpp, fase 4). FImpactInfo: HitActor +0,
        // HitLocation +4, HitInfo +0x34 (TraceHitInfo: HitComponent +0x18).
        float hitLoc[3] = {0, 0, 0}, st[3] = {start.x, start.y, start.z};
        uint32_t hitActor = 0, hitComp = 0;
        if (SafeCopy(hitLoc, static_cast<char*>(impact) + 4, 12) && TrySafeReadUint32(impact, &hitActor) &&
            TrySafeReadUint32(static_cast<char*>(impact) + 0x4C, &hitComp) && hitActor && hitComp)
            vr::props::OnShot(st, hitLoc, reinterpret_cast<void*>(hitActor), reinterpret_cast<void*>(hitComp), self);
    }
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
        float aimAt[3] = {0, 0, 0}, fireStart[3] = {0, 0, 0};
        SafeCopy(aimAt, static_cast<char*>(pawn) + kOffPawnAimTargetEnd, 12);
        SafeCopy(fireStart, static_cast<char*>(pawn) + 0x17F8, 12);
        int32_t pcRot[3] = {0, 0, 0}, pawnRot[3] = {0, 0, 0};
        SafeCopy(pcRot, cam - 0x77C + 0x60, 12);
        SafeCopy(pawnRot, static_cast<char*>(pawn) + 0x60, 12);
        // In coda (i parser leggono i campi sopra dall'inizio della riga): arma
        // che ha sparato, attore colpito per nome e se e' un oggetto tenuto o la
        // sfera di una mano (props::IsHeldActor), e l'ostacolo che ha fermato la
        // partenza tra spalla e volata (MuzzleStart), con la penetrazione.
        // Review 25/10: colpi con l'impatto sulla partenza "on KActorSpawnable".
        std::string hitObj = hitActor ? vr::script::ObjectName(reinterpret_cast<void*>(hitActor)) : "none";
        bool hitHeld = hitActor && vr::props::IsHeldActor(reinterpret_cast<void*>(hitActor));
        std::string clipObj = !muzzleTraced ? "n/a" : (muzzleClip ? vr::script::ObjectName(muzzleClip) : "none");
        bool clipHeld = muzzleClip && vr::props::IsHeldActor(muzzleClip);
        char b[1100];
        snprintf(b, sizeof(b),
                 "SHOT: from (%.1f, %.1f, %.1f) to (%.1f, %.1f, %.1f), game wanted (%.1f, %.1f, %.1f), barrel -> (%.1f, %.1f, %.1f), "
                 "barrel origin (%.1f, %.1f, %.1f) axis (%.3f, %.3f, %.3f), impact (%.1f, %.1f, %.1f) on %s, cone %.2f deg%s, "
                 "game start (%.1f, %.1f, %.1f) aimat (%.1f, %.1f, %.1f) firestart (%.1f, %.1f, %.1f) view (%.1f, %.1f, %.1f) "
                 "pcrot (%d, %d) pawnrot (%d, %d) | weapon %s | hit %s held %d | muzzle clip %s held %d pen %.1f",
                 start.x, start.y, start.z, end.x, end.y, end.z, origEnd.x, origEnd.y, origEnd.z, g_muzzleTarget[0],
                 g_muzzleTarget[1], g_muzzleTarget[2], bo[0], bo[1], bo[2], bd[0], bd[1], bd[2], hit[0], hit[1], hit[2],
                 hitName.c_str(), coneDeg, passThrough ? " (pass-through continuation)" : "", origStart.x, origStart.y,
                 origStart.z, aimAt[0], aimAt[1], aimAt[2], fireStart[0], fireStart[1], fireStart[2], g_lastViewLoc[0],
                 g_lastViewLoc[1], g_lastViewLoc[2], pcRot[0], pcRot[1], pawnRot[0], pawnRot[1],
                 CurrentWeaponInfo(self).cls.c_str(), hitObj.c_str(), hitHeld ? 1 : 0, clipObj.c_str(), clipHeld ? 1 : 0,
                 muzzlePen);
        vr::log::Line(b);
    }
}

// YGamePawn::RaiseWeapon(YE_RaiseWeaponState) (FUN_0052f2b0, __thiscall(this,
// BYTE state), chiamata dallo script tramite execRaiseWeapon 0x00A27380):
// con il corpo di Walker a un passo da un muro davanti lo script la chiama con
// YRWS_Raised (2) e m_weaponRaised (+0x499) diventa 2. Da li' MayAim
// (FUN_0065b7c0) e UpdateMayFireWeaponInternal (FUN_0068a820) rispondono "ne'
// mira ne' fuoco": il grilletto arriva al gioco (m_primaryFireInput) e non
// parte nessun colpo, anche con l'arma vera (nelle mani) gia' fuori dal muro
// (23/10, scena degli ostaggi del cap. 1, Walker col viso contro il container
// fuori copertura: nessun colpo; girato di 180 gradi colpi subito). In
// copertura invece non succede: li' decide la posa di mira del gioco
// (UpdateCoverLean). In VR l'arma e' quella nelle mani
// e il muro lo considera gia' MuzzleStart (il colpo parte prima dell'ostacolo
// se la canna e' dentro): Raised diventa None. YRWS_NoAim (1, zone in cui lo
// script vieta la mira) resta del gioco.
const uintptr_t& kRaiseWeaponAddr = vr::addr::g[vr::addr::RaiseWeapon];
constexpr uint8_t kRaiseWeaponRaised = 2;  // YE_RaiseWeaponState.YRWS_Raised
typedef void(__thiscall* RaiseWeapon_t)(void* self, uint8_t state);
RaiseWeapon_t g_RaiseWeapon_Original = nullptr;
void __fastcall Hooked_RaiseWeapon(void* self, void* /*edx*/, uint8_t state) {
    if (state == kRaiseWeaponRaised && vr::control::g_mode != vr::control::kModeOff && vr::control::g_firstPerson &&
        !vr::control::g_cinemaActive && vr::control::g_muzzleAim && g_armsActive) {
        char* cam = static_cast<char*>(g_cameraPtr.load());
        if (HasCameraSignature(cam) && PlayerPawnIfViewTarget(cam) == self) {
            state = 0;
            static bool logged = false;
            if (!logged) {
                logged = true;
                vr::log::Line("FIRE: Walker's body faces a wall up close: the game would raise the weapon and block "
                              "aim and fire; kept ready (the shot leaves from the barrel in your hands).");
            }
        }
    }
    g_RaiseWeapon_Original(self, state);
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

// Portata del braccio dell'arma (WI-3 fix 1) per l'ancora del collo: spalla
// (Arm_*), gomito e mano dopo la composizione, bersaglio dell'IK della mano
// (spazio dell'attore), lunghezza del braccio = somma delle due ossa.
// g_reachNeed: di quanto la mesh dovrebbe andare avanti perche' il bersaglio
// stia 2 unita' dentro la portata, con lo spostamento della mesh di adesso.
void MeasureGunReach(void* mesh) {
    ReachNeed& rn = g_reachNeed;
    float eff[3], loc[3], scale = 1.0f, tx = 0.0f;
    int32_t rot[3];
    char* p = static_cast<char*>(g_fpPawn);
    if (!g_rig.ctrl[0] || !p || mesh != g_fpHeadMesh ||
        !SafeCopy(eff, static_cast<char*>(g_rig.ctrl[0]) + kOffLimbEffector, 12) || !SafeCopy(loc, p + 0x54, 12) ||
        !SafeCopy(rot, p + kOffActorRotation, 12) || !SafeCopy(&scale, p + kOffActorDrawScale, 4) ||
        !SafeCopy(&tx, static_cast<char*>(mesh) + kOffMeshTranslationX, 4))
        return;
    if (!(scale > 0.01f)) scale = 1.0f;
    const bool right = WalkerRightArm(0);
    static const int32_t names[2][3] = {{vr::script::NameIndex("Arm_Left"), vr::script::NameIndex("Ellbow_Left"), vr::script::NameIndex("Hand_Left")},
                                        {vr::script::NameIndex("Arm_Right"), vr::script::NameIndex("Ellbow_Right"),
                                         vr::script::NameIndex("Hand_Right")}};
    float b[3][3];
    for (int i = 0; i < 3; ++i) {
        struct {
            int32_t name[2];
            int32_t space;
            float ret[3];
        } bl = {{names[right ? 1 : 0][i], 0}, 0, {0, 0, 0}};
        if (bl.name[0] < 0 || !vr::script::CallFunction(mesh, "GetBoneLocation", &bl)) return;
        std::memcpy(b[i], bl.ret, sizeof(b[i]));
    }
    auto dist = [](const float* a, const float* c) {
        return std::sqrt((a[0] - c[0]) * (a[0] - c[0]) + (a[1] - c[1]) * (a[1] - c[1]) + (a[2] - c[2]) * (a[2] - c[2]));
    };
    const float posedLen = dist(b[0], b[1]) + dist(b[1], b[2]);
    // Fix 2 (review media): portata = lunghezza di riposo del braccio (ArmReach:
    // LocalAtoms, la minima vista; senza armstretch), non il braccio come
    // disegnato. L'IK lo allunga fino a armstretch (1,25) verso il bersaglio:
    // con la lunghezza disegnata il bisogno era sempre >= il margine e la spinta
    // saliva a scatti. L'allungamento NON conta come portata (deciso dalle
    // misure): contandolo (riposo x 1,25) il corpo restava all'ancora col braccio
    // allungato e l'arma disegnata girava fino a 1,6 gradi al primo colpo
    // (gunframe 8 pose su 45 oltre 0,3 gradi; fix 1, che spingeva finche' il
    // braccio non era piu' allungato: 45/45 entro 0,045). Il corpo avanza quanto
    // basta perche' il braccio dell'arma non si allunghi (margine reachslack).
    float shoulderW[3], armLen = 0.0f;
    if (!ArmReach(mesh, 0, shoulderW, &armLen) || !(armLen > 10.0f && armLen < 100.0f)) return;
    const float stretchMax = vr::control::g_armStretch.load();
    if (stretchMax > 1.0f) armLen /= stretchMax;  // lunghezza di riposo
    float af[3], ar[3], au[3];
    RotatorToAxes(rot[0], rot[1], rot[2], af, ar, au);
    float target[3];
    for (int i = 0; i < 3; ++i) target[i] = loc[i] + (af[i] * eff[0] + ar[i] * eff[1] + au[i] * eff[2]) * scale;
    const float d[3] = {target[0] - b[0][0], target[1] - b[0][1], target[2] - b[0][2]};
    const float v[3] = {d[0] * af[0] + d[1] * af[1] + d[2] * af[2], d[0] * ar[0] + d[1] * ar[1] + d[2] * ar[2],
                        d[0] * au[0] + d[1] * au[1] + d[2] * au[2]};
    // Margine prima della portata massima ("reachslack", Avanzate; predefinito 2 unita' = 4 cm).
    // Con segno (fix 2): negativo = margine per arretrare, cosi' la spinta torna giu' a mano vicina.
    rn.need = vr::pure::ReachForwardMargin(v, armLen - vr::control::g_reachSlack.load());
    rn.shiftX = tx - g_fpSavedMeshX;
    rn.armLen = armLen;
    rn.posedLen = posedLen;
    rn.dist = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    rn.t = ProfMs() * 0.001;
    rn.valid = true;
}

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
    const int32_t handName = vr::script::NameIndex(HandBone(side));
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

// Stato di KeepGoodBones (L9: le pose buone di una mesh rilasciata venivano
// rimesse per un tick sulla mesh seguente). Azzerato da Acquire/ReleaseArms.
std::vector<uint8_t> g_kgbGood[2];
void* g_kgbMesh = nullptr;
double g_kgbFarSince[2] = {0.0, 0.0};
bool g_kgbSustained[2] = {false, false};
// f4: LocalToWorld con cui sono state composte le ossa buone salvate (quella prevista, se corretta) e
// riferimento delle mani nel mondo di quel momento (g_armsFrameW).
float g_kgbL2W[16] = {};
vr::pure::ue3m::HandFrameW g_kgbFrame{};
bool g_kgbFrameValid = false;
// f4 (review del f3, media): ossa corrette quando si rimettono quelle buone: solo le braccia (Shoulder_* / Arm_*
// e discendenti, piu' Weapon_* e IK_Hand_* se stanno fuori), non tutto lo scheletro: busto, gambe e attrezzatura
// restano le ossa buone nello spazio della mesh e seguono la mesh (nel f3 giravano tutte: il corpo intero restava
// per un tick allo yaw del tick prima, 1 tick su 5-6 mentre il corpo gira). Indici dallo scheletro di riferimento
// (ParentIndex: i genitori vengono prima dei figli).
struct KgbArmSet {
    void* mesh = nullptr;
    uint32_t num = 0;
    bool ok = false;
    std::vector<int> bones;  // ossa corrette
    std::vector<int> roots;  // radici dei sottoalberi
    std::vector<int> seamRoots;  // radici Shoulder_* (attacco al busto: "seam" nella diagnostica)
    int hand = -1, spine = -1;
};
KgbArmSet g_kgbArms;
void ResetSkelFix() {
    g_kgbGood[0].clear();
    g_kgbGood[1].clear();
    g_kgbMesh = nullptr;
    g_kgbFarSince[0] = g_kgbFarSince[1] = 0.0;
    g_kgbSustained[0] = g_kgbSustained[1] = false;
    g_kgbFrameValid = false;
}

// Genitore di un osso nello scheletro di riferimento (USkeletalMesh.RefSkeleton +0x88, FMeshBone da 80 byte,
// ParentIndex +64; vedi RefBoneLocal).
bool RefBoneParent(void* mesh, int idx, int* parent) {
    uint32_t skel = 0, data = 0, num = 0;
    if (!TrySafeReadUint32(static_cast<char*>(mesh) + 0x20C, &skel) || !skel ||
        !TrySafeReadUint32(reinterpret_cast<char*>(skel) + 0x88, &data) || !data ||
        !TrySafeReadUint32(reinterpret_cast<char*>(skel) + 0x8C, &num) || idx < 0 || static_cast<uint32_t>(idx) >= num || num > 1024)
        return false;
    return SafeCopy(parent, reinterpret_cast<char*>(data) + idx * 80 + 64, 4);
}

const KgbArmSet& KgbArms(void* mesh, uint32_t num) {
    KgbArmSet& s = g_kgbArms;
    if (s.mesh == mesh && s.num == num) return s;
    s = KgbArmSet();
    s.mesh = mesh;
    s.num = num;
    std::vector<char> in(num, 0);
    std::string rootNames;
    for (uint32_t i = 0; i < num; ++i) {
        struct {
            int32_t index;
            int32_t ret[2];
        } bn = {static_cast<int32_t>(i), {0, 0}};
        int parent = -1;
        if (!vr::script::CallFunction(mesh, "GetBoneName", &bn) || bn.ret[0] <= 0 || !RefBoneParent(mesh, static_cast<int>(i), &parent)) {
            vr::log::Line("SKELFIX: skeleton not readable (bone " + std::to_string(i) + "), restored bones not corrected");
            s.bones.clear();
            s.roots.clear();
            s.seamRoots.clear();
            return s;
        }
        const std::string n = vr::script::NameFromIndex(bn.ret[0]);
        const bool under = i > 0 && parent >= 0 && static_cast<uint32_t>(parent) < i && in[parent];
        const bool root = !under && (n.rfind("Shoulder_", 0) == 0 || n.rfind("Arm_", 0) == 0 || n.rfind("Weapon_", 0) == 0 ||
                                     n.rfind("IK_Hand_", 0) == 0);
        in[i] = under || root;
        if (in[i]) s.bones.push_back(static_cast<int>(i));
        if (root) {
            s.roots.push_back(static_cast<int>(i));
            if (n.rfind("Shoulder_", 0) == 0) s.seamRoots.push_back(static_cast<int>(i));
            rootNames += (rootNames.empty() ? "" : ", ") + n;
        }
        if (n == HandBone(0)) s.hand = static_cast<int>(i);
        if (n == "Spine3") s.spine = static_cast<int>(i);
    }
    s.ok = s.hand >= 0 && in[s.hand] && !s.roots.empty();
    vr::log::Line("SKELFIX: restored bones corrected on " + std::to_string(s.bones.size()) + " arm bones of " + std::to_string(num) +
                  " (subtrees of " + rootNames + ")" + (s.ok ? "" : ": hand bone not among them, no correction"));
    return s;
}

// f3 (27/10): riferimento d'attore del Pawn (assi da Rotation, origine Location).
bool PawnFrame(void* pawn, vr::pure::ue3m::Frame* f) {
    int32_t rot[3];
    if (!pawn || !SafeCopy(f->org, static_cast<char*>(pawn) + kOffActorLocation, 12) ||
        !SafeCopy(rot, static_cast<char*>(pawn) + kOffActorRotation, 12))
        return false;
    RotatorToAxes(rot[0], rot[1], rot[2], f->ax[0], f->ax[1], f->ax[2]);
    return true;
}

// f3: scatto all'inizio del Draw (prima che la mod giri o sposti il Pawn): la LocalToWorld della mesh appena
// aggiornata dal tick, nel riferimento dell'attore di adesso. Diventa la relativa usata da UpdateSkelPoseThunk
// quando due scatti di fila coincidono (al netto della Translation): uno scatto con la LocalToWorld non
// aggiornata dal tick (attore girato dopo) non la cambia.
void SnapMeshL2W(void* pawn) {
    MeshL2WSnap& s = g_meshL2WSnap;
    void* mesh = g_rig.mesh;
    if (!pawn || !mesh || !g_armsActive || !MeshOwnedBy(mesh, pawn)) {
        s.valid = s.haveCand = false;
        return;
    }
    if (mesh != s.mesh || pawn != s.pawn) {
        s.valid = s.haveCand = false;
        s.mesh = mesh;
        s.pawn = pawn;
    }
    float l2w[16], tr[3], ds = 1.0f;
    vr::pure::ue3m::Frame a;
    if (!SafeCopy(l2w, static_cast<char*>(mesh) + kOffCompLocalToWorld, sizeof(l2w)) ||
        !SafeCopy(tr, static_cast<char*>(mesh) + kOffMeshTranslationX, 12) ||
        !SafeCopy(&ds, static_cast<char*>(pawn) + kOffActorDrawScale, 4) || !PawnFrame(pawn, &a)) {
        s.haveCand = false;
        return;
    }
    if (!(ds > 0.01f)) ds = 1.0f;
    float rel[4][3];
    vr::pure::ue3m::L2WToActor(l2w, a, rel);
    bool same = s.haveCand;
    for (int r = 0; r < 3 && same; ++r) {
        const float n = std::sqrt(rel[r][0] * rel[r][0] + rel[r][1] * rel[r][1] + rel[r][2] * rel[r][2]);
        for (int i = 0; i < 3 && same; ++i) same = std::fabs(rel[r][i] - s.cand[r][i]) <= 2e-3f * n;  // ~0,1 gradi
    }
    for (int i = 0; i < 3 && same; ++i)
        same = std::fabs((rel[3][i] - tr[i] * ds) - (s.cand[3][i] - s.candTrans[i] * ds)) <= 0.5f;
    // Valida solo se l'ultimo scatto conferma il precedente: una relativa che cambia di continuo (posa del gioco
    // che gira la mesh rispetto all'attore) non viene mai usata, si resta come prima del f3.
    s.valid = same;
    if (same) {
        std::memcpy(s.rel, rel, sizeof(rel));
        std::memcpy(s.trans, tr, sizeof(tr));
    }
    std::memcpy(s.cand, rel, sizeof(rel));
    std::memcpy(s.candTrans, tr, sizeof(tr));
    s.haveCand = true;
}

// f4: ossa delle braccia rimesse da KeepGoodBones portate da m (spazio della componente, vedi
// vr::pure::ue3m::RestoreCorrection). LocalAtoms non toccate (relative; la radice resta quella del busto).
bool CorrectRestoredArms(uint32_t spaceBases, uint32_t num, const KgbArmSet& arms, const vr::pure::ue3m::Xf& m) {
    float atom[8];
    for (int i : arms.bones) {
        if (static_cast<uint32_t>(i) >= num) return false;
        char* at = reinterpret_cast<char*>(spaceBases) + i * kBoneAtomSize;
        if (!SafeCopy(atom, at, kBoneAtomSize)) return false;
        vr::pure::ue3m::TransformBoneAtom(atom, atom + 4, m);
        if (!SafeCopy(at, atom, kBoneAtomSize)) return false;
    }
    return true;
}

// f4 (review del f3): passo della stanza (RoomStepMove). Tutte le SpaceBases di Walker spostate di -d nel mondo
// (nello spazio della componente: rotazione invariata, posizione - d x R^-1), prima di MoveSmooth che aggiorna
// rendering, corpi fisici e arma agganciata con queste ossa: per questo frame lo scheletro resta dov'era nel
// mondo, il tick dopo lo ricompone (anche KeepGoodBones: le ossa buone salvate non sono spostate). Solo con le
// braccia della mod attive; "armslead off" = come prima (collaudi).
bool ShiftBonesForRoomStep(void* pawn, const float d[3]) {
    void* mesh = g_rig.mesh;
    if (!vr::control::g_armsLead || !g_armsActive || !mesh || !g_rig.ctrl[0] || pawn != g_fpPawn || !PawnAlive(pawn) ||
        !MeshOwnedBy(mesh, pawn))
        return false;
    char* m = static_cast<char*>(mesh);
    uint32_t data = 0, num = 0;
    float l2w[16];
    vr::pure::ue3m::Xf inv;
    if (!TrySafeReadUint32(m + kOffMeshSpaceBases, &data) || !TrySafeReadUint32(m + kOffMeshSpaceBases + 4, &num) || !data || num == 0 ||
        num > 1024 || !SafeCopy(l2w, m + kOffCompLocalToWorld, sizeof(l2w)) ||
        !vr::pure::ue3m::XfInverse(vr::pure::ue3m::XfFromL2W(l2w), &inv))
        return false;
    float dl[3];
    for (int i = 0; i < 3; ++i) dl[i] = d[0] * inv.r[0][i] + d[1] * inv.r[1][i] + d[2] * inv.r[2][i];
    std::vector<float> atoms(num * (kBoneAtomSize / 4));
    if (!SafeCopy(atoms.data(), reinterpret_cast<void*>(data), num * kBoneAtomSize)) return false;
    for (uint32_t b = 0; b < num; ++b)
        for (int i = 0; i < 3; ++i) atoms[b * 8 + 4 + i] -= dl[i];
    if (!SafeCopy(reinterpret_cast<void*>(data), atoms.data(), num * kBoneAtomSize)) return false;
    if (vr::control::g_gunLogFrames.load() > 0) ++g_skelPoseDiag.roomShift;
    return true;
}

// Diagnostica f4 (riga SKELDIAG "trunk"): yaw di Spine3 rispetto alla LocalToWorld della mesh (gradi): il busto
// segue la mesh e cambia solo con l'animazione; nel f3 saltava della rotazione delle ossa rimesse.
float TrunkYawRelDeg(void* mesh) {
    const KgbArmSet& s = g_kgbArms;
    uint32_t data = 0, num = 0;
    float atom[8], l2w[16];
    char* m = static_cast<char*>(mesh);
    if (!mesh || s.mesh != mesh || s.spine < 0 || !TrySafeReadUint32(m + kOffMeshSpaceBases, &data) ||
        !TrySafeReadUint32(m + kOffMeshSpaceBases + 4, &num) || !data || static_cast<uint32_t>(s.spine) >= num ||
        !SafeCopy(atom, reinterpret_cast<char*>(data) + s.spine * kBoneAtomSize, sizeof(atom)) ||
        !SafeCopy(l2w, m + kOffCompLocalToWorld, sizeof(l2w)))
        return -999.0f;
    float rows[3][3], w[3];
    vr::pure::ue3m::QuatToRows(atom, rows);
    for (int i = 0; i < 3; ++i) w[i] = rows[0][0] * l2w[i] + rows[0][1] * l2w[4 + i] + rows[0][2] * l2w[8 + i];
    const float d = (std::atan2(w[1], w[0]) - std::atan2(l2w[1], l2w[0])) * 57.2957795f;
    return std::remainder(d, 360.0f);
}

void KeepGoodBones(void* mesh, bool log) {
    if (!PawnAlive(g_fpPawn)) return;  // ragdoll: le ossa le muove la fisica
    auto& good = g_kgbGood;
    auto& goodMesh = g_kgbMesh;
    static uint32_t restored = 0, checks = 0;
    // Per mano (0 destra, 1 sinistra, controllata solo se la guida la mod).
    auto& farSince = g_kgbFarSince;    // da quando la mano e' lontana (ms)
    auto& sustained = g_kgbSustained;  // lontana davvero: pose accettate finche' non torna
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
        g_kgbFrame = g_armsFrameW;
        g_kgbFrameValid = g_armsFrameValid && SafeCopy(g_kgbL2W, m + kOffCompLocalToWorld, sizeof(g_kgbL2W));
        if (vr::control::g_armsLead) KgbArms(mesh, num[0]);  // ossa delle braccia risolte una volta per mesh
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
    // f3 (27/10): le ossa buone sono nello spazio della mesh, che da allora e' girata col Pawn; il riferimento
    // delle mani (tracciamento) no, se la rotazione l'ha fatta la mod sotto la testa (corpo con la testa, corpo
    // che torna sotto la testa): prima la canna stava avanti di un passo in questi tick (circa 1 su 6 mentre il
    // corpo gira, 0,55-1,8 gradi), e con "muzzleaim" anche colpi e mirino.
    // f4 (review del f3): il f3 girava le ossa solo di yaw attorno all'asse del Pawn, e tutte. La volata restava
    // spostata della Translation della mesh (ancora del collo, 0,65 unita') e dei passi della stanza (2,8 unita'),
    // e il corpo intero restava per un tick allo yaw di prima. Ora correzione rigida intera
    // M = L2W buona * D * L2W di adesso^-1 (D: riferimento delle mani di allora -> di adesso), solo sulle braccia.
    float restoreDeg = 0.0f, restoreHand = 0.0f, restoreSeam = 0.0f;
    bool corrected = false;
    if (ok && vr::control::g_armsLead && g_kgbFrameValid && g_armsFrameValid) {
        const KgbArmSet& arms = KgbArms(mesh, num[0]);
        float l2wNow[16];
        vr::pure::ue3m::Xf mx;
        if (arms.ok && SafeCopy(l2wNow, m + kOffCompLocalToWorld, sizeof(l2wNow)) &&
            vr::pure::ue3m::RestoreCorrection(g_kgbL2W, g_kgbFrame, g_armsFrameW, l2wNow, &mx)) {
            float handT[3] = {0.0f, 0.0f, 0.0f};
            SafeCopy(handT, reinterpret_cast<char*>(data[0]) + arms.hand * kBoneAtomSize + 16, 12);
            restoreDeg = vr::pure::ue3m::XfAngle(mx) * 57.2957795f;
            restoreHand = vr::pure::ue3m::XfMoveAt(mx, handT);
            for (int r : arms.seamRoots) {
                float rt[3];
                if (SafeCopy(rt, reinterpret_cast<char*>(data[0]) + r * kBoneAtomSize + 16, 12))
                    restoreSeam = (std::max)(restoreSeam, vr::pure::ue3m::XfMoveAt(mx, rt));
            }
            // Come la LocalToWorld prevista: non oltre 60 gradi / 100 unita' (salto, teletrasporto: si lascia).
            if ((restoreHand > 1e-3f || restoreDeg > 1e-3f) && restoreDeg < 60.0f && restoreHand < 100.0f)
                corrected = CorrectRestoredArms(data[0], num[0], arms, mx);
        }
    }
    if (vr::control::g_gunLogFrames.load() > 0) {
        ++g_skelPoseDiag.restored;
        g_skelPoseDiag.restoreDeg = corrected ? restoreDeg : 0.0f;
        g_skelPoseDiag.restoreHand = corrected ? restoreHand : 0.0f;
        g_skelPoseDiag.restoreSeam = corrected ? restoreSeam : 0.0f;
    }
    ++restored;
    if (log || restored <= 3 || restored % 1000 == 0) {
        char b[200];
        snprintf(b, sizeof(b), "SKELFIX: hand %.1f units from its IK target, previous bones restored%s (%u of %u updates)", d,
                 ok ? "" : " FAILED", restored, checks);
        vr::log::Line(b);
    }
}

const uintptr_t& kUpdateSkelPoseAddr = vr::addr::g[vr::addr::UpdateSkelPose];
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
        const bool f3diag = this == g_rig.mesh && g_fpPawn && vr::control::g_gunLogFrames.load() > 0;
        int f3k = -1;
        if (f3diag) {
            SkelPoseDiag& sd = g_skelPoseDiag;
            f3k = sd.n < 4 ? sd.n : 3;
            ++sd.n;
            sd.ret[f3k] = ret;
            sd.pre[f3k] = CompL2WYawDeg(this);
            int32_t ay = 0;
            SafeCopy(&ay, static_cast<char*>(g_fpPawn) + kOffActorRotation + 4, 4);
            sd.actor[f3k] = static_cast<int16_t>(ay & 0xFFFF) * (360.0f / 65536.0f);
        }
        // f3 (27/10): LocalToWorld della mesh di Walker = quella che il gioco calcolera' subito dopo (relativa
        // dall'inizio del Draw, Translation e attore di adesso) per la composizione delle ossa, poi rimessa com'era
        // (vedi MeshL2WSnap). Solo con le braccia della mod attive.
        float l2wSaved[16], l2wPred[16];
        bool l2wPatched = false;
        // f4 (review del f3): non sul ragdoll (Walker morto: le ossa le compone la fisica con la LocalToWorld) ne'
        // su una mesh di un altro Pawn, come KeepGoodBones, MeasureGunReach e PoseFingers qui sotto.
        if (vr::control::g_armsLead && this == g_rig.mesh && g_armsActive && g_rig.ctrl[0] && g_fpPawn && PawnAlive(g_fpPawn) &&
            MeshOwnedBy(this, g_fpPawn)) {
            const MeshL2WSnap& sn = g_meshL2WSnap;
            vr::pure::ue3m::Frame a;
            float tr[3], ds = 1.0f;
            int32_t fixUnits = 0;
            const bool snapOk = sn.valid && sn.mesh == this && sn.pawn == g_fpPawn;
            // Tick con una rotazione della mod (UpdatePawnFacing in questo Draw) senza scatto valido: niente
            // correzione, UpdateArms ha anticipato del passo (review 2). Riga ogni 30 s, se ce ne sono stati.
            if (g_pawnFacingStep.load() != 0) {
                ++g_l2wTurnTicks;
                if (!snapOk) ++g_l2wTurnUnfixed;
            }
            {
                static double lastReport = 0.0;
                const double nowR = ProfMs();
                if (lastReport == 0.0) lastReport = nowR;
                if (nowR - lastReport > 30000.0) {
                    if (g_l2wTurnUnfixed > 0) {
                        char b[260];
                        std::snprintf(b, sizeof(b),
                                      "SKELFIX: bone update not corrected in %u of %u turning ticks in the last 30 s (the mesh moved against "
                                      "the actor, e.g. a hit reaction); the hands were led by the turn step instead (review-2 lead)",
                                      g_l2wTurnUnfixed, g_l2wTurnTicks);
                        vr::log::Line(b);
                    }
                    g_l2wTurnTicks = g_l2wTurnUnfixed = 0;
                    lastReport = nowR;
                }
            }
            if (snapOk && PawnFrame(g_fpPawn, &a) &&
                SafeCopy(tr, reinterpret_cast<char*>(this) + kOffMeshTranslationX, 12) &&
                SafeCopy(&ds, static_cast<char*>(g_fpPawn) + kOffActorDrawScale, 4) &&
                SafeCopy(l2wSaved, reinterpret_cast<char*>(this) + kOffCompLocalToWorld, sizeof(l2wSaved))) {
                if (!(ds > 0.01f)) ds = 1.0f;
                const float dOrg[3] = {(tr[0] - sn.trans[0]) * ds, (tr[1] - sn.trans[1]) * ds, (tr[2] - sn.trans[2]) * ds};
                std::memcpy(l2wPred, l2wSaved, sizeof(l2wPred));
                vr::pure::ue3m::L2WFromActor(sn.rel, a, dOrg, l2wPred);
                float dYaw = (std::atan2(l2wPred[1], l2wPred[0]) - std::atan2(l2wSaved[1], l2wSaved[0])) * 57.2957795f;
                dYaw = std::remainder(dYaw, 360.0f);
                float dAx = 0.0f, dOr = 0.0f;
                for (int r = 0; r < 3; ++r)
                    for (int i = 0; i < 3; ++i) dAx = (std::max)(dAx, std::fabs(l2wPred[4 * r + i] - l2wSaved[4 * r + i]));
                for (int i = 0; i < 3; ++i) dOr += (l2wPred[12 + i] - l2wSaved[12 + i]) * (l2wPred[12 + i] - l2wSaved[12 + i]);
                dOr = std::sqrt(dOr);
                // Uguale (niente da fare) o troppo diversa (la relativa non vale piu': cambio di posa del gioco,
                // salto): come prima.
                const bool same = dAx < 1e-5f && dOr < 0.01f;
                if (!same && std::fabs(dYaw) < 60.0f && dOr < 100.0f &&
                    SafeCopy(reinterpret_cast<char*>(this) + kOffCompLocalToWorld, l2wPred, sizeof(l2wPred))) {
                    l2wPatched = true;
                    fixUnits = static_cast<int32_t>(std::lround(dYaw * (65536.0f / 360.0f)));
                }
                if (f3k >= 0) {
                    g_skelPoseDiag.fixDeg[f3k] = l2wPatched ? dYaw : 0.0f;
                    g_skelPoseDiag.fixUnits[f3k] = l2wPatched ? dOr : 0.0f;
                }
            }
            // Riga GUNLOG "lead": correzione di yaw dell'ultimo aggiornamento delle ossa (senza scatto valido resta
            // l'anticipo scritto da UpdateArms).
            if (snapOk) g_armsLeadUsed = fixUnits;
        }
        g_UpdateSkelPose_Original(this, dt, tickFaceFx);
        if (f3k >= 0) g_skelPoseDiag.post[f3k] = CompL2WYawDeg(this);
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
        // Portata del braccio dell'arma per l'ancora del collo (WI-3 fix 1).
        if (this == g_rig.mesh && g_armsActive && g_rig.ctrl[0] && g_fpPawn && PawnAlive(g_fpPawn) && MeshOwnedBy(this, g_fpPawn))
            MeasureGunReach(this);
        // Dita della mano di supporto sull'oggetto tenuto (props.cpp).
        if (this == g_rig.mesh && g_armsActive && PawnAlive(g_fpPawn))
            vr::props::PoseFingers(this, 1, ArmsSwapped() ? "Right" : "Left");
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
        // f3: LocalToWorld rimessa com'era (il gioco la ricalcola subito dopo), se nessuno l'ha cambiata.
        if (l2wPatched) {
            float now[16];
            if (SafeCopy(now, reinterpret_cast<char*>(this) + kOffCompLocalToWorld, sizeof(now)) && std::memcmp(now, l2wPred, sizeof(now)) == 0)
                SafeCopy(reinterpret_cast<char*>(this) + kOffCompLocalToWorld, l2wSaved, sizeof(l2wSaved));
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
        bool relayout = vr::control::g_cinemaActive && g_relayoutFrames.load() > 0;
        if (relayout) --g_relayoutFrames;
        if (vr::control::g_cinemaActive && !relayout) {
            // Schermo virtuale: il gioco torna a disegnare in 16:9 (bande
            // sopra e sotto); bConstrainAspectRatio era stato azzerato da noi.
            // (Una sotto-area 16:9 di LocalPlayer restringe solo il 3D: la
            // Scaleform dei menu resta a tutta finestra, provato 21/10.)
            char* pc = *reinterpret_cast<char**>(reinterpret_cast<char*>(this) + 0x40);
            uint32_t camPtr = 0, flags = 0;
            if (pc && TrySafeReadUint32(pc + 0x38C, &camPtr) && camPtr &&
                TrySafeReadUint32(reinterpret_cast<char*>(camPtr) + 0x208, &flags) && !(flags & 2)) {
                *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(camPtr) + 0x208) = flags | 2u;
            }
        } else if (vr::control::g_fullViewport || relayout) {
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
            {43, reinterpret_cast<void*>(&Hooked_Clear), reinterpret_cast<void**>(&g_Clear_Original)},  // hidden-area mesh (R-C7)
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
    g_presentTarget = presentTarget;
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

    // Filtro dei crash del motore (FUN_00827770, Ghidra: minidump proprio +
    // ReportFault, ritorna 1): le eccezioni sul thread di gioco finivano li'
    // e la mod non ne sapeva nulla (L24, R-R2). Prima si scrive la riga CRASH
    // e il nostro minidump, poi si passa al filtro del gioco.
    {
        void* target = reinterpret_cast<void*>(kEngineCrashFilterAddr);
        if (MH_CreateHook(target, reinterpret_cast<void*>(&Hooked_EngineCrashFilter),
                          reinterpret_cast<void**>(&g_EngineCrashFilter_Original)) == MH_OK &&
            MH_EnableHook(target) == MH_OK)
            vr::log::Line("Hook on the engine crash filter installed (crash reports before the game's own).");
        else
            vr::log::Line("ERROR: hook on the engine crash filter failed.");
    }
    {
        void* t1 = reinterpret_cast<void*>(kViewportSizeAddr);
        void* t2 = reinterpret_cast<void*>(kUiViewportSizeAddr);
        // Il getter del GameViewportClient e' usato anche dal motore (4 chiamanti):
        // con l'altezza ridotta il gioco usciva in silenzio 3 s dopo l'avvio
        // (21/10). Si aggancia solo quello della UIScene.
        bool ok1 = vr::control::g_uiHookGameViewport &&
                   MH_CreateHook(t1, ViewportSizeThunkAddress(), reinterpret_cast<void**>(&g_ViewportSize_Original)) == MH_OK &&
                   MH_EnableHook(t1) == MH_OK;
        bool ok2 = vr::control::g_uiHookUiScene &&
                   MH_CreateHook(t2, UiViewportSizeThunkAddress(), reinterpret_cast<void**>(&g_UiViewportSize_Original)) == MH_OK &&
                   MH_EnableHook(t2) == MH_OK;
        void* t3 = reinterpret_cast<void*>(kSceneViewportSizeAddr);
        // Dimensione dichiarata ai menu: il ritaglio non li ridisponeva (21/10),
        // e il back buffer 16:9 (menu169) faceva un fail-fast nell'overlay di
        // Steam al caricamento della campagna. Entrambi spenti di default.
        bool ok3 = vr::control::g_ui169 &&
                   MH_CreateHook(t3, SceneViewportSizeThunkAddress(), reinterpret_cast<void**>(&g_SceneViewportSize_Original)) == MH_OK &&
                   MH_EnableHook(t3) == MH_OK;
        vr::log::Line(std::string("Hook on the UI viewport size: scene client ") + (ok3 ? "ok" : "FAILED") +
                      " (menus laid out 16:9 on the virtual screen); diagnostics: game viewport " + (ok1 ? "ok" : "off") +
                      ", UI scene " + (ok2 ? "ok" : "off") + ".");
    }
    {
        void* target = reinterpret_cast<void*>(kCanvasProjectAddr);
        if (MH_CreateHook(target, CanvasProjectThunkAddress(), reinterpret_cast<void**>(&g_ExecCanvasProject_Original)) ==
                MH_OK &&
            MH_EnableHook(target) == MH_OK)
            vr::log::Line("Hook on UCanvas::execProject installed (HUD projection count).");
    }
    {
        void* smTarget = reinterpret_cast<void*>(kSelectMarkableAddr);
        if (MH_CreateHook(smTarget, reinterpret_cast<void*>(&Hooked_SelectMarkable),
                          reinterpret_cast<void**>(&g_SelectMarkable_Original)) == MH_OK &&
            MH_EnableHook(smTarget) == MH_OK)
            vr::log::Line("Hook on the squad target validation installed (mod target kept).");
        else
            vr::log::Line("ERROR: hook on the squad target validation failed.");
    }
    // Senza il ritorno della chiamata dagli script (CwfRetScript) non si
    // distinguerebbero i colpi veri dalle chiamate degli script: niente hook.
    void* cwfTarget = vr::addr::Ok(vr::addr::CwfRetScript) ? reinterpret_cast<void*>(kCalcWeaponFireAddr) : nullptr;
    if (cwfTarget && MH_CreateHook(cwfTarget, reinterpret_cast<void*>(&Hooked_CalcWeaponFire),
                      reinterpret_cast<void**>(&g_CalcWeaponFire_Original)) == MH_OK &&
        MH_EnableHook(cwfTarget) == MH_OK) {
        vr::log::Line("Hook on YWeapon::CalcWeaponFire installed (shots from the barrel).");
    } else {
        vr::log::Line("ERROR: hook on CalcWeaponFire failed.");
    }
    void* rwTarget = reinterpret_cast<void*>(kRaiseWeaponAddr);
    if (MH_CreateHook(rwTarget, reinterpret_cast<void*>(&Hooked_RaiseWeapon),
                      reinterpret_cast<void**>(&g_RaiseWeapon_Original)) == MH_OK &&
        MH_EnableHook(rwTarget) == MH_OK)
        vr::log::Line("Hook on YGamePawn::RaiseWeapon installed (aim and fire with the body against a wall).");
    else
        vr::log::Line("ERROR: hook on RaiseWeapon failed.");

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
    vr::audio::LogDefaultDevice("at start");
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

// Texture MANAGED convertite senza DYNAMIC: il gioco non puo' bloccarle
// (LockRect fallisce). Mai viste finora (indagine sul crash al cambio di
// livello del 26/10), ma se capitano devono finire nel log.
std::atomic<uint32_t> g_dynFallback{0};
void LogDynFallback(const char* what, UINT w, UINT h, UINT levels, D3DFORMAT fmt, HRESULT first) {
    uint32_t n = ++g_dynFallback;
    if (n > 20 && n % 200 != 0) return;
    char b[200];
    std::snprintf(b, sizeof(b), "GPUSHARE: %s %ux%u fmt %d levels %u created WITHOUT DYNAMIC (DYNAMIC hr=0x%08X), %u so far",
                  what, w, h, static_cast<int>(fmt), levels, static_cast<unsigned>(first), n);
    vr::log::Line(b);
}
HRESULT WINAPI Hooked_CreateTexture(IDirect3DDevice9* d, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt,
                                    D3DPOOL pool, IDirect3DTexture9** out, HANDLE* shared) {
    if (pool != D3DPOOL_MANAGED) return g_CreateTexture_Original(d, w, h, levels, usage, fmt, pool, out, shared);
    HRESULT hr = g_CreateTexture_Original(d, w, h, levels, ExUsage(usage), fmt, D3DPOOL_DEFAULT, out, shared);
    if (FAILED(hr)) {
        const HRESULT first = hr;
        hr = g_CreateTexture_Original(d, w, h, levels, usage, fmt, D3DPOOL_DEFAULT, out, shared);
        if (SUCCEEDED(hr) && ExUsage(usage) != usage) LogDynFallback("texture", w, h, levels, fmt, first);
    }
    CountManaged(hr, "texture");
    return hr;
}

HRESULT WINAPI Hooked_CreateVolumeTexture(IDirect3DDevice9* d, UINT w, UINT h, UINT depth, UINT levels, DWORD usage,
                                          D3DFORMAT fmt, D3DPOOL pool, IDirect3DVolumeTexture9** out, HANDLE* shared) {
    if (pool != D3DPOOL_MANAGED)
        return g_CreateVolumeTexture_Original(d, w, h, depth, levels, usage, fmt, pool, out, shared);
    HRESULT hr =
        g_CreateVolumeTexture_Original(d, w, h, depth, levels, ExUsage(usage), fmt, D3DPOOL_DEFAULT, out, shared);
    if (FAILED(hr)) {
        const HRESULT first = hr;
        hr = g_CreateVolumeTexture_Original(d, w, h, depth, levels, usage, fmt, D3DPOOL_DEFAULT, out, shared);
        if (SUCCEEDED(hr) && ExUsage(usage) != usage) LogDynFallback("volume", w, h, levels, fmt, first);
    }
    CountManaged(hr, "volume");
    return hr;
}

HRESULT WINAPI Hooked_CreateCubeTexture(IDirect3DDevice9* d, UINT edge, UINT levels, DWORD usage, D3DFORMAT fmt,
                                        D3DPOOL pool, IDirect3DCubeTexture9** out, HANDLE* shared) {
    if (pool != D3DPOOL_MANAGED) return g_CreateCubeTexture_Original(d, edge, levels, usage, fmt, pool, out, shared);
    HRESULT hr = g_CreateCubeTexture_Original(d, edge, levels, ExUsage(usage), fmt, D3DPOOL_DEFAULT, out, shared);
    if (FAILED(hr)) {
        const HRESULT first = hr;
        hr = g_CreateCubeTexture_Original(d, edge, levels, usage, fmt, D3DPOOL_DEFAULT, out, shared);
        if (SUCCEEDED(hr) && ExUsage(usage) != usage) LogDynFallback("cubemap", edge, edge, levels, fmt, first);
    }
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

bool g_createDeviceInner = false;
// CreateDeviceEx fallito una volta: niente piu' oggetti D3D9Ex in questa sessione.
bool g_gpushareFellBack = false;

// File "specopsvr_forcedevexfail" nella cartella del gioco: simula il fallimento
// di CreateDeviceEx per provare il fallback.
bool GpuShareFailTestRequested() {
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string flag = exe;
    flag = flag.substr(0, flag.find_last_of("\\/")) + "\\specopsvr_forcedevexfail";
    return GetFileAttributesA(flag.c_str()) != INVALID_FILE_ATTRIBUTES;
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
    // Chiamata interna del fallback (oggetto D3D9 normale con la stessa CreateDevice
    // agganciata): passa diritta, i device hook li installa il chiamante.
    if (g_createDeviceInner)
        return g_CreateDevice_Original(self, Adapter, DeviceType, hFocusWindow, BehaviorFlags,
                                       pPresentationParameters, ppReturnedDeviceInterface);
    vr::log::Line("CreateDevice called (Adapter=" + std::to_string(Adapter) + ").");
    if (pPresentationParameters) {
        const D3DPRESENT_PARAMETERS& p = *pPresentationParameters;
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "CreateDevice params: windowed %d, %ux%u, format %d, refresh %u Hz, buffers %u, "
                      "multisample %d/%lu, swap %d, flags 0x%lX, behavior 0x%lX.",
                      p.Windowed, p.BackBufferWidth, p.BackBufferHeight, static_cast<int>(p.BackBufferFormat),
                      p.FullScreen_RefreshRateInHz, p.BackBufferCount, static_cast<int>(p.MultiSampleType),
                      p.MultiSampleQuality, static_cast<int>(p.SwapEffect), p.Flags, BehaviorFlags);
        vr::log::Line(buf);
    }
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
    bool exFallback = false;
    if (g_d3d9ex) {
        IDirect3D9Ex* ex = nullptr;
        IDirect3DDevice9Ex* devEx = nullptr;
        // Fullscreen esclusivo: CreateDeviceEx vuole il display mode, con nullptr
        // risponde D3DERR_INVALIDCALL.
        D3DDISPLAYMODEEX mode = {};
        D3DDISPLAYMODEEX* fullscreenMode = nullptr;
        if (pPresentationParameters && !pPresentationParameters->Windowed) {
            mode.Size = sizeof(mode);
            mode.Width = pPresentationParameters->BackBufferWidth;
            mode.Height = pPresentationParameters->BackBufferHeight;
            mode.RefreshRate = pPresentationParameters->FullScreen_RefreshRateInHz;
            mode.Format = pPresentationParameters->BackBufferFormat;
            mode.ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
            fullscreenMode = &mode;
        }
        if (GpuShareFailTestRequested()) {
            hr = D3DERR_INVALIDCALL;
            vr::log::Line("GPUSHARE: CreateDeviceEx skipped (specopsvr_forcedevexfail test file).");
        } else if (SUCCEEDED(self->QueryInterface(__uuidof(IDirect3D9Ex), reinterpret_cast<void**>(&ex))) && ex) {
            hr = ex->CreateDeviceEx(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters,
                                    fullscreenMode, &devEx);
            ex->Release();
        }
        if (SUCCEEDED(hr) && devEx) {
            *ppReturnedDeviceInterface = devEx;
            vr::log::Line("GPUSHARE: device created with CreateDeviceEx.");
            InstallExResourceHooks(devEx);
        } else {
            vr::log::Line("GPUSHARE: CreateDeviceEx failed hr=" + std::to_string(hr) + " (windowed " +
                          std::to_string(pPresentationParameters ? pPresentationParameters->Windowed : -1) +
                          "), normal device.");
            g_d3d9ex = false;
            g_gpushareFellBack = true;
            exFallback = true;
        }
    }
    if (!g_d3d9ex) {
        // Un device creato con CreateDevice da un oggetto D3D9Ex rifiuta il pool
        // MANAGED (il gioco andava in errore fatale alla prima CreateTexture): il
        // fallback crea il device da un oggetto D3D9 normale, come con gpushare off.
        IDirect3D9* plain = nullptr;
        if (exFallback && vr::realdll::Direct3DCreate9_Real) plain = vr::realdll::Direct3DCreate9_Real(D3D_SDK_VERSION);
        if (plain) {
            g_createDeviceInner = true;
            hr = plain->CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters,
                                     ppReturnedDeviceInterface);
            g_createDeviceInner = false;
            plain->Release();  // il device tiene il suo riferimento
            vr::log::Line("GPUSHARE: fallback device created from a plain D3D9 object, hr=" + std::to_string(hr) + ".");
        } else {
            hr = g_CreateDevice_Original(self, Adapter, DeviceType, hFocusWindow, BehaviorFlags,
                                         pPresentationParameters, ppReturnedDeviceInterface);
        }
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
    // Prima che il motore inizializzi FMOD (dopo il D3D): l'audio del gioco
    // segue il predefinito di Windows.
    vr::audio::InstallAudioHooks();

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

void* PlayerControllerAlive() { return LivePlayerController(); }
bool OnTurret() { return g_onTurret.load(); }
void TestUnhookPresent() { g_testPresentUnhook = true; }

bool GpuShareRequested() {
    if (g_gpushareFellBack) return false;
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
