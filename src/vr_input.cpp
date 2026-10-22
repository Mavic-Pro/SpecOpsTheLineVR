#include "vr_input.h"
#include "logger.h"
#include "control.h"
#include "haptics_ext.h"
#include "vr_session.h"
#include "vr_menu.h"
#include "hooks.h"
#include "vr_output.h"

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
// dpadleft/dpadright: D-pad esplicito (fuoco secondario, tipo di granata) per
// i controller senza stick, dove il modificatore "stick premuto + direzione"
// e' scomodo (Vive wand: il clic centrale e le zone sono lo stesso trackpad, L28).
const char* const kNames[] = {"stick", "trigger", "grip", "primary", "secondary", "stickclick", "menu", "dpadleft", "dpadright"};
enum { kStick, kTrigger, kGrip, kPrimary, kSecondary, kStickClick, kMenu, kDpadL, kDpadR, kCount };

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
        // Index: grip come presa vera ("grab": soglia con isteresi del
        // sensore di forza), non come grilletto (audit 19/10).
        {"knuckles",
         {{-1, "thumbstick", "joystick", "position", kStick}, {-1, "thumbstick", "joystick", "click", kStickClick},
          {-1, "trigger", "trigger", "pull", kTrigger}, {-1, "grip", "grab", "grab", kGrip},
          {-1, "a", "button", "click", kPrimary}, {-1, "b", "button", "click", kSecondary},
          {0, "trackpad", "button", "click", kMenu}}},
        // Vive: trackpad diviso in zone col clic (sotto = A/X, sopra = B/Y,
        // centro = clic dello stick): prima mancavano B, X e Y (audit 19/10).
        {"vive_controller",
         {{-1, "trackpad", "trackpad", "position", kStick}, {-1, "trackpad", "dpad", "center", kStickClick},
          {-1, "trackpad", "dpad", "south", kPrimary}, {-1, "trackpad", "dpad", "north", kSecondary},
          // Destro: zone est/ovest = D-pad destra/sinistra (L28).
          {1, "trackpad", "dpad", "west", kDpadL}, {1, "trackpad", "dpad", "east", kDpadR},
          {-1, "trigger", "trigger", "pull", kTrigger}, {-1, "grip", "button", "click", kGrip},
          {0, "application_menu", "button", "click", kMenu}, {1, "application_menu", "button", "click", kPrimary}}},
        {"vive_cosmos_controller",
         {{-1, "joystick", "joystick", "position", kStick}, {-1, "joystick", "joystick", "click", kStickClick},
          {-1, "trigger", "trigger", "pull", kTrigger}, {-1, "grip", "button", "click", kGrip},
          {1, "a", "button", "click", kPrimary}, {1, "b", "button", "click", kSecondary},
          {0, "x", "button", "click", kPrimary}, {0, "y", "button", "click", kSecondary},
          {0, "application_menu", "button", "click", kMenu}}},
        {"hpmotioncontroller",
         {{-1, "joystick", "joystick", "position", kStick}, {-1, "joystick", "joystick", "click", kStickClick},
          {-1, "trigger", "trigger", "pull", kTrigger}, {-1, "grip", "trigger", "click", kGrip},
          {1, "a", "button", "click", kPrimary}, {1, "b", "button", "click", kSecondary},
          {0, "x", "button", "click", kPrimary}, {0, "y", "button", "click", kSecondary},
          {0, "application_menu", "button", "click", kMenu}}},
        {"holographic_controller",
         {{-1, "joystick", "joystick", "position", kStick}, {-1, "joystick", "joystick", "click", kStickClick},
          {-1, "trigger", "trigger", "pull", kTrigger}, {-1, "grip", "button", "click", kGrip},
          // Sinistro: trackpad in zone come sul Vive (sotto = X, sopra = Y):
          // prima Y (arma successiva, calibrazione B+Y) non si poteva premere.
          {1, "trackpad", "button", "click", kPrimary}, {0, "trackpad", "dpad", "south", kPrimary},
          {0, "trackpad", "dpad", "north", kSecondary}, {0, "application_menu", "button", "click", kMenu},
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
        // sticky: la zona e' quella del momento del clic (scivolando col
        // pollice dal centro verso un bordo non si premono B o A, L28).
        std::string params = src[i].mode == "dpad" ? ",\"parameters\":{\"sub_mode\":\"click\",\"sticky\":\"true\"}" : "";
        j += std::string("{\"path\":\"/user/hand/") + kHand[src[i].hand] + "/input/" + src[i].input + "\",\"mode\":\"" +
             src[i].mode + "\"" + params + ",\"inputs\":{" + src[i].inputs + "}}";
    }
    j += "],\"haptics\":[{\"output\":\"/actions/main/out/haptic_left\",\"path\":\"/user/hand/left/output/haptic\"},"
         "{\"output\":\"/actions/main/out/haptic_right\",\"path\":\"/user/hand/right/output/haptic\"}]}}}";
    return j;
}

std::string ManifestJson() {
    static const char* const kTypes[] = {"vector2", "vector1", "boolean", "boolean", "boolean", "boolean", "boolean",
                                         "boolean", "boolean"};
    static const char* const kLabel[] = {"stick", "trigger", "grip", "A / X button", "B / Y button", "stick click",
                                         "pause menu", "D-pad left", "D-pad right"};
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
    (void)path;
    // Scrittura dei file tramite la cartella del gioco sicura in ANSI (vedi
    // vr::control::ModuleDirectory: nome corto 8.3 se serve).
    std::string dir = vr::control::GameDirectory() + "\\specopsvr_input\\";
    CreateDirectoryA(dir.c_str(), nullptr);
    for (const Profile& p : Profiles())
        if (!WriteIfChanged(dir + "bindings_" + p.type + ".json", BindingJson(p))) return "";
    if (!WriteIfChanged(dir + "actions.json", ManifestJson())) return "";
    // SteamVR vuole il percorso del manifesto in UTF-8: in ANSI una cartella
    // con lettere accentate faceva fallire SetActionManifestPath e si passava
    // all'input legacy (audit 20/10).
    wchar_t wpath[MAX_PATH] = {};
    GetModuleFileNameW(self, wpath, MAX_PATH);
    std::wstring wdir(wpath);
    wdir = wdir.substr(0, wdir.find_last_of(L"\\/") + 1) + L"specopsvr_input\\actions.json";
    int n = WideCharToMultiByte(CP_UTF8, 0, wdir.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, wdir.c_str(), -1, &utf8[0], n, nullptr, nullptr);
    return utf8.empty() ? dir + "actions.json" : utf8;
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
                                                 vr::k_EButton_System, vr::k_EButton_DPad_Left, vr::k_EButton_DPad_Right};
    for (int a = kGrip; a < kCount; ++a) {
        vr::InputDigitalActionData_t d = {};
        if (in->GetDigitalActionData(g_action[h][a], &d, sizeof(d), vr::k_ulInvalidInputValueHandle) !=
                vr::VRInputError_None || !d.bActive)
            continue;
        any = true;
        if (d.bState) s->ulButtonPressed |= vr::ButtonMaskFromId(kBit[a]);
    }
    // Vive: A/B/X/Y sono zone del trackpad, che e' anche lo stick; il clic su
    // una zona faceva anche camminare (avanti con Y, indietro con X) e
    // bloccava il gesto della copertura. Coi tasti veri (Touch, Index) il
    // pollice non puo' stare sullo stick e sul tasto insieme: azzerare lo
    // stick non toglie nulla (audit 20/10).
    if (s->ulButtonPressed & (vr::ButtonMaskFromId(vr::k_EButton_A) | vr::ButtonMaskFromId(vr::k_EButton_ApplicationMenu) |
                              vr::ButtonMaskFromId(vr::k_EButton_DPad_Left) | vr::ButtonMaskFromId(vr::k_EButton_DPad_Right)))
        s->rAxis[0].x = s->rAxis[0].y = 0.0f;
    return any;
}

}  // namespace act

