#include "vr_input.h"
#include "logger.h"
#include "control.h"
#include "vr_session.h"

#include <windows.h>
#include <openvr.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace vr::input {

namespace {

// Strutture XInput (senza includere xinput.h per non legarci a una versione).
struct XGamepad {
    WORD wButtons;
    BYTE bLeftTrigger;
    BYTE bRightTrigger;
    SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
};
struct XState {
    DWORD dwPacketNumber;
    XGamepad Gamepad;
};
struct XVibration {
    WORD wLeftMotorSpeed;
    WORD wRightMotorSpeed;
};

constexpr WORD kDpadUp = 0x0001, kDpadDown = 0x0002, kDpadLeft = 0x0004, kDpadRight = 0x0008;
constexpr WORD kStart = 0x0010, kBack = 0x0020, kLThumb = 0x0040, kRThumb = 0x0080;
constexpr WORD kLB = 0x0100, kRB = 0x0200, kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000;

// IAT di SpecOpsTheLine.exe: XINPUT1_3 ordinale 3 (SetState) e 2 (GetState).
constexpr uintptr_t kIatXInputSetState = 0x01109748;
constexpr uintptr_t kIatXInputGetState = 0x0110974C;

typedef DWORD(WINAPI* XInputGetState_t)(DWORD, XState*);
typedef DWORD(WINAPI* XInputSetState_t)(DWORD, XVibration*);
XInputGetState_t g_GetState_Real = nullptr;
XInputSetState_t g_SetState_Real = nullptr;
std::atomic<DWORD> g_packet{1};

std::mutex g_fakeMutex;
XGamepad g_fake = {};
ULONGLONG g_fakeUntil = 0;

SHORT AxisToShort(float v) {
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    return static_cast<SHORT>(v * 32767.0f);
}

BYTE TriggerToByte(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return static_cast<BYTE>(v * 255.0f);
}

bool ButtonPressed(const vr::VRControllerState_t& s, vr::EVRButtonId id) {
    return (s.ulButtonPressed & vr::ButtonMaskFromId(id)) != 0;
}

// Legge i due Touch tramite l'API "legacy" di OpenVR (SteamVR applica i
// binding legacy_bindings_rift: asse 0 = stick, asse 1 = grilletto,
// asse 2 = grip; A/X = k_EButton_A, B/Y = k_EButton_ApplicationMenu).
bool ReadTouchControllers(XGamepad* out) {
    vr::session::Shared vrLock(true);
    if (!vrLock.ok()) return false;
    vr::IVRSystem* sys = vr::VRSystem();
    if (!sys) return false;
    vr::TrackedDeviceIndex_t left = sys->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand);
    vr::TrackedDeviceIndex_t right = sys->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand);
    vr::VRControllerState_t ls = {}, rs = {};
    bool haveL = left != vr::k_unTrackedDeviceIndexInvalid && sys->IsTrackedDeviceConnected(left) &&
                 sys->GetControllerState(left, &ls, sizeof(ls));
    bool haveR = right != vr::k_unTrackedDeviceIndexInvalid && sys->IsTrackedDeviceConnected(right) &&
                 sys->GetControllerState(right, &rs, sizeof(rs));
    if (!haveL && !haveR) return false;
    if (vr::control::g_inputLogFrames.load() > 0) {
        static ULONGLONG last = 0;
        static uint64_t lastL = 0, lastR = 0;
        ULONGLONG now = GetTickCount64();
        bool changed = ls.ulButtonPressed != lastL || rs.ulButtonPressed != lastR;
        lastL = ls.ulButtonPressed;
        lastR = rs.ulButtonPressed;
        if (changed || now - last > 1000) {
            last = now;
            vr::control::g_inputLogFrames.fetch_sub(1);
            char b[400];
            std::snprintf(b, sizeof(b),
                          "INPUTLOG: L%s stick=(%.2f %.2f) grill=%.2f grip=%.2f tasti=%llx | R%s stick=(%.2f %.2f) grill=%.2f "
                          "grip=%.2f tasti=%llx",
                          haveL ? "" : "(assente)", ls.rAxis[0].x, ls.rAxis[0].y, ls.rAxis[1].x, ls.rAxis[2].x,
                          static_cast<unsigned long long>(ls.ulButtonPressed), haveR ? "" : "(assente)", rs.rAxis[0].x,
                          rs.rAxis[0].y, rs.rAxis[1].x, rs.rAxis[2].x, static_cast<unsigned long long>(rs.ulButtonPressed));
            vr::log::Line(b);
        }
    }

    *out = {};
    if (haveL) {
        out->sThumbLX = AxisToShort(ls.rAxis[0].x);
        out->sThumbLY = AxisToShort(ls.rAxis[0].y);
        out->bLeftTrigger = TriggerToByte(ls.rAxis[1].x);
        if (ls.rAxis[2].x > 0.6f || ButtonPressed(ls, vr::k_EButton_Grip)) out->wButtons |= kLB;
        if (ButtonPressed(ls, vr::k_EButton_A)) out->wButtons |= kX;
        if (ButtonPressed(ls, vr::k_EButton_ApplicationMenu)) out->wButtons |= kY;
        if (ButtonPressed(ls, vr::k_EButton_SteamVR_Touchpad)) out->wButtons |= kLThumb;
        if (ButtonPressed(ls, vr::k_EButton_System)) out->wButtons |= kStart;  // tasto menu sinistro
    }
    if (haveR) {
        out->sThumbRX = AxisToShort(rs.rAxis[0].x);
        out->sThumbRY = AxisToShort(rs.rAxis[0].y);
        out->bRightTrigger = TriggerToByte(rs.rAxis[1].x);
        if (rs.rAxis[2].x > 0.6f || ButtonPressed(rs, vr::k_EButton_Grip)) out->wButtons |= kRB;
        if (ButtonPressed(rs, vr::k_EButton_A)) out->wButtons |= kA;
        if (ButtonPressed(rs, vr::k_EButton_ApplicationMenu)) out->wButtons |= kB;
        if (ButtonPressed(rs, vr::k_EButton_SteamVR_Touchpad)) out->wButtons |= kRThumb;
    }
    return true;
}

