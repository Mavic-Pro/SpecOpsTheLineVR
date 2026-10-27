// Runtime VR finto: vedi vr_fake.h. Struttura del file:
//  1. tempo (QPC), configurazione e traiettorie (testa, controller, file di punti)
//  2. predizione delle pose e ricentramento (simevent zeropose)
//  3. frame loop finto (vsync, WaitGetPoses, Submit) e statistiche JITTER
//  4. le interfacce OpenVR (classi derivate da quelle generate per OpenXR,
//     xr_shim_ifaces.h: i metodi che la mod non usa restano gli stub vuoti)
//  5. comandi "fake ..." e API pubblica
#include "vr_fake.h"
#include "xr_shim_ifaces.h"
#include "control.h"
#include "logger.h"
#include "pure_utils.h"

#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace vr::fake {

namespace {

// --- 1. tempo, configurazione, traiettorie ------------------------------------

constexpr vr::TrackedDeviceIndex_t kHmd = 0, kLeft = 1, kRight = 2;
constexpr double kRunStart = 0.002;  // WaitGetPoses torna 2 ms prima del vsync ("running start" di SteamVR)
constexpr double kLatch = 0.001;     // il compositor prende un frame fino a 1 ms prima del vsync
constexpr double kTwoPi = 6.283185307179586;

double QpcSec() {
    static LARGE_INTEGER f = {};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) / static_cast<double>(f.QuadPart);
}

// Posa in gradi (convenzioni della mod: yaw a destra, pitch in su) e metri
// (spazio Standing di OpenVR: x destra, y su dal pavimento, z indietro).
struct Pose6 {
    float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
    float pos[3] = {0.0f, 0.0f, 0.0f};
};

struct Key {
    double t;
    Pose6 p;
};

struct Config {
    // Frame e display.
    float hz = 90.0f;
    float latencyMs = 45.0f;  // dal vsync ai fotoni (streaming compreso)
    bool openxr = false;      // semantica d'invio: false = openvr, true = openxr
    uint32_t w = 1296, h = 1440;  // per occhio (piccolo: la copia A gira accanto)
    // Tangenti dell'occhio sinistro (gradi: sinistra, destra, su, giu'); il destro e' speculare.
    float fovDeg[4] = {-50.0f, 43.0f, 44.0f, -50.0f};
    float ipdMm = 63.0f;
    bool present = true;
    // Testa: posa di base, oscillazioni (ampiezza gradi, frequenza Hz per yaw/pitch/roll),
    // spostamento (cm per x/y/z e frequenza), sguardo in giro (gradi/s rms),
    // tremore (gradi, 4-10 Hz), rotazione continua (gradi/s), file di punti chiave.
    Pose6 head;
    float swayAmp[3] = {0, 0, 0}, swayHz[3] = {0, 0, 0};
    float swayPosCm[3] = {0, 0, 0}, swayPosHz = 0.0f;
    float lookDps = 0.0f, tremorDeg = 0.0f, turnDps = 0.0f;
    std::vector<Key> script;
    bool scriptLoop = false;
    std::string scriptPath;
    // Controller (0 = sinistra, 1 = destra), pose raw nello spazio Standing.
    bool handOn[2] = {true, true};
    Pose6 hand[2];
    float handSwayDeg[2] = {0, 0}, handSwayCm[2] = {0, 0}, handSwayHz[2] = {0, 0};
    // Predizione: lineare (velocita' vera del momento, piu' rumore) o esatta (traiettoria futura).
    bool predictExact = false;
    float predNoiseDps = 0.0f, predNoiseCorrMs = 50.0f;
    // Intoppo del compositor: WaitGetPoses in ritardo di hitchMs ogni hitchEverySec.
    float hitchMs = 0.0f, hitchEverySec = 0.0f;
    // Statistiche: riga JITTER ogni logEvery frame; righe JITTERF per i prossimi frameLog frame.
    int logEvery = 300;
    int frameLog = 0;
    Config() {
        head.pos[1] = 1.70f;
        // Controller come tenuti a riposo (progetto del runtime finto, b2): in
        // giu' di 20 gradi e ruotati di 15 verso l'interno (rollio), non in bolla.
        hand[0].pos[0] = -0.20f; hand[0].pos[1] = 1.10f; hand[0].pos[2] = -0.25f;
        hand[1].pos[0] = 0.20f;  hand[1].pos[1] = 1.20f; hand[1].pos[2] = -0.35f;
        hand[0].pitch = hand[1].pitch = -20.0f;
        hand[0].roll = 15.0f;
        hand[1].roll = -15.0f;
    }
};

struct Ov {
    std::string key, name;
    bool visible = false;
    uint32_t flags = 0;
    float alpha = 1.0f, width = 1.0f;
    uint32_t sort = 0;
    vr::VRTextureBounds_t bounds = {0, 0, 1, 1};
    vr::HmdMatrix34_t xform = {};
    vr::ETrackingUniverseOrigin origin = vr::TrackingUniverseStanding;
    vr::TrackedDeviceIndex_t device = vr::k_unTrackedDeviceIndexInvalid;
    vr::VROverlayInputMethod input = vr::VROverlayInputMethod_None;
    vr::HmdVector2_t mouse = {1.0f, 1.0f};
    uint32_t texW = 0, texH = 0;
};

struct ViewRec {
    uint32_t tick = 0;
    int eye = -1;
    int32_t rot[3] = {0, 0, 0};
    int32_t body = 0;
    float ref = 0.0f;
    bool valid = false;
};

struct State {
    std::mutex mutex;
    Config cfg;
    bool running = false;
    double t0 = 0.0;      // epoca delle traiettorie (s, QPC)
    double gridT0 = 0.0;  // epoca della griglia dei vsync: ripartita a ogni cambio di frequenza
    // Ricentramento del runtime: lo spazio Standing ruotato di originYaw (gradi) e spostato di originPos (x, z).
    float originYaw = 0.0f, originPos[3] = {0, 0, 0};
    std::deque<vr::VREvent_t> events;
    // Frame loop.
    long long lastK = -1;          // vsync dell'ultimo WaitGetPoses
    double frameDisplayT = 0.0;    // fotoni del frame di quel WaitGetPoses (s, QPC)
    vr::HmdMatrix34_t waitPose = {};
    bool waitPoseValid = false;
    vr::TrackedDevicePose_t lastPoses[3] = {};
    long long lastShownM = -1;
    double lastHitchT = 0.0;
    int hitches = 0;
    bool eyeSet[2] = {}, eyePosed[2] = {};
    vr::HmdMatrix34_t eyePose[2] = {};
    // Dal mod (NoteSubmit) per la coppia in invio.
    uint32_t noteTick = 0;
    bool noteValid = false, noteCinema = false;
    vr::HmdMatrix34_t notePose = {};
    // Finestra delle statistiche. rpRot / rpPos: posa della testa con cui la
    // mod ha disegnato (NoteSubmit) contro la posa dichiarata al compositor
    // (gradi, millimetri): la parte di posizione che "mismatch" non vede.
    std::vector<float> mismatch, step, edge, rpRot, rpPos;
    int frames = 0, posed = 0, repeats = 0, late = 0, dropped = 0, cinema = 0, noView = 0, overStep = 0;
    int totalFrames = 0;
    double windowStart = 0.0;
    bool haveQ = false;
    float prevQ[3][3] = {};
    // Dissolvenze del compositor.
    float fadeFrom = 0.0f, fadeTo = 0.0f;
    double fadeStart = 0.0, fadeDur = 0.0;
    int fadesToBlack = 0;
    // Overlay e input.
    std::map<vr::VROverlayHandle_t, Ov> overlays;
    uint64_t nextOverlay = 1;
    std::map<std::string, uint64_t> actions;
    uint64_t nextAction = 100;
    int haptics = 0;
    // Rumore della predizione (Ornstein-Uhlenbeck, gradi/s) per yaw/pitch/roll.
    float noise[3] = {0, 0, 0};
    double noiseT = 0.0;
    std::mt19937 rng{20261025u};
};
State g;

std::mutex g_viewMutex;
ViewRec g_views[64];

float Deg2Rad(float d) { return d * 0.01745329252f; }

// Punti chiave: interpolazione lineare per canale (yaw continuo, senza avvolgere).
Pose6 ScriptAt(const Config& c, double t) {
    const std::vector<Key>& k = c.script;
    if (k.size() == 1 || t <= k.front().t) return k.front().p;
    double span = k.back().t - k.front().t;
    if (t >= k.back().t) {
        if (!c.scriptLoop || span <= 0.0) return k.back().p;
        t = k.front().t + std::fmod(t - k.front().t, span);
    }
    size_t i = 1;
    while (i < k.size() && k[i].t < t) ++i;
    if (i >= k.size()) return k.back().p;
    const Key& a = k[i - 1];
    const Key& b = k[i];
    float u = b.t > a.t ? static_cast<float>((t - a.t) / (b.t - a.t)) : 1.0f;
    Pose6 p;
    p.yaw = a.p.yaw + (b.p.yaw - a.p.yaw) * u;
    p.pitch = a.p.pitch + (b.p.pitch - a.p.pitch) * u;
    p.roll = a.p.roll + (b.p.roll - a.p.roll) * u;
    for (int j = 0; j < 3; ++j) p.pos[j] = a.p.pos[j] + (b.p.pos[j] - a.p.pos[j]) * u;
    return p;
}

