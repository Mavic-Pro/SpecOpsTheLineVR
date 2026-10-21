#include "control.h"
#include "default_ini.h"
#include "logger.h"
#include "pure_utils.h"
#include "vr_toast.h"
#include "vr_menu.h"
#include "vr_input.h"
#include "haptics_ext.h"
#include "display_spoof.h"

#include <windows.h>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <cctype>
#include <set>
#include <vector>
#include <mutex>
#include <array>
#include <map>
#include "script_call.h"
#include <sstream>

namespace vr::control {

std::atomic<int> g_mode{kModeHmd};
std::atomic<bool> g_recenterRequested{true};
std::atomic<bool> g_recenterSilent{true};
std::atomic<bool> g_uiHookGameViewport{false};
std::atomic<bool> g_uiHookUiScene{false};
std::atomic<bool> g_ui169{false};
std::atomic<bool> g_menu169{false};  // il primo (avvio) e quelli da evento sono silenziosi
std::atomic<bool> g_dumpCameraRequested{false};
std::atomic<float> g_synthYawAmpDeg{30.0f};
std::atomic<float> g_synthPitchAmpDeg{0.0f};
std::atomic<float> g_synthPeriodSec{4.0f};
std::atomic<int> g_yawSign{1};
std::atomic<int> g_pitchSign{1};
std::atomic<bool> g_disablePostFx{true};
std::atomic<bool> g_vrOutput{true};
std::atomic<bool> g_vrOutputForce{false};
std::atomic<int> g_mirrorRequest{-1};
std::atomic<int> g_monitorEvery{1};
std::atomic<bool> g_mirrorWin{false};
std::atomic<bool> g_cinemaEnabled{true};
std::atomic<bool> g_cinemaActive{false};
std::atomic<bool> g_cinemaForce{false};
std::atomic<float> g_cinemaWidthM{3.2f}, g_cinemaDistM{2.6f};
std::atomic<int> g_outputTest{0};
std::atomic<float> g_outputScale{1.0f};
std::atomic<bool> g_submitWithPose{true};
std::atomic<bool> g_pairCheck{false};
std::atomic<bool> g_commandFollowsGun{true};
std::atomic<float> g_turnWithHeadDeg{45.0f};
std::atomic<int32_t> g_ownYawUnits{0};
std::atomic<bool> g_stickTurning{false};
std::atomic<bool> g_coverSnapCompensate{true};
std::atomic<bool> g_coverPeekEnabled{true};
std::atomic<bool> g_coverPeek{false};
std::atomic<bool> g_turretAimHand{true};
std::atomic<bool> g_adsEnabled{true};
std::atomic<bool> g_adsActive{false};
std::atomic<bool> g_twoHandEngaged{false};
std::atomic<bool> g_leftOnGun{false};
std::atomic<bool> g_headSampleOnDraw{true};
std::atomic<float> g_turnSpeedDeg{150.0f};
std::atomic<bool> g_moveWithHead{true};
std::atomic<bool> g_moveDirHand{false};
std::atomic<int32_t> g_leftYawUnits{0};
std::atomic<bool> g_leftYawValid{false};
std::atomic<bool> g_vrDashboard{false};
std::atomic<bool> g_vsDiag{false};
std::atomic<bool> g_fades{true};
std::atomic<bool> g_leftHanded{false};
std::atomic<bool> g_cinemaRecenter{false};
std::atomic<float> g_forcedMotion{0.0f};
std::atomic<bool> g_heightAuto{true};
std::atomic<bool> g_seated{false};
std::atomic<float> g_eyeHeightAuto{-1.0f};
std::atomic<float> g_heightFixedM{-1.0f};
std::atomic<bool> g_heightCalibRequested{false};
std::atomic<bool> g_roomScale{true};
std::atomic<int32_t> g_stickYawUnits{0};
std::atomic<bool> g_keepFocus{true};
std::atomic<bool> g_configLoaded{false};
std::atomic<bool> g_visCheck{false};
std::atomic<float> g_roomStepRequest{0.0f};
std::atomic<bool> g_roomStepRaw{false};
std::atomic<bool> g_cinemaAlign{true};
std::atomic<bool> g_toasts{true};
std::atomic<int> g_hints{2};
std::atomic<bool> g_bodyFacingHands{false};
std::atomic<bool> g_interactLog{false};
std::atomic<bool> g_interactZone[2]{false, false};
std::atomic<bool> g_grabUse{true};
std::atomic<bool> g_ham{false};
std::atomic<bool> g_cinema3d{false};
std::atomic<float> g_spreadAds{0.1f}, g_spreadTwoHand{0.5f}, g_spreadOneHand{1.5f};
std::atomic<bool> g_laser{false};
std::atomic<bool> g_gestureGunMelee{false};
std::atomic<int> g_vrMenu{1};
std::atomic<bool> g_vrMenuOpen{false};
std::atomic<bool> g_vdDetected{false};
std::atomic<bool> g_vrMenuFlipY{true};  // origine in basso a sinistra (verificato nel visore 21/10)
std::atomic<bool> g_waitBeforeSubmit{true};
std::atomic<unsigned long long> g_interactSimUntil[2]{0, 0};
std::atomic<int> g_hintsShown{0};
std::atomic<bool> g_weaponNeedsAim{false};
std::atomic<bool> g_handPredictAuto{false};
std::atomic<bool> g_crouchViewReal{true};
std::atomic<bool> g_snapFade{true};
std::atomic<bool> g_vrPauseRequest{false};
std::atomic<bool> g_vrProjectionDirty{false};
std::atomic<int32_t> g_headYawUnits{0};
std::atomic<bool> g_gameLocked{false};
std::atomic<bool> g_inCover{false};
std::atomic<bool> g_gamePaused{false};
std::atomic<bool> g_cinemaWalker3D{true};
std::atomic<bool> g_stockCalibRequest{false};
std::atomic<bool> g_protube{true}, g_protubeSwap{false}, g_bhaptics{true};
std::atomic<bool> g_gameCrosshair{false};
std::atomic<bool> g_actionInput{true};
std::atomic<bool> g_perEyeViewState{true};
std::atomic<bool> g_offCenter{true};
std::atomic<bool> g_projFlip{false};
std::atomic<float> g_simCantDeg{0.0f};
std::atomic<bool> g_dynRes{true};
std::atomic<int> g_dynResMin{65};
std::atomic<float> g_dynResBudgetMs{0.0f};
std::atomic<int> g_dynResTestFrames{0};
std::atomic<bool> g_vrPreset{true};
std::atomic<bool> g_lowLatency{true};
std::atomic<bool> g_autoResolution{true};
std::atomic<bool> g_gpuTimer{true};
std::atomic<bool> g_linkAswOff{true};
std::atomic<bool> g_motionSmoothingOff{true};
std::atomic<bool> g_threadLag{true};
std::atomic<bool> g_poseCorrect{true};
std::atomic<float> g_predictFrames{-1.0f};
std::atomic<bool> g_predictAuto{false};
std::atomic<float> g_handPredictMs{0.0f};
std::atomic<bool> g_gpuCopy{true};
std::atomic<float> g_stereoHalfIpd{1.6f};  // ~6,4 cm a ~2 cm per unita' Unreal
std::atomic<int> g_presentEye{-1};
std::atomic<bool> g_syncStereo{true};
std::atomic<float> g_fovOverride{0.0f};
std::atomic<bool> g_fovAuto{true};
std::atomic<uint64_t> g_lastStereoDrawMs{0};
std::atomic<bool> g_firstPerson{true};
std::atomic<float> g_fpEyeHeight{40.0f};
std::atomic<float> g_fpForward{15.0f};
std::atomic<float> g_fpNearPlane{18.0f};
std::atomic<bool> g_aimFollowsBody{true};
std::atomic<bool> g_aimHand{true};
std::atomic<float> g_aimPitchOffsetDeg{0.0f};
std::atomic<float> g_synthAimYawDeg{0.0f};
std::atomic<float> g_synthAimPitchDeg{0.0f};
std::atomic<bool> g_aimMarker{false};  // mirino verde: spento di default dal 21/10 (mirini veri; "laser on" in alternativa)
std::atomic<bool> g_headCollision{true};
std::atomic<float> g_snapTurnDeg{0.0f};
std::atomic<bool> g_stickPitch{false};
std::atomic<void*> g_playerController{nullptr};
std::atomic<float> g_vignette{0.0f};
std::atomic<float> g_motionAmount{0.0f};
std::atomic<float> g_hudScale{0.6f};
std::atomic<float> g_hudDepth{150.0f};
std::atomic<bool> g_sixDof{true};
std::atomic<float> g_posScale{50.0f};
std::atomic<int> g_rollSign{1};
std::atomic<float> g_synthPosCm[3];
std::atomic<bool> g_gunInHand{true};
std::atomic<bool> g_scopeZoom{true};
std::mutex g_gunClassMutex;
std::map<std::string, std::array<float, 3>> g_gunClassOffsets;
bool GunOffsetForClass(const std::string& cls, float out[3]) {
    std::lock_guard<std::mutex> lock(g_gunClassMutex);
    auto it = g_gunClassOffsets.find(cls);
    if (it == g_gunClassOffsets.end()) return false;
    for (int i = 0; i < 3; ++i) out[i] = it->second[i];
    return true;
}
std::atomic<float> g_gunOffsetCm[3];
std::atomic<float> g_synthHandCm[3] = {35.0f, 20.0f, -35.0f};
std::atomic<bool> g_twoHand{true};
std::atomic<float> g_twoHandCm{12.0f};
std::atomic<float> g_twoHandSmoothSec{0.08f};
std::atomic<bool> g_arms{true};
std::atomic<float> g_armStretch{1.25f};
std::atomic<bool> g_muzzleAim{true};
std::atomic<bool> g_noAimAssist{true};
std::atomic<bool> g_spread{true};
std::atomic<float> g_spreadScale{1.0f};
std::atomic<bool> g_fireLog{false};
std::atomic<int> g_projLogSeconds{0};
std::atomic<bool> g_rightTriggerHeld{false};
std::atomic<uint32_t> g_weaponPtr{0};
std::atomic<uint64_t> g_meleeIntentMs{0};
std::atomic<bool> g_exiting{false};
std::atomic<bool> g_execAssist{true};
std::atomic<bool> g_honestReload{true};
std::atomic<int> g_audioLog{0};
std::atomic<int32_t> g_weaponAmmo[2] = {{-1}, {-1}};
std::atomic<bool> g_squadButtonHeld{false};
std::atomic<bool> g_autoUpdate{true};
std::atomic<bool> g_updateBeta{false};
static std::mutex g_updateRepoMutex;
static std::string g_updateRepo;
std::string UpdateRepo() {
    std::lock_guard<std::mutex> lock(g_updateRepoMutex);
    return g_updateRepo;
}
std::atomic<bool> g_fpBody{true};
std::atomic<bool> g_leftGrip{true};
std::atomic<float> g_leftGripCm{20.0f};
std::atomic<float> g_bodyBack{-10.0f};
std::atomic<float> g_bodyBackLook{26.0f};
std::atomic<bool> g_bodyFollowsHead{true};
std::atomic<float> g_armRotDeg[3];
std::atomic<float> g_armOffsetCm[3];
std::atomic<int> g_armDebugFrames{0};
std::atomic<bool> g_armBonesRequested{false};
std::atomic<bool> g_weaponBonesRequested{false};
std::atomic<bool> g_bodySizeRequested{false};
std::atomic<int> g_camLogFrames{0};
std::atomic<int> g_gunLogFrames{0};
std::atomic<int> g_skelLogCalls{0};
std::atomic<bool> g_armSkelFix{true};
std::atomic<float> g_armSkelFixDist{8.0f};
std::atomic<float> g_eyeHeightSmooth{0.12f};
std::atomic<bool> g_ipdFromHmd{true};
static std::mutex g_armNamesMutex;
static std::string g_armNames[4] = {"IK_RightHand", "IK_LeftHand", "IKRot_RightHand", "IKRot_LeftHand"};
std::string ArmControlName(int i) {
    std::lock_guard<std::mutex> lock(g_armNamesMutex);
    return (i >= 0 && i < 4) ? g_armNames[i] : std::string();
}
std::atomic<float> g_synthLeftCm[3];
std::atomic<bool> g_gestureGrenade{true}, g_gestureReload{true}, g_gestureCover{true};
std::atomic<bool> g_gestureMelee{true};
std::atomic<bool> g_reloadPhysical{true};
std::atomic<bool> g_manualAction{false};
std::atomic<float> g_magwellCm[2] = {4.0f, 10.0f};
std::atomic<float> g_magwellClassCm[2] = {0.0f, 0.0f};
std::atomic<float> g_throwDir[3];
std::atomic<float> g_throwSpeed{0.0f};
std::atomic<unsigned long long> g_throwStamp{0};
std::atomic<bool> g_gestureWeapon{true};
std::atomic<int> g_inputLogFrames{0};
std::atomic<int> g_gestureLogFrames{0};
std::mutex g_gestureSimMutex;
std::string g_gestureSim;
bool TakeGestureSimRequest(std::string* out) {
    std::lock_guard<std::mutex> lock(g_gestureSimMutex);
    if (g_gestureSim.empty()) return false;
    *out = g_gestureSim;
    g_gestureSim.clear();
    return true;
}
std::atomic<bool> g_fullViewport{true};

namespace {

// Coda di screenshot: ogni voce attende "wait" Present prima di essere
// catturata (burst = frame consecutivi, alternando gli occhi in stereo).
struct ShotRequest {
    std::string path;
    int wait;
};
std::mutex g_shotMutex;
std::deque<ShotRequest> g_shots;
std::string g_mirrorShotPath;

std::string ModuleDirectory() {
    // Cartella del gioco in Unicode: se contiene caratteri fuori dalla code
    // page ANSI (nome utente, libreria Steam) il percorso ANSI aveva "?" e ini,
    // log e file dei comandi non si trovavano. In quel caso si usa il nome
    // corto 8.3, che e' solo ASCII (audit 20/10).
    wchar_t wpath[MAX_PATH] = {};
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&ModuleDirectory), &self);
    GetModuleFileNameW(self, wpath, MAX_PATH);
    std::wstring wfull(wpath);
    size_t wpos = wfull.find_last_of(L"\\/");
    std::wstring wdir = wpos == std::wstring::npos ? L"." : wfull.substr(0, wpos);
    BOOL lossy = FALSE;
    char path[MAX_PATH * 2] = {};
    WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wdir.c_str(), -1, path, sizeof(path), nullptr, &lossy);
    if (lossy) {
        wchar_t shortPath[MAX_PATH] = {};
        if (GetShortPathNameW(wdir.c_str(), shortPath, MAX_PATH))
            WideCharToMultiByte(CP_ACP, 0, shortPath, -1, path, sizeof(path), nullptr, nullptr);
    }
    std::string full(path);
    return full.empty() ? "." : full;
}

