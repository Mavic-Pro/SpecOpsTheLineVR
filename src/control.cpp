#include "control.h"
#include "logger.h"
#include "vr_input.h"

#include <windows.h>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <mutex>
#include <array>
#include <map>
#include "script_call.h"
#include <sstream>

namespace vr::control {

std::atomic<int> g_mode{kModeHmd};
std::atomic<bool> g_recenterRequested{true};
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
std::atomic<bool> g_cinemaEnabled{true};
std::atomic<bool> g_cinemaActive{false};
std::atomic<bool> g_cinemaForce{false};
std::atomic<float> g_cinemaWidthM{3.2f}, g_cinemaDistM{2.6f};
std::atomic<int> g_outputTest{0};
std::atomic<float> g_outputScale{1.0f};
std::atomic<bool> g_submitWithPose{false};
std::atomic<bool> g_pairCheck{false};
std::atomic<bool> g_commandFollowsGun{true};
std::atomic<float> g_turnWithHeadDeg{45.0f};
std::atomic<float> g_predictFrames{-1.0f};
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
std::atomic<bool> g_aimMarker{true};
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
std::atomic<bool> g_arms{true};
std::atomic<float> g_armStretch{1.25f};
std::atomic<bool> g_muzzleAim{true};
std::atomic<bool> g_noAimAssist{true};
std::atomic<bool> g_spread{true};
std::atomic<bool> g_fireLog{false};
std::atomic<int> g_projLogSeconds{0};
std::atomic<bool> g_rightTriggerHeld{false};
std::atomic<bool> g_squadButtonHeld{false};
std::atomic<bool> g_autoUpdate{true};
static std::mutex g_updateRepoMutex;
static std::string g_updateRepo;
std::string UpdateRepo() {
    std::lock_guard<std::mutex> lock(g_updateRepoMutex);
    return g_updateRepo;
}
std::atomic<bool> g_fpBody{true};
std::atomic<bool> g_leftGrip{false};
std::atomic<float> g_bodyBack{6.0f};
std::atomic<float> g_bodyBackLook{10.0f};
std::atomic<bool> g_bodyFollowsHead{true};
std::atomic<float> g_armRotDeg[3];
std::atomic<float> g_armOffsetCm[3];
std::atomic<int> g_armDebugFrames{0};
std::atomic<bool> g_armBonesRequested{false};
std::atomic<bool> g_bodySizeRequested{false};
std::atomic<int> g_camLogFrames{0};
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
    char path[MAX_PATH] = {};
    HMODULE self = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCSTR>(&ModuleDirectory),
        &self);
    GetModuleFileNameA(self, path, MAX_PATH);
    std::string full(path);
    size_t pos = full.find_last_of("\\/");
    return pos == std::string::npos ? "." : full.substr(0, pos);
}

std::string ShotPath(const std::string& name) {
    std::string dir = ModuleDirectory() + "\\specopsvr_shots";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir + "\\" + name + ".bmp";
}