// Testa nello spazio della stanza (prima del ricentramento), t in secondi dall'avvio.
Pose6 RoomHeadAt(const Config& c, double t) {
    Pose6 p = c.script.empty() ? c.head : ScriptAt(c, t);
    float* ang[3] = {&p.yaw, &p.pitch, &p.roll};
    for (int i = 0; i < 3; ++i)
        if (c.swayAmp[i] != 0.0f && c.swayHz[i] > 0.0f) *ang[i] += c.swayAmp[i] * static_cast<float>(std::sin(kTwoPi * c.swayHz[i] * t));
    p.yaw += static_cast<float>(c.turnDps * t);
    if (c.lookDps > 0.0f) {
        // Sguardo in giro: somma di sinusoidi a frequenze incommensurabili,
        // ognuna con la stessa velocita' di picco: velocita' rms = lookDps (yaw), meta' in pitch.
        static const double fy[4] = {0.11, 0.23, 0.37, 0.53}, py[4] = {0.0, 1.3, 2.9, 4.4};
        static const double fp[3] = {0.17, 0.29, 0.43}, pp[3] = {0.7, 2.1, 3.6};
        const double a = c.lookDps / (kTwoPi * std::sqrt(2.0));  // 4 termini: rms = 2*pi*a*sqrt(2)
        double y = 0.0, q = 0.0;
        for (int i = 0; i < 4; ++i) y += a / fy[i] * std::sin(kTwoPi * fy[i] * t + py[i]);
        const double b = 0.5 * c.lookDps / (kTwoPi * std::sqrt(1.5));
        for (int i = 0; i < 3; ++i) q += b / fp[i] * std::sin(kTwoPi * fp[i] * t + pp[i]);
        p.yaw += static_cast<float>(y);
        p.pitch += static_cast<float>(q);
    }
    if (c.tremorDeg > 0.0f) {
        const double tr = c.tremorDeg;
        p.yaw += static_cast<float>(tr * 0.5 * (std::sin(kTwoPi * 4.7 * t) + 0.6 * std::sin(kTwoPi * 7.3 * t + 1.0) +
                                                0.4 * std::sin(kTwoPi * 9.1 * t + 2.0)));
        p.pitch += static_cast<float>(tr * 0.5 * (std::sin(kTwoPi * 5.3 * t + 0.4) + 0.6 * std::sin(kTwoPi * 8.1 * t + 1.7)));
        p.roll += static_cast<float>(tr * 0.3 * std::sin(kTwoPi * 6.2 * t + 0.9));
    }
    if (c.swayPosHz > 0.0f)
        for (int i = 0; i < 3; ++i)
            p.pos[i] += c.swayPosCm[i] * 0.01f * static_cast<float>(std::sin(kTwoPi * c.swayPosHz * t + i * 1.1));
    return p;
}

Pose6 RoomHandAt(const Config& c, int h, double t) {
    Pose6 p = c.hand[h];
    if (c.handSwayHz[h] > 0.0f) {
        const float s = static_cast<float>(std::sin(kTwoPi * c.handSwayHz[h] * t + h));
        const float s2 = static_cast<float>(std::sin(kTwoPi * c.handSwayHz[h] * 1.37 * t + 0.5 + h));
        p.yaw += c.handSwayDeg[h] * s;
        p.pitch += c.handSwayDeg[h] * 0.7f * s2;
        p.roll += c.handSwayDeg[h] * 0.5f * s;
        p.pos[0] += c.handSwayCm[h] * 0.01f * s;
        p.pos[1] += c.handSwayCm[h] * 0.01f * s2;
    }
    return p;
}

// Spazio della stanza -> spazio Standing dopo i ricentramenti del runtime.
Pose6 ToStanding(const State& s, Pose6 p) {
    if (s.originYaw == 0.0f && s.originPos[0] == 0.0f && s.originPos[2] == 0.0f) return p;
    float m[3][4];
    vr::pure::PoseFromEuler(0.0f, Deg2Rad(-s.originYaw), 0.0f, m);
    const float d[3] = {p.pos[0] - s.originPos[0], p.pos[1], p.pos[2] - s.originPos[2]};
    for (int i = 0; i < 3; ++i) p.pos[i] = m[i][0] * d[0] + m[i][1] * d[1] + m[i][2] * d[2];
    p.yaw -= s.originYaw;
    return p;
}

// Dispositivo d (kHmd, kLeft, kRight) al tempo t (secondi dall'avvio), con g.mutex preso.
Pose6 TrueAt(int d, double t) {
    Pose6 p = d == kHmd ? RoomHeadAt(g.cfg, t) : RoomHandAt(g.cfg, d == kLeft ? 0 : 1, t);
    return ToStanding(g, p);
}

void ToMatrix(const Pose6& p, float m[3][4]) {
    vr::pure::PoseFromEuler(Deg2Rad(p.pitch), Deg2Rad(p.yaw), Deg2Rad(p.roll), m);
    for (int i = 0; i < 3; ++i) m[i][3] = p.pos[i];
}

// --- 2. predizione --------------------------------------------------------------

float WrapDeg(float d) { return std::remainder(d, 360.0f); }

void UpdateNoise(double now) {
    if (g.cfg.predNoiseDps <= 0.0f) {
        g.noise[0] = g.noise[1] = g.noise[2] = 0.0f;
        g.noiseT = now;
        return;
    }
    double dt = now - g.noiseT;
    if (dt <= 0.0) return;
    if (dt > 0.2) dt = 0.2;
    g.noiseT = now;
    const double tau = (std::max)(0.005, g.cfg.predNoiseCorrMs * 0.001);
    std::normal_distribution<double> n(0.0, 1.0);
    for (float& v : g.noise) v += static_cast<float>(-v * dt / tau + g.cfg.predNoiseDps * std::sqrt(2.0 * dt / tau) * n(g.rng));
}

// Posa che il runtime prevede ora (secondi dall'avvio) per tra dt secondi.
Pose6 PredictedAt(int d, double now, double dt) {
    if (g.cfg.predictExact || dt <= 0.0) return TrueAt(d, now + (dt > 0.0 ? dt : 0.0));
    Pose6 p0 = TrueAt(d, now), p1 = TrueAt(d, now + 0.001);
    Pose6 p = p0;
    const float k = static_cast<float>(dt / 0.001);
    p.yaw += WrapDeg(p1.yaw - p0.yaw) * k;
    p.pitch += (p1.pitch - p0.pitch) * k;
    p.roll += WrapDeg(p1.roll - p0.roll) * k;
    for (int i = 0; i < 3; ++i) p.pos[i] += (p1.pos[i] - p0.pos[i]) * k;
    if (d == kHmd) {
        p.yaw += g.noise[0] * static_cast<float>(dt);
        p.pitch += g.noise[1] * static_cast<float>(dt);
        p.roll += g.noise[2] * static_cast<float>(dt);
    }
    if (p.pitch > 89.9f) p.pitch = 89.9f;
    if (p.pitch < -89.9f) p.pitch = -89.9f;
    return p;
}

void FillPose(int d, double now, double dt, vr::TrackedDevicePose_t* out) {
    *out = {};
    Pose6 p = PredictedAt(d, now, dt);
    ToMatrix(p, out->mDeviceToAbsoluteTracking.m);
    Pose6 a = TrueAt(d, now + dt), b = TrueAt(d, now + dt + 0.001);
    for (int i = 0; i < 3; ++i) out->vVelocity.v[i] = (b.pos[i] - a.pos[i]) / 0.001f;
    // Velocita' angolare approssimata (rad/s, assi di OpenVR: pitch attorno a x, yaw attorno a -y, roll attorno a -z).
    out->vAngularVelocity.v[0] = Deg2Rad((b.pitch - a.pitch) / 0.001f);
    out->vAngularVelocity.v[1] = -Deg2Rad(WrapDeg(b.yaw - a.yaw) / 0.001f);
    out->vAngularVelocity.v[2] = -Deg2Rad(WrapDeg(b.roll - a.roll) / 0.001f);
    out->eTrackingResult = vr::TrackingResult_Running_OK;
    out->bPoseIsValid = true;
    out->bDeviceIsConnected = true;
}

void PushEvent(vr::EVREventType type, vr::TrackedDeviceIndex_t dev = kHmd) {
    vr::VREvent_t ev = {};
    ev.eventType = type;
    ev.trackedDeviceIndex = dev;
    g.events.push_back(ev);
    if (g.events.size() > 64) g.events.pop_front();
}

// --- 3. frame loop e statistiche --------------------------------------------------

void WaitUntil(double t) {
    thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    for (;;) {
        double left = t - QpcSec();
        if (left <= 0.0) return;
        if (left > 0.0015 && timer) {
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>((left - 0.001) * 1e7);
            if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(timer, 100);
                continue;
            }
        }
        if (left > 0.002) Sleep(1);
        else YieldProcessor();
    }
}

float FadeAlphaNow() {
    double now = QpcSec();
    if (g.fadeDur <= 0.0 || now - g.fadeStart >= g.fadeDur) return g.fadeTo;
    float u = static_cast<float>((now - g.fadeStart) / g.fadeDur);
    return g.fadeFrom + (g.fadeTo - g.fadeFrom) * u;
}

// Rotazione 3x3 (colonne destra, su, indietro come OpenVR) della vista del gioco:
// FRotator di UE3 nel mondo, assi di UE3 riportati a quelli di OpenVR.
void WorldViewAxes(const int32_t* rot, float r[3][3]) {
    const float k = 6.28318530718f / 65536.0f;
    float f[3], rt[3], u[3];
    vr::pure::UeAxesFromRotator(rot[0] * k, rot[1] * k, rot[2] * k, f, rt, u);
    // UE (X avanti, Y destra, Z su) -> OpenVR (x destra = Y, y su = Z, z indietro = -X).
    auto conv = [](const float* v, float* o) {
        o[0] = v[1];
        o[1] = v[2];
        o[2] = -v[0];
    };
    float cr[3], cu[3], cf[3];
    conv(rt, cr);
    conv(u, cu);
    conv(f, cf);
    for (int i = 0; i < 3; ++i) {
        r[i][0] = cr[i];
        r[i][1] = cu[i];
        r[i][2] = -cf[i];
    }
}

void Mul3(const float a[3][3], const float b[3][3], float o[3][3]) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) o[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
}

void Rot3(const float m[3][4], float o[3][3]) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) o[i][j] = m[i][j];
}

// Angolo tra due rotazioni 3x3 (matrici con le colonne come assi).
double Angle3(const float a[3][3], const float b[3][3]) {
    float ta[3][3], tb[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            ta[j][i] = a[i][j];
            tb[j][i] = b[i][j];
        }
    return vr::pure::AxesAngleDeg(ta, tb);
}