std::string ShotPath(const std::string& name) {
    std::string dir = ModuleDirectory() + "\\specopsvr_shots";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir + "\\" + name + ".bmp";
}

void ExecuteNormalized(const std::string& line);

// Righe scritte a mano: "hud 0,6 150" (virgola decimale) leggeva 0 e l'HUD
// spariva, "spread On" spegneva la dispersione, un commento a fine riga
// finiva nei valori (audit 19/10). Fuori dai comandi che passano nomi del
// gioco o percorsi, i valori vanno in minuscolo, la virgola tra cifre
// diventa punto, true/yes/false/no diventano on/off.
void Execute(const std::string& raw) { ExecuteNormalized(vr::pure::NormalizeConfigLine(raw)); }

// Comandi aggiunti con l'audit del 19/10, fuori dalla catena principale di
// Execute (MSVC ha un limite di annidamento degli else if).
// Valori on/off dell'ini: accetta anche 1/0, true/false, yes/no (prima
// "spread 1" spegneva la dispersione e "marker 0" lasciava il mirino acceso).
// Un valore non riconosciuto lascia il predefinito e lo scrive nel log
// (audit 20/10).
bool OnOff(const std::string& cmd, const std::string& v, bool def) {
    std::string l;
    for (char c : v) l += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (l == "on" || l == "1" || l == "true" || l == "yes") return true;
    if (l == "off" || l == "0" || l == "false" || l == "no") return false;
    vr::log::Line("CONFIG: \"" + cmd + " " + v + "\" not understood (expected on or off), using " +
                  (def ? "on" : "off") + ".");
    return def;
}