void Execute(const std::string& line) {
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;
    if (cmd.empty() || cmd[0] == '#') return;
    vr::log::Line("CMD: " + line);

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
        g_sixDof = (v != "off");
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
        g_gunInHand = (v != "off");
    } else if (cmd == "gunoffset") {
        float f = 0, r = 0, u = 0;
        in >> f >> r >> u;
        g_gunOffsetCm[0] = f;
        g_gunOffsetCm[1] = r;
        g_gunOffsetCm[2] = u;
    } else if (cmd == "arms") {
        std::string v;
        in >> v;
        g_arms = (v != "off");
    } else if (cmd == "autoupdate") {
        std::string v;
        in >> v;
        g_autoUpdate = (v != "off");
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
        g_fireLog = (v != "off");
    } else if (cmd == "spread") {
        std::string v;
        in >> v;
        g_spread = (v == "on");
    } else if (cmd == "aimassist") {
        std::string v;
        in >> v;
        g_noAimAssist = (v == "off");
    } else if (cmd == "muzzleaim") {
        std::string v;
        in >> v;
        g_muzzleAim = (v != "off");
    } else if (cmd == "fpbody") {
        std::string v;
        in >> v;
        g_fpBody = (v != "off");
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
        g_leftGrip = (v != "off");
    } else if (cmd == "bodyfollow") {
        std::string v;
        in >> v;
        g_bodyFollowsHead = (v != "off");
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
    } else if (cmd == "bodysize") {
        g_bodySizeRequested = true;
    } else if (cmd == "twohand") {
        std::string v;
        in >> v;
        g_twoHand = (v != "off");
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
        bool on = (v != "off");
        if (which == "grenade" || which == "all") g_gestureGrenade = on;
        if (which == "reload" || which == "all") g_gestureReload = on;
        if (which == "cover" || which == "all") g_gestureCover = on;
        if (which == "weapon" || which == "all") g_gestureWeapon = on;
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
        g_scopeZoom = (v != "off");
    } else if (cmd == "synthhand") {
        float f = 0, r = 0, u = 0;
        in >> f >> r >> u;
        g_synthHandCm[0] = f;
        g_synthHandCm[1] = r;
        g_synthHandCm[2] = u;
    } else if (cmd == "aimpitch") {
        float v = 0.0f;
        in >> v;
        g_aimPitchOffsetDeg = v;
    } else if (cmd == "synthaim") {
        float y = 0.0f, p = 0.0f;
        in >> y >> p;
        g_synthAimYawDeg = y;
        g_synthAimPitchDeg = p;
    } else if (cmd == "marker") {
        std::string v;
        in >> v;
        g_aimMarker = (v != "off");
    } else if (cmd == "headcollision") {
        std::string v;
        in >> v;
        g_headCollision = (v != "off");
    } else if (cmd == "snapturn") {
        float v = 0.0f;
        in >> v;
        g_snapTurnDeg = v < 0.0f ? 0.0f : v;
    } else if (cmd == "stickpitch") {
        std::string v;
        in >> v;
        g_stickPitch = (v == "on");
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
        g_gpuCopy = (v != "off");
    } else if (cmd == "paircheck") {
        std::string v;
        in >> v;
        g_pairCheck = (v == "on");
    } else if (cmd == "submitpose") {
        std::string v;
        in >> v;
        g_submitWithPose = (v != "off");
    } else if (cmd == "turnwithhead") {
        float v = 45.0f;
        in >> v;
        g_turnWithHeadDeg = v < 0.0f ? 0.0f : (v > 170.0f ? 170.0f : v);
    } else if (cmd == "commandaim") {
        std::string v;
        in >> v;
        g_commandFollowsGun = (v != "head");
    } else if (cmd == "predict") {
        std::string t;
        in >> t;
        float v = t == "off" ? -1.0f : static_cast<float>(std::atof(t.c_str()));
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
        g_cinemaEnabled = (v != "off");
        g_cinemaForce = (v == "force");
    } else if (cmd == "cinemasize") {
        float w = 3.2f, d = 2.6f;
        in >> w >> d;
        if (w > 0.3f) g_cinemaWidthM = w;
        if (d > 0.3f) g_cinemaDistM = d;
    } else if (cmd == "monitor") {
        int v = 1;
        in >> v;
        g_monitorEvery = v < 1 ? 1 : v;
    } else if (cmd == "mirror") {
        std::string v;
        in >> v;
        g_mirrorRequest = (v == "off") ? 0 : 1;
    } else if (cmd == "recenter") {
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

// specopsvr.ini (accanto alla dll): stessi comandi di specopsvr_cmd.txt,
// eseguiti una volta all'avvio. Righe vuote e '#' ignorate. Il file NON
// viene cancellato.
void LoadConfigFile() {
    std::ifstream f(ModuleDirectory() + "\\specopsvr.ini");
    if (!f) {
        vr::log::Line("CONFIG: specopsvr.ini not found, using the defaults.");
        return;
    }
    vr::log::Line("CONFIG: loading specopsvr.ini");
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        Execute(line);
    }
}

void Poll() {
    static ULONGLONG lastPoll = 0;
    static bool configLoaded = false;
    if (!configLoaded) {
        configLoaded = true;
        LoadConfigFile();
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

}  // namespace vr::control