bool FindView(uint32_t tick, int eye, ViewRec* out) {
    std::lock_guard<std::mutex> lock(g_viewMutex);
    for (const ViewRec& v : g_views)
        if (v.valid && v.tick == tick && v.eye == eye) {
            *out = v;
            return true;
        }
    return false;
}

float Percentile(std::vector<float> v, double q) {
    if (v.empty()) return 0.0f;
    std::sort(v.begin(), v.end());
    size_t i = static_cast<size_t>(std::floor(q * (v.size() - 1) + 0.5));
    return v[(std::min)(i, v.size() - 1)];
}

float Rms(const std::vector<float>& v) {
    if (v.empty()) return 0.0f;
    double s = 0.0;
    for (float x : v) s += static_cast<double>(x) * x;
    return static_cast<float>(std::sqrt(s / v.size()));
}

float MaxOf(const std::vector<float>& v) { return v.empty() ? 0.0f : *std::max_element(v.begin(), v.end()); }

// Con g.mutex preso.
void LogWindow(double now) {
    char b[900];
    const double dur = now - g.windowStart;
    std::snprintf(b, sizeof(b),
                  "JITTER: %d frames at %.0f Hz (%s, display %.0f ms, posed %d) | mismatch declared vs drawn rms %.3f p99 %.3f max %.3f deg"
                  " | world step rms %.3f p99 %.3f max %.3f deg, over 0.1 deg %.1f%% | edge rms %.2f p99 %.2f max %.2f deg"
                  " | repeats %d late %d dropped %d hitches %d | fades to black %d | %.1f frames/s | no view %d cinema %d"
                  " | render pose vs declared rot rms %.3f max %.3f deg pos rms %.2f max %.2f mm (%d)",
                  g.frames, g.cfg.hz, g.cfg.openxr ? "openxr" : "openvr", g.cfg.latencyMs, g.posed, Rms(g.mismatch),
                  Percentile(g.mismatch, 0.99), MaxOf(g.mismatch), Rms(g.step), Percentile(g.step, 0.99), MaxOf(g.step),
                  g.step.empty() ? 0.0 : 100.0 * g.overStep / g.step.size(), Rms(g.edge), Percentile(g.edge, 0.99), MaxOf(g.edge),
                  g.repeats, g.late, g.dropped, g.hitches, g.fadesToBlack, dur > 0.0 ? g.frames / dur : 0.0, g.noView, g.cinema,
                  Rms(g.rpRot), MaxOf(g.rpRot), Rms(g.rpPos), MaxOf(g.rpPos), static_cast<int>(g.rpRot.size()));
    vr::log::Line(b);
    g.mismatch.clear();
    g.step.clear();
    g.edge.clear();
    g.rpRot.clear();
    g.rpPos.clear();
    g.frames = g.posed = g.repeats = g.late = g.dropped = g.cinema = g.noView = g.overStep = 0;
    g.hitches = 0;
    g.fadesToBlack = 0;
    g.windowStart = now;
}

// Coppia di occhi completa (secondo Submit), con g.mutex preso.
void FrameDone(double s) {
    const double P = 1.0 / g.cfg.hz, D = g.cfg.latencyMs * 0.001;
    const long long k = g.lastK;
    long long m = static_cast<long long>(std::ceil((s - g.gridT0 + kLatch) / P));
    if (m < k + 1) m = k + 1;  // mai prima del vsync per cui e' stato atteso
    if (g.lastShownM >= 0) {
        if (m <= g.lastShownM) {
            ++g.dropped;  // due frame per lo stesso vsync: si vede l'ultimo
            m = g.lastShownM;
        } else {
            g.repeats += static_cast<int>(m - g.lastShownM - 1);
        }
    }
    if (m > k + 1) ++g.late;
    g.lastShownM = m;
    const double tShown = g.gridT0 + m * P + D;
    ++g.frames;
    ++g.totalFrames;
    if (g.noteCinema) {
        ++g.cinema;
        g.haveQ = false;
    } else {
        float decl[3][4];
        bool haveDecl = true;
        if (g.eyePosed[0]) {
            std::memcpy(decl, g.eyePose[0].m, sizeof(decl));
            ++g.posed;
        } else if (g.waitPoseValid) {
            std::memcpy(decl, g.waitPose.m, sizeof(decl));  // openvr e openxr: la posa del frame atteso
        } else {
            haveDecl = false;
        }
        float mis = -1.0f, st = -1.0f, ed = -1.0f;
        if (haveDecl) {
            float D3[3][3];
            Rot3(decl, D3);
            ViewRec v;
            if (FindView(g.noteTick, 0, &v)) {
                float W[3][3];
                WorldViewAxes(v.rot, W);
                // Vista nello spazio del tracciamento: il mondo e' girato rispetto alla
                // stanza di (corpo - riferimento della testa).
                const float yawWorld = v.body * (6.28318530718f / 65536.0f) - v.ref;
                float yw[3][4], Y[3][3], C[3][3];
                vr::pure::PoseFromEuler(0.0f, -yawWorld, 0.0f, yw);
                Rot3(yw, Y);
                Mul3(Y, W, C);
                mis = static_cast<float>(Angle3(C, D3));
                g.mismatch.push_back(mis);
                // Mondo visto rispetto alla stanza: W * D^-1. Il suo cambio tra due
                // frame e' quanto il mondo si muove senza che il giocatore lo muova.
                float Dt[3][3], Q[3][3];
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j) Dt[i][j] = D3[j][i];
                Mul3(W, Dt, Q);
                if (g.haveQ) {
                    st = static_cast<float>(Angle3(Q, g.prevQ));
                    g.step.push_back(st);
                    if (st > 0.1f) ++g.overStep;
                }
                std::memcpy(g.prevQ, Q, sizeof(Q));
                g.haveQ = true;
            } else {
                ++g.noView;
                g.haveQ = false;
            }
            if (g.noteValid) {
                // Posa di disegno della mod contro quella dichiarata: rotazione e posizione.
                float R3[3][3];
                Rot3(g.notePose.m, R3);
                g.rpRot.push_back(static_cast<float>(Angle3(R3, D3)));
                float d2 = 0.0f;
                for (int i = 0; i < 3; ++i) {
                    const float d = g.notePose.m[i][3] - decl[i][3];
                    d2 += d * d;
                }
                g.rpPos.push_back(std::sqrt(d2) * 1000.0f);
            }
            float tr[3][4], T3[3][3];
            Pose6 pt = TrueAt(kHmd, tShown - g.t0);
            ToMatrix(pt, tr);
            Rot3(tr, T3);
            ed = static_cast<float>(Angle3(T3, D3));
            g.edge.push_back(ed);
        }
        if (g.cfg.frameLog > 0) {
            --g.cfg.frameLog;
            char b[260];
            std::snprintf(b, sizeof(b),
                          "JITTERF: frame %d tick %u wait vsync %lld shown %lld (%s) posed %d mismatch %.4f step %.4f edge %.3f "
                          "submit %.2f ms before the vsync",
                          g.totalFrames, g.noteTick, k, m, m > k + 1 ? "late" : "on time", g.eyePosed[0] ? 1 : 0, mis, st, ed,
                          (g.gridT0 + m * P - s) * 1000.0);
            vr::log::Line(b);
        }
    }
    if (g.cfg.logEvery > 0 && g.frames >= g.cfg.logEvery) LogWindow(QpcSec());
}

// --- 4. interfacce OpenVR ---------------------------------------------------------

uint32_t CopyStr(const std::string& s, char* out, uint32_t size, vr::ETrackedPropertyError* e) {
    if (e) *e = vr::TrackedProp_Success;
    if (out && size) strncpy_s(out, size, s.c_str(), _TRUNCATE);
    return static_cast<uint32_t>(s.size() + 1);
}

void EyeTangents(int eye, float* l, float* r, float* u, float* d) {
    const float* f = g.cfg.fovDeg;
    // Occhio destro speculare al sinistro (lato interno e esterno scambiati).
    float left = eye == 0 ? f[0] : -f[1], right = eye == 0 ? f[1] : -f[0];
    *l = std::tan(Deg2Rad(left));
    *r = std::tan(Deg2Rad(right));
    *u = std::tan(Deg2Rad(f[2]));
    *d = std::tan(Deg2Rad(f[3]));
}