bool ExecuteExtra(const std::string& cmd, std::istringstream& in) {
    if (cmd == "audiolog") {
        int n = 10;
        in >> n;
        g_audioLog = n;
    } else if (cmd == "vischeck") {
        std::string v;
        in >> v;
        g_visCheck = OnOff(cmd, v, false);
    } else if (cmd == "roomstep") {
        // roomstep <unita'> [raw]: "raw" non corregge la base della vista
        // (mostra il ritardo di un frame del POV, L8).
        float u = 20.0f;
        std::string raw;
        in >> u >> raw;
        g_roomStepRaw = raw == "raw";
        g_roomStepRequest = u;
    } else if (cmd == "cinemaalign") {
        std::string v;
        in >> v;
        g_cinemaAlign = OnOff(cmd, v, true);
    } else if (cmd == "toasts") {
        std::string v;
        in >> v;
        g_toasts = OnOff(cmd, v, true);
    } else if (cmd == "waitbeforesubmit") {
        std::string v;
        in >> v;
        g_waitBeforeSubmit = OnOff(cmd, v, true);
    } else if (cmd == "vrmenu") {
        // vrmenu on|off (all'avvio) | show | hide
        std::string v;
        in >> v;
        if (v == "show") vr::menu::Show(true);
        else if (v == "hide") vr::menu::Show(false);
        else if (v == "flipy") {
            std::string w;
            in >> w;
            g_vrMenuFlipY = OnOff(cmd, w, false);
        }
        else g_vrMenu = OnOff(cmd, v, true) ? 1 : 0;
    } else if (cmd == "laser") {
        std::string v;
        in >> v;
        g_laser = OnOff(cmd, v, false);
    } else if (cmd == "cinema3d") {
        std::string v;
        in >> v;
        g_cinema3d = OnOff(cmd, v, false);
    } else if (cmd == "ham") {
        std::string v;
        in >> v;
        g_ham = OnOff(cmd, v, false);
    } else if (cmd == "grabuse") {
        std::string v;
        in >> v;
        g_grabUse = OnOff(cmd, v, true);
    } else if (cmd == "interactsim") {
        // interactsim right|left [ms]: finge la mano nella zona di presa (test di grabuse)
        std::string v;
        int ms = 2000;
        in >> v >> ms;
        g_interactSimUntil[v == "left" ? 1 : 0] = GetTickCount64() + ms;
    } else if (cmd == "interactlog") {
        std::string v;
        in >> v;
        g_interactLog = OnOff(cmd, v, false);
    } else if (cmd == "bodyfacing") {
        std::string v;
        in >> v;
        g_bodyFacingHands = (v == "hands");
    } else if (cmd == "hints") {
        std::string v;
        in >> v;
        g_hints = v == "auto" ? 2 : (OnOff(cmd, v, true) ? 1 : 0);
    } else if (cmd == "hintsshown") {
        int n = 0;
        in >> n;
        g_hintsShown = n;
    } else if (cmd == "toast") {
        // toast <testo>: avviso di prova nel visore
        std::string rest;
        std::getline(in, rest);
        size_t a = rest.find_first_not_of(' ');
        if (a != std::string::npos) vr::toast::Notify(rest.substr(a));
    } else if (cmd == "inputfiles") {
        std::string p = vr::input::WriteActionFiles();
        vr::log::Line(p.empty() ? "INPUT: action files not written." : "INPUT: action files written, manifest " + p);
    } else if (cmd == "honestreload") {
        std::string v;
        in >> v;
        g_honestReload = OnOff(cmd, v, true);
    } else if (cmd == "execassist") {
        std::string v;
        in >> v;
        g_execAssist = OnOff(cmd, v, true);
    } else if (cmd == "wndinfo") {
        vr::display::LogWindowInfo();
    } else if (cmd == "protubetest") {
        // Prova del rinculo senza sparare: un colpo da cecchino a due mani.
        vr::haptics::Shot("Barrett", true);
    } else if (cmd == "hand") {
        std::string v;
        in >> v;
        g_leftHanded = (v == "left");
    } else if (cmd == "headsample") {
        std::string v;
        in >> v;
        // "render" (campionamento nel BeginScene, thread di rendering) faceva
        // girare UnrealScript, l'aiuto alle esecuzioni e le scritture sul
        // PlayerController fuori dal thread di gioco (L1): non piu' ammesso.
        if (v == "render") vr::log::Line("CONFIG: headsample render is no longer supported, using draw.");
        g_headSampleOnDraw = true;
    } else if (cmd == "turnspeed") {
        float v = 150.0f;
        in >> v;
        g_turnSpeedDeg = v < 0.0f ? 0.0f : (v > 720.0f ? 720.0f : v);
    } else if (cmd == "movedir") {
        std::string v;
        in >> v;
        g_moveWithHead = (v != "body");
        // "movedir hand": verso dove punta il controller sinistro (camminare
        // guardando altrove, come in molti giochi VR; 20/10).
        g_moveDirHand = (v == "hand");
    } else if (cmd == "vsdiag") {
        std::string v;
        in >> v;
        g_vsDiag = OnOff(cmd, v, false);
    } else if (cmd == "fades") {
        std::string v;
        in >> v;
        g_fades = OnOff(cmd, v, true);
    } else if (cmd == "snapfade") {
        std::string v;
        in >> v;
        g_snapFade = OnOff(cmd, v, true);
    } else if (cmd == "crouchview") {
        std::string v;
        in >> v;
        g_crouchViewReal = (v != "game");
    } else if (cmd == "keepfocus") {
        std::string v;
        in >> v;
        g_keepFocus = OnOff(cmd, v, true);
    } else if (cmd == "roomscale") {
        std::string v;
        in >> v;
        g_roomScale = OnOff(cmd, v, true);
    } else if (cmd == "height") {
        // height auto | fixed | <metri>: altezza della testa in piedi
        std::string v;
        in >> v;
        float m = static_cast<float>(std::atof(v.c_str()));
        if (m >= 1.0f && m <= 2.3f) {
            g_heightFixedM = m;
            g_heightAuto = true;
        } else {
            g_heightFixedM = -1.0f;
            g_heightAuto = (v == "auto");
        }
    } else if (cmd == "seated") {
        std::string v;
        in >> v;
        g_seated = OnOff(cmd, v, false);
    } else {
        return false;
    }
    return true;
}

