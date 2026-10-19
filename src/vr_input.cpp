#include "vr_input.h"
#include "logger.h"
#include "control.h"
#include "haptics_ext.h"
#include "vr_session.h"

#include <windows.h>
#include <openvr.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <cstring>
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

// --- SteamVR Input (18/10) ----------------------------------------------------
//
// Con l'API legacy SteamVR traduce ogni controller nel layout dei vecchi
// controller Vive: sugli Index "A" arriva come grip e X/A non esistono
// (legacy_bindings_index_controller.json), quindi copertura e ricarica
// mancavano ("my index controllers are probably missing bindings", 18/10).
// Con un manifest di azioni ogni controller ha i suoi binding predefiniti
// e il giocatore puo' cambiarli in SteamVR. Le azioni vengono poi rimesse nello
// stesso stato "legacy dei Touch" letto dal resto del codice (asse 0 = stick,
// asse 1 = grilletto, Grip, A = primario, ApplicationMenu = secondario,
// SteamVR_Touchpad = clic dello stick, System = menu sinistro).
// Il manifest e i binding li scrive la dll accanto al gioco (cartella
// specopsvr_input), cosi' arrivano anche con l'aggiornamento automatico.
namespace act {

const char* const kHand[2] = {"left", "right"};
const char* const kNames[] = {"stick", "trigger", "grip", "primary", "secondary", "stickclick", "menu"};
enum { kStick, kTrigger, kGrip, kPrimary, kSecondary, kStickClick, kMenu, kCount };

struct Bind {
    int hand;  // 0 sinistra, 1 destra, -1 entrambe
    const char* input;
    const char* mode;
    const char* key;
    int action;
};
struct Profile {
    const char* type;
    std::vector<Bind> binds;
};

const std::vector<Profile>& Profiles() {
    static const std::vector<Profile> p = {
        {"oculus_touch",
         {{-1, "joystick", "joystick", "position", kStick}, {-1, "joystick", "joystick", "click", kStickClick},
          {-1, "trigger", "trigger", "pull", kTrigger}, {-1, "grip", "trigger", "click", kGrip},
          {1, "a", "button", "click", kPrimary}, {1, "b", "button", "click", kSecondary},
          {0, "x", "button", "click", kPrimary}, {0, "y", "button", "click", kSecondary},
          {0, "system", "button", "click", kMenu}}},
        {"knuckles",
         {{-1, "thumbstick", "joystick", "position", kStick}, {-1, "thumbstick", "joystick", "click", kStickClick},
          {-1, "trigger", "trigger", "pull", kTrigger}, {-1, "grip", "trigger", "click", kGrip},
          {-1, "a", "button", "click", kPrimary}, {-1, "b", "button", "click", kSecondary},
          {0, "trackpad", "button", "click", kMenu}}},
        {"vive_controller",
         {{-1, "trackpad", "trackpad", "position", kStick}, {-1, "trackpad", "trackpad", "click", kStickClick},
          {-1, "trigger", "trigger", "pull", kTrigger}, {-1, "grip", "button", "click", kGrip},
          {0, "application_menu", "button", "click", kMenu}, {1, "application_menu", "button", "click", kPrimary}}},
        {"holographic_controller",
         {{-1, "joystick", "joystick", "position", kStick}, {-1, "joystick", "joystick", "click", kStickClick},
          {-1, "trigger", "trigger", "pull", kTrigger}, {-1, "grip", "button", "click", kGrip},
          {-1, "trackpad", "button", "click", kPrimary}, {0, "application_menu", "button", "click", kMenu},
          {1, "application_menu", "button", "click", kSecondary}}},
    };
    return p;
}

std::string ActionName(int hand, int a) {
    return std::string("/actions/main/in/") + kHand[hand] + "_" + kNames[a];
}

std::string BindingJson(const Profile& p) {
    // Una sorgente per (mano, ingresso, modo) con tutti i suoi tasti.
    struct Src {
        int hand;
        std::string input, mode, inputs;
    };
    std::vector<Src> src;
    for (const Bind& b : p.binds) {
        for (int h = 0; h < 2; ++h) {
            if (b.hand != -1 && b.hand != h) continue;
            Src* s = nullptr;
            for (Src& x : src)
                if (x.hand == h && x.input == b.input && x.mode == b.mode) s = &x;
            if (!s) {
                src.push_back({h, b.input, b.mode, ""});
                s = &src.back();
            }
            if (!s->inputs.empty()) s->inputs += ",";
            s->inputs += std::string("\"") + b.key + "\":{\"output\":\"" + ActionName(h, b.action) + "\"}";
        }
    }
    std::string j = std::string("{\"controller_type\":\"") + p.type +
                    "\",\"action_manifest_version\":1,\"name\":\"SpecOpsTheLineVR default\","
                    "\"description\":\"Spec Ops: The Line VR default bindings\",\"bindings\":{\"/actions/main\":{\"sources\":[";
    for (size_t i = 0; i < src.size(); ++i) {
        if (i) j += ",";
        j += std::string("{\"path\":\"/user/hand/") + kHand[src[i].hand] + "/input/" + src[i].input + "\",\"mode\":\"" +
             src[i].mode + "\",\"inputs\":{" + src[i].inputs + "}}";
    }
    j += "],\"haptics\":[{\"output\":\"/actions/main/out/haptic_left\",\"path\":\"/user/hand/left/output/haptic\"},"
         "{\"output\":\"/actions/main/out/haptic_right\",\"path\":\"/user/hand/right/output/haptic\"}]}}}";
    return j;
}

std::string ManifestJson() {
    static const char* const kTypes[] = {"vector2", "vector1", "boolean", "boolean", "boolean", "boolean", "boolean"};
    static const char* const kLabel[] = {"stick", "trigger", "grip", "A / X button", "B / Y button", "stick click",
                                         "pause menu"};
    std::string j = "{\"default_bindings\":[";
    for (size_t i = 0; i < Profiles().size(); ++i) {
        if (i) j += ",";
        j += std::string("{\"controller_type\":\"") + Profiles()[i].type + "\",\"binding_url\":\"bindings_" +
             Profiles()[i].type + ".json\"}";
    }
    j += "],\"actions\":[";
    std::string loc = "\"language_tag\":\"en_US\",\"/actions/main\":\"Spec Ops: The Line VR\"";
    for (int h = 0; h < 2; ++h) {
        for (int a = 0; a < kCount; ++a) {
            j += "{\"name\":\"" + ActionName(h, a) + "\",\"type\":\"" + kTypes[a] + "\"},";
            loc += ",\"" + ActionName(h, a) + "\":\"" + (h ? "Right " : "Left ") + kLabel[a] + "\"";
        }
    }
    j += "{\"name\":\"/actions/main/out/haptic_left\",\"type\":\"vibration\"},"
         "{\"name\":\"/actions/main/out/haptic_right\",\"type\":\"vibration\"}],"
         "\"action_sets\":[{\"name\":\"/actions/main\",\"usage\":\"leftright\"}],\"localization\":[{" +
         loc + ",\"/actions/main/out/haptic_left\":\"Left haptics\",\"/actions/main/out/haptic_right\":\"Right haptics\"}]}";
    return j;
}

bool WriteIfChanged(const std::string& path, const std::string& text) {
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        std::string old(static_cast<size_t>(GetFileSize(f, nullptr)), '\0');
        DWORD got = 0;
        bool same = old.size() == text.size() && ReadFile(f, &old[0], static_cast<DWORD>(old.size()), &got, nullptr) &&
                    old == text;
        CloseHandle(f);
        if (same) return true;
    }
    f = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &put, nullptr) && put == text.size();
    CloseHandle(f);
    return ok;
}