// --- Gesti -------------------------------------------------------------------
//
// Pose in metri nello spazio di SteamVR (y su). Un rilevatore puro, separato
// dalla lettura dei controller, cosi' "gesturesim" puo' provarlo con pose
// finte. Ogni gesto preme per un attimo un tasto del pad (mappatura dello
// schema di input del gioco, letta il 16/10 da YGamepadInputScheme):
// LB = granata (tenuto: traiettoria, rilascio: lancio), X = ricarica (tocco),
// A = copertura / azione contestuale (tocco).
struct GestureSample {
    double t;           // secondi
    float hmd[3];       // posizione della testa
    float fwd[3];       // avanti orizzontale della testa (unitario)
    float hmdVelY;      // velocita' verticale della testa
    bool leftValid, rightValid;
    float left[3], leftVel[3];
    float right[3];
    bool rightGrip;     // grip destro premuto
};

class GestureDetector {
public:
    void Update(const GestureSample& s) {
        if (vr::control::g_gestureLogFrames.load() > 0 && s.t - m_lastLogT > 0.25) {
            m_lastLogT = s.t;
            vr::control::g_gestureLogFrames.fetch_sub(1);
            float vf = s.leftVel[0] * s.fwd[0] + s.leftVel[2] * s.fwd[2];
            float rel[3] = {s.right[0] - s.hmd[0], s.right[1] - s.hmd[1], s.right[2] - s.hmd[2]};
            char b[300];
            std::snprintf(b, sizeof(b),
                          "GESTILOG: testa y=%.2f (in piedi %.2f, vy %.2f) | sx valida=%d y=%.2f vel avanti=%.2f vy=%.2f | "
                          "dx valida=%d y-testa=%.2f davanti=%.2f grip=%d | dist mani=%.2f",
                          s.hmd[1], m_standingY, s.hmdVelY, (int)s.leftValid, s.left[1], vf, s.leftVel[1],
                          (int)s.rightValid, rel[1], rel[0] * s.fwd[0] + rel[2] * s.fwd[2], (int)s.rightGrip,
                          Dist(s.left, s.right));
            vr::log::Line(b);
        }
        // Altezza in piedi: segue subito verso l'alto, scende lentamente.
        if (!m_haveStanding || s.hmd[1] > m_standingY) {
            m_standingY = s.hmd[1];
            m_haveStanding = true;
        } else if (m_lastT > 0.0) {
            m_standingY -= 0.03f * static_cast<float>(s.t - m_lastT);
        }
        m_lastT = s.t;
        if (s.leftValid && vr::control::g_gestureGrenade && s.t > m_throwReady) {
            float vf = s.leftVel[0] * s.fwd[0] + s.leftVel[2] * s.fwd[2];
            if (vf > 3.2f && s.left[1] > s.hmd[1] - 0.35f) {
                Press(kLB, s.t, 0.30, "lancio della granata", vf);
                m_throwReady = s.t + 1.0;
            }
        }
        if (s.leftValid && s.rightValid && vr::control::g_gestureReload) {
            float d = Dist(s.left, s.right);
            if (d < 0.18f && s.left[1] < s.right[1]) m_nearMagT = s.t;
            if (s.t > m_reloadReady && s.t - m_nearMagT < 0.6 && s.leftVel[1] < -1.0f && d > 0.2f) {
                Press(kX, s.t, 0.12, "ricarica", -s.leftVel[1]);
                m_reloadReady = s.t + 1.5;
                m_nearMagT = -10.0;
            }
        }
        // Arma dalla spalla: grip destro premuto con la mano sopra la spalla,
        // all'altezza della testa e non davanti al viso -> Y (arma seguente).
        // Il grip, che altrimenti e' RB (ordini alla squadra), resta escluso
        // finche' non lo si rilascia.
        if (s.rightValid && s.rightGrip && !m_gripWasDown && vr::control::g_gestureWeapon) {
            float rel[3] = {s.right[0] - s.hmd[0], s.right[1] - s.hmd[1], s.right[2] - s.hmd[2]};
            float ahead = rel[0] * s.fwd[0] + rel[2] * s.fwd[2];
            if (rel[1] > -0.20f && ahead < 0.08f) {
                Press(kY, s.t, 0.12, "arma presa dalla spalla", 0.0f);
                m_gripIsSwap = true;
                m_hapticPending = true;
            }
        }
        if (!s.rightGrip) m_gripIsSwap = false;
        m_gripWasDown = s.rightValid && s.rightGrip;
        if (vr::control::g_gestureCover && s.t > m_duckReady && s.hmdVelY < -1.0f &&
            s.hmd[1] < m_standingY - 0.15f) {
            Press(kA, s.t, 0.12, "copertura (abbassato)", -s.hmdVelY);
            m_duckReady = s.t + 1.5;
        }
    }