void ExecuteNormalized(const std::string& line) {
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;
    if (cmd.empty() || cmd[0] == '#') return;
    vr::log::Line("CMD: " + line);
    if (ExecuteExtra(cmd, in)) return;

    if (cmd == "mode") {
        std::string m;
        in >> m;
        if (m == "off") g_mode = kModeOff;
        else if (m == "hmd") g_mode = kModeHmd;
        else if (m == "synth") g_mode = kModeSynthetic;
        else vr::log::Line("CMD: unknown mode '" + m + "' (off|hmd|synth)");
        g_recenterRequested = true;
    } else if (cmd == "synth") {
        float yaw = 30, pitch = 0, period = 4;
        in >> yaw >> pitch >> period;
        g_synthYawAmpDeg = yaw;
        g_synthPitchAmpDeg = pitch;
        g_synthPeriodSec = period <= 0.0f ? 0.0f : (period > 0.1f ? period : 0.1f);  // 0 = fisso
    } else if (cmd == "sign") {
        int y = 1, p = 1;
        in >> y >> p;
        g_yawSign = y < 0 ? -1 : 1;
        g_pitchSign = p < 0 ? -1 : 1;
    } else if (cmd == "postfx") {
        std::string v;
        in >> v;
        g_disablePostFx = (v == "off");
    } else if (cmd == "output") {
        std::string v;
        in >> v;
        g_vrOutput = (v == "on" || v == "force");
        g_vrOutputForce = (v == "force");
    } else if (cmd == "stereo") {
        float half = 0;
        std::string kind;
        in >> half >> kind;
        g_stereoHalfIpd = half < 0 ? 0 : half;
        g_syncStereo = (kind != "afr");
    } else if (cmd == "fov") {
        std::string v;
        in >> v;
        if (v == "auto") {
            g_fovAuto = true;
        } else if (v == "off") {
            g_fovAuto = false;
            g_fovOverride = 0.0f;
        } else {
            g_fovAuto = false;
            g_fovOverride = static_cast<float>(std::atof(v.c_str()));
        }
    } else if (cmd == "fp") {
        std::string v;
        in >> v;
        if (v == "off") {
            g_firstPerson = false;
        } else {
            float forward = g_fpForward;
            in >> forward;
            g_fpEyeHeight = static_cast<float>(std::atof(v.c_str()));
            g_fpForward = forward;
            g_firstPerson = true;
        }
    } else if (cmd == "viewport") {
        std::string v;
        in >> v;
        g_fullViewport = (v != "game");
    } else if (cmd == "near") {
        float v = 18.0f;
        in >> v;
        g_fpNearPlane = v < 1.0f ? 1.0f : v;
    } else if (cmd == "aim") {
        std::string v;
        in >> v;
        g_aimFollowsBody = (v != "view");
        g_aimHand = (v == "hand");
    } else if (cmd == "hud") {
        float scale = 0.6f, depth = 150.0f;
        in >> scale >> depth;
        g_hudScale = scale;
        g_hudDepth = depth;
    } else if (cmd == "pad") {
        // pad fake <lx> <ly> <rx> <ry> <lt> <rt> <bottoni hex> <ms>
        std::string sub;
        in >> sub;
        if (sub == "fake") {
            float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
            unsigned buttons = 0, ms = 1000;
            in >> lx >> ly >> rx >> ry >> lt >> rt >> std::hex >> buttons >> std::dec >> ms;
            vr::input::SetFakeInput(lx, ly, rx, ry, lt, rt, buttons, ms);
        }
    } else if (cmd == "6dof") {
        std::string v;
        in >> v;
        g_sixDof = OnOff(cmd, v, true);
    } else if (cmd == "posscale") {
        float v = 50.0f;
        in >> v;
        g_posScale = v;
    } else if (cmd == "rollsign") {
        int v = 1;
        in >> v;
        g_rollSign = v < 0 ? -1 : 1;
    } else if (cmd == "synthpos") {
        float f = 0, r = 0, u = 0;
        in >> f >> r >> u;
        g_synthPosCm[0] = f;
        g_synthPosCm[1] = r;
        g_synthPosCm[2] = u;
    } else if (cmd == "gun") {
        std::string v;
        in >> v;
        g_gunInHand = OnOff(cmd, v, true);
    } else if (cmd == "gunoffset") {
        float f = 0, r = 0, u = 0;
        in >> f >> r >> u;
        g_gunOffsetCm[0] = f;
        g_gunOffsetCm[1] = r;
        g_gunOffsetCm[2] = u;
    } else if (cmd == "arms") {
        std::string v;
        in >> v;
        g_arms = OnOff(cmd, v, true);
    } else if (cmd == "autoupdate") {
        std::string v;
        in >> v;
        g_autoUpdate = OnOff(cmd, v, true);
    } else if (cmd == "updatechannel") {
        std::string v;
        in >> v;
        g_updateBeta = (v == "beta");
    } else if (cmd == "updaterepo") {
        std::string v;
        in >> v;
        std::lock_guard<std::mutex> lock(g_updateRepoMutex);
        g_updateRepo = v;
    } else if (cmd == "projlog") {
        int n = 10;
        in >> n;
        g_projLogSeconds = n > 0 ? n : 10;
    } else if (cmd == "firelog") {
        std::string v;
        in >> v;
        g_fireLog = OnOff(cmd, v, true);
    } else if (cmd == "spread") {
        // on = 1 (come il gioco), off = 0 (lungo la canna), oppure una scala 0..2.
        std::string v;
        in >> v;
        float s = 1.0f;
        if (v == "off" || v == "0" || v == "false" || v == "no") s = 0.0f;
        else if (v == "on" || v == "1" || v == "true" || v == "yes" || v.empty()) s = 1.0f;
        else {
            char* endp = nullptr;
            s = std::strtof(v.c_str(), &endp);
            if (endp == v.c_str() || !(s >= 0.0f) || s > 2.0f) {
                vr::log::Line("CONFIG: spread " + v + " ignored (on, off or 0..2).");
                s = g_spreadScale.load();
            }
        }
        // Fattori per postura dopo il valore: spread on <scala> <ads> <due mani> <una mano>
        {
            float sc = -1.0f, ads = -1.0f, two = -1.0f, one = -1.0f;
            in >> sc >> ads >> two >> one;
            if (sc >= 0.0f && sc <= 2.0f && (v == "on" || v == "1" || v == "true" || v == "yes")) s = sc;
            if (ads >= 0.0f && ads <= 2.0f) g_spreadAds = ads;
            if (two >= 0.0f && two <= 2.0f) g_spreadTwoHand = two;
            if (one >= 0.0f && one <= 3.0f) g_spreadOneHand = one;
        }
        g_spreadScale = s;
        g_spread = s > 0.0f;
    } else if (cmd == "aimassist") {
        std::string v;
        in >> v;
        g_noAimAssist = (v == "off");
    } else if (cmd == "muzzleaim") {
        std::string v;
        in >> v;
        g_muzzleAim = OnOff(cmd, v, true);
    } else if (cmd == "fpbody") {
        std::string v;
        in >> v;
        g_fpBody = OnOff(cmd, v, true);
    } else if (cmd == "gesturelog") {
        int n = 240;
        in >> n;
        g_gestureLogFrames = n;
    } else if (cmd == "inputlog") {
        int n = 40;
        in >> n;
        g_inputLogFrames = n;
    } else if (cmd == "leftgrip") {
        std::string v;
        in >> v;
        g_leftGrip = OnOff(cmd, v, true);
        float cm = 0.0f;
        if (in >> cm && cm > 0.0f) g_leftGripCm = cm;
    } else if (cmd == "bodyfollow") {
        std::string v;
        in >> v;
        g_bodyFollowsHead = OnOff(cmd, v, true);
    } else if (cmd == "bodyback") {
        float v = 0, look = 10.0f;
        in >> v >> look;
        g_bodyBack = v;
        g_bodyBackLook = look;
    } else if (cmd == "armrot") {
        float p = 0, y = 0, r = 0;
        in >> p >> y >> r;
        g_armRotDeg[0] = p;
        g_armRotDeg[1] = y;
        g_armRotDeg[2] = r;
    } else if (cmd == "armstretch") {
        float v = 1.25f;
        in >> v;
        g_armStretch = v < 1.0f ? 1.0f : (v > 1.6f ? 1.6f : v);
    } else if (cmd == "armoffset") {
        float f = 0, r = 0, u = 0;
        in >> f >> r >> u;
        g_armOffsetCm[0] = f;
        g_armOffsetCm[1] = r;
        g_armOffsetCm[2] = u;
    } else if (cmd == "armcontrols") {
        std::string n[4];
        in >> n[0] >> n[1] >> n[2] >> n[3];
        std::lock_guard<std::mutex> lock(g_armNamesMutex);
        for (int i = 0; i < 4; ++i)
            if (!n[i].empty()) g_armNames[i] = n[i];
    } else if (cmd == "armdebug") {
        g_armDebugFrames = 1;
    } else if (cmd == "armbones") {
        g_armBonesRequested = true;
    } else if (cmd == "weaponbones") {
        g_weaponBonesRequested = true;
    } else if (cmd == "ipd") {
        std::string v;
        in >> v;
        g_ipdFromHmd = (v != "fixed");
    } else if (cmd == "eyesmooth") {
        float v = 0.12f;
        in >> v;
        g_eyeHeightSmooth = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    } else if (cmd == "camlog") {
        int v = 60;
        in >> v;
        g_camLogFrames = v;
    } else if (cmd == "skelfix") {
        std::string v;
        float d = 0.0f;
        in >> v >> d;
        g_armSkelFix = OnOff(cmd, v, true);
        if (d > 0.0f) g_armSkelFixDist = d;
    } else if (cmd == "skellog") {
        int v = 200;
        in >> v;
        g_skelLogCalls = v;
    } else if (cmd == "gunlog") {
        int v = 200;
        in >> v;
        g_gunLogFrames = v;
    } else if (cmd == "bodysize") {
        g_bodySizeRequested = true;
    } else if (cmd == "twohand") {
        // twohand on|off [cm]: tolleranza della sinistra dalla linea dell'arma
        std::string v;
        float cm = 0.0f, sm = -1.0f;
        in >> v >> cm >> sm;
        g_twoHand = OnOff(cmd, v, true);
        if (cm >= 3.0f && cm <= 40.0f) g_twoHandCm = cm;
        if (sm >= 0.0f && sm <= 0.5f) g_twoHandSmoothSec = sm < 0.005f ? 0.005f : sm;
    } else if (cmd == "synthleft") {
        float f = 0, r = 0, u = 0;
        in >> f >> r >> u;
        g_synthLeftCm[0] = f;
        g_synthLeftCm[1] = r;
        g_synthLeftCm[2] = u;
    } else if (cmd == "gesture") {
        // gesture grenade|reload|cover|all on|off
        std::string which, v;
        in >> which >> v;
        bool on = OnOff(cmd, v, true);
        if (which == "grenade" || which == "all") g_gestureGrenade = on;
        if (which == "melee" || which == "all") g_gestureMelee = on;
        if (which == "reload" || which == "all") g_gestureReload = on;
        if (which == "gunmelee") g_gestureGunMelee = on;
        if (which == "cover" || which == "all") g_gestureCover = on;
        if (which == "weapon" || which == "all") g_gestureWeapon = on;
        if (which != "grenade" && which != "melee" && which != "reload" && which != "cover" && which != "weapon" && which != "gunmelee" &&
            which != "all")
            vr::log::Line("CONFIG: unknown gesture \"" + which + "\" (grenade, melee, reload, cover, weapon, all).");
    } else if (cmd == "reload") {
        // reload physical|simple
        std::string v;
        in >> v;
        g_reloadPhysical = (v != "simple");
    } else if (cmd == "manualaction") {
        std::string v;
        in >> v;
        g_manualAction = OnOff(cmd, v, false);
    } else if (cmd == "magwell") {
        // magwell <avanti cm> <sotto cm>: caricatore rispetto al controller destro
        float f = 4.0f, d = 10.0f;
        in >> f >> d;
        g_magwellCm[0] = f;
        g_magwellCm[1] = d;
    } else if (cmd == "gesturesim") {
        std::string v;
        in >> v;
        std::lock_guard<std::mutex> lock(g_gestureSimMutex);
        g_gestureSim = v;
    } else if (cmd == "gunoffsetclass") {
        std::string c;
        float f = 0, r = 0, u = 0;
        in >> c >> f >> r >> u;
        if (!c.empty()) {
            std::lock_guard<std::mutex> lock(g_gunClassMutex);
            g_gunClassOffsets[c] = {f, r, u};
        }
    } else if (cmd == "scopezoom") {
        std::string v;
        in >> v;
        g_scopeZoom = OnOff(cmd, v, true);
    } else if (cmd == "synthhand") {
        float f = 0, r = 0, u = 0;
        in >> f >> r >> u;
        g_synthHandCm[0] = f;
        g_synthHandCm[1] = r;
        g_synthHandCm[2] = u;
    } else if (cmd == "aimpitch") {
        float v = 0.0f;
        in >> v;
        // Oltre +-45 gradi: valore della calibrazione sbagliata della 0.9.3
        // (anche -67/-96): si torna a 0.
        if (v < -45.0f || v > 45.0f) {
            vr::log::Line("CONFIG: aimpitch " + std::to_string(v) + " out of range (bad 0.9.3 calibration), using 0.");
            v = 0.0f;
        }
        g_aimPitchOffsetDeg = v;
    } else if (cmd == "synthaim") {
        float y = 0.0f, p = 0.0f;
        in >> y >> p;
        g_synthAimYawDeg = y;
        g_synthAimPitchDeg = p;
    } else if (cmd == "marker") {
        std::string v;
        in >> v;
        g_aimMarker = OnOff(cmd, v, true);
    } else if (cmd == "headcollision") {
        std::string v;
        in >> v;
        g_headCollision = OnOff(cmd, v, true);
    } else if (cmd == "snapturn") {
        float v = 0.0f;
        in >> v;
        g_snapTurnDeg = v < 0.0f ? 0.0f : v;
    } else if (cmd == "stickpitch") {
        std::string v;
        in >> v;
        g_stickPitch = OnOff(cmd, v, false);
    } else if (cmd == "vignette") {
        float v = 0.0f;
        in >> v;
        g_vignette = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    } else if (cmd == "mirrorshot") {
        std::string name;
        in >> name;
        if (name.empty()) name = "mirror";
        std::lock_guard<std::mutex> lock(g_shotMutex);
        g_mirrorShotPath = ShotPath(name);
    } else if (cmd == "console") {
        std::string rest;
        std::getline(in, rest);
        size_t a = rest.find_first_not_of(' ');
        if (a != std::string::npos) vr::script::QueueConsole(rest.substr(a));
    } else if (cmd == "cheats") {
        vr::script::QueueCall("!cheats");
    } else if (cmd == "ce") {
        // Evento Kismet da console (salti ai checkpoint dei livelli: cp01, CP03, ...).
        // YPlayerController.CauseEvent e' vuota nella build finale: si usa
        // direttamente PlayerController.ServerCauseEvent.
        std::string ev;
        in >> ev;
        if (!ev.empty()) vr::script::QueueCall("pc ServerCauseEvent " + ev);
    } else if (cmd == "ucall") {
        std::string rest;
        std::getline(in, rest);
        size_t a = rest.find_first_not_of(' ');
        if (a != std::string::npos) vr::script::QueueCall(rest.substr(a));
    } else if (cmd == "giveweapon") {
        std::string c;
        in >> c;
        if (!c.empty()) vr::script::QueueGiveWeapon(c);
    } else if (cmd == "gpushare") {
        // Letto all'avvio da GpuShareRequested (tipo di device); qui ignorato.
    } else if (cmd == "gpucopy") {
        std::string v;
        in >> v;
        g_gpuCopy = OnOff(cmd, v, true);
    } else if (cmd == "paircheck") {
        std::string v;
        in >> v;
        g_pairCheck = OnOff(cmd, v, false);
    } else if (cmd == "submitpose") {
        std::string v;
        in >> v;
        g_submitWithPose = OnOff(cmd, v, true);
    } else if (cmd == "threadlag") {
        std::string v;
        in >> v;
        g_threadLag = OnOff(cmd, v, true);
    } else if (cmd == "motionsmoothing") {
        std::string v;
        in >> v;
        g_motionSmoothingOff = (v != "keep");
    } else if (cmd == "linkasw") {
        std::string v;
        in >> v;
        g_linkAswOff = (v != "keep");
    } else if (cmd == "gputimer") {
        std::string v;
        in >> v;
        g_gpuTimer = OnOff(cmd, v, true);
    } else if (cmd == "resolution") {
        std::string v;
        in >> v;
        g_autoResolution = (v != "window");
    } else if (cmd == "lowlatency") {
        std::string v;
        in >> v;
        g_lowLatency = OnOff(cmd, v, true);
    } else if (cmd == "vrpreset") {
        std::string v;
        in >> v;
        g_vrPreset = OnOff(cmd, v, true);
    } else if (cmd == "dynres") {
        std::string v;
        in >> v;
        if (v == "budget") {
            float ms = 0.0f;
            in >> ms;
            g_dynResBudgetMs = ms;
        } else if (v == "test") {
            g_dynResTestFrames = 1200;
        } else if (v == "min") {
            int m = 65;
            in >> m;
            g_dynResMin = m < 40 ? 40 : (m > 100 ? 100 : m);
        } else {
            g_dynRes = OnOff(cmd, v, true);
        }
    } else if (cmd == "eyecant") {
        float d = 0.0f;
        in >> d;
        g_simCantDeg = d;
    } else if (cmd == "offcenter") {
        std::string v;
        in >> v;
        bool on = OnOff(cmd, v, true);
        // Il ritaglio degli occhi dipende dal tipo di proiezione: va
        // ricalcolato (prima restava quello vecchio, immagine schiacciata; audit 20/10).
        if (g_offCenter.exchange(on) != on) g_vrProjectionDirty = true;
    } else if (cmd == "projflip") {
        // Prova nel visore: alto e basso del frustum degli occhi scambiati
        // (on = come fino alla 0.9.5).
        std::string v;
        in >> v;
        g_projFlip = OnOff(cmd, v, false);
        vr::log::Line(std::string("CMD: projflip ") + (g_projFlip ? "on" : "off"));
    } else if (cmd == "eyestate") {
        std::string v;
        in >> v;
        g_perEyeViewState = OnOff(cmd, v, true);
    } else if (cmd == "vrinput") {
        std::string v;
        in >> v;
        g_actionInput = (v != "legacy");
    } else if (cmd == "controllerpose") {
        std::string v;
        in >> v;
        g_poseCorrect = (v != "raw");
    } else if (cmd == "gamecrosshair") {
        std::string v;
        in >> v;
        g_gameCrosshair = OnOff(cmd, v, false);
    } else if (cmd == "protube") {
        std::string v;
        in >> v;
        g_protube = (v != "off");
        g_protubeSwap = (v == "swap");
    } else if (cmd == "bhaptics") {
        std::string v;
        in >> v;
        g_bhaptics = OnOff(cmd, v, true);
    } else if (cmd == "cinemawalker") {
        std::string v;
        in >> v;
        g_cinemaWalker3D = (v != "screen");
    } else if (cmd == "ads") {
        std::string v;
        in >> v;
        g_adsEnabled = OnOff(cmd, v, true);
    } else if (cmd == "turretaim") {
        std::string v;
        in >> v;
        g_turretAimHand = (v != "stick");
    } else if (cmd == "coversnap") {
        std::string v;
        in >> v;
        g_coverSnapCompensate = OnOff(cmd, v, true);
    } else if (cmd == "coverpeek") {
        std::string v;
        in >> v;
        g_coverPeekEnabled = OnOff(cmd, v, true);
    } else if (cmd == "turnwithhead") {
        float v = 45.0f;
        in >> v;
        g_turnWithHeadDeg = v < 0.0f ? 0.0f : (v > 170.0f ? 170.0f : v);
    } else if (cmd == "commandaim") {
        std::string v;
        in >> v;
        g_commandFollowsGun = (v != "head");
    } else if (cmd == "handpredict") {
        // handpredict <ms> | auto (= stessa predizione della testa)
        std::string t;
        in >> t;
        g_handPredictAuto = (t == "auto");
        float v = g_handPredictAuto ? 0.0f : static_cast<float>(std::atof(t.c_str()));
        g_handPredictMs = v < 0.0f ? 0.0f : (v > 60.0f ? 60.0f : v);
    } else if (cmd == "predict") {
        std::string t;
        in >> t;
        g_predictAuto = (t == "auto");
        float v = t == "off" || t == "auto" ? -1.0f : static_cast<float>(std::atof(t.c_str()));
        g_predictFrames = v > 4.0f ? 4.0f : v;
    } else if (cmd == "outputscale") {
        float v = 1.0f;
        in >> v;
        g_outputScale = v < 0.25f ? 0.25f : (v > 1.0f ? 1.0f : v);
    } else if (cmd == "outputtest") {
        int v = 0;
        in >> v;
        g_outputTest = v;
    } else if (cmd == "cinema") {
        std::string v;
        in >> v;
        g_cinemaEnabled = OnOff(cmd, v, true);
        g_cinemaForce = (v == "force");
    } else if (cmd == "cinemasize") {
        float w = 3.2f, d = 2.6f;
        in >> w >> d;
        if (w > 0.3f) g_cinemaWidthM = w;
        if (d > 0.3f) g_cinemaDistM = d;
    } else if (cmd == "configversion") {
        // Versione del formato dell'ini (vedi MigrateConfig): niente da fare.
    } else if (cmd == "mirrorwin") {
        std::string v;
        in >> v;
        g_mirrorWin = OnOff(cmd, v, true);
    } else if (cmd == "monitor") {
        int v = 1;
        in >> v;
        g_monitorEvery = v < 1 ? 1 : v;
    } else if (cmd == "mirror") {
        std::string v;
        in >> v;
        g_mirrorRequest = (v == "off") ? 0 : 1;
    } else if (cmd == "menu169") {
        std::string v;
        in >> v;
        g_menu169 = (v != "off");
    } else if (cmd == "ui169") {
        std::string v;
        in >> v;
        g_ui169 = (v != "off");
    } else if (cmd == "uihookuiscene") {
        std::string v;
        in >> v;
        g_uiHookUiScene = (v == "on");
    } else if (cmd == "uihookgameviewport") {
        std::string v;
        in >> v;
        g_uiHookGameViewport = (v == "on");
    } else if (cmd == "recenter") {
        g_recenterSilent = false;
        g_recenterRequested = true;
    } else if (cmd == "dumpcam") {
        g_dumpCameraRequested = true;
    } else if (cmd == "shot") {
        std::string name;
        in >> name;
        if (name.empty()) name = "shot";
        std::lock_guard<std::mutex> lock(g_shotMutex);
        g_shots.push_back({ShotPath(name), 0});
    } else if (cmd == "burst") {
        // burst <nome> <numero> <ogni N Present>: nome_0, nome_1, ...
        std::string name;
        int count = 4, step = 1;
        in >> name >> count >> step;
        if (name.empty()) name = "burst";
        std::lock_guard<std::mutex> lock(g_shotMutex);
        for (int i = 0; i < count && i < 64; ++i) {
            g_shots.push_back({ShotPath(name + "_" + std::to_string(i)), i == 0 ? 0 : (step > 1 ? step - 1 : 0)});
        }
    } else {
        vr::log::Line("CMD: unknown command '" + cmd + "'");
    }
}

}  // namespace