// Scrive i file e restituisce il percorso del manifest ("" se fallisce).
std::string WriteFiles() {
    char path[MAX_PATH] = {};
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&WriteFiles), &self);
    GetModuleFileNameA(self, path, MAX_PATH);
    std::string dir(path);
    dir = dir.substr(0, dir.find_last_of("\\/") + 1) + "specopsvr_input\\";
    CreateDirectoryA(dir.c_str(), nullptr);
    for (const Profile& p : Profiles())
        if (!WriteIfChanged(dir + "bindings_" + p.type + ".json", BindingJson(p))) return "";
    if (!WriteIfChanged(dir + "actions.json", ManifestJson())) return "";
    return dir + "actions.json";
}

vr::VRActionSetHandle_t g_set = vr::k_ulInvalidActionSetHandle;
vr::VRActionHandle_t g_action[2][kCount];
vr::VRActionHandle_t g_haptic[2];
unsigned g_gen = 0;      // generazione della sessione per cui valgono gli handle
bool g_ready = false;    // azioni attive in questa generazione

// Con il lock di sessione, dal thread che legge il pad. Solo in Scene (le app
// Background non ricevono input).
bool Ensure() {
    if (!vr::control::g_actionInput || !vr::session::IsScene()) return false;
    unsigned gen = vr::session::Generation();
    if (gen == g_gen) return g_ready;
    g_gen = gen;
    g_ready = false;
    vr::IVRInput* in = vr::VRInput();
    if (!in) return false;
    static std::string manifest = WriteFiles();
    if (manifest.empty()) {
        vr::log::Line("INPUT: could not write specopsvr_input\\*.json, using the legacy input API.");
        return false;
    }
    vr::EVRInputError e = in->SetActionManifestPath(manifest.c_str());
    if (e != vr::VRInputError_None) {
        vr::log::Line("INPUT: SetActionManifestPath failed (" + std::to_string(e) + "), using the legacy input API.");
        return false;
    }
    in->GetActionSetHandle("/actions/main", &g_set);
    for (int h = 0; h < 2; ++h) {
        for (int a = 0; a < kCount; ++a) in->GetActionHandle(ActionName(h, a).c_str(), &g_action[h][a]);
        in->GetActionHandle(h ? "/actions/main/out/haptic_right" : "/actions/main/out/haptic_left", &g_haptic[h]);
    }
    g_ready = true;
    vr::log::Line("INPUT: SteamVR Input actions active (" + manifest + ").");
    return true;
}