    bool SuppressRightGrip() const { return m_gripIsSwap; }
    bool TakeHaptic() {
        bool h = m_hapticPending;
        m_hapticPending = false;
        return h;
    }

    WORD Buttons(double now) const {
        WORD b = 0;
        for (const auto& p : m_presses)
            if (now >= p.from && now < p.until) b |= p.button;
        return b;
    }

private:
    struct Pressed {
        WORD button;
        double from, until;
    };
    static float Dist(const float* a, const float* b) {
        float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
        return std::sqrt(x * x + y * y + z * z);
    }
    void Press(WORD button, double t, double dur, const char* what, float speed) {
        m_presses.erase(std::remove_if(m_presses.begin(), m_presses.end(),
                                       [t](const Pressed& p) { return p.until < t - 1.0; }),
                        m_presses.end());
        m_presses.push_back({button, t, t + dur});
        char b[120];
        std::snprintf(b, sizeof(b), "GESTO: %s (%.2f m/s)", what, speed);
        vr::log::Line(b);
    }
    std::vector<Pressed> m_presses;
    bool m_haveStanding = false;
    float m_standingY = 0.0f;
    bool m_gripWasDown = false, m_gripIsSwap = false, m_hapticPending = false;
    double m_lastLogT = 0.0;
    double m_lastT = 0.0, m_throwReady = 0.0, m_reloadReady = 0.0, m_duckReady = 0.0, m_nearMagT = -10.0;
};

GestureDetector g_gestures;

double NowSec() {
    static LARGE_INTEGER f = {};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
}