// Cartella del gioco sicura per le API ANSI (vedi ModuleDirectory).
std::string GameDirectory() { return ModuleDirectory(); }

// specopsvr.ini (accanto alla dll): stessi comandi di specopsvr_cmd.txt,
// eseguiti una volta all'avvio. Righe vuote e '#' ignorate. Il file NON
// viene cancellato.
// Scrittura dell'ini su un file temporaneo e poi sostituzione: prima si
// riscriveva sul posto, e un crash a meta' lasciava l'ini dell'utente vuoto
// (audit 20/10).
void WriteIniLines(const std::string& path, const std::vector<std::string>& lines) {
    std::string tmp = path + ".tmp";
    {
        std::ofstream o(tmp, std::ios::trunc);
        for (const auto& l : lines) o << l << "\n";
        if (!o) {
            vr::log::Line("CONFIG: cannot write " + tmp + ", specopsvr.ini left unchanged.");
            return;
        }
    }
    if (!MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        vr::log::Line("CONFIG: cannot replace specopsvr.ini (error " + std::to_string(GetLastError()) + ").");
        DeleteFileA(tmp.c_str());
    }
}

void SaveIniValue(const std::string& key, const std::string& value) {
    std::string path = ModuleDirectory() + "\\specopsvr.ini";
    std::vector<std::string> lines;
    {
        std::ifstream f(path);
        std::string l;
        while (std::getline(f, l)) lines.push_back(l);
    }
    bool done = false;
    for (auto& l : lines) {
        if (l.rfind(key + " ", 0) == 0 || l == key) {
            l = key + " " + value;
            done = true;
        }
    }
    if (!done) lines.push_back(key + " " + value);
    WriteIniLines(path, lines);
    vr::log::Line("CONFIG: saved \"" + key + " " + value + "\" in specopsvr.ini");
}