// Stato in formato "legacy dei Touch" per una mano; false se la mano non da' dati.
bool ReadHand(vr::IVRInput* in, int h, vr::VRControllerState_t* s) {
    *s = {};
    bool any = false;
    vr::InputAnalogActionData_t an = {};
    if (in->GetAnalogActionData(g_action[h][kStick], &an, sizeof(an), vr::k_ulInvalidInputValueHandle) ==
            vr::VRInputError_None && an.bActive) {
        s->rAxis[0].x = an.x;
        s->rAxis[0].y = an.y;
        any = true;
    }
    if (in->GetAnalogActionData(g_action[h][kTrigger], &an, sizeof(an), vr::k_ulInvalidInputValueHandle) ==
            vr::VRInputError_None && an.bActive) {
        s->rAxis[1].x = an.x;
        any = true;
    }
    static const vr::EVRButtonId kBit[kCount] = {vr::k_EButton_Max, vr::k_EButton_Max, vr::k_EButton_Grip, vr::k_EButton_A,
                                                 vr::k_EButton_ApplicationMenu, vr::k_EButton_SteamVR_Touchpad,
                                                 vr::k_EButton_System};
    for (int a = kGrip; a < kCount; ++a) {
        vr::InputDigitalActionData_t d = {};
        if (in->GetDigitalActionData(g_action[h][a], &d, sizeof(d), vr::k_ulInvalidInputValueHandle) !=
                vr::VRInputError_None || !d.bActive)
            continue;
        any = true;
        if (d.bState) s->ulButtonPressed |= vr::ButtonMaskFromId(kBit[a]);
    }
    return any;
}

}  // namespace act

// Ultimo stato letto dei controller (anche per i gesti, su un altro thread).
std::mutex g_handStateMutex;
vr::VRControllerState_t g_handState[2] = {};
bool g_handStateValid[2] = {false, false};

// --- Pose dei controller --------------------------------------------------------
//
// Mira e fucile sono tarati sulla posa "raw" dei Touch. La posa raw cambia da
// un controller all'altro (Vive: ~38 gradi; Index: qualche grado e qualche
// cm), mentre il punto "tip" dei modelli SteamVR indica per tutti la
// direzione in cui si punta. Correzione: raw * tip(controller) * inv(tip(Touch)).
struct Mat34 {
    float m[3][4];
};

Mat34 Mul(const Mat34& a, const Mat34& b) {
    Mat34 r = {};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 4; ++j) {
            float v = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
            if (j == 3) v += a.m[i][3];
            r.m[i][j] = v;
        }
    }
    return r;
}

Mat34 Inverse(const Mat34& a) {  // solo rotazione + traslazione
    Mat34 r = {};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i][j] = a.m[j][i];
    for (int i = 0; i < 3; ++i) r.m[i][3] = -(r.m[i][0] * a.m[0][3] + r.m[i][1] * a.m[1][3] + r.m[i][2] * a.m[2][3]);
    return r;
}

Mat34 FromHmd(const vr::HmdMatrix34_t& h) {
    Mat34 r;
    std::memcpy(r.m, h.m, sizeof(r.m));
    return r;
}

// Componente "tip" di un modello (false se il modello non lo ha o non e'
// ancora caricato: il caricamento parte qui e si riprova al frame dopo).
bool TipOf(const char* model, Mat34* out) {
    vr::IVRRenderModels* rm = vr::VRRenderModels();
    if (!rm || !model || !*model) return false;
    if (rm->GetComponentCount(model) == 0) {
        vr::RenderModel_t* m = nullptr;
        if (rm->LoadRenderModel_Async(model, &m) == vr::VRRenderModelError_None && m) rm->FreeRenderModel(m);
        if (rm->GetComponentCount(model) == 0) return false;
    }
    vr::VRControllerState_t cs = {};
    vr::RenderModel_ControllerMode_State_t mode = {};
    vr::RenderModel_ComponentState_t comp = {};
    if (!rm->GetComponentState(model, "tip", &cs, &mode, &comp)) return false;
    *out = FromHmd(comp.mTrackingToComponentLocal);
    return true;
}

struct PoseFix {
    unsigned gen = 0;
    std::string model;
    bool valid = false;
    int tries = 0;
    Mat34 fix = {};
};
PoseFix g_poseFix[2];