class FakeSystem : public vr::xr::SystemShim {
public:
    void GetRecommendedRenderTargetSize(uint32_t* w, uint32_t* h) override {
        std::lock_guard<std::mutex> lock(g.mutex);
        if (w) *w = g.cfg.w;
        if (h) *h = g.cfg.h;
    }
    vr::HmdMatrix44_t GetProjectionMatrix(vr::EVREye eye, float zNear, float zFar) override {
        std::lock_guard<std::mutex> lock(g.mutex);
        float l, r, u, d;
        EyeTangents(eye == vr::Eye_Left ? 0 : 1, &l, &r, &u, &d);
        vr::HmdMatrix44_t m = {};
        m.m[0][0] = 2.0f / (r - l);
        m.m[0][2] = (r + l) / (r - l);
        m.m[1][1] = 2.0f / (u - d);
        m.m[1][2] = (u + d) / (u - d);
        m.m[2][2] = -(zFar + zNear) / (zFar - zNear);
        m.m[2][3] = -2.0f * zFar * zNear / (zFar - zNear);
        m.m[3][2] = -1.0f;
        return m;
    }
    void GetProjectionRaw(vr::EVREye eye, float* pl, float* pr, float* pt, float* pb) override {
        std::lock_guard<std::mutex> lock(g.mutex);
        float l, r, u, d;
        EyeTangents(eye == vr::Eye_Left ? 0 : 1, &l, &r, &u, &d);
        if (pl) *pl = l;
        if (pr) *pr = r;
        if (pt) *pt = -u;  // convenzione OpenVR: alto negativo
        if (pb) *pb = -d;
    }
    vr::HmdMatrix34_t GetEyeToHeadTransform(vr::EVREye eye) override {
        std::lock_guard<std::mutex> lock(g.mutex);
        vr::HmdMatrix34_t m = {};
        m.m[0][0] = m.m[1][1] = m.m[2][2] = 1.0f;
        m.m[0][3] = (eye == vr::Eye_Left ? -0.0005f : 0.0005f) * g.cfg.ipdMm;
        return m;
    }
    bool GetTimeSinceLastVsync(float* since, uint64_t* counter) override {
        std::lock_guard<std::mutex> lock(g.mutex);
        if (!g.running) return false;
        const double P = 1.0 / g.cfg.hz, e = QpcSec() - g.gridT0;
        if (since) *since = static_cast<float>(std::fmod(e, P));
        if (counter) *counter = static_cast<uint64_t>(e / P);
        return true;
    }
    void GetDXGIOutputInfo(int32_t* idx) override {
        if (idx) *idx = 0;
    }
    void GetDeviceToAbsoluteTrackingPose(vr::ETrackingUniverseOrigin, float predictSec, vr::TrackedDevicePose_t* poses,
                                         uint32_t count) override {
        if (!poses || !count) return;
        std::memset(poses, 0, sizeof(vr::TrackedDevicePose_t) * count);
        for (uint32_t i = 0; i < count; ++i) poses[i].eTrackingResult = vr::TrackingResult_Uninitialized;
        std::lock_guard<std::mutex> lock(g.mutex);
        if (!g.running) return;
        const double now = QpcSec() - g.t0;
        UpdateNoise(now);
        FillPose(kHmd, now, predictSec, &poses[kHmd]);
        for (int h = 0; h < 2; ++h) {
            const vr::TrackedDeviceIndex_t idx = h == 0 ? kLeft : kRight;
            if (idx >= count || !g.cfg.handOn[h]) continue;
            FillPose(idx, now, predictSec, &poses[idx]);
        }
    }
    vr::TrackedDeviceIndex_t GetTrackedDeviceIndexForControllerRole(vr::ETrackedControllerRole role) override {
        std::lock_guard<std::mutex> lock(g.mutex);
        if (role == vr::TrackedControllerRole_LeftHand) return g.cfg.handOn[0] ? kLeft : vr::k_unTrackedDeviceIndexInvalid;
        if (role == vr::TrackedControllerRole_RightHand) return g.cfg.handOn[1] ? kRight : vr::k_unTrackedDeviceIndexInvalid;
        return vr::k_unTrackedDeviceIndexInvalid;
    }
    vr::ETrackedControllerRole GetControllerRoleForTrackedDeviceIndex(vr::TrackedDeviceIndex_t i) override {
        return i == kLeft ? vr::TrackedControllerRole_LeftHand
                          : (i == kRight ? vr::TrackedControllerRole_RightHand : vr::TrackedControllerRole_Invalid);
    }
    vr::ETrackedDeviceClass GetTrackedDeviceClass(vr::TrackedDeviceIndex_t i) override {
        if (i == kHmd) return vr::TrackedDeviceClass_HMD;
        if (i == kLeft || i == kRight) return IsTrackedDeviceConnected(i) ? vr::TrackedDeviceClass_Controller : vr::TrackedDeviceClass_Invalid;
        return vr::TrackedDeviceClass_Invalid;
    }
    bool IsTrackedDeviceConnected(vr::TrackedDeviceIndex_t i) override {
        std::lock_guard<std::mutex> lock(g.mutex);
        if (i == kHmd) return g.running;
        if (i == kLeft) return g.running && g.cfg.handOn[0];
        if (i == kRight) return g.running && g.cfg.handOn[1];
        return false;
    }
    bool GetBoolTrackedDeviceProperty(vr::TrackedDeviceIndex_t, vr::ETrackedDeviceProperty, vr::ETrackedPropertyError* e) override {
        if (e) *e = vr::TrackedProp_UnknownProperty;
        return false;
    }
    float GetFloatTrackedDeviceProperty(vr::TrackedDeviceIndex_t i, vr::ETrackedDeviceProperty prop, vr::ETrackedPropertyError* e) override {
        std::lock_guard<std::mutex> lock(g.mutex);
        if (e) *e = vr::TrackedProp_Success;
        if (i == kHmd) {
            if (prop == vr::Prop_DisplayFrequency_Float) return g.cfg.hz;
            // Come i due runtime veri: SteamVR dichiara il ritardo vsync->fotoni, il backend OpenXR 0
            // (il tempo di visualizzazione arriva da xrWaitFrame).
            if (prop == vr::Prop_SecondsFromVsyncToPhotons_Float) return g.cfg.openxr ? 0.0f : g.cfg.latencyMs * 0.001f;
            if (prop == vr::Prop_UserIpdMeters_Float) return g.cfg.ipdMm * 0.001f;
        }
        if (e) *e = vr::TrackedProp_UnknownProperty;
        return 0.0f;
    }
    int32_t GetInt32TrackedDeviceProperty(vr::TrackedDeviceIndex_t, vr::ETrackedDeviceProperty, vr::ETrackedPropertyError* e) override {
        if (e) *e = vr::TrackedProp_UnknownProperty;
        return 0;
    }
    uint32_t GetStringTrackedDeviceProperty(vr::TrackedDeviceIndex_t i, vr::ETrackedDeviceProperty prop, char* out, uint32_t size,
                                            vr::ETrackedPropertyError* e) override {
        std::lock_guard<std::mutex> lock(g.mutex);
        if (i == kHmd) {
            switch (prop) {
            case vr::Prop_TrackingSystemName_String: return CopyStr("specopsvr_fake", out, size, e);
            case vr::Prop_ModelNumber_String: {
                char b[96];
                std::snprintf(b, sizeof(b), "SpecOpsVR fake HMD (%s, %.0f Hz)", g.cfg.openxr ? "openxr" : "openvr", g.cfg.hz);
                return CopyStr(b, out, size, e);
            }
            case vr::Prop_ManufacturerName_String: return CopyStr("SpecOpsVR", out, size, e);
            case vr::Prop_SerialNumber_String: return CopyStr("fake-hmd", out, size, e);
            case vr::Prop_RenderModelName_String: return CopyStr("generic_hmd", out, size, e);
            default: break;
            }
        } else if (i == kLeft || i == kRight) {
            const bool right = i == kRight;
            switch (prop) {
            // Pose gia' nel riferimento "raw" dei Touch: nessuna correzione nel mod (UpdatePoseFix).
            case vr::Prop_RenderModelName_String:
                return CopyStr(right ? "oculus_quest2_controller_right" : "oculus_quest2_controller_left", out, size, e);
            case vr::Prop_ControllerType_String: return CopyStr("oculus_touch", out, size, e);
            case vr::Prop_ModelNumber_String: return CopyStr(right ? "fake Touch (right)" : "fake Touch (left)", out, size, e);
            case vr::Prop_SerialNumber_String: return CopyStr(right ? "fake-right" : "fake-left", out, size, e);
            case vr::Prop_ManufacturerName_String: return CopyStr("SpecOpsVR", out, size, e);
            default: break;
            }
        }
        if (e) *e = vr::TrackedProp_UnknownProperty;
        if (out && size) out[0] = 0;
        return 0;
    }
    bool PollNextEvent(vr::VREvent_t* ev, uint32_t size) override {
        std::lock_guard<std::mutex> lock(g.mutex);
        if (g.events.empty() || !ev) return false;
        std::memcpy(ev, &g.events.front(), (std::min)(size, static_cast<uint32_t>(sizeof(vr::VREvent_t))));
        g.events.pop_front();
        return true;
    }
    vr::HiddenAreaMesh_t GetHiddenAreaMesh(vr::EVREye, vr::EHiddenAreaMeshType) override { return {}; }
    bool GetControllerState(vr::TrackedDeviceIndex_t i, vr::VRControllerState_t* s, uint32_t) override {
        if (!s || (i != kLeft && i != kRight)) return false;
        *s = {};
        return IsTrackedDeviceConnected(i);  // collegato, nessun tasto premuto
    }
    bool GetControllerStateWithPose(vr::ETrackingUniverseOrigin o, vr::TrackedDeviceIndex_t i, vr::VRControllerState_t* s,
                                    uint32_t size, vr::TrackedDevicePose_t* pose) override {
        bool ok = GetControllerState(i, s, size);
        if (pose) {
            vr::TrackedDevicePose_t all[3];
            GetDeviceToAbsoluteTrackingPose(o, 0.0f, all, 3);
            *pose = all[i < 3 ? i : 0];
        }
        return ok;
    }
    void TriggerHapticPulse(vr::TrackedDeviceIndex_t, uint32_t, unsigned short) override {
        std::lock_guard<std::mutex> lock(g.mutex);
        ++g.haptics;
    }
    bool IsInputAvailable() override { return true; }
    bool IsSteamVRDrawingControllers() override { return false; }
    bool ShouldApplicationPause() override { return false; }
    bool ShouldApplicationReduceRenderingWork() override { return false; }
    vr::EDeviceActivityLevel GetTrackedDeviceActivityLevel(vr::TrackedDeviceIndex_t i) override {
        std::lock_guard<std::mutex> lock(g.mutex);
        if (i != kHmd) return vr::k_EDeviceActivityLevel_Unknown;
        return g.cfg.present ? vr::k_EDeviceActivityLevel_UserInteraction : vr::k_EDeviceActivityLevel_Standby;
    }
    void AcknowledgeQuit_Exiting() override {}
    const char* GetRuntimeVersion() override { return "specopsvr-fake-1"; }
};