// Campione dalle pose reali di SteamVR (false se manca la testa).
bool ReadGestureSample(GestureSample* s) {
    vr::session::Shared vrLock(true);
    if (!vrLock.ok()) return false;
    vr::IVRSystem* sys = vr::VRSystem();
    if (!sys) return false;
    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
    sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, poses, vr::k_unMaxTrackedDeviceCount);
    const vr::TrackedDevicePose_t& h = poses[vr::k_unTrackedDeviceIndex_Hmd];
    if (!h.bPoseIsValid) return false;
    *s = {};
    s->t = NowSec();
    for (int i = 0; i < 3; ++i) s->hmd[i] = h.mDeviceToAbsoluteTracking.m[i][3];
    // Avanti della testa = -Z (colonna 2), proiettato in orizzontale.
    float fx = -h.mDeviceToAbsoluteTracking.m[0][2], fz = -h.mDeviceToAbsoluteTracking.m[2][2];
    float fl = std::sqrt(fx * fx + fz * fz);
    if (fl < 1e-3f) return false;
    s->fwd[0] = fx / fl;
    s->fwd[2] = fz / fl;
    s->hmdVelY = h.vVelocity.v[1];
    auto hand = [&](vr::ETrackedControllerRole role, float* pos, float* vel) {
        vr::TrackedDeviceIndex_t i = sys->GetTrackedDeviceIndexForControllerRole(role);
        if (i == vr::k_unTrackedDeviceIndexInvalid || i >= vr::k_unMaxTrackedDeviceCount || !poses[i].bPoseIsValid)
            return false;
        for (int k = 0; k < 3; ++k) {
            pos[k] = poses[i].mDeviceToAbsoluteTracking.m[k][3];
            if (vel) vel[k] = poses[i].vVelocity.v[k];
        }
        return true;
    };
    s->leftValid = hand(vr::TrackedControllerRole_LeftHand, s->left, s->leftVel);
    s->rightValid = hand(vr::TrackedControllerRole_RightHand, s->right, nullptr);
    vr::TrackedDeviceIndex_t ri = sys->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand);
    vr::VRControllerState_t rs = {};
    if (ri != vr::k_unTrackedDeviceIndexInvalid && sys->GetControllerState(ri, &rs, sizeof(rs)))
        s->rightGrip = rs.rAxis[2].x > 0.6f || ButtonPressed(rs, vr::k_EButton_Grip);
    if (g_gestures.TakeHaptic() && ri != vr::k_unTrackedDeviceIndexInvalid) sys->TriggerHapticPulse(ri, 0, 3000);
    return true;
}

// Pose finte per "gesturesim": testa a 1,7 m che guarda verso -Z.
void RunGestureSim(const std::string& which) {
    double t0 = NowSec();  // pose da adesso in poi: le pressioni cadono nel futuro prossimo
    vr::log::Line("GESTURESIM: " + which);
    for (int k = 0; k <= 30; ++k) {
        GestureSample s = {};
        s.t = t0 + k * 0.02;
        s.hmd[1] = 1.7f;
        s.fwd[2] = -1.0f;
        s.leftValid = s.rightValid = true;
        s.right[0] = 0.2f; s.right[1] = 1.3f; s.right[2] = -0.3f;
        s.left[0] = -0.1f; s.left[1] = 1.2f; s.left[2] = -0.2f;
        float u = k / 30.0f;
        if (which == "throw") {  // mano sinistra dietro la spalla, poi in avanti veloce
            s.left[1] = 1.6f;
            s.left[2] = 0.2f - 0.9f * u;
            s.leftVel[2] = (k > 10 && k < 25) ? -3.0f : -0.3f;
        } else if (which == "reload") {  // tocca il caricatore, poi tira giu'
            if (k < 15) {
                s.left[0] = 0.2f; s.left[1] = 1.2f; s.left[2] = -0.3f;
            } else {
                s.left[0] = 0.2f; s.left[1] = 1.2f - 0.5f * (k - 15) / 15.0f; s.left[2] = -0.3f;
                s.leftVel[1] = -1.6f;
            }
        } else if (which == "duck") {  // la testa scende di 40 cm in 0,3 s
            if (k > 15) {
                s.hmd[1] = 1.7f - 0.4f * std::fmin(1.0f, (k - 15) / 15.0f);
                s.hmdVelY = -1.4f;
            }
        } else if (which == "weapon") {  // mano destra sopra la spalla, poi grip
            s.right[0] = 0.15f; s.right[1] = 1.75f; s.right[2] = 0.1f;
            s.rightGrip = k > 10;
        } else if (which == "rest") {  // braccia lungo i fianchi, testa ferma: nessun gesto
            s.left[1] = 0.9f; s.right[1] = 0.9f;
        }
        g_gestures.Update(s);
    }
}

void ApplyGestures(XGamepad* pad) {
    std::string sim;
    if (vr::control::TakeGestureSimRequest(&sim)) RunGestureSim(sim);
    GestureSample s;
    if (ReadGestureSample(&s)) g_gestures.Update(s);
    WORD g = g_gestures.Buttons(NowSec());
    static WORD lastLogged = 0;
    if (g && g != lastLogged) {
        char b[80];
        std::snprintf(b, sizeof(b), "GESTO: tasti 0x%04X inviati al gioco", g);
        vr::log::Line(b);
    }
    lastLogged = g;
    pad->wButtons |= g;
    if (g_gestures.SuppressRightGrip()) pad->wButtons &= ~kRB;
}