// Migrazione dell'ini: l'aggiornamento automatico e install.bat conservano
// l'ini dell'utente, quindi i nuovi predefiniti non arriverebbero mai. Con
// "configversion" minore di kConfigVersion si cambiano solo le righe rimaste
// ai vecchi predefiniti (una scelta diversa dell'utente resta).
// 2 (0.9.6): "submitpose on" -> off, "predict off" -> auto (fisheye e bande
// nere muovendo la testa, 19/10).
// 3 (0.9.7): "bodyback 6 10" -> "bodyback -10 26" (corpo sotto la testa: le
// braccia arrivano ai controller e la sinistra all'astina).
// 4 (0.9.7, prove del 19/10): "bodyback -10 10" -> "bodyback -10 26" (guardando
// in basso col corpo avanti si vedeva l'interno del colletto).
// 5 (0.9.7): "hud 0.6 150" -> "hud 0.5 100" (angoli dell'HUD fuori dalla zona nitida delle lenti, HUD a 3 m
// davanti a ripari a 30 cm; audit 19/10).
constexpr int kConfigVersion = 6;

void MigrateConfig(std::vector<std::string>& lines, const std::string& path) {
    // Ogni cambio vale solo per gli ini piu' vecchi della sua versione: chi era
    // gia' alla 2 e aveva rimesso a mano "submitpose on" non viene toccato.
    static const vr::pure::ConfigChange kChanges[] = {
        {2, "submitpose on", "submitpose off"}, {2, "predict off", "predict auto"}, {3, "bodyback 6 10", "bodyback -10 26"},
        {4, "bodyback -10 10", "bodyback -10 26"}, {5, "hud 0.6 150", "hud 0.5 100"},
        {6, "marker on", "marker off"}};  // 21/10: mirino verde spento di default (mirini veri + laser opzionale)
    std::string done;
    if (!vr::pure::MigrateConfigLines(lines, kConfigVersion, kChanges, sizeof(kChanges) / sizeof(kChanges[0]), &done))
        return;
    WriteIniLines(path, lines);
    vr::log::Line("CONFIG: specopsvr.ini updated to version " + std::to_string(kConfigVersion) + ":" +
                  (done.empty() ? std::string(" no default to change") : done));
}