class FakeCompositor : public vr::xr::CompositorShim {
public:
    void SetTrackingSpace(vr::ETrackingUniverseOrigin) override {}
    vr::ETrackingUniverseOrigin GetTrackingSpace() override { return vr::TrackingUniverseStanding; }
    vr::EVRCompositorError WaitGetPoses(vr::TrackedDevicePose_t* render, uint32_t nr, vr::TrackedDevicePose_t* game,
                                        uint32_t ng) override {
        double P, t0, hitchMs, hitchEvery, lastHitch;
        long long lastK;
        bool running;
        {
            std::lock_guard<std::mutex> lock(g.mutex);
            running = g.running;
            P = 1.0 / g.cfg.hz;
            t0 = g.gridT0;
            hitchMs = g.cfg.hitchMs;
            hitchEvery = g.cfg.hitchEverySec;
            lastHitch = g.lastHitchT;
            lastK = g.lastK;
        }
        if (!running) {
            Sleep(10);
            return vr::VRCompositorError_DoNotHaveFocus;
        }
        const double now = QpcSec();
        long long k = static_cast<long long>(std::ceil((now - t0 + kRunStart) / P));
        if (k <= lastK) k = lastK + 1;
        double start = t0 + k * P - kRunStart;
        bool hitch = false;
        if (hitchMs > 0.0 && hitchEvery > 0.0 && start - lastHitch >= hitchEvery) {
            // Intoppo del compositor: il frame torna in ritardo di hitchMs (vsync saltati).
            hitch = true;
            k = static_cast<long long>(std::ceil((start + hitchMs * 0.001 - t0 + kRunStart) / P));
            start = t0 + k * P - kRunStart;
        }
        WaitUntil(start);
        std::lock_guard<std::mutex> lock(g.mutex);
        auto copy = [&](vr::TrackedDevicePose_t* out, uint32_t n, const vr::TrackedDevicePose_t* src) {
            if (!out || !n) return;
            std::memset(out, 0, sizeof(vr::TrackedDevicePose_t) * n);
            for (uint32_t i = 0; i < n && i < 3; ++i) out[i] = src[i];
        };
        if (t0 != g.gridT0) {
            // Frequenza cambiata durante l'attesa: niente frame nuovo, ma le uscite
            // vanno sempre scritte (prima restavano non inizializzate; review 25/10).
            copy(render, nr, g.lastPoses);
            copy(game, ng, g.lastPoses);
            return vr::VRCompositorError_None;
        }
        if (hitch) {
            g.lastHitchT = start;
            ++g.hitches;
        }
        g.lastK = k;
        const double D = g.cfg.latencyMs * 0.001;
        g.frameDisplayT = t0 + (k + 1) * P + D;
        const double tNow = QpcSec() - g.t0;
        const double dt = g.frameDisplayT - g.t0 - tNow;
        UpdateNoise(tNow);
        vr::TrackedDevicePose_t poses[3];
        FillPose(kHmd, tNow, dt, &poses[kHmd]);
        for (int h = 0; h < 2; ++h) {
            const vr::TrackedDeviceIndex_t idx = h == 0 ? kLeft : kRight;
            if (g.cfg.handOn[h]) FillPose(idx, tNow, dt, &poses[idx]);
            else poses[idx] = {};
        }
        std::memcpy(g.lastPoses, poses, sizeof(poses));
        g.waitPose = poses[kHmd].mDeviceToAbsoluteTracking;
        g.waitPoseValid = true;
        copy(render, nr, poses);
        copy(game, ng, poses);
        return vr::VRCompositorError_None;
    }
    vr::EVRCompositorError GetLastPoses(vr::TrackedDevicePose_t* render, uint32_t nr, vr::TrackedDevicePose_t* game,
                                        uint32_t ng) override {
        std::lock_guard<std::mutex> lock(g.mutex);
        auto copy = [&](vr::TrackedDevicePose_t* out, uint32_t n) {
            if (!out || !n) return;
            std::memset(out, 0, sizeof(vr::TrackedDevicePose_t) * n);
            for (uint32_t i = 0; i < n && i < 3; ++i) out[i] = g.lastPoses[i];
        };
        copy(render, nr);
        copy(game, ng);
        return vr::VRCompositorError_None;
    }
    vr::EVRCompositorError Submit(vr::EVREye eye, const vr::Texture_t* tex, const vr::VRTextureBounds_t*,
                                  vr::EVRSubmitFlags flags) override {
        if (!tex || !tex->handle) return vr::VRCompositorError_InvalidTexture;
        const double s = QpcSec();
        std::lock_guard<std::mutex> lock(g.mutex);
        if (!g.running) return vr::VRCompositorError_DoNotHaveFocus;
        const int e = eye == vr::Eye_Left ? 0 : 1;
        g.eyeSet[e] = true;
        g.eyePosed[e] = (flags & vr::Submit_TextureWithPose) != 0;
        if (g.eyePosed[e]) g.eyePose[e] = static_cast<const vr::VRTextureWithPose_t*>(tex)->mDeviceToAbsoluteTracking;
        if (e == 0) return vr::VRCompositorError_None;
        FrameDone(s);
        g.eyeSet[0] = g.eyeSet[1] = false;
        g.noteCinema = false;
        g.noteValid = false;  // la posa di disegno vale per una sola coppia
        return vr::VRCompositorError_None;
    }
    void ClearLastSubmittedFrame() override {}
    void PostPresentHandoff() override {}
    float GetFrameTimeRemaining() override {
        std::lock_guard<std::mutex> lock(g.mutex);
        const double P = 1.0 / g.cfg.hz;
        return static_cast<float>(P - std::fmod(QpcSec() - g.gridT0, P));
    }
    void FadeToColor(float seconds, float, float, float, float alpha, bool) override {
        std::lock_guard<std::mutex> lock(g.mutex);
        const float from = FadeAlphaNow();
        g.fadeFrom = from;
        g.fadeTo = alpha;
        g.fadeStart = QpcSec();
        g.fadeDur = seconds;
        if (alpha >= 0.99f && from < 0.99f) {
            ++g.fadesToBlack;
            char b[96];
            std::snprintf(b, sizeof(b), "FAKE: compositor fade to black in %.2f s.", seconds);
            vr::log::Line(b);
        }
    }
    vr::HmdColor_t GetCurrentFadeColor(bool) override {
        std::lock_guard<std::mutex> lock(g.mutex);
        return {0.0f, 0.0f, 0.0f, FadeAlphaNow()};
    }
    bool IsFullscreen() override { return true; }
    uint32_t GetCurrentSceneFocusProcess() override { return GetCurrentProcessId(); }
    bool CanRenderScene() override { return true; }
    void ShowMirrorWindow() override {}
    void HideMirrorWindow() override {}
    bool IsMirrorWindowVisible() override { return false; }
    vr::EVRCompositorError GetMirrorTextureD3D11(vr::EVREye, void*, void** out) override {
        if (out) *out = nullptr;
        return vr::VRCompositorError_RequestFailed;  // il mod usa la propria finestra specchio
    }
    void ReleaseMirrorTextureD3D11(void*) override {}
};

#define FAKE_OV(h)                                     \
    std::lock_guard<std::mutex> lock(g.mutex);         \
    auto it = g.overlays.find(h);                      \
    if (it == g.overlays.end()) return vr::VROverlayError_UnknownOverlay; \
    Ov& o = it->second;