// Passaggio automatico all'API legacy (nessuna azione attiva): si riprova.
bool g_autoLegacy = false;

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
        bool gunHand = h == 1 || vr::control::g_twoHandEngaged || vr::control::g_leftOnGun;
        if (game && gunHand) us = static_cast<unsigned short>(600 + game / 20);
        if (now < g_hapticUntil[h]) us = 3500;
        if (!us) continue;
        if (act::g_ready) {
            vr::VRInput()->TriggerHapticVibrationAction(act::g_haptic[vr::control::PhysicalHand(h)], 0.0f, 0.02f, 150.0f,
                                                        us > 3900 ? 1.0f : us / 3900.0f, vr::k_ulInvalidInputValueHandle);
            continue;
        }
        vr::TrackedDeviceIndex_t idx = sys->GetTrackedDeviceIndexForControllerRole(
            h == 0 ? vr::control::SupportRole() : vr::control::GunRole());
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
double NowSec();

bool ReadTouchControllers(XGamepad* out) {
    vr::session::Shared vrLock(true);
    if (!vrLock.ok()) return false;
    vr::IVRSystem* sys = vr::VRSystem();
    if (!sys) return false;
    vr::TrackedDeviceIndex_t left = sys->GetTrackedDeviceIndexForControllerRole(vr::control::SupportRole());
    vr::TrackedDeviceIndex_t right = sys->GetTrackedDeviceIndexForControllerRole(vr::control::GunRole());
    // Ruolo assegnato a un dispositivo che non e' un controller (Virtual
    // Desktop espone una tastiera virtuale come tracker, 21/10: stick e tasti
    // sinistri sempre a zero): si cerca il controller vero non usato
    // dall'altra mano. Ogni cambio di dispositivo per ruolo va nel log.
    auto realController = [&](vr::TrackedDeviceIndex_t idx, vr::TrackedDeviceIndex_t other) {
        if (idx != vr::k_unTrackedDeviceIndexInvalid && sys->GetTrackedDeviceClass(idx) == vr::TrackedDeviceClass_Controller)
            return idx;
        for (vr::TrackedDeviceIndex_t i = 0; i < vr::k_unMaxTrackedDeviceCount; ++i)
            if (i != other && sys->GetTrackedDeviceClass(i) == vr::TrackedDeviceClass_Controller && sys->IsTrackedDeviceConnected(i))
                return i;
        return idx;
    };
    left = realController(left, right);
    right = realController(right, left);
    {
        static vr::TrackedDeviceIndex_t lastL = 0xFFFF, lastR = 0xFFFF;
        if (left != lastL || right != lastR) {
            lastL = left;
            lastR = right;
            auto describe = [&](vr::TrackedDeviceIndex_t i) {
                if (i == vr::k_unTrackedDeviceIndexInvalid) return std::string("none");
                char model[64] = {};
                sys->GetStringTrackedDeviceProperty(i, vr::Prop_ModelNumber_String, model, sizeof(model));
                return std::to_string(i) + " (class " + std::to_string(static_cast<int>(sys->GetTrackedDeviceClass(i))) + ", " +
                       model + ")";
            };
            vr::log::Line("INPUT: hand devices: left " + describe(left) + ", right " + describe(right));
        }
    }
    vr::VRControllerState_t ls = {}, rs = {};
    bool haveL = false, haveR = false;
    static double lastActionRead = 0.0;
    // QPC: GetTickCount64 avanza a passi di 10-16 ms e lo stato restava vecchio
    // fino a un frame intero (audit 19/10).
    double nowMs = NowSec() * 1000.0;
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
        haveL = left != vr::k_unTrackedDeviceIndexInvalid && act::ReadHand(in, vr::control::PhysicalHand(0), &ls);
        haveR = right != vr::k_unTrackedDeviceIndexInvalid && act::ReadHand(in, vr::control::PhysicalHand(1), &rs);
        // Una mano con le azioni ferme mentre l'altra funziona (ruolo SteamVR
        // sbagliato): quella mano si legge con l'API legacy dal controller
        // vero trovato sopra, cosi' stick e tasti arrivano comunque.
        {
            static ULONGLONG oneSince = 0;
            static bool hybridLogged = false;
            bool oneMissing = (haveL != haveR) && left != vr::k_unTrackedDeviceIndexInvalid &&
                              right != vr::k_unTrackedDeviceIndexInvalid;
            if (oneMissing) {
                if (!oneSince) oneSince = GetTickCount64();
                if (GetTickCount64() - oneSince > 2000) {
                    vr::VRControllerState_t st = {};
                    vr::TrackedDeviceIndex_t idx = haveL ? right : left;
                    if (sys->GetControllerState(idx, &st, sizeof(st))) {
                        if (haveL) { rs = st; haveR = true; } else { ls = st; haveL = true; }
                        if (!hybridLogged) {
                            hybridLogged = true;
                            vr::log::Line(std::string("INPUT: ") + (haveL && idx == left ? "left" : "right") +
                                          " hand actions inactive for 2 s: that hand is read with the legacy API from device " +
                                          std::to_string(idx) + ".");
                        }
                    }
                }
            } else {
                oneSince = 0;
                hybridLogged = false;
            }
        }
        // Controller collegati ma nessuna azione attiva per 5 s (fuori dalla
        // dashboard): tipo di controller senza binding predefinito. Si passa
        // all'API legacy invece di restare senza comandi (audit 19/10).
        // Non mentre il menu del mod e' aperto o un'altra app ha il focus
        // dell'input: il menu e' un overlay interattivo, SteamVR (vrcompositor)
        // prende il focus e le azioni della scena restano inattive di
        // proposito; il passaggio al legacy in quel momento era definitivo
        // (22/10 su Link: pad morto nel menu principale).
        static ULONGLONG noneSince = 0;
        bool connected = left != vr::k_unTrackedDeviceIndexInvalid && right != vr::k_unTrackedDeviceIndexInvalid &&
                         sys->IsTrackedDeviceConnected(left) && sys->IsTrackedDeviceConnected(right);
        if (!haveL && !haveR && connected && !vr::control::g_vrDashboard && !vr::menu::IsOpen() && sys->IsInputAvailable() &&
            vr::session::HmdPresent()) {
            if (!noneSince) noneSince = GetTickCount64();
            if (GetTickCount64() - noneSince > 5000) {
                char type[64] = {};
                sys->GetStringTrackedDeviceProperty(right, vr::Prop_ControllerType_String, type, sizeof(type));
                vr::log::Line(std::string("INPUT: no SteamVR Input bindings active for controller type '") + type +
                              "', using the legacy input API.");
                vr::control::g_actionInput = false;
                g_autoLegacy = true;
                noneSince = 0;
            }
        } else {
            noneSince = 0;
        }
    } else {
        // Passati da soli all'API legacy: ogni 3 s si riprova con le azioni
        // (il menu di sistema di Meta o di Virtual Desktop nasconde i
        // controller senza eventi della dashboard, e il passaggio era per
        // sempre; audit 20/10).
        static ULONGLONG lastProbe = 0;
        // act::Ensure() rifa' gli handle se la sessione e' ripartita (dopo un
        // riavvio di SteamVR la sonda usava handle vecchi, L27).
        if (g_autoLegacy && vr::session::IsScene() && GetTickCount64() - lastProbe > 3000 && act::Ensure()) {
            lastProbe = GetTickCount64();
            vr::IVRInput* in = vr::VRInput();
            vr::VRControllerState_t probe;
            if (in) {
                vr::VRActiveActionSet_t set = {};
                set.ulActionSet = act::g_set;
                set.ulRestrictedToDevice = vr::k_ulInvalidInputValueHandle;
                set.ulSecondaryActionSet = vr::k_ulInvalidActionSetHandle;
                in->UpdateActionState(&set, sizeof(set), 1);
                if (act::ReadHand(in, 0, &probe) || act::ReadHand(in, 1, &probe)) {
                    g_autoLegacy = false;
                    vr::control::g_actionInput = true;
                    vr::log::Line("INPUT: SteamVR Input actions active again, back from the legacy input API.");
                }
            }
        }
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
        // Grip sinistro = granata (LB). Se la pressione appartiene all'arma o
        // alla ricarica lo decide il rilevatore dei gesti alla pressione
        // (SuppressLeftGrip in ApplyGestures).
        if (GripValue(ls) > 0.6f) out->wButtons |= kLB;
        if (ButtonPressed(ls, vr::k_EButton_A)) out->wButtons |= kX;
        if (ButtonPressed(ls, vr::k_EButton_ApplicationMenu)) out->wButtons |= kY;
        if (ButtonPressed(ls, vr::k_EButton_SteamVR_Touchpad)) out->wButtons |= kLThumb;
    }
    // Tasto menu sinistro: tocco = Start (pausa), tenuto 0,6 s = Back
    // (obiettivi), che prima non si poteva premere (audit 19/10). Il tasto e'
    // sempre quello del controller sinistro fisico: con "hand left" ls e' il
    // destro, e i mancini restavano senza pausa (audit 20/10). Con la
    // dashboard aperta (o appena chiusa) non conta: il rilascio del tasto che
    // l'ha chiusa mandava Start al gioco.
    {
        const vr::VRControllerState_t& menuHand = vr::control::g_leftHanded ? rs : ls;
        bool menuValid = vr::control::g_leftHanded ? haveR : haveL;
        static ULONGLONG since = 0, pulseUntil = 0, dashboardSeen = 0;
        static WORD pulse = 0;
        static bool backSent = false;
        ULONGLONG now = GetTickCount64();
        if (vr::control::g_vrDashboard || !menuValid) {
            dashboardSeen = now;
            since = 0;
            backSent = true;  // il rilascio successivo non manda Start
        } else if (ButtonPressed(menuHand, vr::k_EButton_System)) {
            if (!since && now - dashboardSeen > 300) since = now;
            if (since && !backSent && now - since >= 600) {
                backSent = true;
                pulse = kBack;
                pulseUntil = now + 150;
            }
        } else {
            if (since && !backSent) {
                pulse = kStart;
                pulseUntil = now + 120;
            }
            since = 0;
            backSent = false;
        }
        if (now < pulseUntil) out->wButtons |= pulse;
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
        // D-pad esplicito (azioni dpadleft/dpadright, Vive wand: zone est/ovest del trackpad destro, L28).
        if (ButtonPressed(rs, vr::k_EButton_DPad_Left)) out->wButtons |= kDpadLeft;
        if (ButtonPressed(rs, vr::k_EButton_DPad_Right)) out->wButtons |= kDpadRight;
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
    float right[3], rightVel[3];
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
        bool onGun = vr::control::g_twoHandEngaged || vr::control::g_leftOnGun;
        if (onGun) m_twoHandT = s.t;
        m_onGun = onGun;
        // Mano sinistra vicina all'arma (entro 20 cm dalla sua linea, fino a 45 cm
        // davanti alla destra): afferrare l'astina con uno scatto in avanti non
        // e' un pugno ne' un lancio (audit 20/10).
        bool nearGunNow = false;
        if (s.leftValid && s.rightValid) {
            float d[3] = {s.left[0] - s.right[0], s.left[1] - s.right[1], s.left[2] - s.right[2]};
            float t = d[0] * s.rightFwd[0] + d[1] * s.rightFwd[1] + d[2] * s.rightFwd[2];
            t = t < -0.05f ? -0.05f : (t > 0.45f ? 0.45f : t);
            float p[3] = {d[0] - t * s.rightFwd[0], d[1] - t * s.rightFwd[1], d[2] - t * s.rightFwd[2]};
            nearGunNow = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]) < 0.20f;
        }
        // Arma cambiata (o persa: morte): la ricarica fisica riparte da capo.
        {
            uint32_t w = vr::control::g_weaponPtr;
            if (w != m_weapon) {
                if (m_mag != Mag::In) vr::log::Line("GESTURE: weapon changed, physical reload reset");
                m_weapon = w;
                m_mag = Mag::In;
                m_magGrab = false;
            }
        }
        // Il significato del grip sinistro si decide alla pressione e resta
        // fino al rilascio: granata (LB) solo se la mano non e' sull'arma (astina
        // o caricatore, entro 15 cm dalla linea dell'arma fino a 45 cm davanti
        // alla destra) e la ricarica non e' in corso. Prima la soppressione
        // cambiava a meta' pressione e il gioco vedeva un rilascio: granata
        // lanciata (audit 19/10).
        if (s.leftGrip && !m_lbGripWas) {
            bool nearGun = false;
            if (s.leftValid && s.rightValid) {
                float d[3] = {s.left[0] - s.right[0], s.left[1] - s.right[1], s.left[2] - s.right[2]};
                float t = d[0] * s.rightFwd[0] + d[1] * s.rightFwd[1] + d[2] * s.rightFwd[2];
                t = t < -0.05f ? -0.05f : (t > 0.45f ? 0.45f : t);
                float p[3] = {d[0] - t * s.rightFwd[0], d[1] - t * s.rightFwd[1], d[2] - t * s.rightFwd[2]};
                nearGun = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]) < 0.15f;
            }
            m_lbAllowed = !(onGun || nearGun || m_mag != Mag::In || m_magGrab || m_leftLatch || !Gameplay());
            if (!m_lbAllowed) vr::log::Line(nearGun || onGun ? "GESTURE: left grip on the gun, not a grenade"
                                                              : "GESTURE: left grip kept for the reload / menu");
        }
        if (!s.leftGrip) m_lbAllowed = false;
        m_lbGripWas = s.leftGrip;
        // Niente gesti in cinematiche, scene bloccate e menu di pausa: un pugno
        // era B (indietro nel menu), abbassarsi A (conferma), un lancio LB.
        if (!Gameplay()) {
            m_gripWasDown = s.rightValid && s.rightGrip;
            m_leftGripWas = s.leftGrip;
            // Ricarica fisica interrotta da scena, menu o mossa del gioco:
            // si ricomincia (col caricatore "fuori" il grilletto restava
            // bloccato e il timeout non correva, M15).
            if (m_mag != Mag::In) {
                vr::log::Line("GESTURE: physical reload reset (cinematic, menu or game move)");
                m_mag = Mag::In;
                m_magGrab = m_leftLatch = false;
            }
            return;
        }
        // Direzione del lancio = velocita' media della mano nei 60 ms dopo il
        // riconoscimento (prima un solo campione, all'inizio del movimento;
        // audit 20/10). Il gioco lancia al rilascio di LB (0,3 s dopo).
        if (s.t < m_throwSampleUntil && s.leftValid) {
            for (int k = 0; k < 3; ++k) m_throwVel[k] += s.leftVel[k];
            ++m_throwSamples;
            float sp = std::sqrt(m_throwVel[0] * m_throwVel[0] + m_throwVel[1] * m_throwVel[1] +
                                 m_throwVel[2] * m_throwVel[2]);
            if (sp > 0.1f) {
                for (int k = 0; k < 3; ++k) vr::control::g_throwDir[k] = m_throwVel[k] / sp;
                // Velocita' media della mano (m/s): scala la velocita' della
                // granata nel reindirizzamento (R-W6).
                vr::control::g_throwSpeed = sp / static_cast<float>(m_throwSamples);
                vr::control::g_throwStamp = GetTickCount64();
            }
        }
        float vf = s.leftVel[0] * s.fwd[0] + s.leftVel[2] * s.fwd[2];
        bool leftFree = s.leftValid && s.t - m_twoHandT > 1.0 && !nearGunNow;
        // Lancio armato: finche' il grip resta premuto (traiettoria) la
        // direzione resta valida, e al rilascio (il gioco lancia allora) il
        // momento viene aggiornato: prima dopo 2 s dal gesto il lancio non
        // veniva piu' reindirizzato (L21).
        if (m_throwArmed) {
            if (s.leftValid && s.leftGrip) vr::control::g_throwStamp = GetTickCount64();
            else {
                m_throwArmed = false;
                vr::control::g_throwStamp = GetTickCount64();
            }
        }
        if (leftFree && vr::control::g_gestureGrenade && s.t > m_throwReady && vf > 3.2f &&
            s.left[1] > s.hmd[1] - 0.20f) {  // 2,5 m/s e -0,35 m: troppo sensibile (18/10)
            Press(kLB, s.t, 0.30, "lancio della granata", vf);
            vr::haptics::GrenadeThrow();
            m_throwReady = s.t + 1.0;
            m_throwArmed = true;
            for (int k = 0; k < 3; ++k) m_throwVel[k] = 0.0f;
            m_throwSamples = 0;
            vr::control::g_throwSpeed = 0.0f;
            m_throwSampleUntil = s.t + 0.06;
        } else if (leftFree && s.t > m_throwReady) {
            // Il corpo a corpo non dipende piu' dal gesto della granata
            // ("gesture grenade off" spegneva anche il pugno; audit 20/10).
            if (vf > 2.8f && vr::control::g_gestureMelee && s.left[1] <= s.hmd[1] - 0.35f &&
                       s.t > m_meleeReady) {
                // Pugno in avanti all'altezza del petto (il lancio parte dall'alto): corpo a corpo.
                Press(kB, s.t, 0.12, "corpo a corpo", vf);
                vr::haptics::Melee();
                m_meleeReady = s.t + 0.8;
                m_throwReady = s.t + 0.5;
            }
        }
        // Colpo con l'arma (R-I3): mano destra che scatta in avanti lungo la
        // canna (colpo di calcio) o giu' (colpo dall'alto) oltre 3,5 m/s, con
        // l'arma sotto l'altezza degli occhi; non in mira (ADS: arma all'occhio)
        // e non durante la ricarica fisica. "gesture melee off" spegne anche questo.
        // Opzionali ("gesture gunmelee on"): in gioco scattavano girando l'arma
        // in fretta (3 falsi in una partita, 21/10) e ogni colpo blocca la
        // camminata per l'animazione; soglia 5 m/s.
        if (s.rightValid && vr::control::g_gestureGunMelee && s.t > m_meleeReady && m_mag == Mag::In && !m_magGrab) {
            float along = s.rightVel[0] * s.rightFwd[0] + s.rightVel[1] * s.rightFwd[1] + s.rightVel[2] * s.rightFwd[2];
            float down = -s.rightVel[1];
            bool belowEyes = s.right[1] < s.hmd[1] - 0.08f;
            if (belowEyes && (along > 5.0f || down > 5.0f)) {
                Press(kB, s.t, 0.12, along > 3.5f ? "colpo di calcio" : "colpo dall'alto", along > 3.5f ? along : down);
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
            float side = std::fabs(-rel[0] * s.fwd[2] + rel[2] * s.fwd[0]);
            // Dietro il piano del viso e di lato alla testa: con 8 cm davanti
            // una pistola alla guancia o la posizione di guardia cambiavano arma.
            if (rel[1] > -0.15f && ahead < -0.02f && side > 0.08f) {
                Press(kY, s.t, 0.12, "arma presa dalla spalla", 0.0f);
                m_gripIsSwap = true;
                m_hapticPending = true;
            } else if (rel[1] < -0.55f && rel[1] > -1.0f && side > 0.12f && ahead < 0.15f && !m_magGrab &&
                       m_mag == Mag::In) {
                // Fondina al fianco (R-W5): grip con la mano all'altezza
                // dell'anca (55-100 cm sotto la testa), di lato e non davanti:
                // stessa azione della spalla (il gioco ha due armi, Y le alterna).
                Press(kY, s.t, 0.12, "arma presa dalla fondina", 0.0f);
                m_gripIsSwap = true;
                m_hapticPending = true;
            }
        }
        if (!s.rightGrip) m_gripIsSwap = false;
        m_gripWasDown = s.rightValid && s.rightGrip;
        // Abbassarsi = copertura solo fuori copertura e con lo stick fermo: in
        // copertura A con lo stick scavalca il riparo, A con lo stick corre.
        // Non durante la ricarica fisica (chinarsi al caricatore alla cintura
        // mandava A; audit 20/10).
        bool reloading = m_mag != Mag::In || m_magGrab || m_leftLatch || s.t - m_reloadActiveT < 1.5;
        if (vr::control::g_gestureCover && s.t > m_duckReady && s.hmdVelY < -1.0f &&
            s.hmd[1] < m_standingY - 0.15f && !vr::control::g_inCover && !m_stickMoving && !reloading) {
            Press(kA, s.t, 0.12, "copertura (abbassato)", -s.hmdVelY);
            m_duckReady = s.t + 1.5;
        }
    }

    bool SuppressRightGrip() const { return m_gripIsSwap; }
    // Ricarica onesta: col caricatore fuori dall'arma (espulso o nuovo in mano)
    // il grilletto non spara ("honestreload off" per il comportamento di prima).
    bool MagazineOut() const { return m_mag != Mag::In; }
    // Caricatore fisicamente fuori dall'arma (estratto o in mano): per nasconderne l'osso (R-W7).
    bool MagazineRemoved() const { return m_mag == Mag::Out || m_mag == Mag::Held; }
    // Pressione di un tasto da fuori (grabuse): stessa coda dei gesti.
    void PressButton(WORD button, double t, double dur, const char* what) { Press(button, t, dur, what, 0.0f); }
    // Grip sinistro dell'arma o della ricarica: non arriva al gioco come LB
    // (granata). Deciso alla pressione (Update).
    bool SuppressLeftGrip() const { return !m_lbAllowed; }
    void SetStickMoving(bool moving) { m_stickMoving = moving; }
    static bool Gameplay() {
        return !vr::control::g_cinemaActive && !vr::control::g_gameLocked && !vr::control::g_gamePaused &&
               !vr::control::g_vrDashboard;
    }
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
    // Chamber: caricatore inserito ma arma da armare ("manualaction on", R-W3):
    // sinistra che afferra vicino al castello e tira indietro di 8 cm.
    enum class Mag { In, Out, Held, Chamber };
    bool m_rackGrab = false;
    float m_rackStart[3] = {0.0f, 0.0f, 0.0f};
    void UpdateReload(const GestureSample& s) {
        m_leftBusy = false;
        if (m_mag != Mag::In || m_magGrab || m_leftLatch) m_reloadActiveT = s.t;
        bool gripDown = s.leftGrip && !m_leftGripWas;
        m_leftGripWas = s.leftGrip;
        if (!vr::control::g_gestureReload) {
            m_mag = Mag::In;
            m_magGrab = m_leftLatch = false;
            return;
        }
        if (!s.leftValid || !s.rightValid) {
            // Posa persa per un attimo (mano coperta): lo stato resta com'e'.
            // Prima si tornava a "caricatore dentro" al primo campione non
            // valido: grilletto sbloccato a meta' ricarica e X ripetuta alla
            // presa successiva (M14). Oltre 12 s senza finire si ricomincia.
            if (m_mag != Mag::In && s.t - m_magT > 12.0) {
                vr::log::Line("GESTURE: physical reload not finished in 12 s (controller lost), reset");
                m_mag = Mag::In;
                m_magGrab = m_leftLatch = false;
            } else if (m_mag != Mag::In && !m_poseLostLogged) {
                vr::log::Line("GESTURE: controller lost during the physical reload, state kept");
            }
            m_poseLostLogged = m_mag != Mag::In;
            return;
        }
        m_poseLostLogged = false;
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
        float fwdM = (vr::control::g_magwellCm[0] + vr::control::g_magwellClassCm[0]) * 0.01f, downM = (vr::control::g_magwellCm[1] + vr::control::g_magwellClassCm[1]) * 0.01f;
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
            // Con la pistola a due mani la sinistra stringe l'impugnatura, a pochi
            // cm dal caricatore: quella presa non e' un'espulsione (audit 20/10).
            if (gripDown && dw < 0.12f && s.t > m_reloadReady && !m_onGun) m_magGrab = true;
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
                    // La ricarica del gioco (X) parte gia' qui e corre mentre si
                    // prende e si inserisce il caricatore nuovo: prima partiva
                    // all'inserimento e il tempo di ricarica raddoppiava (audit 19/10).
                    Press(kX, s.t, 0.12, "ricarica (caricatore espulso)", 0.0f);
                    m_ejectAmmo = vr::control::g_weaponAmmo[1];
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
        case Mag::Chamber: {
            // Castello: 10 cm davanti alla mano destra lungo la canna, 3 cm sopra.
            float rc[3];
            for (int j = 0; j < 3; ++j) rc[j] = s.right[j] + s.rightFwd[j] * 0.10f + s.rightUp[j] * 0.03f;
            float dr = s.leftValid && s.rightValid ? Dist(s.left, rc) : 9.0f;
            if (dr < 0.15f) m_leftBusy = true;
            if (!m_rackGrab && gripDown && dr < 0.15f) {
                m_rackGrab = true;
                for (int j = 0; j < 3; ++j) m_rackStart[j] = s.left[j];
            }
            if (m_rackGrab) {
                m_leftBusy = true;
                if (!s.leftGrip) {
                    m_rackGrab = false;
                } else {
                    float back = 0.0f;  // indietro lungo la canna (contro rightFwd)
                    for (int j = 0; j < 3; ++j) back -= (s.left[j] - m_rackStart[j]) * s.rightFwd[j];
                    if (back > 0.08f) {
                        m_rackGrab = false;
                        m_mag = Mag::In;
                        m_leftLatch = true;
                        m_reloadReady = s.t + 0.5;
                        Pulse(0, 60);
                        Pulse(1, 60);
                        Log("GESTURE: action racked (%.0f cm back)", back * 100.0f);
                    }
                }
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
                vr::log::Line("GESTURE: magazine inserted");
                if (vr::control::g_manualAction) {
                    // Azione manuale: niente fuoco finche' non si arma (R-W3).
                    m_mag = Mag::Chamber;
                    m_magT = s.t;
                    m_rackGrab = false;
                    vr::log::Line("GESTURE: rack the action (grip the receiver with the left hand and pull back)");
                }
                // Il gioco puo' aver ignorato la X dell'espulsione (corsa, cambio
                // d'arma, mossa speciale): caricatore non risalito = X di nuovo.
                {
                    int32_t ammoNow = vr::control::g_weaponAmmo[1];
                    if (m_ejectAmmo >= 0 && ammoNow >= 0 && ammoNow <= m_ejectAmmo)
                        Press(kX, s.t, 0.12, "ricarica (X ripetuta all'inserimento)", 0.0f);
                }
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
    bool m_lbAllowed = false, m_lbGripWas = false, m_stickMoving = false;
    bool m_onGun = false;
    bool m_poseLostLogged = false;
    bool m_throwArmed = false;
    uint32_t m_weapon = 0;
    int32_t m_ejectAmmo = -1;
    double m_reloadActiveT = -10.0, m_throwSampleUntil = 0.0;
    float m_throwVel[3] = {0.0f, 0.0f, 0.0f};
    int m_throwSamples = 0;
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
    s->leftValid = hand(vr::control::SupportRole(), s->left, s->leftVel);
    s->rightValid = hand(vr::control::GunRole(), s->right, s->rightVel);
    if (s->rightValid) {
        // Assi dell'arma: avanti = -Z del controller e su = +Y, ruotati
        // dell'angolo del calcio (aimpitch) come la mira.
        vr::TrackedDeviceIndex_t ri = sys->GetTrackedDeviceIndexForControllerRole(vr::control::GunRole());
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
// Un campione (k = 0..30) per chiamata: la simulazione avanza di un campione
// ogni due tick (vedi ApplyGestures), come una mano vera, cosi' gli stati
// che durano nel tempo (caricatore fuori, R-W7) si vedono davvero. Prima i
// 31 campioni passavano tutti in un tick.
GestureSample g_simLast = {};  // ultimo campione simulato (grip letti anche da grabuse)
void RunGestureSimStep(const std::string& which, int k) {
    {
        GestureSample s = {};
        s.t = NowSec();
        s.hmd[1] = 1.7f;
        s.fwd[2] = -1.0f;
        s.leftValid = s.rightValid = true;
        s.right[0] = 0.2f; s.right[1] = 1.3f; s.right[2] = -0.3f;
        s.left[0] = -0.1f; s.left[1] = 1.2f; s.left[2] = -0.2f;
        float u = k / 30.0f;
        if (which == "throw") {  // mano sinistra dietro la spalla, poi in avanti veloce
            s.left[1] = 1.6f;
            s.left[2] = 0.2f - 0.9f * u;
            s.leftVel[2] = (k > 10 && k < 25) ? -4.0f : -0.3f;  // soglia 3,2 m/s dal 18/10
        } else if (which == "melee") {  // pugno in avanti all'altezza del petto
            s.left[1] = 1.2f;
            s.left[2] = -0.1f - 0.5f * u;
            s.leftVel[2] = (k > 10 && k < 20) ? -3.2f : -0.2f;
        } else if (which == "jab") {  // colpo di calcio: arma in avanti veloce (R-I3)
            s.rightFwd[2] = -1.0f;
            s.rightUp[1] = 1.0f;
            s.right[1] = 1.3f;
            s.right[2] = -0.2f - 0.4f * u;
            s.rightVel[2] = (k > 10 && k < 20) ? -4.0f : -0.2f;
        } else if (which == "strike") {  // colpo dall'alto: arma giu' veloce
            s.rightFwd[2] = -1.0f;
            s.rightUp[1] = 1.0f;
            s.right[1] = 1.4f - 0.5f * u;
            s.rightVel[1] = (k > 10 && k < 20) ? -4.0f : -0.2f;
        } else if ((which == "reload" || which == "reloadlost") && vr::control::g_reloadPhysical) {
            // "reloadlost": come "reload", ma con la mano sinistra non
            // tracciata per 3 campioni dopo l'espulsione (M14: lo stato resta).
            if (which == "reloadlost" && k >= 5 && k < 8) s.leftValid = false;
            // Fucile davanti al petto che punta a -Z: caricatore 10 cm sotto
            // la mano destra. Grip sul caricatore, via, grip alla cintura,
            // ritorno al caricatore.
            s.rightFwd[2] = -1.0f;
            s.rightUp[1] = 1.0f;
            float fw = (vr::control::g_magwellCm[0] + vr::control::g_magwellClassCm[0]) * 0.01f, dn = (vr::control::g_magwellCm[1] + vr::control::g_magwellClassCm[1]) * 0.01f;
            float well[3] = {s.right[0], s.right[1] - dn, s.right[2] - fw};
            float belt[3] = {-0.15f, 1.0f, -0.1f};
            const float* at = well;
            if (k >= 3 && k < 8) at = belt;               // estratto e portato alla cintura
            else if (k >= 8 && k < 14) at = belt;         // presa del caricatore nuovo
            for (int j = 0; j < 3; ++j) s.left[j] = at[j];
            if (k >= 14 && k < 18) for (int j = 0; j < 3; ++j) s.left[j] = 0.5f * (belt[j] + well[j]);
            s.leftGrip = (k >= 1 && k < 5) || k >= 10;
            // Con "manualaction on": dopo l'inserimento la sinistra afferra il
            // castello (10 cm avanti, 3 cm sopra la destra) e tira indietro 12 cm.
            if (k >= 19) {
                float rc[3] = {s.right[0], s.right[1] + 0.03f, s.right[2] - 0.10f};
                float pull = k >= 23 ? 0.12f * std::fmin(1.0f, (k - 23) / 5.0f) : 0.0f;
                s.left[0] = rc[0];
                s.left[1] = rc[1];
                s.left[2] = rc[2] + pull;
                s.leftGrip = k >= 20;
            }
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
        } else if (which == "holster") {  // mano destra all'anca, di lato, poi grip (R-W5)
            s.right[0] = 0.25f; s.right[1] = 1.0f; s.right[2] = 0.0f;
            s.rightGrip = k > 10;
        } else if (which == "grab") {  // mani ferme, grip sinistro chiuso a meta' (con interactsim left = usa)
            s.left[1] = 0.9f; s.right[1] = 0.9f;
            s.leftGrip = k > 10;
        }
        g_simLast = s;
        g_gestures.Update(s);
    }
}

// Ritorna i tasti dei gesti: si aggiungono dopo GateCombo (vedi sotto).
WORD ApplyGestures(XGamepad* pad) {
    static std::string simWhich;
    static int simK = -1, simTick = 0;
    std::string sim;
    if (vr::control::TakeGestureSimRequest(&sim)) {
        vr::log::Line("GESTURESIM: " + sim);
        simWhich = sim;
        simK = 0;
        simTick = 0;
    }
    {
        float lx = pad->sThumbLX / 32767.0f, ly = pad->sThumbLY / 32767.0f;
        g_gestures.SetStickMoving(lx * lx + ly * ly > 0.3f * 0.3f);
    }
    GestureSample s = {};
    if (simK >= 0) {
        if (simTick++ % 2 == 0) RunGestureSimStep(simWhich, simK++);
        if (simK > 30) simK = -1;
        s = g_simLast;  // grip e validita' dal campione simulato (grabuse, LB)
    } else if (ReadGestureSample(&s)) {
        g_gestures.Update(s);
    }
    // Fase 2 (grabuse): grip che si chiude con la mano nella zona di presa di
    // un'interazione (corda, zipline, torretta) = A, il tasto d'uso del gioco;
    // quel grip non vale come granata (LB) o ordine (RB).
    {
        static bool gripWas[2] = {false, false};
        bool grip[2] = {s.rightGrip, s.leftGrip};  // 0 destra, 1 sinistra come g_interactZone
        for (int h = 0; h < 2; ++h) {
            bool zone = vr::control::g_grabUse && GestureDetector::Gameplay() &&
                        (vr::control::g_interactZone[h].load() || GetTickCount64() < vr::control::g_interactSimUntil[h].load());
            if (zone && grip[h]) {
                pad->wButtons &= static_cast<WORD>(~(h ? kLB : kRB));
                if (!gripWas[h]) {
                    g_gestures.PressButton(kA, NowSec(), 0.15, h ? "presa con la sinistra (usa)" : "presa con la destra (usa)");
                    vr::haptics::GrenadeThrow();
                }
            }
            gripWas[h] = grip[h];
        }
    }
    WORD g = g_gestures.Buttons(NowSec());
    static WORD lastLogged = 0;
    if (g && g != lastLogged) {
        char b[80];
        std::snprintf(b, sizeof(b), "GESTURE: buttons 0x%04X sent to the game", g);
        vr::log::Line(b);
    }
    lastLogged = g;
    // Prima dei gesti: il lancio col gesto resta. Senza controller sinistro
    // (pad vero o input finto) il grip non c'e' e LB passa com'e'.
    if (g_gestures.SuppressLeftGrip() && s.leftValid) pad->wButtons &= ~kLB;
    if (g_gestures.SuppressRightGrip()) pad->wButtons &= ~kRB;
    if (vr::control::g_honestReload && g_gestures.MagazineOut() && GestureDetector::Gameplay()) pad->bRightTrigger = 0;
    return g;
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
// rawClick: stick destro premuto davvero (prima del cancello delle
// combinazioni, che lo trattiene 120 ms: nel frattempo l'inclinazione
// passava come rotazione e una pressione-e-spinta rapida perdeva la
// direzione, M13). combo: i due stick insieme (ricentramento), niente clic.
void ApplyDpadModifier(XGamepad* pad, bool rawClick, bool combo) {
    static bool held = false, usedAsDpad = false, stickLocked = false;
    static ULONGLONG pulseUntil = 0, suppressUntil = 0;
    bool click = rawClick;
    // Finche' e' premuto (e per 150 ms dopo un uso come croce) il clic non
    // arriva al gioco, nemmeno quello lasciato passare dal cancello.
    if (click || GetTickCount64() < suppressUntil) pad->wButtons &= ~kRThumb;
    // Dopo la croce lo stick resta inclinato per un attimo oltre il rilascio
    // del clic: finche' non torna al centro non ruota (prima partiva una
    // rotazione a scatti o continua; audit 20/10).
    if (stickLocked && !click) {
        float x = pad->sThumbRX / 32767.0f, y = pad->sThumbRY / 32767.0f;
        if (x * x + y * y < 0.3f * 0.3f) stickLocked = false;
        else pad->sThumbRX = pad->sThumbRY = 0;
    }
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
        if (!usedAsDpad && !combo) pulseUntil = GetTickCount64() + 120;  // clic semplice
        else {
            suppressUntil = GetTickCount64() + 150;
            stickLocked = true;
            pad->sThumbRX = pad->sThumbRY = 0;
        }
    }
    if (GetTickCount64() < pulseUntil) pad->wButtons |= kRThumb;
}

// Tasti che formano una combinazione (B+Y calibrazione del calcio, i due
// stick = ricentramento): ognuno e' trattenuto 120 ms dalla pressione. Se
// arriva l'altro, la pressione appartiene alla combinazione e nessuno dei due
// va al gioco finche' non sono rilasciati entrambi; un tocco piu' breve
// arriva al gioco come impulso di 100 ms. Prima il primo tasto premuto
// passava (corpo a corpo o cambio d'arma calibrando, clic dello stick
// ricentrando; audit 19/10).
// Al rilascio, una pressione che non e' mai passata (e non era una
// combinazione) arriva sempre come impulso: prima solo se piu' breve di
// 120 ms misurati, e con GetTickCount64 a scatti di ~16 ms le pressioni di
// ~105-135 ms non arrivavano mai al gioco (corpo a corpo ed esecuzioni con B
// persi, gesto del pugno = 120 ms esatti; audit 20/10).
struct ComboGate {
    ULONGLONG since[2] = {0, 0}, pulseUntil[2] = {0, 0};
    bool passed[2] = {false, false};
    bool combo = false;
};
void GateCombo(XGamepad* pad, WORD a, WORD b, ComboGate& g) {
    const WORD keys[2] = {a, b};
    ULONGLONG now = GetTickCount64();
    bool down[2] = {(pad->wButtons & a) != 0, (pad->wButtons & b) != 0};
    if (down[0] && down[1]) g.combo = true;
    if (!down[0] && !down[1]) g.combo = false;
    for (int k = 0; k < 2; ++k) {
        if (!down[k]) {
            if (g.since[k] && !g.combo && !g.passed[k]) g.pulseUntil[k] = now + 100;  // tocco breve
            g.since[k] = 0;
            g.passed[k] = false;
        } else if (!g.since[k]) {
            g.since[k] = now;
        }
        bool pass = down[k] && !g.combo && now - g.since[k] >= 120;
        if (pass) g.passed[k] = true;
        if (!pass) pad->wButtons &= ~keys[k];
        if (!g.combo && now < g.pulseUntil[k]) pad->wButtons |= keys[k];
    }
}

// Ricentramento: i due stick premuti insieme per 1 secondo (i clic non
// arrivano al gioco: GateCombo).
void ApplyRecenterCombo(XGamepad* pad) {
    static ULONGLONG since = 0;
    static bool fired = false, heightFired = false;
    bool both = (pad->wButtons & kLThumb) && (pad->wButtons & kRThumb);
    if (!both) {
        since = 0;
        fired = false;
        heightFired = false;
        return;
    }
    ULONGLONG now = GetTickCount64();
    if (!since) since = now;
    if (!fired && now - since > 1000) {
        fired = true;
        vr::control::g_recenterSilent = false;
        vr::control::g_recenterRequested = true;
        vr::log::Line("RECENTER: requested from the controllers (both sticks held).");
    }
    // Tenuti 3 s: anche l'altezza in piedi (R-B6).
    if (!heightFired && now - since > 3000) {
        heightFired = true;
        vr::control::g_heightCalibRequested = true;
        vr::log::Line("HEIGHT: standing-height calibration requested (both sticks held 3 s).");
    }
}

void ApplyVrStickRules(XGamepad* pad) {
    {
        // Dashboard di SteamVR aperta: Start per 150 ms (pausa del gioco), se
        // il gioco non e' gia' in pausa; poi nessun input finche' resta aperta.
        // Solo durante il gameplay in 3D: SteamVR apre la dashboard da sola
        // all'avvio del gioco (visto con Virtual Desktop, 20/10) e Start nel
        // menu principale ne attivava una voce.
        static ULONGLONG pauseUntil = 0;
        bool stereoNow = GetTickCount64() - vr::control::g_lastStereoDrawMs < 1500;
        if (vr::control::g_vrPauseRequest.exchange(false) && !vr::control::g_gamePaused &&
            !vr::control::g_cinemaActive && stereoNow)
            pauseUntil = GetTickCount64() + 150;
        if (GetTickCount64() < pauseUntil) {
            *pad = {};
            pad->wButtons = kStart;
            return;
        }
        if (vr::control::g_vrDashboard || vr::menu::IsOpen()) {  // dashboard o menu del mod: pad neutro
            *pad = {};
            return;
        }
    }
    {
        static ULONGLONG lastTurn = 0;
        if (std::abs(static_cast<int>(pad->sThumbRX)) > 5000) lastTurn = GetTickCount64();
        vr::control::g_stickTurning = GetTickCount64() - lastTurn < 300;
    }
    // Menu di pausa come lo schermo virtuale: niente regole del gameplay (LT
    // forzato, stick ruotato con la testa, rotazioni dietro il menu; audit 20/10).
    bool menu = vr::control::g_cinemaActive || vr::control::g_gamePaused;
    // Sporgersi dal riparo col corpo: LT (Walker esce per mirare).
    if (vr::control::g_coverPeek && !menu) pad->bLeftTrigger = 255;
    // RPG: spara solo mirando, LT insieme al grilletto.
    if (vr::control::g_weaponNeedsAim && pad->bRightTrigger > 30 && !menu) pad->bLeftTrigger = 255;
    // Arma portata all'occhio (mira con le tacche): LT.
    if (vr::control::g_adsActive && !menu) pad->bLeftTrigger = 255;
    WORD gestureButtons = ApplyGestures(pad);
    {
        // Intenzione di corpo a corpo (aiuto alle esecuzioni, hooks.cpp): al
        // fronte di salita di B, non nel menu di pausa ne' nella combinazione
        // B+Y della calibrazione (L15).
        static bool bWas = false;
        bool b = ((pad->wButtons | gestureButtons) & kB) != 0;
        if (b && !bWas && !menu && !(pad->wButtons & kY)) vr::control::g_meleeIntentMs = GetTickCount64();
        bWas = b;
    }
    {
        // Il B del gesto (pugno) arriva al gioco un sondaggio dopo (~1 tick):
        // l'aiuto alle esecuzioni gira il corpo nel Draw di questo tick, e il
        // gioco legge B gia' girato verso il nemico a terra. Prima B passava
        // nello stesso tick e il colpo partiva nella direzione vecchia (M3).
        // Il B fisico passa dal cancello B+Y (120 ms): e' gia' in ritardo.
        static WORD gestureBHeld = 0;
        WORD gestureB = gestureButtons & kB;
        gestureButtons = (gestureButtons & ~kB) | gestureBHeld;
        gestureBHeld = gestureB;
    }
    ApplyRecenterCombo(pad);
    bool rThumbRaw = (pad->wButtons & kRThumb) != 0;
    static ComboGate sticks, calib;
    GateCombo(pad, kLThumb, kRThumb, sticks);
    GateCombo(pad, kB, kY, calib);
    // Gesti dopo il cancello: arrivano subito e per tutta la loro durata.
    pad->wButtons |= gestureButtons;
    ApplyDpadModifier(pad, rThumbRaw, sticks.combo);
    // Schermo virtuale (cinematiche, veicoli, menu): lo stick destro torna
    // quello del pad (anche su/giu'), senza rotazione a scatti.
    if (menu) {
        vr::control::g_motionAmount = 0.0f;
        return;
    }
    // Stick sinistro verso dove si guarda: prima andava verso il corpo, fino
    // a 45 gradi (turnwithhead) di lato rispetto allo sguardo (audit 19/10).
    // In copertura no: li' lo stick muove lungo il riparo.
    if (vr::control::g_moveWithHead && !vr::control::g_inCover && !vr::control::g_gameLocked) {
        int32_t dirUnits = vr::control::g_moveDirHand && vr::control::g_leftYawValid ? vr::control::g_leftYawUnits.load()
                                                                                      : vr::control::g_headYawUnits.load();
        float a = dirUnits * (6.28318530718f / 65536.0f);
        float lx = pad->sThumbLX / 32767.0f, ly = pad->sThumbLY / 32767.0f;
        float fwd = ly * std::cos(a) - lx * std::sin(a);
        float rt = ly * std::sin(a) + lx * std::cos(a);
        auto toShort = [](float v) {
            v = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
            return static_cast<SHORT>(v * 32767.0f);
        };
        pad->sThumbLX = toShort(rt);
        pad->sThumbLY = toShort(fwd);
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
    // Su una torretta mirata con lo stick ("turretaim stick") il su/giu' serve (L18).
    bool turretStick = vr::hooks::OnTurret() && !vr::control::g_turretAimHand;
    if (!vr::control::g_stickPitch && !turretStick) pad->sThumbRY = 0;
    float snap = vr::control::g_snapTurnDeg;
    // Scene e mosse animate dal gioco (corda, scavalcare, scene scriptate): il
    // corpo lo muove l'animazione, una rotazione nostra lo porterebbe fuori posa.
    if (vr::control::g_gameLocked) {
        pad->sThumbRX = 0;
        return;
    }
    if (snap <= 0.0f) {
        // Rotazione continua fatta dalla mod a velocita' costante (turnspeed,
        // gradi/s, curva quadratica dopo la zona morta): quella del gioco ha
        // accelerazione e cambia sensibilita' con LT tenuto (copertura, ADS).
        // Essendo "nostra" (g_ownYawUnits) non viene presa per una rotazione
        // imposta dal gioco. turnspeed 0 = rotazione del gioco.
        float speed = vr::control::g_turnSpeedDeg;
        if (speed <= 0.0f) return;
        static double last = 0.0;
        double now = NowSec();
        float dt = last > 0.0 ? static_cast<float>(now - last) : 0.0f;
        last = now;
        if (dt > 0.05f) dt = 0.05f;
        float x = pad->sThumbRX / 32767.0f;
        pad->sThumbRX = 0;
        float ax = std::fabs(x);
        if (ax < 0.15f || dt <= 0.0f) return;
        float k = (ax - 0.15f) / 0.85f;
        static float carry = 0.0f;  // frazioni di unita' tra una lettura e l'altra
        carry += (x > 0 ? 1.0f : -1.0f) * k * k * speed * dt / 360.0f * 65536.0f;
        int32_t delta = static_cast<int32_t>(carry);
        carry -= static_cast<float>(delta);
        void* pc = vr::hooks::PlayerControllerAlive();  // verificato vivo (H4)
        if (delta && pc && SafeAddYaw(pc, delta)) {
            vr::control::g_ownYawUnits.fetch_add(delta);
            vr::control::g_stickYawUnits.fetch_add(delta);
        }
        return;
    }
    static bool armed = true;
    float x = pad->sThumbRX / 32767.0f;
    pad->sThumbRX = 0;
    if (armed && (x > 0.7f || x < -0.7f)) {
        armed = false;
        void* pc = vr::hooks::PlayerControllerAlive();  // verificato vivo (H4)
        int32_t delta = static_cast<int32_t>((x > 0 ? snap : -snap) / 360.0f * 65536.0f);
        if (pc && SafeAddYaw(pc, delta)) {
            vr::control::g_ownYawUnits.fetch_add(delta);
            vr::control::g_stickYawUnits.fetch_add(delta);
            if (vr::control::g_snapFade) vr::output::Blink(0.12f);
        }
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
    DWORD r = g_GetState_Real ? g_GetState_Real(user, state) : ERROR_DEVICE_NOT_CONNECTED;
    // Sessione VR attiva ma controller senza dati (dashboard di SteamVR,
    // controller spenti un attimo): pad neutro ma collegato, invece del pad
    // scollegato che il gioco vede se non c'e' un pad vero (audit 19/10).
    // Anche senza sessione VR (SteamVR chiuso o ripartito): il motore, vista
    // una lettura fallita, smette di interrogare il pad 0 (Ghidra
    // FUN_006d3340, M16) e i controller non tornavano piu'.
    if (user == 0 && state && r != ERROR_SUCCESS) {
        XGamepad pad = {};
        ApplyVrStickRules(&pad);  // anche la pausa per la dashboard
        state->dwPacketNumber = g_packet.fetch_add(1);
        state->Gamepad = pad;
        return ERROR_SUCCESS;
    }
    return r;
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

unsigned PadPolls() { return g_packet.load(); }
std::string WriteActionFiles() { return act::WriteFiles(); }
bool MagazineRemoved() { return g_gestures.MagazineRemoved(); }

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

void PulseHand(int hand, unsigned ms) {
    if (hand < 0 || hand > 1) return;
    g_hapticUntil[hand] = GetTickCount64() + ms;
}

}  // namespace vr::input