void UpdatePoseFix(vr::IVRSystem* sys, int h, vr::TrackedDeviceIndex_t idx) {
    PoseFix& f = g_poseFix[h];
    char model[256] = {};
    sys->GetStringTrackedDeviceProperty(idx, vr::Prop_RenderModelName_String, model, sizeof(model));
    unsigned gen = vr::session::Generation();
    if (f.gen == gen && f.model == model) return;
    if (f.model != model) f.tries = 0;
    f.gen = gen;
    f.model = model;
    f.valid = false;
    char type[64] = {};
    sys->GetStringTrackedDeviceProperty(idx, vr::Prop_ControllerType_String, type, sizeof(type));
    std::string what = std::string("INPUT: ") + act::kHand[h] + " controller " + type + " (" + model + ")";
    if (std::strncmp(model, "oculus_", 7) == 0) {  // Touch: e' il riferimento
        vr::log::Line(what + ", raw pose used.");
        return;
    }
    Mat34 tip, tipRef;
    if (!TipOf(model, &tip) || !TipOf(h ? "oculus_quest2_controller_right" : "oculus_quest2_controller_left", &tipRef)) {
        // Modelli in caricamento: si riprova (circa 5 s), poi posa raw.
        if (++f.tries < 300) f.gen = 0;  // generazione mai valida: si riprova
        else vr::log::Line(what + ": no tip pose, raw pose used.");
        return;
    }
    f.fix = Mul(tip, Inverse(tipRef));
    f.valid = true;
    float pitch = std::asin((std::max)(-1.0f, (std::min)(1.0f, f.fix.m[1][2]))) * 57.2957795f;
    float yaw = std::atan2(f.fix.m[0][2], f.fix.m[2][2]) * 57.2957795f;
    char b[200];
    std::snprintf(b, sizeof(b), ": pose aligned to Touch (%.1f deg pitch, %.1f deg yaw, offset %.1f %.1f %.1f cm).", pitch,
                  yaw, f.fix.m[0][3] * 100.0f, f.fix.m[1][3] * 100.0f, f.fix.m[2][3] * 100.0f);
    vr::log::Line(what + b);
}

// Vibrazione: il gioco chiama XInputSetState solo quando cambia il motore,
// mentre l'API legacy di SteamVR accetta impulsi di ~4 ms: l'intensita' viene
// ricordata e l'impulso ripetuto a ogni lettura del pad (ogni frame) finche'
// il motore resta acceso. Va alla mano che regge l'arma (destra) e anche alla
// sinistra quando tiene la canna; prima il motore sinistro del pad finiva
// sulla mano sinistra vuota. g_hapticUntil: conferme dei gesti (ms).
std::atomic<WORD> g_rumble[2];
std::atomic<ULONGLONG> g_hapticUntil[2];

void UpdateHaptics(vr::IVRSystem* sys) {
    ULONGLONG now = GetTickCount64();
    WORD game = g_rumble[0] > g_rumble[1] ? g_rumble[0].load() : g_rumble[1].load();
    for (int h = 0; h < 2; ++h) {
        unsigned short us = 0;
        bool gunHand = h == 1 || vr::control::g_twoHandEngaged;
        if (game && gunHand) us = static_cast<unsigned short>(600 + game / 20);
        if (now < g_hapticUntil[h]) us = 3500;
        if (!us) continue;
        if (act::g_ready) {
            vr::VRInput()->TriggerHapticVibrationAction(act::g_haptic[h], 0.0f, 0.02f, 150.0f,
                                                        us > 3900 ? 1.0f : us / 3900.0f, vr::k_ulInvalidInputValueHandle);
            continue;
        }
        vr::TrackedDeviceIndex_t idx = sys->GetTrackedDeviceIndexForControllerRole(
            h == 0 ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand);
        if (idx != vr::k_unTrackedDeviceIndexInvalid) sys->TriggerHapticPulse(idx, 0, us > 3900 ? 3900 : us);
    }
}