class FakeOverlay : public vr::xr::OverlayShim {
public:
    vr::EVROverlayError FindOverlay(const char* key, vr::VROverlayHandle_t* out) override {
        std::lock_guard<std::mutex> lock(g.mutex);
        for (auto& kv : g.overlays)
            if (key && kv.second.key == key) {
                if (out) *out = kv.first;
                return vr::VROverlayError_None;
            }
        return vr::VROverlayError_UnknownOverlay;
    }
    vr::EVROverlayError CreateOverlay(const char* key, const char* name, vr::VROverlayHandle_t* out) override {
        std::lock_guard<std::mutex> lock(g.mutex);
        for (auto& kv : g.overlays)
            if (key && kv.second.key == key) return vr::VROverlayError_KeyInUse;
        vr::VROverlayHandle_t h = g.nextOverlay++;
        Ov o;
        o.key = key ? key : "";
        o.name = name ? name : "";
        o.xform.m[0][0] = o.xform.m[1][1] = o.xform.m[2][2] = 1.0f;
        g.overlays[h] = o;
        if (out) *out = h;
        return vr::VROverlayError_None;
    }
    vr::EVROverlayError DestroyOverlay(vr::VROverlayHandle_t h) override {
        std::lock_guard<std::mutex> lock(g.mutex);
        return g.overlays.erase(h) ? vr::VROverlayError_None : vr::VROverlayError_UnknownOverlay;
    }
    uint32_t GetOverlayKey(vr::VROverlayHandle_t h, char* out, uint32_t size, vr::EVROverlayError* e) override {
        std::lock_guard<std::mutex> lock(g.mutex);
        auto it = g.overlays.find(h);
        if (e) *e = it == g.overlays.end() ? vr::VROverlayError_UnknownOverlay : vr::VROverlayError_None;
        std::string s = it == g.overlays.end() ? "" : it->second.key;
        if (out && size) strncpy_s(out, size, s.c_str(), _TRUNCATE);
        return static_cast<uint32_t>(s.size() + 1);
    }
    uint32_t GetOverlayName(vr::VROverlayHandle_t h, char* out, uint32_t size, vr::EVROverlayError* e) override {
        std::lock_guard<std::mutex> lock(g.mutex);
        auto it = g.overlays.find(h);
        if (e) *e = it == g.overlays.end() ? vr::VROverlayError_UnknownOverlay : vr::VROverlayError_None;
        std::string s = it == g.overlays.end() ? "" : it->second.name;
        if (out && size) strncpy_s(out, size, s.c_str(), _TRUNCATE);
        return static_cast<uint32_t>(s.size() + 1);
    }
    vr::EVROverlayError SetOverlayName(vr::VROverlayHandle_t h, const char* name) override {
        FAKE_OV(h);
        o.name = name ? name : "";
        return vr::VROverlayError_None;
    }
    vr::EVROverlayError SetOverlayFlag(vr::VROverlayHandle_t h, vr::VROverlayFlags f, bool on) override {
        FAKE_OV(h);
        o.flags = on ? (o.flags | f) : (o.flags & ~static_cast<uint32_t>(f));
        return vr::VROverlayError_None;
    }
    vr::EVROverlayError GetOverlayFlag(vr::VROverlayHandle_t h, vr::VROverlayFlags f, bool* on) override {
        FAKE_OV(h);
        if (on) *on = (o.flags & f) != 0;
        return vr::VROverlayError_None;
    }
    vr::EVROverlayError GetOverlayFlags(vr::VROverlayHandle_t h, uint32_t* f) override {
        FAKE_OV(h);
        if (f) *f = o.flags;
        return vr::VROverlayError_None;
    }
    vr::EVROverlayError SetOverlayColor(vr::VROverlayHandle_t h, float, float, float) override {
        FAKE_OV(h);
        (void)o;
        return vr::VROverlayError_None;
    }
    vr::EVROverlayError SetOverlayAlpha(vr::VROverlayHandle_t h, float a) override {
        FAKE_OV(h);
        o.alpha = a;
        return vr::VROverlayError_None;
    }
    vr::EVROverlayError GetOverlayAlpha(vr::VROverlayHandle_t h, float* a) override {
        FAKE_OV(h);
        if (a) *a = o.alpha;
        return vr::VROverlayError_None;
    }
    vr::EVROverlayError SetOverlayWidthInMeters(vr::VROverlayHandle_t h, float w) override {
        FAKE_OV(h);
        o.width = w;
        return vr::VROverlayError_None;
    }
    vr::EVROverlayError GetOverlayWidthInMeters(vr::VROverlayHandle_t h, float* w) override {
        FAKE_OV(h);
        if (w) *w = o.width;
        return vr::VROverlayError_None;
    }
    vr::EVROverlayError SetOverlaySortOrder(vr::VROverlayHandle_t h, uint32_t s) override {
        FAKE_OV(h);
        o.sort = s;
        return vr::VROverlayError_None;
    }
    vr::EVROverlayError GetOverlaySortOrder(vr::VROverlayHandle_t h, uint32_t* s) override {
        FAKE_OV(h);
        if (s) *s = o.sort;
        return vr::VROverlayError_None;
    }
    vr::EVROverlayError SetOverlayTextureBounds(vr::VROverlayHandle_t h, const vr::VRTextureBounds_t* b) override {
        FAKE_OV(h);
        if (b) o.bounds = *b;
        return vr::VROverlayError_None;
    }
    vr::EVROverlayError GetOverlayTextureBounds(vr::VROverlayHandle_t h, vr::VRTextureBounds_t* b) override {
        FAKE_OV(h);
        if (b) *b = o.bounds;
        return vr::VROverlayError_None;
    }
    vr::EVROverlayError SetOverlayTransformAbsolute(vr::VROverlayHandle_t h, vr::ETrackingUniverseOrigin org,
                                                    const vr::HmdMatrix34_t* m) override {
        FAKE_OV(h);
        o.origin = org;
        o.device = vr::k_unTrackedDeviceIndexInvalid;
        if (m) o.xform = *m;
        return vr::VROverlayError_None;
    }
    vr::EVROverlayError GetOverlayTransformAbsolute(vr::VROverlayHandle_t h, vr::ETrackingUniverseOrigin* org,
                                                    vr::HmdMatrix34_t* m) override {
        FAKE_OV(h);
        if (org) *org = o.origin;
        if (m) *m = o.xform;
        return vr::VROverlayError_None;
    }
    vr::EVROverlayError SetOverlayTransformTrackedDeviceRelative(vr::VROverlayHandle_t h, vr::TrackedDeviceIndex_t dev,
                                                                 const vr::HmdMatrix34_t* m) override {
        FAKE_OV(h);
        o.device = dev;
        if (m) o.xform = *m;
        return vr::VROverlayError_None;
    }
    vr::EVROverlayError GetOverlayTransformTrackedDeviceRelative(vr::VROverlayHandle_t h, vr::TrackedDeviceIndex_t* dev,
                                                                 vr::HmdMatrix34_t* m) override {
        FAKE_OV(h);
        if (dev) *dev = o.device;
        if (m) *m = o.xform;
        return vr::VROverlayError_None;
    }
    vr::EVROverlayError ShowOverlay(vr::VROverlayHandle_t h) override {
        FAKE_OV(h);
        o.visible = true;
        return vr::VROverlayError_None;
    }
    vr::EVROverlayError HideOverlay(vr::VROverlayHandle_t h) override {
        FAKE_OV(h);
        o.visible = false;
        return vr::VROverlayError_None;
    }
    bool IsOverlayVisible(vr::VROverlayHandle_t h) override {
        std::lock_guard<std::mutex> lock(g.mutex);
        auto it = g.overlays.find(h);
        return it != g.overlays.end() && it->second.visible;
    }
    vr::EVROverlayError GetTransformForOverlayCoordinates(vr::VROverlayHandle_t h, vr::ETrackingUniverseOrigin,
                                                          vr::HmdVector2_t, vr::HmdMatrix34_t* m) override {
        FAKE_OV(h);
        if (m) *m = o.xform;
        return vr::VROverlayError_None;
    }
    bool PollNextOverlayEvent(vr::VROverlayHandle_t, vr::VREvent_t*, uint32_t) override { return false; }
    vr::EVROverlayError GetOverlayInputMethod(vr::VROverlayHandle_t h, vr::VROverlayInputMethod* m) override {
        FAKE_OV(h);
        if (m) *m = o.input;
        return vr::VROverlayError_None;
    }
    vr::EVROverlayError SetOverlayInputMethod(vr::VROverlayHandle_t h, vr::VROverlayInputMethod m) override {
        FAKE_OV(h);
        o.input = m;
        return vr::VROverlayError_None;
    }
    vr::EVROverlayError GetOverlayMouseScale(vr::VROverlayHandle_t h, vr::HmdVector2_t* s) override {
        FAKE_OV(h);
        if (s) *s = o.mouse;
        return vr::VROverlayError_None;
    }
    vr::EVROverlayError SetOverlayMouseScale(vr::VROverlayHandle_t h, const vr::HmdVector2_t* s) override {
        FAKE_OV(h);
        if (s) o.mouse = *s;
        return vr::VROverlayError_None;
    }
    bool ComputeOverlayIntersection(vr::VROverlayHandle_t, const vr::VROverlayIntersectionParams_t*,
                                    vr::VROverlayIntersectionResults_t*) override {
        return false;  // nessun puntatore laser senza controller veri
    }
    bool IsHoverTargetOverlay(vr::VROverlayHandle_t) override { return false; }
    vr::EVROverlayError SetOverlayTexture(vr::VROverlayHandle_t h, const vr::Texture_t* t) override {
        FAKE_OV(h);
        if (t && t->handle && t->eType == vr::TextureType_DirectX) {
            D3D11_TEXTURE2D_DESC d = {};
            static_cast<ID3D11Texture2D*>(t->handle)->GetDesc(&d);
            o.texW = d.Width;
            o.texH = d.Height;
        }
        return vr::VROverlayError_None;
    }
    vr::EVROverlayError ClearOverlayTexture(vr::VROverlayHandle_t h) override {
        FAKE_OV(h);
        o.texW = o.texH = 0;
        return vr::VROverlayError_None;
    }
    vr::EVROverlayError SetOverlayRaw(vr::VROverlayHandle_t h, void*, uint32_t w, uint32_t h2, uint32_t) override {
        FAKE_OV(h);
        o.texW = w;
        o.texH = h2;
        return vr::VROverlayError_None;
    }
    vr::EVROverlayError GetOverlayTextureSize(vr::VROverlayHandle_t h, uint32_t* w, uint32_t* h2) override {
        FAKE_OV(h);
        if (w) *w = o.texW;
        if (h2) *h2 = o.texH;
        return vr::VROverlayError_None;
    }
    bool IsDashboardVisible() override { return false; }
};
#undef FAKE_OV

class FakeInput : public vr::xr::InputShim {
public:
    vr::EVRInputError SetActionManifestPath(const char*) override { return vr::VRInputError_None; }
    vr::EVRInputError GetActionSetHandle(const char*, vr::VRActionSetHandle_t* h) override {
        if (h) *h = 1;
        return vr::VRInputError_None;
    }
    vr::EVRInputError GetActionHandle(const char* name, vr::VRActionHandle_t* out) override {
        std::lock_guard<std::mutex> lock(g.mutex);
        std::string n = name ? name : "";
        auto it = g.actions.find(n);
        uint64_t h = it != g.actions.end() ? it->second : (g.actions[n] = g.nextAction++);
        if (out) *out = h;
        return vr::VRInputError_None;
    }
    vr::EVRInputError GetInputSourceHandle(const char*, vr::VRInputValueHandle_t* h) override {
        if (h) *h = 1;
        return vr::VRInputError_None;
    }
    vr::EVRInputError UpdateActionState(vr::VRActiveActionSet_t*, uint32_t, uint32_t) override { return vr::VRInputError_None; }
    // Controller collegati e fermi: azioni attive, nessun tasto (il pad finto "pad fake" ha la precedenza).
    vr::EVRInputError GetDigitalActionData(vr::VRActionHandle_t, vr::InputDigitalActionData_t* d, uint32_t,
                                           vr::VRInputValueHandle_t) override {
        if (d) {
            *d = {};
            d->bActive = true;
        }
        return vr::VRInputError_None;
    }
    vr::EVRInputError GetAnalogActionData(vr::VRActionHandle_t, vr::InputAnalogActionData_t* d, uint32_t,
                                          vr::VRInputValueHandle_t) override {
        if (d) {
            *d = {};
            d->bActive = true;
        }
        return vr::VRInputError_None;
    }
    vr::EVRInputError GetPoseActionDataRelativeToNow(vr::VRActionHandle_t, vr::ETrackingUniverseOrigin, float,
                                                     vr::InputPoseActionData_t* d, uint32_t, vr::VRInputValueHandle_t) override {
        if (d) *d = {};
        return vr::VRInputError_None;
    }
    vr::EVRInputError TriggerHapticVibrationAction(vr::VRActionHandle_t, float, float, float, float,
                                                   vr::VRInputValueHandle_t) override {
        std::lock_guard<std::mutex> lock(g.mutex);
        ++g.haptics;
        return vr::VRInputError_None;
    }
    vr::EVRInputError GetOriginLocalizedName(vr::VRInputValueHandle_t, char* out, uint32_t size, int32_t) override {
        if (out && size) out[0] = 0;
        return vr::VRInputError_None;
    }
    vr::EVRInputError GetOriginTrackedDeviceInfo(vr::VRInputValueHandle_t, vr::InputOriginInfo_t* i, uint32_t) override {
        if (i) *i = {};
        return vr::VRInputError_InvalidHandle;
    }
};