// Chiave di una riga dell'ini: il comando, piu' il sottocomando per quelli
// che ne hanno ("gesture grenade on", "dynres min 65").
std::string ConfigKey(const std::string& line) { return vr::pure::ConfigKey(line); }

// Predefiniti distribuiti (deploy/specopsvr.ini incorporato) per le voci che
// l'ini dell'utente non ha: un ini vecchio o accorciato riceve i valori delle
// versioni nuove invece dei predefiniti del codice (audit 19/10: submitpose,
// predict e near divergevano).
void ApplyDefaultsMissingFrom(const std::vector<std::string>& lines) {
    std::set<std::string> have;
    for (const auto& l : lines)
        if (!l.empty() && l[0] != '#') have.insert(ConfigKey(l));
    std::istringstream all(kDefaultIni);
    std::string l, added;
    while (std::getline(all, l)) {
        if (l.empty() || ConfigKey(l) == "configversion" || have.count(ConfigKey(l))) continue;
        Execute(l);
        added += " \"" + l + "\";";
    }
    if (!added.empty()) vr::log::Line("CONFIG: defaults for settings missing from specopsvr.ini:" + added);
}

// I thread partiti all'avvio aspettano l'ini invece di un tempo fisso: con un
// avvio lento "autoupdate off" o "protube off" arrivavano dopo (audit 19/10).
void WaitForConfig(unsigned maxMs, unsigned extraMs) {
    for (unsigned waited = 0; !g_configLoaded && waited < maxMs; waited += 250) Sleep(250);
    Sleep(extraMs);
}