// Grip dei Touch: con i binding legacy di SteamVR (legacy_bindings_touch.json)
// arriva come tasto Grip e come valore analogico sulla seconda componente
// dell'asse 2 ("axis2_value1"); si accettano tutte le forme.
float GripValue(const vr::VRControllerState_t& s) {
    float v = s.rAxis[2].x > s.rAxis[2].y ? s.rAxis[2].x : s.rAxis[2].y;
    return ButtonPressed(s, vr::k_EButton_Grip) ? 1.0f : v;
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
    bool haveL = false, haveR = false;
    static double lastActionRead = 0.0;
    double nowMs = static_cast<double>(GetTickCount64());
    if (act::Ensure() && nowMs - lastActionRead < 4.0) {
        // Il gioco interroga il pad piu' volte per frame: le azioni si leggono
        // al massimo ogni 4 ms, le altre volte vale l'ultimo stato.
        std::lock_guard<std::mutex> lock(g_handStateMutex);
        ls = g_handState[0];
        rs = g_handState[1];
        haveL = g_handStateValid[0];
        haveR = g_handStateValid[1];
    } else if (act::Ensure()) {
        lastActionRead = nowMs;
        vr::IVRInput* in = vr::VRInput();
        vr::VRActiveActionSet_t set = {};
        set.ulActionSet = act::g_set;
        set.ulRestrictedToDevice = vr::k_ulInvalidInputValueHandle;
        set.ulSecondaryActionSet = vr::k_ulInvalidActionSetHandle;
        in->UpdateActionState(&set, sizeof(set), 1);
        haveL = left != vr::k_unTrackedDeviceIndexInvalid && act::ReadHand(in, 0, &ls);
        haveR = right != vr::k_unTrackedDeviceIndexInvalid && act::ReadHand(in, 1, &rs);
    } else {
        haveL = left != vr::k_unTrackedDeviceIndexInvalid && sys->IsTrackedDeviceConnected(left) &&
                sys->GetControllerState(left, &ls, sizeof(ls));
        haveR = right != vr::k_unTrackedDeviceIndexInvalid && sys->IsTrackedDeviceConnected(right) &&
                sys->GetControllerState(right, &rs, sizeof(rs));
    }
    {
        std::lock_guard<std::mutex> lock(g_handStateMutex);
        g_handState[0] = ls;
        g_handState[1] = rs;
        g_handStateValid[0] = haveL;
        g_handStateValid[1] = haveR;
    }
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
                          "INPUTLOG: L%s stick=(%.2f %.2f) trigger=%.2f grip=%.2f buttons=%llx | R%s stick=(%.2f %.2f) trigger=%.2f "
                          "grip=%.2f buttons=%llx",
                          haveL ? "" : "(missing)", ls.rAxis[0].x, ls.rAxis[0].y, ls.rAxis[1].x, GripValue(ls),
                          static_cast<unsigned long long>(ls.ulButtonPressed), haveR ? "" : "(missing)", rs.rAxis[0].x,
                          rs.rAxis[0].y, rs.rAxis[1].x, GripValue(rs), static_cast<unsigned long long>(rs.ulButtonPressed));
            vr::log::Line(b);
        }
    }

    *out = {};
    if (haveL) {
        out->sThumbLX = AxisToShort(ls.rAxis[0].x);
        out->sThumbLY = AxisToShort(ls.rAxis[0].y);
        out->bLeftTrigger = TriggerToByte(ls.rAxis[1].x);
        // Grip sinistro = granata (LB), tranne quando la mano tiene la canna.
        if (GripValue(ls) > 0.6f && !vr::control::g_twoHandEngaged) out->wButtons |= kLB;
        if (ButtonPressed(ls, vr::k_EButton_A)) out->wButtons |= kX;
        if (ButtonPressed(ls, vr::k_EButton_ApplicationMenu)) out->wButtons |= kY;
        if (ButtonPressed(ls, vr::k_EButton_SteamVR_Touchpad)) out->wButtons |= kLThumb;
        if (ButtonPressed(ls, vr::k_EButton_System)) out->wButtons |= kStart;  // tasto menu sinistro
    }
    // Calibrazione del calcio: B (destro) + Y (sinistro) tenuti insieme 1 s.
    // Mentre sono tenuti insieme non arrivano al gioco (niente corpo a corpo
    // ne' cambio d'arma per sbaglio).
    bool calibCombo = haveL && haveR && ButtonPressed(ls, vr::k_EButton_ApplicationMenu) &&
                      ButtonPressed(rs, vr::k_EButton_ApplicationMenu);
    {
        static ULONGLONG since = 0;
        static bool fired = false;
        if (calibCombo) {
            if (!since) since = GetTickCount64();
            if (!fired && GetTickCount64() - since > 1000) {
                fired = true;
                vr::control::g_stockCalibRequest = true;
            }
        } else {
            since = 0;
            fired = false;
        }
    }
    if (calibCombo) out->wButtons &= ~kY;
    if (haveR) {
        out->sThumbRX = AxisToShort(rs.rAxis[0].x);
        out->sThumbRY = AxisToShort(rs.rAxis[0].y);
        out->bRightTrigger = TriggerToByte(rs.rAxis[1].x);
        if (GripValue(rs) > 0.6f) out->wButtons |= kRB;
        if (ButtonPressed(rs, vr::k_EButton_A)) out->wButtons |= kA;
        if (ButtonPressed(rs, vr::k_EButton_ApplicationMenu) && !calibCombo) out->wButtons |= kB;
        if (ButtonPressed(rs, vr::k_EButton_SteamVR_Touchpad)) out->wButtons |= kRThumb;
    }
    UpdateHaptics(sys);  // sotto lo stesso lock di sessione
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
    float rightFwd[3], rightUp[3];  // assi dell'arma (controller destro corretto con aimpitch)
    bool rightGrip;     // grip destro premuto
    bool leftGrip;      // grip sinistro premuto
    bool reloadButton;  // X premuto (ricarica col tasto)
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
                          "GESTURELOG: head y=%.2f (standing %.2f, vy %.2f) | left valid=%d y=%.2f forward vel=%.2f vy=%.2f | "
                          "right valid=%d y-head=%.2f ahead=%.2f grip=%d | hands dist=%.2f",
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
        // Mano sinistra sulla canna (e per 1 s dopo averla lasciata): muovendo
        // il fucile la mano corre in avanti, non e' un lancio (18/10).
        if (vr::control::g_twoHandEngaged) m_twoHandT = s.t;
        if (s.leftValid && vr::control::g_gestureGrenade && s.t > m_throwReady && s.t - m_twoHandT > 1.0) {
            float vf = s.leftVel[0] * s.fwd[0] + s.leftVel[2] * s.fwd[2];
            if (vf > 3.2f && s.left[1] > s.hmd[1] - 0.20f) {  // 2,5 m/s e -0,35 m: troppo sensibile (18/10)
                Press(kLB, s.t, 0.30, "lancio della granata", vf);
                vr::haptics::GrenadeThrow();
                m_throwReady = s.t + 1.0;
                // Direzione del lancio = velocita' della mano (la granata la segue).
                float sp = std::sqrt(s.leftVel[0] * s.leftVel[0] + s.leftVel[1] * s.leftVel[1] +
                                     s.leftVel[2] * s.leftVel[2]);
                if (sp > 0.1f) {
                    for (int k = 0; k < 3; ++k) vr::control::g_throwDir[k] = s.leftVel[k] / sp;
                    vr::control::g_throwStamp = GetTickCount64();
                }
            } else if (vf > 2.8f && vr::control::g_gestureMelee && s.left[1] <= s.hmd[1] - 0.35f &&
                       s.t > m_meleeReady) {
                // Pugno in avanti all'altezza del petto (il lancio parte dall'alto): corpo a corpo.
                Press(kB, s.t, 0.12, "corpo a corpo", vf);
                vr::haptics::Melee();
                m_meleeReady = s.t + 0.8;
                m_throwReady = s.t + 0.5;
            }
        }
        UpdateReload(s);
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
    // Grip sinistro usato dalla ricarica: non arriva al gioco come LB (granata).
    bool SuppressLeftGrip() const { return m_leftBusy; }
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
    // Ricarica. "simple": mano sinistra sul caricatore e tirare giu'.
    // "physical" (predefinita), in tre tempi con conferma a vibrazione:
    //  1. grip sinistro sul caricatore (sotto l'arma) e tirarlo via: espulso;
    //  2. grip alla cintura (mano 35+ cm sotto la testa, vicina al corpo):
    //     caricatore nuovo in mano, da tenere col grip;
    //  3. portarlo sul caricatore dell'arma: inserito, il gioco ricarica (X).
    // Lasciando il grip prima dell'inserimento il caricatore cade (si torna
    // al punto 2); dopo 12 s senza finire o col tasto X si ricomincia.
    enum class Mag { In, Out, Held };
    void UpdateReload(const GestureSample& s) {
        m_leftBusy = false;
        bool gripDown = s.leftGrip && !m_leftGripWas;
        m_leftGripWas = s.leftGrip;
        if (!s.leftValid || !s.rightValid || !vr::control::g_gestureReload) {
            m_mag = Mag::In;
            m_magGrab = m_leftLatch = false;
            return;
        }
        if (!vr::control::g_reloadPhysical) {
            m_mag = Mag::In;
            float d = Dist(s.left, s.right);
            if (d < 0.18f && s.left[1] < s.right[1]) m_nearMagT = s.t;
            if (s.t > m_reloadReady && s.t - m_nearMagT < 0.6 && s.leftVel[1] < -1.0f && d > 0.2f) {
                Press(kX, s.t, 0.12, "ricarica", -s.leftVel[1]);
                vr::haptics::Reload();
                m_reloadReady = s.t + 1.5;
                m_nearMagT = -10.0;
            }
            return;
        }
        float well[3];
        float fwdM = vr::control::g_magwellCm[0] * 0.01f, downM = vr::control::g_magwellCm[1] * 0.01f;
        for (int k = 0; k < 3; ++k) well[k] = s.right[k] + s.rightFwd[k] * fwdM - s.rightUp[k] * downM;
        float dw = Dist(s.left, well);
        if (s.reloadButton && m_mag != Mag::In) {
            vr::log::Line("GESTURE: reload finished with the X button");
            m_mag = Mag::In;
        }
        if (m_mag != Mag::In && s.t - m_magT > 12.0) {
            vr::log::Line("GESTURE: physical reload not finished in 12 s, reset");
            m_mag = Mag::In;
        }
        switch (m_mag) {
        case Mag::In:
            if (dw < 0.12f) m_leftBusy = true;  // mano sul caricatore: il grip non e' una granata
            if (gripDown && dw < 0.12f && s.t > m_reloadReady) m_magGrab = true;
            if (m_magGrab) {
                m_leftBusy = true;
                if (!s.leftGrip) {
                    m_magGrab = false;
                } else if (dw > 0.18f) {
                    m_magGrab = false;
                    m_leftLatch = true;
                    m_mag = Mag::Out;
                    m_magT = s.t;
                    Pulse(0, 40);
                    Pulse(1, 40);
                    Log("GESTURE: magazine out (left hand %.0f cm from the magwell)", dw * 100.0f);
                }
            }
            break;
        case Mag::Out: {
            float below = s.hmd[1] - s.left[1];
            float hx = s.left[0] - s.hmd[0], hz = s.left[2] - s.hmd[2];
            float horiz = std::sqrt(hx * hx + hz * hz);
            // Grip premuto alla cintura, oppure tenuto dall'espulsione fino
            // all'anca (18/10: nel visore il grip restava premuto e la presa
            // non scattava mai). Tenuto serve scendere di piu', se no il
            // caricatore vecchio diventava subito quello nuovo.
            bool held = s.leftGrip && m_leftLatch;
            float need = held ? 0.55f : 0.35f;
            if ((gripDown || held) && below > need && horiz < 0.45f && dw > 0.20f) {
                m_mag = Mag::Held;
                m_magT = s.t;
                m_leftBusy = true;
                Pulse(0, 60);
                Log("GESTURE: new magazine taken (%.0f cm below the head)", below * 100.0f);
            } else if (gripDown) {
                char b[160];
                std::snprintf(b, sizeof(b),
                              "GESTURE: grip with the magazine out, not at the belt (%.0f cm below the head, %.0f cm "
                              "from it horizontally, %.0f cm from the magwell)",
                              below * 100.0f, horiz * 100.0f, dw * 100.0f);
                vr::log::Line(b);
            }
            break;
        }
        case Mag::Held:
            m_leftBusy = true;
            if (!s.leftGrip) {
                m_mag = Mag::Out;
                m_magT = s.t;
                vr::log::Line("GESTURE: magazine dropped (grip released), take a new one");
            } else if (dw < 0.07f) {
                m_mag = Mag::In;
                m_leftLatch = true;
                m_reloadReady = s.t + 1.0;
                Press(kX, s.t, 0.12, "ricarica (caricatore inserito)", 0.0f);
                vr::haptics::Reload();
                Pulse(0, 90);
                Pulse(1, 90);
            }
            break;
        }
        // Grip tenuto dopo l'espulsione o l'inserimento: resta della ricarica.
        if (m_leftLatch) {
            if (s.leftGrip) m_leftBusy = true;
            else m_leftLatch = false;
        }
    }
    static void Pulse(int hand, int ms) { g_hapticUntil[hand] = GetTickCount64() + ms; }
    static void Log(const char* fmt, float v) {
        char b[160];
        std::snprintf(b, sizeof(b), fmt, v);
        vr::log::Line(b);
    }
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
        std::snprintf(b, sizeof(b), "GESTURE: %s (%.2f m/s)", what, speed);
        vr::log::Line(b);
    }
    std::vector<Pressed> m_presses;
    bool m_haveStanding = false;
    float m_standingY = 0.0f;
    bool m_gripWasDown = false, m_gripIsSwap = false, m_hapticPending = false;
    double m_lastLogT = 0.0;
    double m_lastT = 0.0, m_throwReady = 0.0, m_reloadReady = 0.0, m_duckReady = 0.0, m_nearMagT = -10.0;
    double m_meleeReady = 0.0, m_twoHandT = -10.0;
    Mag m_mag = Mag::In;
    double m_magT = 0.0;
    bool m_magGrab = false, m_leftLatch = false, m_leftBusy = false, m_leftGripWas = false;
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
    CorrectControllerPoses(sys, poses);
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
    if (s->rightValid) {
        // Assi dell'arma: avanti = -Z del controller e su = +Y, ruotati
        // dell'angolo del calcio (aimpitch) come la mira.
        vr::TrackedDeviceIndex_t ri = sys->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand);
        const vr::HmdMatrix34_t& c = poses[ri].mDeviceToAbsoluteTracking;
        float a = vr::control::g_aimPitchOffsetDeg * 0.01745329252f;
        float ca = std::cos(a), sa = std::sin(a);
        for (int k = 0; k < 3; ++k) {
            s->rightFwd[k] = -ca * c.m[k][2] + sa * c.m[k][1];
            s->rightUp[k] = ca * c.m[k][1] + sa * c.m[k][2];
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_handStateMutex);  // letto da ReadTouchControllers
        s->rightGrip = g_handStateValid[1] && GripValue(g_handState[1]) > 0.6f;
        s->leftGrip = g_handStateValid[0] && GripValue(g_handState[0]) > 0.6f;
        s->reloadButton = g_handStateValid[0] && ButtonPressed(g_handState[0], vr::k_EButton_A);
    }
    if (g_gestures.TakeHaptic()) {
        ULONGLONG until = GetTickCount64() + 70;  // conferma del gesto su entrambe le mani
        g_hapticUntil[0] = until;
        g_hapticUntil[1] = until;
    }
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
        } else if (which == "melee") {  // pugno in avanti all'altezza del petto
            s.left[1] = 1.2f;
            s.left[2] = -0.1f - 0.5f * u;
            s.leftVel[2] = (k > 10 && k < 20) ? -3.2f : -0.2f;
        } else if (which == "reload" && vr::control::g_reloadPhysical) {
            // Fucile davanti al petto che punta a -Z: caricatore 10 cm sotto
            // la mano destra. Grip sul caricatore, via, grip alla cintura,
            // ritorno al caricatore.
            s.rightFwd[2] = -1.0f;
            s.rightUp[1] = 1.0f;
            float fw = vr::control::g_magwellCm[0] * 0.01f, dn = vr::control::g_magwellCm[1] * 0.01f;
            float well[3] = {s.right[0], s.right[1] - dn, s.right[2] - fw};
            float belt[3] = {-0.15f, 1.0f, -0.1f};
            const float* at = well;
            if (k >= 3 && k < 8) at = belt;               // estratto e portato alla cintura
            else if (k >= 8 && k < 14) at = belt;         // presa del caricatore nuovo
            for (int j = 0; j < 3; ++j) s.left[j] = at[j];
            if (k >= 14 && k < 18) for (int j = 0; j < 3; ++j) s.left[j] = 0.5f * (belt[j] + well[j]);
            s.leftGrip = (k >= 1 && k < 5) || k >= 10;
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
        std::snprintf(b, sizeof(b), "GESTURE: buttons 0x%04X sent to the game", g);
        vr::log::Line(b);
    }
    lastLogged = g;
    if (g_gestures.SuppressLeftGrip()) pad->wButtons &= ~kLB;  // prima dei gesti: il lancio resta
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
            std::snprintf(b, sizeof(b), "DPAD: 0x%04X (right stick held)", d);
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
        vr::log::Line("RECENTER: requested from the controllers (both sticks held).");
    }
}