bool SafeAddYaw(void* pc, int32_t delta) {
    __try {
        // AActor::Rotation a +0x60 (Pitch, Yaw, Roll), verificato sul PC il 16/10.
        *reinterpret_cast<int32_t*>(static_cast<char*>(pc) + 0x64) += delta;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Regole VR per lo stick destro (Touch o input finto): beccheggio escluso e,
// se attiva, rotazione a scatti al posto di quella continua.
// Croce direzionale (fuoco secondario, tipo di granata): tenendo premuto lo
// stick destro, la sua direzione diventa la croce (e lo stick non ruota). Un
// clic senza direzione resta il clic dello stick (lato della camera).
void ApplyDpadModifier(XGamepad* pad) {
    static bool held = false, usedAsDpad = false;
    static ULONGLONG pulseUntil = 0;
    bool click = (pad->wButtons & kRThumb) != 0;
    if (click) {
        if (!held) {
            held = true;
            usedAsDpad = false;
        }
        float x = pad->sThumbRX / 32767.0f, y = pad->sThumbRY / 32767.0f;
        WORD d = 0;
        if (x > 0.6f) d |= kDpadRight;
        if (x < -0.6f) d |= kDpadLeft;
        if (y > 0.6f) d |= kDpadUp;
        if (y < -0.6f) d |= kDpadDown;
        if (d) usedAsDpad = true;
        static WORD lastD = 0;
        if (d && d != lastD) {
            char b[64];
            std::snprintf(b, sizeof(b), "DPAD: croce 0x%04X (stick destro premuto)", d);
            vr::log::Line(b);
        }
        lastD = d;
        pad->wButtons = static_cast<WORD>((pad->wButtons & ~kRThumb) | d);
        pad->sThumbRX = pad->sThumbRY = 0;
    } else if (held) {
        held = false;
        if (!usedAsDpad) pulseUntil = GetTickCount64() + 120;  // clic semplice
    }
    if (GetTickCount64() < pulseUntil) pad->wButtons |= kRThumb;
}

// Ricentramento: i due stick premuti insieme per 1 secondo (i due clic non
// arrivano al gioco mentre si tengono entrambi).
void ApplyRecenterCombo(XGamepad* pad) {
    static ULONGLONG since = 0;
    static bool fired = false;
    bool both = (pad->wButtons & kLThumb) && (pad->wButtons & kRThumb);
    if (!both) {
        since = 0;
        fired = false;
        return;
    }
    pad->wButtons &= ~(kLThumb | kRThumb);
    ULONGLONG now = GetTickCount64();
    if (!since) since = now;
    if (!fired && now - since > 1000) {
        fired = true;
        vr::control::g_recenterRequested = true;
        vr::log::Line("RICENTRA: richiesto dai controller (due stick premuti).");
    }
}

void ApplyVrStickRules(XGamepad* pad) {
    ApplyGestures(pad);
    ApplyRecenterCombo(pad);
    ApplyDpadModifier(pad);
    // Schermo virtuale (cinematiche, veicoli, menu): lo stick destro torna
    // quello del pad (anche su/giu'), senza rotazione a scatti.
    if (vr::control::g_cinemaActive) {
        vr::control::g_motionAmount = 0.0f;
        return;
    }
    // Intensita' del movimento per la vignettatura: stick sinistro (camminata)
    // o rotazione continua con lo stick destro.
    {
        float lx = pad->sThumbLX / 32767.0f, ly = pad->sThumbLY / 32767.0f;
        float move = std::sqrt(lx * lx + ly * ly);
        float turn = vr::control::g_snapTurnDeg > 0.0f ? 0.0f : std::fabs(pad->sThumbRX / 32767.0f);
        float m = move > turn ? move : turn;
        vr::control::g_motionAmount = m < 0.2f ? 0.0f : (m > 1.0f ? 1.0f : (m - 0.2f) / 0.8f);
    }
    if (!vr::control::g_stickPitch) pad->sThumbRY = 0;
    float snap = vr::control::g_snapTurnDeg;
    if (snap <= 0.0f) return;
    static bool armed = true;
    float x = pad->sThumbRX / 32767.0f;
    pad->sThumbRX = 0;
    if (armed && (x > 0.7f || x < -0.7f)) {
        armed = false;
        void* pc = vr::control::g_playerController;
        int32_t delta = static_cast<int32_t>((x > 0 ? snap : -snap) / 360.0f * 65536.0f);
        if (pc) SafeAddYaw(pc, delta);
    } else if (x > -0.3f && x < 0.3f) {
        armed = true;
    }
}

DWORD WINAPI Hooked_XInputGetState(DWORD user, XState* state) {
    if (user == 0 && state) {
        {
            std::lock_guard<std::mutex> lock(g_fakeMutex);
            if (GetTickCount64() < g_fakeUntil) {
                state->dwPacketNumber = g_packet.fetch_add(1);
                state->Gamepad = g_fake;
                ApplyVrStickRules(&state->Gamepad);
                return ERROR_SUCCESS;
            }
        }
        XGamepad pad;
        if (ReadTouchControllers(&pad)) {
            static bool logged = false;
            if (!logged) {
                logged = true;
                vr::log::Line("INPUT: controller Touch attivi, presentati al gioco come pad Xbox 0.");
            }
            ApplyVrStickRules(&pad);
            state->dwPacketNumber = g_packet.fetch_add(1);
            state->Gamepad = pad;
            return ERROR_SUCCESS;
        }
    }
    if (user == 0) vr::control::g_motionAmount = 0.0f;  // nessun input VR: niente vignettatura
    return g_GetState_Real ? g_GetState_Real(user, state) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI Hooked_XInputSetState(DWORD user, XVibration* vib) {
    if (user == 0 && vib) {
        vr::session::Shared vrLock(true);
        vr::IVRSystem* sys = vrLock.ok() ? vr::VRSystem() : nullptr;
        if (sys && (vib->wLeftMotorSpeed || vib->wRightMotorSpeed)) {
            // L'API legacy accetta impulsi fino a ~3,9 ms; chiamata ogni frame
            // finche' il gioco tiene acceso il motore.
            auto pulse = [&](vr::ETrackedControllerRole role, WORD speed) {
                if (!speed) return;
                vr::TrackedDeviceIndex_t idx = sys->GetTrackedDeviceIndexForControllerRole(role);
                if (idx != vr::k_unTrackedDeviceIndexInvalid)
                    sys->TriggerHapticPulse(idx, 0, static_cast<unsigned short>(500 + speed / 20));
            };
            pulse(vr::TrackedControllerRole_LeftHand, vib->wLeftMotorSpeed);
            pulse(vr::TrackedControllerRole_RightHand, vib->wRightMotorSpeed);
        }
    }
    return g_SetState_Real ? g_SetState_Real(user, vib) : ERROR_SUCCESS;
}

bool PatchSlot(uintptr_t slot, const char* name, void* replacement, void** original) {
    HMODULE xinput = GetModuleHandleA("XINPUT1_3.dll");
    void** entry = reinterpret_cast<void**>(slot);
    void* current = *entry;
    HMODULE owner = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCSTR>(current), &owner);
    if (!xinput || owner != xinput) {
        vr::log::Line(std::string("INPUT: voce IAT ") + name + " inattesa, non tocco nulla.");
        return false;
    }
    DWORD old;
    if (!VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *original = current;
    *entry = replacement;
    VirtualProtect(entry, sizeof(void*), old, &old);
    return true;
}

}  // namespace

void InstallXInputHooks() {
    bool a = PatchSlot(kIatXInputGetState, "XInputGetState", reinterpret_cast<void*>(&Hooked_XInputGetState),
                       reinterpret_cast<void**>(&g_GetState_Real));
    bool b = PatchSlot(kIatXInputSetState, "XInputSetState", reinterpret_cast<void*>(&Hooked_XInputSetState),
                       reinterpret_cast<void**>(&g_SetState_Real));
    vr::log::Line(std::string("INPUT: hook XInput ") + (a && b ? "installati" : "NON installati"));
}

void SetFakeInput(float lx, float ly, float rx, float ry, float lt, float rt, unsigned buttons, unsigned durationMs) {
    std::lock_guard<std::mutex> lock(g_fakeMutex);
    g_fake.sThumbLX = AxisToShort(lx);
    g_fake.sThumbLY = AxisToShort(ly);
    g_fake.sThumbRX = AxisToShort(rx);
    g_fake.sThumbRY = AxisToShort(ry);
    g_fake.bLeftTrigger = TriggerToByte(lt);
    g_fake.bRightTrigger = TriggerToByte(rt);
    g_fake.wButtons = static_cast<WORD>(buttons);
    g_fakeUntil = GetTickCount64() + durationMs;
}

}  // namespace vr::input