void ReadUpdateSettingsFromIni() {
    std::ifstream f(ModuleDirectory() + "\\specopsvr.ini");
    if (!f) return;
    std::string line;
    int applied = 0;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);
        std::istringstream in(line);
        std::string key;
        in >> key;
        for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (key == "autoupdate" || key == "updatechannel" || key == "updaterepo") {
            Execute(line);
            ++applied;
        }
    }
    vr::log::Line("UPDATE: settings read directly from specopsvr.ini (" + std::to_string(applied) +
                  " lines; the ini was not loaded by the game hooks).");
}

void LoadConfigFile() {
    std::string path = ModuleDirectory() + "\\specopsvr.ini";
    std::vector<std::string> lines;
    {
        std::ifstream f(path);
        if (!f) {
            vr::log::Line("CONFIG: specopsvr.ini not found, using the defaults.");
            ApplyDefaultsMissingFrom(lines);
            return;
        }
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            // BOM UTF-8 (ini salvato da editor che lo aggiungono).
            if (lines.empty() && line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);
            lines.push_back(line);
        }
    }
    MigrateConfig(lines, path);
    ApplyDefaultsMissingFrom(lines);
    vr::log::Line("CONFIG: loading specopsvr.ini");
    for (const auto& line : lines) Execute(line);
}

void Poll() {
    static ULONGLONG lastPoll = 0;
    static bool configLoaded = false;
    if (!configLoaded) {
        configLoaded = true;
        LoadConfigFile();
        g_configLoaded = true;
    }
    ULONGLONG now = GetTickCount64();
    if (now - lastPoll < 500) return;
    lastPoll = now;

    std::string path = ModuleDirectory() + "\\specopsvr_cmd.txt";
    std::string contents;
    {
        std::ifstream f(path);
        if (!f) return;
        std::ostringstream ss;
        ss << f.rdbuf();
        contents = ss.str();
    }
    DeleteFileA(path.c_str());

    std::istringstream lines(contents);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        Execute(line);
    }
}

bool TakeMirrorShotRequest(std::string* outPath) {
    std::lock_guard<std::mutex> lock(g_shotMutex);
    if (g_mirrorShotPath.empty()) return false;
    *outPath = g_mirrorShotPath;
    g_mirrorShotPath.clear();
    return true;
}

bool TakeScreenshotRequest(std::string* outPath) {
    std::lock_guard<std::mutex> lock(g_shotMutex);
    if (g_shots.empty()) return false;
    if (g_shots.front().wait > 0) {
        --g_shots.front().wait;
        return false;
    }
    *outPath = g_shots.front().path;
    g_shots.pop_front();
    return true;
}

void ExecuteLine(const std::string& raw) { Execute(raw); }

}  // namespace vr::control