void ApplyVrStickRules(XGamepad* pad) {
    {
        static ULONGLONG lastTurn = 0;
        if (std::abs(static_cast<int>(pad->sThumbRX)) > 5000) lastTurn = GetTickCount64();
        vr::control::g_stickTurning = GetTickCount64() - lastTurn < 300;
    }
    // Sporgersi dal riparo col corpo: LT (Walker esce per mirare).
    if (vr::control::g_coverPeek && !vr::control::g_cinemaActive) pad->bLeftTrigger = 255;
    // Arma portata all'occhio (mira con le tacche): LT.
    if (vr::control::g_adsActive && !vr::control::g_cinemaActive) pad->bLeftTrigger = 255;
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
        if (pc && SafeAddYaw(pc, delta)) vr::control::g_ownYawUnits.fetch_add(delta);
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
                vr::control::g_rightTriggerHeld = state->Gamepad.bRightTrigger > 100;
                vr::control::g_squadButtonHeld = (state->Gamepad.wButtons & kRB) != 0;
                return ERROR_SUCCESS;
            }
        }
        XGamepad pad;
        if (ReadTouchControllers(&pad)) {
            static bool logged = false;
            if (!logged) {
                logged = true;
                vr::log::Line("INPUT: Touch controllers active, shown to the game as Xbox pad 0.");
            }
            ApplyVrStickRules(&pad);
            state->dwPacketNumber = g_packet.fetch_add(1);
            state->Gamepad = pad;
            vr::control::g_rightTriggerHeld = pad.bRightTrigger > 100;
            vr::control::g_squadButtonHeld = (pad.wButtons & kRB) != 0;
            return ERROR_SUCCESS;
        }
    }
    if (user == 0) {
        vr::control::g_motionAmount = 0.0f;  // nessun input VR: niente vignettatura
        vr::control::g_rightTriggerHeld = false;
        vr::control::g_squadButtonHeld = false;
    }
    return g_GetState_Real ? g_GetState_Real(user, state) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI Hooked_XInputSetState(DWORD user, XVibration* vib) {
    if (user == 0 && vib) {
        g_rumble[0] = vib->wLeftMotorSpeed;
        g_rumble[1] = vib->wRightMotorSpeed;
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
        vr::log::Line(std::string("INPUT: IAT entry ") + name + " unexpected, leaving it alone.");
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
    vr::log::Line(std::string("INPUT: XInput hooks ") + (a && b ? "installed" : "NOT installed"));
}

void CorrectControllerPoses(vr::IVRSystem* sys, vr::TrackedDevicePose_t* poses) {
    if (!vr::control::g_poseCorrect || !sys || !poses) return;
    for (int h = 0; h < 2; ++h) {
        vr::TrackedDeviceIndex_t idx = sys->GetTrackedDeviceIndexForControllerRole(
            h ? vr::TrackedControllerRole_RightHand : vr::TrackedControllerRole_LeftHand);
        if (idx == vr::k_unTrackedDeviceIndexInvalid || idx >= vr::k_unMaxTrackedDeviceCount) continue;
        UpdatePoseFix(sys, h, idx);
        if (!g_poseFix[h].valid || !poses[idx].bPoseIsValid) continue;
        Mat34 r = Mul(FromHmd(poses[idx].mDeviceToAbsoluteTracking), g_poseFix[h].fix);
        std::memcpy(poses[idx].mDeviceToAbsoluteTracking.m, r.m, sizeof(r.m));
    }
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