class FakeRenderModels : public vr::xr::RenderModelsShim {
public:
    vr::EVRRenderModelError LoadRenderModel_Async(const char*, vr::RenderModel_t** m) override {
        if (m) *m = nullptr;
        return vr::VRRenderModelError_NotSupported;
    }
    void FreeRenderModel(vr::RenderModel_t*) override {}
    uint32_t GetRenderModelName(uint32_t, char* out, uint32_t size) override {
        if (out && size) out[0] = 0;
        return 0;
    }
    uint32_t GetRenderModelCount() override { return 0; }
    uint32_t GetComponentCount(const char*) override { return 0; }
    uint32_t GetComponentName(const char*, uint32_t, char* out, uint32_t size) override {
        if (out && size) out[0] = 0;
        return 0;
    }
    bool GetComponentState(const char*, const char*, const vr::VRControllerState_t*, const vr::RenderModel_ControllerMode_State_t*,
                           vr::RenderModel_ComponentState_t*) override {
        return false;
    }
};

class FakeSettings : public vr::xr::SettingsShim {
public:
    const char* GetSettingsErrorNameFromEnum(vr::EVRSettingsError) override { return "not available (fake runtime)"; }
    void SetBool(const char*, const char*, bool, vr::EVRSettingsError* e) override { if (e) *e = vr::VRSettingsError_WriteFailed; }
    void SetInt32(const char*, const char*, int32_t, vr::EVRSettingsError* e) override { if (e) *e = vr::VRSettingsError_WriteFailed; }
    void SetFloat(const char*, const char*, float, vr::EVRSettingsError* e) override { if (e) *e = vr::VRSettingsError_WriteFailed; }
    void SetString(const char*, const char*, const char*, vr::EVRSettingsError* e) override { if (e) *e = vr::VRSettingsError_WriteFailed; }
    bool GetBool(const char*, const char*, vr::EVRSettingsError* e) override {
        if (e) *e = vr::VRSettingsError_ReadFailed;
        return false;
    }
    int32_t GetInt32(const char*, const char*, vr::EVRSettingsError* e) override {
        if (e) *e = vr::VRSettingsError_ReadFailed;
        return 0;
    }
    float GetFloat(const char*, const char*, vr::EVRSettingsError* e) override {
        if (e) *e = vr::VRSettingsError_ReadFailed;
        return 0.0f;
    }
    void GetString(const char*, const char*, char* out, uint32_t size, vr::EVRSettingsError* e) override {
        if (out && size) out[0] = 0;
        if (e) *e = vr::VRSettingsError_ReadFailed;
    }
};

class FakeApplications : public vr::xr::ApplicationsShim {
public:
    vr::EVRApplicationError GetApplicationKeyByProcessId(uint32_t, char* out, uint32_t size) override {
        if (out && size) out[0] = 0;
        return vr::VRApplicationError_UnknownApplication;
    }
    const char* GetApplicationsErrorNameFromEnum(vr::EVRApplicationError) override { return "not available (fake runtime)"; }
};

FakeSystem g_sys;
FakeCompositor g_comp;
FakeOverlay g_ov;
FakeInput g_in;
FakeRenderModels g_rm;
FakeSettings g_set;
FakeApplications g_app;

// --- 5. comandi --------------------------------------------------------------------

std::string Describe() {
    const Config& c = g.cfg;
    char b[600];
    std::snprintf(b, sizeof(b),
                  "%.0f Hz, %s semantics, display %.0f ms after the vsync, %ux%u per eye, fov L%.0f R%.0f U%.0f D%.0f, IPD %.1f mm, "
                  "head (%.1f, %.1f, %.1f deg; %.2f, %.2f, %.2f m), sway yaw %.1f/%.2f pitch %.1f/%.2f roll %.1f/%.2f, look %.0f deg/s, "
                  "tremor %.2f, turn %.1f deg/s, script %s, hands %s/%s, predict %s noise %.1f deg/s, hitch %.0f ms every %.1f s",
                  c.hz, c.openxr ? "openxr" : "openvr", c.latencyMs, c.w, c.h, c.fovDeg[0], c.fovDeg[1], c.fovDeg[2], c.fovDeg[3], c.ipdMm,
                  c.head.yaw, c.head.pitch, c.head.roll, c.head.pos[0], c.head.pos[1], c.head.pos[2], c.swayAmp[0], c.swayHz[0],
                  c.swayAmp[1], c.swayHz[1], c.swayAmp[2], c.swayHz[2], c.lookDps, c.tremorDeg, c.turnDps,
                  c.script.empty() ? "off" : c.scriptPath.c_str(), c.handOn[0] ? "on" : "off", c.handOn[1] ? "on" : "off",
                  c.predictExact ? "exact" : "linear", c.predNoiseDps, c.hitchMs, c.hitchEverySec);
    return b;
}

bool LoadScript(const std::string& path, bool loop, std::string* err) {
    std::string full = path;
    if (full.size() < 2 || (full[1] != ':' && full[0] != '\\' && full[0] != '/')) full = vr::control::GameDirectory() + "\\" + path;
    std::ifstream f(full);
    if (!f) {
        *err = "cannot open " + full;
        return false;
    }
    std::vector<Key> keys;
    std::string line;
    while (std::getline(f, line)) {
        size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        std::istringstream in(line);
        Key k;
        if (!(in >> k.t >> k.p.yaw >> k.p.pitch >> k.p.roll >> k.p.pos[0] >> k.p.pos[1] >> k.p.pos[2])) continue;
        if (!keys.empty() && k.t < keys.back().t) {
            *err = "times must grow (line with t=" + std::to_string(k.t) + ")";
            return false;
        }
        keys.push_back(k);
    }
    if (keys.empty()) {
        *err = "no points (lines: t yaw pitch roll x y z)";
        return false;
    }
    g.cfg.script = keys;
    g.cfg.scriptLoop = loop;
    g.cfg.scriptPath = full;
    return true;
}

void ApplyPreset(const std::string& name) {
    Config& c = g.cfg;
    for (int i = 0; i < 3; ++i) c.swayAmp[i] = c.swayHz[i] = c.swayPosCm[i] = 0.0f;
    c.swayPosHz = c.lookDps = c.tremorDeg = c.turnDps = 0.0f;
    c.script.clear();
    if (name == "sway") {
        c.swayAmp[0] = 30.0f; c.swayHz[0] = 0.3f;
        c.swayAmp[1] = 10.0f; c.swayHz[1] = 0.2f;
        c.swayAmp[2] = 4.0f;  c.swayHz[2] = 0.7f;
    } else if (name == "look") {
        c.lookDps = 40.0f;
    } else if (name == "b2") {
        // Profilo del piano del bug 2: yaw +-30 a 0,3 Hz piu' sguardo in giro (40 gradi/s rms),
        // pitch -20 +-10, roll +-4 a 0,7 Hz, tremore 0,03 gradi, posizione +-3 cm.
        c.head.pitch = -20.0f;
        c.swayAmp[0] = 30.0f; c.swayHz[0] = 0.3f;
        c.swayAmp[1] = 10.0f; c.swayHz[1] = 0.25f;
        c.swayAmp[2] = 4.0f;  c.swayHz[2] = 0.7f;
        c.lookDps = 40.0f;
        c.tremorDeg = 0.03f;
        c.swayPosCm[0] = c.swayPosCm[1] = c.swayPosCm[2] = 3.0f;
        c.swayPosHz = 0.4f;
    }
    // "still": tutto fermo sulla posa di base.
}

}  // namespace

// --- API pubblica ---------------------------------------------------------------------

void Configure(const std::string& profile) {
    std::lock_guard<std::mutex> lock(g.mutex);
    std::istringstream in(profile);
    std::string w;
    int number = 0;
    while (in >> w) {
        if (w == "openvr") g.cfg.openxr = false;
        else if (w == "openxr") g.cfg.openxr = true;
        else {
            char* e = nullptr;
            float v = std::strtof(w.c_str(), &e);
            if (e == w.c_str()) continue;
            if (number == 0 && v >= 30.0f && v <= 240.0f) g.cfg.hz = v;
            if (number == 1 && v >= 0.0f && v <= 200.0f) g.cfg.latencyMs = v;
            ++number;
        }
    }
}

bool Init(std::string* error) {
    std::lock_guard<std::mutex> lock(g.mutex);
    (void)error;
    g.running = true;
    g.t0 = g.gridT0 = QpcSec();
    g.lastK = -1;
    g.lastShownM = -1;
    g.waitPoseValid = false;
    g.haveQ = false;
    g.windowStart = g.t0;
    g.noiseT = 0.0;
    g.events.clear();
    g.overlays.clear();
    vr::log::Line("FAKE: fake VR runtime started (no headset, no SteamVR, no OpenXR runtime): " + Describe() + ".");
    return true;
}

void Shutdown() {
    std::lock_guard<std::mutex> lock(g.mutex);
    if (g.running) vr::log::Line("FAKE: fake VR runtime stopped.");
    g.running = false;
}

bool Alive() {
    std::lock_guard<std::mutex> lock(g.mutex);
    return g.running;
}

vr::IVRSystem* System() { return Alive() ? &g_sys : nullptr; }
vr::IVRCompositor* Compositor() { return Alive() ? &g_comp : nullptr; }
vr::IVROverlay* Overlay() { return Alive() ? &g_ov : nullptr; }
vr::IVRInput* Input() { return Alive() ? &g_in : nullptr; }
vr::IVRRenderModels* RenderModels() { return Alive() ? &g_rm : nullptr; }
vr::IVRSettings* Settings() { return Alive() ? &g_set : nullptr; }
vr::IVRApplications* Applications() { return Alive() ? &g_app : nullptr; }

bool OpenXrSemantics() {
    std::lock_guard<std::mutex> lock(g.mutex);
    return g.cfg.openxr;
}

float SecondsToPhotons() {
    std::lock_guard<std::mutex> lock(g.mutex);
    if (!g.running) return 0.0f;
    const double P = 1.0 / g.cfg.hz, now = QpcSec();
    if (g.cfg.openxr) {
        // Come il backend OpenXR: fotoni del frame atteso (predictedDisplayTime) meno adesso.
        double t = g.frameDisplayT - now;
        if (g.frameDisplayT <= 0.0) t = P + g.cfg.latencyMs * 0.001;
        while (t < 0.0) t += P;
        return static_cast<float>(t);
    }
    // Come SteamVR: fino al prossimo vsync piu' vsync -> fotoni.
    return static_cast<float>(P - std::fmod(now - g.gridT0, P) + g.cfg.latencyMs * 0.001);
}

void SimulateZeroPose() {
    std::lock_guard<std::mutex> lock(g.mutex);
    // Il runtime rimette l'origine sotto la testa, con l'avanti della testa (altezza dal pavimento invariata).
    Pose6 room = RoomHeadAt(g.cfg, QpcSec() - g.t0);
    g.originYaw = room.yaw;
    g.originPos[0] = room.pos[0];
    g.originPos[2] = room.pos[2];
    PushEvent(vr::VREvent_StandingZeroPoseReset);
    char b[160];
    std::snprintf(b, sizeof(b), "FAKE: runtime recenter (zero pose): origin now at the head (%.2f, %.2f m), yaw %.1f deg.",
                  room.pos[0], room.pos[2], room.yaw);
    vr::log::Line(b);
}

void SetHeadHeight(float meters) {
    std::lock_guard<std::mutex> lock(g.mutex);
    g.cfg.head.pos[1] = meters;
    for (Key& k : g.cfg.script) k.p.pos[1] = meters;
}

void NoteView(uint32_t tick, int eye, const int32_t* rot, int32_t bodyYaw, float refYawRad) {
    if (eye < 0 || eye > 1 || !rot) return;
    std::lock_guard<std::mutex> lock(g_viewMutex);
    ViewRec& v = g_views[(tick * 2 + static_cast<uint32_t>(eye)) % 64];
    v.tick = tick;
    v.eye = eye;
    for (int i = 0; i < 3; ++i) v.rot[i] = rot[i];
    v.body = bodyYaw;
    v.ref = refYawRad;
    v.valid = true;
}

void NoteSubmit(uint32_t tick, const vr::HmdMatrix34_t* renderPose, double, bool cinema) {
    std::lock_guard<std::mutex> lock(g.mutex);
    g.noteTick = tick;
    g.noteValid = renderPose != nullptr;
    if (renderPose) g.notePose = *renderPose;
    g.noteCinema = cinema;
}

void Command(const std::string& args) {
    std::istringstream in(args);
    std::string sub;
    in >> sub;
    std::lock_guard<std::mutex> lock(g.mutex);
    Config& c = g.cfg;
    auto bad = [&](const std::string& what) { vr::log::Line("FAKE: \"fake " + args + "\" not understood: " + what); };
    bool projection = false;  // proiezione degli occhi da ricalcolare nel mod
    if (sub == "hz") {
        float v = 0;
        if (in >> v && v >= 30.0f && v <= 240.0f) {
            // Nuova griglia dei vsync da adesso (gli indici della vecchia non valgono piu').
            c.hz = v;
            projection = true;
            g.gridT0 = QpcSec();
            g.lastK = -1;
            g.lastShownM = -1;
            g.haveQ = false;
        } else {
            bad("hz <30..240>");
        }
    } else if (sub == "latency") {
        float v = -1;
        if (in >> v && v >= 0.0f && v <= 200.0f) c.latencyMs = v;
        else bad("latency <ms>");
    } else if (sub == "semantics") {
        std::string v;
        in >> v;
        if (v == "openvr" || v == "openxr") c.openxr = v == "openxr";
        else bad("semantics openvr|openxr");
    } else if (sub == "size") {
        uint32_t w = 0, h = 0;
        if (in >> w >> h && w >= 640 && h >= 640 && w <= 4096 && h <= 4096) c.w = w, c.h = h;
        else bad("size <w> <h> (640..4096)");
    } else if (sub == "fov") {
        float f[4];
        if (in >> f[0] >> f[1] >> f[2] >> f[3] && f[0] < f[1] && f[3] < f[2]) {
            for (int i = 0; i < 4; ++i) c.fovDeg[i] = f[i];
            projection = true;
        } else {
            bad("fov <left> <right> <up> <down> (degrees, left and down negative)");
        }
    } else if (sub == "ipd") {
        float v = 0;
        if (in >> v && v >= 50.0f && v <= 80.0f) c.ipdMm = v, projection = true;
        else bad("ipd <mm>");
    } else if (sub == "presence") {
        std::string v;
        in >> v;
        c.present = v != "off";
    } else if (sub == "head") {
        Pose6 p;
        if (in >> p.yaw >> p.pitch >> p.roll >> p.pos[0] >> p.pos[1] >> p.pos[2]) c.head = p;
        else bad("head <yaw> <pitch> <roll> <x> <y> <z> (degrees, meters)");
    } else if (sub == "height") {
        float v = 0;
        if (in >> v && v > 0.3f && v < 2.5f) c.head.pos[1] = v;
        else bad("height <m>");
    } else if (sub == "sway") {
        float a[6];
        if (in >> a[0] >> a[1] >> a[2] >> a[3] >> a[4] >> a[5])
            for (int i = 0; i < 3; ++i) c.swayAmp[i] = a[i * 2], c.swayHz[i] = a[i * 2 + 1];
        else bad("sway <yaw deg> <Hz> <pitch deg> <Hz> <roll deg> <Hz>");
    } else if (sub == "swaypos") {
        float a[4];
        if (in >> a[0] >> a[1] >> a[2] >> a[3])
            for (int i = 0; i < 3; ++i) c.swayPosCm[i] = a[i], c.swayPosHz = a[3];
        else bad("swaypos <x cm> <y cm> <z cm> <Hz>");
    } else if (sub == "look") {
        in >> c.lookDps;
    } else if (sub == "tremor") {
        in >> c.tremorDeg;
    } else if (sub == "turn") {
        in >> c.turnDps;
    } else if (sub == "preset") {
        std::string v;
        in >> v;
        if (v == "still" || v == "sway" || v == "look" || v == "b2") ApplyPreset(v);
        else bad("preset still|sway|look|b2");
    } else if (sub == "script") {
        std::string path, loop;
        in >> path >> loop;
        std::string err;
        if (path == "off" || path.empty()) c.script.clear();
        else if (!LoadScript(path, loop == "loop", &err)) bad(err);
    } else if (sub == "hand") {
        std::string which, v;
        in >> which;
        int h = which == "left" || which == "l" ? 0 : (which == "right" || which == "r" ? 1 : -1);
        if (h < 0) {
            bad("hand left|right ...");
        } else {
            std::streampos at = in.tellg();
            in >> v;
            if (v == "off" || v == "on") {
                c.handOn[h] = v == "on";
            } else {
                in.clear();
                in.seekg(at);
                Pose6 p;
                if (in >> p.pos[0] >> p.pos[1] >> p.pos[2] >> p.yaw >> p.pitch >> p.roll) c.hand[h] = p, c.handOn[h] = true;
                else bad("hand left|right <x> <y> <z> <yaw> <pitch> <roll> | on | off");
            }
        }
    } else if (sub == "handsway") {
        std::string which;
        float deg = 0, cm = 0, hz = 0;
        in >> which >> deg >> cm >> hz;
        int h = which == "left" || which == "l" ? 0 : 1;
        c.handSwayDeg[h] = deg;
        c.handSwayCm[h] = cm;
        c.handSwayHz[h] = hz;
    } else if (sub == "predict") {
        std::string v;
        in >> v;
        c.predictExact = v == "exact";
    } else if (sub == "prednoise") {
        float v = 0, ms = 50;
        in >> v >> ms;
        c.predNoiseDps = v < 0.0f ? 0.0f : v;
        if (ms > 0.0f) c.predNoiseCorrMs = ms;
    } else if (sub == "hitch") {
        float ms = 0, every = 0;
        in >> ms >> every;
        c.hitchMs = ms < 0.0f ? 0.0f : ms;
        c.hitchEverySec = every;
    } else if (sub == "log") {
        // log <N>: JITTER ogni N frame (0 = mai); log <N> frames: righe JITTERF per i prossimi N frame;
        // log restart: finestra delle statistiche da capo (dopo un cambio, per non mescolare).
        std::string a, frames;
        in >> a >> frames;
        if (a == "restart") {
            g.mismatch.clear();
            g.step.clear();
            g.edge.clear();
            g.rpRot.clear();
            g.rpPos.clear();
            g.frames = g.posed = g.repeats = g.late = g.dropped = g.cinema = g.noView = g.overStep = 0;
            g.hitches = g.fadesToBlack = 0;
            g.haveQ = false;
            g.windowStart = QpcSec();
            return;  // nessuna riga: e' un comando dei collaudi ripetuto spesso
        }
        int n = std::atoi(a.c_str());
        if (frames == "frames") c.frameLog = n;
        else c.logEvery = n < 0 ? 0 : n;
    } else if (sub == "reset") {
        g.originYaw = 0.0f;
        g.originPos[0] = g.originPos[2] = 0.0f;
        g.t0 = g.gridT0 = QpcSec();
        g.lastK = -1;
        g.lastShownM = -1;
        g.haveQ = false;
    } else if (sub == "status" || sub.empty()) {
    } else {
        bad("unknown subcommand (hz, latency, semantics, size, fov, ipd, presence, head, height, sway, swaypos, look, tremor, "
            "turn, preset, script, hand, handsway, predict, prednoise, hitch, log, reset, status)");
        return;
    }
    if (projection) PushEvent(vr::VREvent_IpdChanged);
    vr::log::Line("FAKE: " + Describe() + ".");
}

}  // namespace vr::fake
