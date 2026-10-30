#include "control.h"
#include "vr_runtime.h"
#include "vr_openxr.h"
#include "audio_diag.h"
#include "game_addrs.h"
#include "default_ini.h"
#include "logger.h"
#include "pure_utils.h"
#include "twohand_math.h"
#include "vr_toast.h"
#include "hooks.h"
#include "vr_menu.h"
#include "vr_input.h"
#include "props.h"
#include "haptics_ext.h"
#include "display_spoof.h"
#include "vr_fake.h"
#include "vr_session.h"

#include <windows.h>
#include <algorithm>
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
std::atomic<uint32_t> g_runtimeRecenterEvents{0};
std::atomic<int> g_headTraceMode{1};
std::atomic<bool> g_fpPivotFix{true};
std::atomic<bool> g_recenterHandsFix{true};
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
std::atomic<bool> g_submitWithPose{true};  // 29/10 (issue #4): di nuovo acceso, senza la posa il mondo tremava sui PC lenti
// "submitpose auto" (predefinito dal 29/10): la posa solo quando il mondo tremerebbe
// senza (vr::pure::SubmitPoseAuto in vr_output.cpp); g_submitWithPose resta acceso
// (overscan OpenXR fisso, niente salti di scala).
std::atomic<bool> g_submitPoseAuto{true};
std::atomic<bool> g_submitPoseAutoOn{false};
std::atomic<float> g_xrOverscanDeg{1.5f};
std::atomic<bool> g_pairCheck{false};
std::atomic<float> g_turnWithHeadDeg{45.0f};
std::atomic<int32_t> g_ownYawUnits{0};
std::atomic<bool> g_stickTurning{false};
std::atomic<bool> g_coverSnapCompensate{true};
std::atomic<bool> g_coverPeekEnabled{true};
std::atomic<bool> g_coverPeek{false};
std::atomic<bool> g_coverLeanEnabled{true};
std::atomic<bool> g_turretAimHand{true};
std::atomic<int> g_turretGrip{0};             // auto (WI-4c)
std::atomic<bool> g_turretLimits{true};
// Taratura del manubrio e limiti per tipo di supporto (review WI-4c round 1: prima fissi nel codice).
std::atomic<float> g_turretBarMinCm{25.0f}, g_turretBarMaxCm{90.0f}, g_turretBarAlignDeg{35.0f}, g_turretBarSlopeDeg{40.0f};
std::atomic<float> g_turretPivotCm{35.0f};  // torrette a terra: perno davanti alle maniglie (visore 27/10)
std::atomic<float> g_turretBarBlendSec{0.10f}, g_turretBarSmoothHz{1.0f}, g_turretBarBeta{4.0f};
std::atomic<float> g_turretLimitDeg[2][4] = {{45.0f, 55.0f, 21.0f, 23.0f}, {50.0f, 65.0f, 18.0f, 25.0f}};
std::atomic<int> g_turretHoldNow{-1};
std::atomic<bool> g_turretGripsAreHandles{false};
std::atomic<bool> g_adsEnabled{true};
std::atomic<bool> g_adsActive{false};
std::atomic<bool> g_twoHandEngaged{false};
std::atomic<int> g_squadValidateLog{0};
std::mutex g_squadPickMutex;
std::string g_squadPickRequest;
std::string TakeSquadPickRequest() {
    std::lock_guard<std::mutex> lock(g_squadPickMutex);
    std::string r;
    r.swap(g_squadPickRequest);
    return r;
}
std::atomic<float> g_cupRel[3] = {0.5f, 2.5f, -4.5f};  // Z negativo = a sinistra dell'arma, Y sotto: scelta sugli screenshot (22/10)
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
std::atomic<bool> g_cinemaLaser{false}, g_cinemaLaserFlipY{false}, g_cinemaLaserFullUv{false};  // sperimentale: il gioco ricentra il mouse
std::atomic<float> g_gunCtrlPose[12];
std::atomic<bool> g_gunCtrlPoseValid{false};
std::atomic<bool> g_wristHud{true};
std::atomic<float> g_wristHudWidthM{0.09f};
std::atomic<float> g_supportFwd[3];
std::atomic<bool> g_supportFwdValid{false};
std::atomic<bool> g_cinemaRecenter{false};
std::atomic<float> g_forcedMotion{0.0f};
std::atomic<bool> g_heightAuto{true};
std::atomic<bool> g_seated{false};
std::atomic<int> g_seatedMode{2};  // auto (WI-3): g_seated diventa vero solo con la seduta riconosciuta
std::atomic<bool> g_seatedDetected{false};
// Chiamata dal thread di gioco (decisione dei seduti) e da quello del menu
// ("seated ..."): il calcolo va fatto sotto un mutex, se no un'alternanza
// lasciava g_seated vero col modo off o il contrario (WI-3 fix 1, review bassa).
// Ognuno scrive il suo ingresso prima di chiamarla: l'ultima chiamata vede entrambi.
void UpdateSeatedEffective() {
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    const int mode = g_seatedMode.load();
    g_seated = mode == 1 || (mode == 2 && g_seatedDetected.load());
}
std::atomic<float> g_eyeHeightAuto{-1.0f};
std::atomic<float> g_heightFixedM{-1.0f};
std::atomic<bool> g_heightCalibRequested{false};
std::atomic<bool> g_seatedCaptureRequested{false};
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
std::atomic<bool> g_grabProps{true};
std::atomic<bool> g_gravityGloves{true};
std::atomic<bool> g_grabHighlight{true};
std::atomic<bool> g_propZone[2]{false, false};
std::atomic<bool> g_propHeld[2]{false, false};
std::atomic<unsigned long long> g_propReleasedMs[2]{0, 0};
std::atomic<bool> g_gripRaw[2]{false, false};
std::atomic<bool> g_gripSim[2]{false, false};
std::atomic<float> g_throwBoost{1.3f};
std::atomic<int> g_throwMode{1};
std::atomic<double> g_poseStampMs{0.0};
std::atomic<float> g_grabZoneCm{8.0f};
std::atomic<float> g_massKgPerUnit{40.0f};
std::atomic<int> g_objectWeight{1};
std::atomic<bool> g_analogGrip{false};
std::atomic<float> g_gripAnalog[2]{0.0f, 0.0f};
std::atomic<float> g_synthGrip{-1.0f}, g_synthGripNoise{0.0f};
std::atomic<float> g_synthGripNext{-1.0f}, g_synthGripNextNoise{0.0f};
std::atomic<unsigned long long> g_synthGripAt{0};
std::atomic<float> g_synthGripNextRamp{0.0f};
std::atomic<int> g_synthGripGen{0};
std::atomic<int> g_impactHaptics{2};
std::atomic<int> g_hapticLog{0};
std::atomic<int> g_solidHands{1};  // held dal 26/10 (configversion 10)
std::atomic<int> g_solidLog{0};
std::atomic<float> g_supportHandSepCm{0.0f};
std::atomic<bool> g_grabRightHand{false};
std::atomic<int> g_grabLog{0};
std::atomic<bool> g_propCombat{true};
std::atomic<bool> g_propThrowDamage{true};
std::atomic<bool> g_propMelee{true};
std::atomic<bool> g_propGlass{true};
std::atomic<bool> g_propShield{true};
std::atomic<bool> g_propExplosive{true};
std::atomic<bool> g_weaponPickup{true};
std::atomic<bool> g_disarm{true};
std::atomic<bool> g_corpseDrag{true};
std::atomic<unsigned long long> g_disarmNearMs[2]{0, 0};
std::atomic<bool> g_fakeGrenade{true};
std::atomic<bool> g_grenadeBack{true};
std::atomic<bool> g_propNoise{false};  // voce 9: spento di default (il piano)
std::atomic<bool> g_noiseProbe{false};  // voce 9: sonda di prova (solo console)
std::atomic<int> g_hitLog{0};
std::atomic<float> g_fingerCurlForce{-1.0f};
std::atomic<int> g_fingerAxis{2}, g_thumbAxis{3};
std::atomic<int> g_graspDump{0};
std::atomic<bool> g_ham{false};
std::atomic<bool> g_cinema3d{false};
std::atomic<float> g_spreadAds{0.1f}, g_spreadTwoHand{0.5f}, g_spreadOneHand{1.5f};
std::atomic<bool> g_laser{false};
std::atomic<bool> g_gestureGunMelee{false};
std::atomic<int> g_vrMenu{1};
std::atomic<bool> g_vrMenuOpen{false};
std::atomic<bool> g_vdDetected{false};
std::atomic<bool> g_nonSteamExe{false};
std::atomic<bool> g_vrMenuFlipY{true};  // origine in basso a sinistra (verificato nel visore 21/10)
std::atomic<bool> g_waitBeforeSubmit{true};
std::atomic<unsigned long long> g_interactSimUntil[2]{0, 0};
std::atomic<int> g_hintsShown{0};
std::atomic<unsigned> g_objectHintsShown{0};
std::atomic<bool> g_weaponNeedsAim{false};
std::atomic<bool> g_handPredictAuto{false};
std::atomic<bool> g_crouchViewReal{true};
std::atomic<int> g_crouchViewMode{0};
std::atomic<float> g_crouchTime{0.35f};
std::atomic<bool> g_physicalCrouch{true};
std::atomic<float> g_hitTestZ{-1000.0f};
std::atomic<bool> g_snapFade{true};
std::atomic<bool> g_vrPauseRequest{false};
std::atomic<bool> g_vrProjectionDirty{false};
std::atomic<int32_t> g_headYawUnits{0};
std::atomic<bool> g_gameLocked{false};
std::atomic<bool> g_inCover{false};
std::atomic<bool> g_gamePaused{false};
std::atomic<bool> g_cinemaWalker3D{true};
std::atomic<bool> g_stockCalibRequest{false};
std::atomic<bool> g_holdMigrationNotice{false};
std::atomic<bool> g_settingsResetNotice{false};
std::atomic<bool> g_protube{true}, g_protubeSwap{false}, g_bhaptics{true};
std::atomic<bool> g_gameCrosshair{false};
std::atomic<bool> g_actionInput{true};
std::atomic<bool> g_perEyeViewState{true};
std::atomic<bool> g_offCenter{true};
std::atomic<bool> g_projFlip{false};
std::atomic<float> g_simCantDeg{0.0f};
std::atomic<bool> g_dynRes{true};
std::atomic<int> g_dynResMin{65};
std::atomic<float> g_dynResDown{1.0f}, g_dynResSlow{0.92f}, g_dynResUp{0.65f}, g_dynResDwellSec{4.0f}, g_dynResBackoffSec{20.0f};
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
std::atomic<int> g_predictFrameMode{2};
std::atomic<bool> g_predictFrameAutoOn{false};
std::atomic<float> g_predictMaxMs{150.0f};
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
std::atomic<float> g_fpNearPlane{3.0f};  // con fpbody braccia e arma vicine agli occhi restano visibili
std::atomic<bool> g_aimFollowsBody{true};
std::atomic<bool> g_aimHand{true};
std::atomic<int> g_holdMode{0};
std::atomic<float> g_gunAngleDeg{0.0f}, g_stockAngleDeg{0.0f}, g_gunYawDeg{0.0f};
std::atomic<bool> g_openXrGripFix{true};
std::atomic<float> g_openXrGripPitch{vr::pure::kGripPitchDeg};
std::atomic<float> g_synthAimYawDeg{0.0f};
std::atomic<float> g_synthAimPitchDeg{0.0f};
std::atomic<bool> g_aimMarker{true};  // mirino verde: acceso di default dal 22/10 (configversion 9), spegnibile dal menu
std::atomic<bool> g_headCollision{true};
std::atomic<float> g_snapTurnDeg{0.0f};
std::atomic<bool> g_stickPitch{false};
std::atomic<void*> g_playerController{nullptr};
std::atomic<float> g_vignette{0.0f};
std::atomic<float> g_motionAmount{0.0f};
std::atomic<float> g_hudScale{0.3f};
std::atomic<float> g_hudDepth{100.0f};
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
std::atomic<float> g_twoHandCm{8.0f};  // WI-4b: presa entro 8 cm dall'astina (prima 12 dalla linea del controller destro)
std::atomic<float> g_twoHandReleaseCm{15.0f};
std::atomic<float> g_twoHandDwellSec{0.15f};
std::atomic<float> g_twoHandFilterHz{1.0f};
std::atomic<float> g_twoHandGuardCm[3] = {0.0f, 0.0f, 0.0f};
std::atomic<float> g_stockTolCm{6.0f}, g_stockTolDeg{25.0f};
std::atomic<int> g_leftHandBusy{0};
std::atomic<bool> g_leftNearBarrel{false};
std::atomic<bool> g_twoHandShiftFix{true};
namespace {
std::mutex g_stockMountMutex;
bool g_stockMountValid = false;
float g_stockMount[6] = {};
}  // namespace
bool StockMount(float pos[3], float rot[3][4]) {
    float v[6];
    {
        std::lock_guard<std::mutex> lock(g_stockMountMutex);
        if (!g_stockMountValid) return false;
        for (int i = 0; i < 6; ++i) v[i] = g_stockMount[i];
    }
    vr::twohand::StockMountToPose(v, pos, rot);
    return true;
}
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
std::atomic<bool> g_updateBeta{false};
static std::mutex g_updateRepoMutex;
static std::string g_updateRepo;
std::string UpdateRepo() {
    std::lock_guard<std::mutex> lock(g_updateRepoMutex);
    return g_updateRepo;
}
std::atomic<bool> g_fpBody{true};
std::atomic<int> g_fpBodyMode{0};
std::atomic<float> g_neckModelCm[2] = {8.0f, 13.0f};
std::atomic<int> g_wi3TestMask{0};
std::atomic<float> g_neckSize{1.0f};
std::atomic<float> g_neckRingR{4.0f};
std::atomic<float> g_reachSlack{2.0f};
std::atomic<float> g_seatedBelowM{1.35f};
std::atomic<float> g_seatedStandRiseM{0.30f};
std::atomic<float> g_seatedStandHoldS{3.0f};
std::atomic<float> g_bodyAlignHoldS{0.4f};
std::atomic<float> g_bodyAlignDegS{40.0f};
std::atomic<bool> g_camOwnYaw{true};
std::atomic<bool> g_armsLead{true};
std::atomic<bool> g_bodyAlignWallStop{true};
std::atomic<bool> g_leftGrip{true};
// WI-3: con l'ancora del collo "bodyback" e' solo una regolazione fine (unita'
// indietro lungo il corpo, 0 = nessuna; prima -10, il corpo avanti perche' le
// braccia arrivassero ai controller); il secondo valore non si usa piu'.
std::atomic<float> g_bodyBack{0.0f};
std::atomic<float> g_bodyBackLook{0.0f};
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
std::atomic<float> g_synthLeftVelCm[3];
std::atomic<float> g_synthLeftRotDeg[3];
std::atomic<unsigned long long> g_synthLeftVelStart{0}, g_synthLeftVelUntil{0};
std::atomic<float> g_synthLeftRotVelDeg[3];
std::atomic<unsigned long long> g_synthLeftRotVelStart{0}, g_synthLeftRotVelUntil{0};
std::atomic<bool> g_synthSmooth{false};
std::atomic<float> g_synthRamp[7];
std::atomic<unsigned> g_synthRampSeq{0};
std::atomic<float> g_synthJumpCm[3];
std::atomic<unsigned long long> g_synthJumpAt{0};
std::atomic<unsigned long long> g_synthLeftLostUntil{0};
std::atomic<bool> g_synthRampRelease{false};
std::atomic<bool> g_synthComp{false};
std::atomic<float> g_fpsCap{0.0f};
// Collaudi senza visore dei bug del tester (25/10, WI-0).
std::atomic<bool> g_synthCtrlOn[2] = {false, false};
std::atomic<float> g_synthCtrlDeg[2][3];
std::atomic<float> g_synthHeightM{0.0f};
std::atomic<int> g_simEvent{0};
std::atomic<int> g_viewLogTicks{0}, g_bodyLogTicks{0};
std::atomic<bool> g_twoHandInfoRequest{false};
std::mutex g_bodyLogMutex;
std::string g_bodyLogBones = "Neck head Spine3 Spine2 Shoulder_Right Shoulder_Left Leg_Right Leg_Left Hips";
std::string BodyLogBones() {
    std::lock_guard<std::mutex> lock(g_bodyLogMutex);
    return g_bodyLogBones;
}
std::atomic<unsigned long long> g_gripSimOffAt[2]{0, 0};
std::atomic<int> g_breakDropMs{-1};
std::atomic<bool> g_gestureGrenade{true}, g_gestureReload{true}, g_gestureCover{true};
std::atomic<bool> g_gestureMelee{true};
std::atomic<bool> g_reloadPhysical{false};
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
    // Chiavi di taratura del WI-1 (review 25/10): qui e non nella catena di
    // Execute, che ha gia' il limite di annidamento del compilatore (C1061).
    auto clampf = [](float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); };
    if (cmd == "presentunhook") {
        vr::hooks::TestUnhookPresent();
        return true;
    }
    if (cmd == "xroverscan") {
        float v = 1.5f;
        in >> v;
        g_xrOverscanDeg = clampf(v, 0.0f, 5.0f);
        return true;
    }
    if (cmd == "predictframe") {
        std::string v;
        in >> v;
        const int m = v == "on" ? 1 : (v == "off" ? 0 : 2);
        if (g_predictFrameMode.exchange(m) != m) g_predictFrameAutoOn = false;  // auto riparte spento
        return true;
    }
    if (cmd == "predictmax") {
        float v = 150.0f;
        in >> v;
        g_predictMaxMs = clampf(v, 20.0f, 250.0f);
        return true;
    }
    if (cmd == "dynresdown" || cmd == "dynresslow" || cmd == "dynresup" || cmd == "dynresdwell" || cmd == "dynresbackoff") {
        float v = 0.0f;
        in >> v;
        if (cmd == "dynresdown") g_dynResDown = clampf(v, 0.5f, 2.0f);
        else if (cmd == "dynresslow") g_dynResSlow = clampf(v, 0.5f, 1.5f);
        else if (cmd == "dynresup") g_dynResUp = clampf(v, 0.2f, 0.95f);
        else if (cmd == "dynresdwell") g_dynResDwellSec = clampf(v, 1.0f, 30.0f);
        else g_dynResBackoffSec = clampf(v, 0.0f, 600.0f);
        return true;
    }
    if (cmd == "coverlean") {
        std::string v;
        in >> v;
        g_coverLeanEnabled = OnOff(cmd, v, true);
        return true;
    }
    if (cmd == "turretgrip") {
        // turretgrip auto|normal|gunstock (WI-4c, Basic "Turret grip"): NORMAL =
        // le due mani sulle maniglie girano la torretta come un manubrio (una
        // mano basta), GUNSTOCK = mira lungo il calcio come col fucile; auto =
        // segue holdmode ("Controller hold").
        std::string v;
        in >> v;
        for (auto& c : v) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (v != "auto" && v != "normal" && v != "gunstock")
            vr::log::Line("CONFIG: turretgrip " + v + " not understood (auto, normal or gunstock), using auto.");
        g_turretGrip = v == "normal" ? 1 : (v == "gunstock" ? 2 : 0);
        return true;
    }
    if (cmd == "turretlimits") {
        std::string v;
        in >> v;
        g_turretLimits = OnOff(cmd, v, true);
        return true;
    }
    if (cmd == "turretpivot") {
        // Torrette a terra: perno davanti alle maniglie (cm), vedi control.h.
        float v = 0.0f;
        if (!(in >> v) || !std::isfinite(v)) {
            vr::log::Line("CONFIG: turretpivot needs a number (cm).");
            return true;
        }
        g_turretPivotCm = clampf(v, 10.0f, 80.0f);
        return true;
    }
    if (cmd == "turretbarmin" || cmd == "turretbarmax" || cmd == "turretbaralign" || cmd == "turretbarslope" ||
        cmd == "turretbarblend" || cmd == "turretbarsmooth" || cmd == "turretbarbeta") {
        // Manubrio della torretta (WI-4c, Avanzate; review round 1): vedi control.h.
        float v = 0.0f;
        if (!(in >> v) || !std::isfinite(v)) {
            vr::log::Line("CONFIG: " + cmd + " needs a number.");
            return true;
        }
        if (cmd == "turretbarmin") g_turretBarMinCm = clampf(v, 10.0f, 60.0f);
        else if (cmd == "turretbarmax") g_turretBarMaxCm = clampf(v, 40.0f, 150.0f);
        else if (cmd == "turretbaralign") g_turretBarAlignDeg = clampf(v, 10.0f, 75.0f);
        else if (cmd == "turretbarslope") g_turretBarSlopeDeg = clampf(v, 10.0f, 75.0f);
        else if (cmd == "turretbarblend") g_turretBarBlendSec = clampf(v, 0.0f, 0.5f);
        else if (cmd == "turretbarsmooth") g_turretBarSmoothHz = clampf(v, 0.1f, 10.0f);
        else g_turretBarBeta = clampf(v, 0.0f, 20.0f);
        return true;
    }
    {
        // Limiti della torretta nel telaio del supporto (gradi): una chiave per
        // numero, come twohandguard* (il menu Avanzate passa solo il primo
        // numero di una riga). "turretlimitsground|turretlimitsflight <destra>
        // <sinistra> <su> <giu'>" resta come comando.
        static const char* const kSide[4] = {"right", "left", "up", "down"};
        for (int m = 0; m < 2; ++m) {
            const std::string base = m == 0 ? "turretground" : "turretflight";
            if (cmd == (m == 0 ? "turretlimitsground" : "turretlimitsflight")) {
                for (int i = 0; i < 4; ++i) {
                    float v = 0.0f;
                    if (!(in >> v) || !std::isfinite(v)) break;
                    g_turretLimitDeg[m][i] = clampf(v, 0.0f, i < 2 ? 180.0f : 89.0f);
                }
                return true;
            }
            for (int i = 0; i < 4; ++i) {
                if (cmd != base + kSide[i]) continue;
                float v = 0.0f;
                if (!(in >> v) || !std::isfinite(v)) {
                    vr::log::Line("CONFIG: " + cmd + " needs a number (degrees).");
                    return true;
                }
                g_turretLimitDeg[m][i] = clampf(v, 0.0f, i < 2 ? 180.0f : 89.0f);
                return true;
            }
        }
    }
    if (cmd == "toastdump") {
        // toastdump <cartella>: suggerimenti e guide della torretta in BMP, impaginazione nel log (review WI-4c round 1)
        std::string dir;
        in >> dir;
        if (!dir.empty()) vr::toast::DumpForTest(dir);
        return true;
    }
    if (cmd == "holdmode") {
        // holdmode normal|gunstock (WI-4a, Basic "Controller hold"): angolo
        // della canna da gunangle (controller nudi) o da stockangle (calcio,
        // B+Y). Anche la calibrazione B+Y scrive la chiave della presa attiva.
        std::string v;
        in >> v;
        for (auto& c : v) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (v != "normal" && v != "gunstock") vr::log::Line("CONFIG: holdmode " + v + " not understood (normal or gunstock), using normal.");
        g_holdMode = v == "gunstock" ? 1 : 0;
        return true;
    }
    if (cmd == "gunangle" || cmd == "stockangle" || cmd == "gunyaw") {
        // Gradi sul raggio di puntamento del controller (0 = come il laser del
        // menu). Si limita l'angolo, non l'aimpitch di prima (WI-4a).
        float v = 0.0f;
        if (!(in >> v) || !std::isfinite(v)) {
            vr::log::Line("CONFIG: " + cmd + " needs a number of degrees.");
            return true;
        }
        float lo = vr::pure::kGunAngleMin, hi = vr::pure::kGunAngleMax;
        if (cmd == "stockangle") lo = vr::pure::kStockAngleMin, hi = vr::pure::kStockAngleMax;
        if (cmd == "gunyaw") lo = -vr::pure::kGunYawMax, hi = vr::pure::kGunYawMax;
        const float c = clampf(v, lo, hi);
        if (c != v) vr::log::Line("CONFIG: " + cmd + " limited to " + std::to_string(c) + " degrees.");
        (cmd == "gunangle" ? g_gunAngleDeg : cmd == "stockangle" ? g_stockAngleDeg : g_gunYawDeg) = c;
        return true;
    }
    if (cmd == "twohandgrip" || cmd == "twohandrelease" || cmd == "twohanddwell" || cmd == "twohandsmooth") {
        // Presa a due mani (WI-4b, Avanzate): vedi control.h.
        float v = 0.0f;
        if (!(in >> v) || !std::isfinite(v)) {
            vr::log::Line("CONFIG: " + cmd + " needs a number.");
            return true;
        }
        if (cmd == "twohandgrip") g_twoHandCm = clampf(v, 3.0f, 20.0f);
        else if (cmd == "twohandrelease") g_twoHandReleaseCm = clampf(v, 5.0f, 40.0f);
        else if (cmd == "twohanddwell") g_twoHandDwellSec = clampf(v, 0.0f, 1.0f);
        else g_twoHandFilterHz = clampf(v, 0.2f, 10.0f);
        return true;
    }
    if (cmd == "twohandguard") {
        // twohandguard <avanti> <destra> <su> cm: spostamento dell'astina misurata (taratura nel visore).
        // Comando; nell'ini e nel menu Avanzate le tre chiavi qui sotto (review WI-4b round 1).
        float f = 0.0f, r = 0.0f, u = 0.0f;
        in >> f >> r >> u;
        g_twoHandGuardCm[0] = clampf(f, -30.0f, 30.0f);
        g_twoHandGuardCm[1] = clampf(r, -20.0f, 20.0f);
        g_twoHandGuardCm[2] = clampf(u, -20.0f, 20.0f);
        return true;
    }
    if (cmd == "twohandguardfwd" || cmd == "twohandguardright" || cmd == "twohandguardup") {
        // Una chiave per numero: il menu Avanzate passa solo il primo numero di
        // una riga (review WI-4b round 1: destra/su, dove cadono gli errori del
        // modello del palmo, si potevano cambiare solo nell'ini).
        float v = 0.0f;
        if (!(in >> v) || !std::isfinite(v)) {
            vr::log::Line("CONFIG: " + cmd + " needs a number (cm).");
            return true;
        }
        if (cmd == "twohandguardfwd") g_twoHandGuardCm[0] = clampf(v, -30.0f, 30.0f);
        else if (cmd == "twohandguardright") g_twoHandGuardCm[1] = clampf(v, -20.0f, 20.0f);
        else g_twoHandGuardCm[2] = clampf(v, -20.0f, 20.0f);
        return true;
    }
    if (cmd == "stocktol") {
        // stocktol <cm> [gradi]: presa col calcio entro tanto dalla posa salvata (WI-4b).
        float cm = 6.0f, deg = -1.0f;
        in >> cm >> deg;
        g_stockTolCm = clampf(cm, 1.0f, 20.0f);
        if (deg > 0.0f) g_stockTolDeg = clampf(deg, 5.0f, 60.0f);
        return true;
    }
    if (cmd == "stocktoldeg") {
        float deg = 25.0f;
        if (!(in >> deg) || !std::isfinite(deg)) {
            vr::log::Line("CONFIG: stocktoldeg needs a number (degrees).");
            return true;
        }
        g_stockTolDeg = clampf(deg, 5.0f, 60.0f);
        return true;
    }
    if (cmd == "twohandshift") {
        // twohandshift <0|1> (collaudi prima/dopo, review WI-4b round 1): lo stato della presa a due mani
        // e del manubrio nello spazio del tracciamento gira con un ricentramento del runtime assorbito.
        int v = 1;
        in >> v;
        g_twoHandShiftFix = v != 0;
        vr::log::Line(std::string("CONFIG: twohandshift ") + (g_twoHandShiftFix ? "1" : "0"));
        return true;
    }
    if (cmd == "stockmount") {
        // stockmount x y z yaw pitch roll (cm, gradi) | none: controller sinistro sul calcio nel raw
        // destro, scritto da B + Y con "holdmode gunstock" (WI-4b).
        std::string first;
        in >> first;
        float v[6] = {};
        bool ok = !first.empty() && first != "none" && first != "off";
        if (ok) {
            v[0] = static_cast<float>(std::atof(first.c_str()));
            for (int i = 1; i < 6 && ok; ++i) ok = static_cast<bool>(in >> v[i]) && std::isfinite(v[i]);
            ok = ok && std::isfinite(v[0]);
        }
        {
            std::lock_guard<std::mutex> lock(g_stockMountMutex);
            g_stockMountValid = ok;
            for (int i = 0; i < 6; ++i) g_stockMount[i] = v[i];
        }
        if (!ok && !first.empty() && first != "none" && first != "off") vr::log::Line("CONFIG: stockmount needs 6 numbers (cm and degrees) or none.");
        return true;
    }
    if (cmd == "stockcal") {
        // Collaudi (WI-4a): come B + Y tenuti 1 s (calibrazione della presa attiva).
        g_stockCalibRequest = true;
        return true;
    }
    if (cmd == "gravitygloves" || cmd == "grabhighlight") {
        std::string v;
        in >> v;
        (cmd == "gravitygloves" ? g_gravityGloves : g_grabHighlight) = OnOff(cmd, v, true);
        return true;
    }
    if (cmd == "grabprops" || cmd == "grabright") {
        std::string v;
        in >> v;
        (cmd == "grabprops" ? g_grabProps : g_grabRightHand) = OnOff(cmd, v, cmd == "grabprops");
        return true;
    }
    if (cmd == "throwboost" || cmd == "grabzone") {
        float v = 0.0f;
        in >> v;
        if (cmd == "throwboost" && v >= 0.5f && v <= 3.0f) g_throwBoost = v;
        if (cmd == "grabzone" && v >= 2.0f && v <= 30.0f) g_grabZoneCm = v;
        return true;
    }
    if (cmd == "throwmode") {
        // throwmode peak|classic: lancio dal picco della mano con polso, leva e
        // limite di energia (props.cpp, voce 4 del piano) o la media di 60 ms di prima.
        std::string v;
        in >> v;
        for (auto& c : v) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (v == "classic" || v == "0") {
            g_throwMode = 0;
        } else {
            if (v != "peak" && v != "1") vr::log::Line("CONFIG: throwmode " + v + " not understood (peak or classic), using peak.");
            g_throwMode = 1;
        }
        return true;
    }
    if (cmd == "throwenergy") {
        // throwenergy <J>: energia massima di un lancio con throwmode peak (ini, menu Advanced) = "griptune energy".
        float v = -1.0f;
        in >> v;
        if (v >= 0.0f && v <= 2000.0f) vr::props::SetGripTune("energy", v);
        else vr::log::Line("CONFIG: throwenergy out of range (0-2000 J), ignored.");
        return true;
    }
    {
        // Taratura del lancio (ini, menu Advanced; revisione della voce 4,
        // 26/10): gli stessi valori di "griptune peakmin|peakfull|peakout".
        static const struct {
            const char* key;
            const char* tune;
            float lo, hi;
        } kThrow[] = {{"throwplace", "peakmin", 0.0f, 5.0f}, {"throwfull", "peakfull", 0.0f, 10.0f}, {"throwglitch", "peakout", 0.0f, 5.0f}};
        for (const auto& e : kThrow) {
            if (cmd != e.key) continue;
            float v = -1.0f;
            in >> v;
            if (v >= e.lo && v <= e.hi) vr::props::SetGripTune(e.tune, v);
            else vr::log::Line("CONFIG: " + cmd + " out of range, ignored.");
            return true;
        }
    }
    if (cmd == "masskg") {
        // masskg <kg>: kg per unita' di massa del motore (GetBodyMass), solo nell'ini (props.cpp MassKg).
        float v = 0.0f;
        in >> v;
        if (v >= 5.0f && v <= 500.0f) g_massKgPerUnit = v;
        return true;
    }
    if (cmd == "objectweight") {
        // objectweight off|light|full: peso degli oggetti nella mano libera
        // (props.cpp, voce 2 del piano): off = la guida di prima, identica.
        std::string v;
        in >> v;
        for (auto& c : v) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (v == "off" || v == "0") {
            g_objectWeight = 0;
        } else if (v == "full" || v == "2") {
            g_objectWeight = 2;
        } else {
            if (v != "light" && v != "1") vr::log::Line("CONFIG: objectweight " + v + " not understood (off, light or full), using light.");
            g_objectWeight = 1;
        }
        return true;
    }
    if (cmd == "analoggrip") {
        // analoggrip on|off: forza della presa dal valore analogico del grip
        // (props.cpp GripStep, voce 6 del piano); solo con objectweight light/full.
        std::string v;
        in >> v;
        g_analogGrip = OnOff(cmd, v, false);
        return true;
    }
    if (cmd == "synthgrip") {
        // synthgrip <0..1> [rumore] [dopo ms] [rampa ms] | off: valore
        // analogico finto del grip per entrambe le mani (collaudi senza
        // controller della voce 6). Non prende ne' lascia: la presa resta
        // quella di grabsim / del grip vero. "dopo ms": il valore cambia fra
        // tanto (grip che si apre poco prima del rilascio di un lancio; rumore
        // e valore insieme); "rampa ms": da li' ci arriva in linea retta in
        // tanti ms (la mano che si apre piano; props.cpp SynthGripTick).
        // Orologio: QueryPerformanceCounter in ms come props.cpp ProfMs
        // (GetTickCount64 andava a scatti di 16 ms).
        std::string v;
        in >> v;
        float noise = 0.0f, ramp = 0.0f;
        int after = 0;
        in >> noise >> after >> ramp;
        char* e = nullptr;
        const float x = static_cast<float>(std::strtod(v.c_str(), &e));
        const bool off = v.empty() || v == "off" || e == v.c_str() || x < 0.0f;
        const float val = off ? -1.0f : (std::min)(1.0f, x);
        const float nz = off ? 0.0f : (std::max)(0.0f, (std::min)(0.5f, noise));
        g_synthGripGen.fetch_add(1);  // ferma una rampa in corso
        if (after > 0 || (ramp > 0.0f && !off)) {
            LARGE_INTEGER qf, qc;
            QueryPerformanceFrequency(&qf);
            QueryPerformanceCounter(&qc);
            const double nowMs = static_cast<double>(qc.QuadPart) * 1000.0 / static_cast<double>(qf.QuadPart);
            g_synthGripNext = val;
            g_synthGripNextNoise = nz;
            g_synthGripNextRamp = off ? 0.0f : (std::max)(0.0f, (std::min)(5000.0f, ramp));
            g_synthGripAt = static_cast<unsigned long long>(nowMs) + static_cast<unsigned long long>((std::max)(0, after)) + 1;
        } else {
            g_synthGripAt = 0;
            g_synthGrip = val;
            g_synthGripNoise = nz;
        }
        return true;
    }
    {
        // Taratura della forza della presa (ini, menu Advanced; voce 6): gli
        // stessi valori di "griptune gripmin|gripturn|gripdroop|griphold|
        // gripease|griprise|gripthrow|gripfloor|griplo|griphi". Fuori scala:
        // riportato dentro (revisione 26/10: era ignorato, e il menu Advanced
        // che scriveva 1.10 mostrava 1.10 mentre valeva ancora 1).
        static const struct {
            const char* key;
            const char* tune;
            float lo, hi;
        } kGrip[] = {{"analoggripmin", "gripmin", 0.05f, 1.0f},     {"analoggripturn", "gripturn", 0.05f, 1.0f},
                     {"analoggripdroop", "gripdroop", 1.0f, 4.0f},  {"analoggriphold", "griphold", 0.0f, 1000.0f},
                     {"analoggripease", "gripease", 0.0f, 1000.0f}, {"analoggriprise", "griprise", 0.0f, 500.0f},
                     {"analoggripthrow", "gripthrow", 0.0f, 1000.0f},
                     {"analoggripfloor", "gripfloor", 1.0f, 3.0f},  {"analoggriploose", "griplo", 0.0f, 0.95f},
                     {"analoggripfirm", "griphi", 0.1f, 1.0f}};
        for (const auto& e : kGrip) {
            if (cmd != e.key) continue;
            float v = 0.0f;
            if (!(in >> v) || !(v == v)) {  // non un numero (lo stream fallito mette 0)
                vr::log::Line("CONFIG: " + cmd + " needs a number, ignored.");
                return true;
            }
            const float c = (std::max)(e.lo, (std::min)(e.hi, v));
            if (c != v) {
                char b[160];
                std::snprintf(b, sizeof(b), "CONFIG: %s %.3g out of range (%.3g-%.3g), using %.3g.", e.key, v, e.lo, e.hi, c);
                vr::log::Line(b);
            }
            vr::props::SetGripTune(e.tune, c);
            return true;
        }
    }
    if (cmd == "solidhands") {
        // solidhands off|held|full: la mano libera disegnata segue la fisica
        // (props.cpp RenderHandPose, voce 5 del piano): held = oggetto tenuto e
        // punto afferrato di portiere e cartelli, full = anche la mano vuota
        // (la sua sfera); off = la mano sul controller, come prima.
        std::string v;
        in >> v;
        for (auto& c : v) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (v == "held" || v == "1") {
            g_solidHands = 1;
        } else if (v == "full" || v == "2") {
            g_solidHands = 2;
        } else {
            if (v != "off" && v != "0") vr::log::Line("CONFIG: solidhands " + v + " not understood (off, held or full), using off.");
            g_solidHands = 0;
        }
        return true;
    }
    {
        // Taratura della mano che segue la fisica (ini, menu Advanced; voce 5):
        // gli stessi valori di "griptune shdead|shturn|shcap|shcapturn|shin|shout".
        static const struct {
            const char* key;
            const char* tune;
            float lo, hi;
        } kSolid[] = {{"solidhandsdead", "shdead", 0.0f, 10.0f},   {"solidhandsturn", "shturn", 0.0f, 45.0f},
                      {"solidhandscap", "shcap", 1.0f, 40.0f},      {"solidhandscapturn", "shcapturn", 1.0f, 90.0f},
                      {"solidhandsin", "shin", 1.0f, 500.0f},       {"solidhandsout", "shout", 1.0f, 1000.0f}};
        for (const auto& e : kSolid) {
            if (cmd != e.key) continue;
            float v = -1.0f;
            in >> v;
            if (v >= e.lo && v <= e.hi) vr::props::SetGripTune(e.tune, v);
            else vr::log::Line("CONFIG: " + cmd + " out of range, ignored.");
            return true;
        }
    }
    if (cmd == "impacthaptics") {
        // impacthaptics off|low|full: vibrazione degli urti (props.cpp, voce 3
        // del piano): full = scalata sull'impulso, low = ampiezza a meta', off =
        // niente urti e gli impulsi fissi di prima per presa e oggetti urtati.
        std::string v;
        in >> v;
        for (auto& c : v) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (v == "off" || v == "0") {
            g_impactHaptics = 0;
        } else if (v == "low" || v == "1") {
            g_impactHaptics = 1;
        } else {
            if (v != "full" && v != "2") vr::log::Line("CONFIG: impacthaptics " + v + " not understood (off, low or full), using full.");
            g_impactHaptics = 2;
        }
        return true;
    }
    {
        // Taratura della vibrazione degli urti (ini, menu Advanced; revisione
        // della voce 3, 26/10): gli stessi valori di "griptune hapdv|hapj|...".
        static const struct {
            const char* key;
            const char* tune;
            float lo, hi;
        } kHap[] = {{"hapticdv", "hapdv", 0.2f, 5.0f},          {"hapticfull", "hapj", 0.5f, 50.0f},
                    {"hapticrefractory", "haprefr", 0.0f, 500.0f}, {"hapticrearm", "haprearm", 0.0f, 500.0f},
                    {"hapticwindow", "hapwindow", 5.0f, 300.0f},   {"hapticapproach", "hapapproach", 5.0f, 200.0f},
                    {"haptichandkg", "hapspherekg", 0.5f, 50.0f},  {"hapticslapkg", "hapslapkg", 0.1f, 20.0f},
                    {"hapticslapspeed", "hapspeed", 0.0f, 5.0f}};
        for (const auto& e : kHap) {
            if (cmd != e.key) continue;
            float v = -1.0f;
            in >> v;
            if (v >= e.lo && v <= e.hi) vr::props::SetGripTune(e.tune, v);
            else vr::log::Line("CONFIG: " + cmd + " out of range, ignored.");
            return true;
        }
    }
    if (cmd == "weightforce" || cmd == "weighttorque" || cmd == "weightsag" || cmd == "weightdroop" || cmd == "weightturn") {
        // Taratura del peso (ini, menu Advanced): gli stessi valori di "griptune force|torque|klin|krot|alphamin".
        float v = 0.0f;
        in >> v;
        const char* name = cmd == "weightforce" ? "force" : cmd == "weighttorque" ? "torque" : cmd == "weightsag" ? "klin" : cmd == "weightturn" ? "alphamin" : "krot";
        const float lo = cmd == "weightforce" ? 100.0f : cmd == "weighttorque" ? 2.0f : cmd == "weightsag" ? 200.0f : cmd == "weightturn" ? 0.5f : 5.0f;
        const float hi = cmd == "weightforce" ? 10000.0f : cmd == "weighttorque" ? 500.0f : cmd == "weightsag" ? 50000.0f : cmd == "weightturn" ? 100.0f : 2000.0f;
        if (v >= lo && v <= hi) vr::props::SetGripTune(name, v);
        else vr::log::Line("CONFIG: " + cmd + " out of range, ignored.");
        return true;
    }
    if (cmd == "weightbreak") {
        // Distacco degli oggetti incastrati con il peso (ini, menu Advanced): ms
        // di raddoppio del budget di forza = "griptune breaktime"; 0 = spento.
        float v = -1.0f;
        in >> v;
        if (v >= 0.0f && v <= 500.0f) vr::props::SetGripTune("breaktime", v);
        else vr::log::Line("CONFIG: weightbreak out of range (0-500 ms), ignored.");
        return true;
    }
    if (cmd == "weightwedge") {
        // Oggetto incastrato senza via d'uscita (ini, menu Advanced): secondi
        // fermo a 16 cm dal palmo prima che la mano lo lasci = "griptune
        // wedgetime" (piu' lontano prima, al piu' 0,3 s; 0 = mai). Dal seguito
        // 2 solo con il palmo fuori portata; predefinito 1 s (revisione).
        float v = -1.0f;
        in >> v;
        if (v >= 0.0f && v <= 10.0f) vr::props::SetGripTune("wedgetime", v);
        else vr::log::Line("CONFIG: weightwedge out of range (0-10 s), ignored.");
        return true;
    }
    if (cmd == "weightregrip") {
        // Oggetto bloccato con la mano ferma che lo tocca (ini, menu Advanced):
        // dopo tanti secondi la presa si riprende dove si trova = "griptune
        // regriptime" (0 = mai: come il seguito della voce 2, con weightwedge > 0).
        float v = -1.0f;
        in >> v;
        if (v >= 0.0f && v <= 5.0f) vr::props::SetGripTune("regriptime", v);
        else vr::log::Line("CONFIG: weightregrip out of range (0-5 s), ignored.");
        return true;
    }
    if (cmd == "synthleftrotvel") {
        // synthleftrotvel <pitch> <yaw> <roll> <ms>: mano sinistra finta che ruota (gradi/s) per <ms> (collaudi del peso).
        float p = 0, y = 0, r = 0;
        int ms = 0;
        in >> p >> y >> r >> ms;
        g_synthLeftRotVelDeg[0] = p;
        g_synthLeftRotVelDeg[1] = y;
        g_synthLeftRotVelDeg[2] = r;
        g_synthLeftRotVelStart = GetTickCount64();
        g_synthLeftRotVelUntil = GetTickCount64() + static_cast<unsigned long long>(ms > 0 ? ms : 0);
        return true;
    }
    if (cmd == "synthleftramp") {
        // synthleftramp <f> <r> <u> <v1> <ms1> <v2> <ms2> [release]: mano sinistra finta con una rampa di
        // velocita' lungo (f, r, u) (cm/s, ms; collaudi del lancio, voce 4 del piano).
        float d[3] = {0, 0, 0}, v1 = 0, t1 = 0, v2 = 0, t2 = 0;
        std::string rel;
        in >> d[0] >> d[1] >> d[2] >> v1 >> t1 >> v2 >> t2 >> rel;
        const float l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (!(l > 1e-6f) || t1 < 0.0f || t2 < 0.0f || t1 + t2 <= 0.0f) {
            vr::log::Line("CONFIG: synthleftramp needs a direction and durations, ignored.");
            return true;
        }
        for (int i = 0; i < 3; ++i) g_synthRamp[i] = d[i] / l;
        g_synthRamp[3] = v1;
        g_synthRamp[4] = t1;
        g_synthRamp[5] = v2;
        g_synthRamp[6] = t2;
        g_synthRampRelease = rel == "release";
        ++g_synthRampSeq;
        return true;
    }
    if (cmd == "synthsmooth") {
        // synthsmooth on|off: synthleftvel / synthleftrotvel col contatore ad alta risoluzione
        // (GetTickCount64 avanza a passi di ~16 ms: la mano finta andava a scatti). Collaudi.
        std::string v;
        in >> v;
        g_synthSmooth = OnOff(cmd, v, false);
        return true;
    }
    if (cmd == "synthcomp") {
        // synthcomp on|off: con le pose finte ogni rotazione del corpo sposta un
        // riferimento finto della testa (come copertura e torretta col visore):
        // vista e mani ferme nel mondo. Collaudi dei salti del riferimento (props.cpp).
        std::string v;
        in >> v;
        g_synthComp = OnOff(cmd, v, false);
        return true;
    }
    if (cmd == "synthctrl") {
        // synthctrl [left] <yaw> <pitch> <roll> | synthctrl [left] off: con le pose finte la mano (dell'arma o
        // la sinistra) diventa una matrice raw di controller con questi angoli (gradi, rispetto al corpo), che
        // passa dallo stesso calcolo della mira del visore (telaio dell'arma compreso, WI-4a) invece di synthaim / synthleftrot.
        std::string a;
        in >> a;
        int h = 0;
        if (a == "left") {
            h = 1;
            in >> a;
        } else if (a == "right") {
            in >> a;
        }
        if (a == "off" || a.empty()) {
            g_synthCtrlOn[h] = false;
        } else {
            float y = static_cast<float>(std::atof(a.c_str())), p = 0.0f, r = 0.0f;
            in >> p >> r;
            g_synthCtrlDeg[h][0] = y;
            g_synthCtrlDeg[h][1] = p;
            g_synthCtrlDeg[h][2] = r;
            g_synthCtrlOn[h] = true;
        }
        return true;
    }
    if (cmd == "wi3test") {
        // wi3test <maschera> (collaudi WI-3, A/B): 1 = corpo nella posizione fissa di prima (+10 avanti) invece
        // dell'ancora del collo; 2 = abbassamento del corpo con l'occhio della formula di prima; fix 1:
        // 4 = abbassamento con l'occhio vero senza il perno del collo (build WI-3); 8 = limite fisso della
        // portata (mai piu' di 4 unita' dietro +10) e niente arretramento per l'anello del collo (build WI-3).
        int v = 0;
        in >> v;
        g_wi3TestMask = v;
        vr::log::Line("CONFIG: wi3test " + std::to_string(v));
        return true;
    }
    if (cmd == "crouchtime") {
        // crouchtime <s> (WI-3): rampa della vista con l'accucciata del tasto; 0 = salto sotto un battito di ciglia.
        float v = 0.35f;
        in >> v;
        g_crouchTime = v < 0.0f ? 0.0f : (v > 1.5f ? 1.5f : v);
        return true;
    }
    if (cmd == "neckmodel") {
        // neckmodel <avanti cm> <su cm> (WI-3): occhi rispetto al perno del collo del giocatore (ancora del corpo).
        float f = 8.0f, u = 13.0f;
        in >> f >> u;
        g_neckModelCm[0] = f < 0.0f ? 0.0f : (f > 20.0f ? 20.0f : f);
        g_neckModelCm[1] = u < 0.0f ? 0.0f : (u > 25.0f ? 25.0f : u);
        return true;
    }
    // WI-3 fix 2: valori da regolare nel visore (Avanzate), vedi control.h.
    if (cmd == "necksize") {
        float v = 1.0f;
        in >> v;
        g_neckSize = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        return true;
    }
    if (cmd == "neckring") {
        float v = 4.0f;
        in >> v;
        g_neckRingR = v < 0.0f ? 0.0f : (v > 10.0f ? 10.0f : v);
        return true;
    }
    if (cmd == "reachslack") {
        float v = 2.0f;
        in >> v;
        g_reachSlack = v < 0.0f ? 0.0f : (v > 10.0f ? 10.0f : v);
        return true;
    }
    if (cmd == "seatedbelow") {
        float v = 1.35f;
        in >> v;
        g_seatedBelowM = v < 0.8f ? 0.8f : (v > 1.7f ? 1.7f : v);
        return true;
    }
    if (cmd == "bodyalign") {
        // bodyalign <s> (follow-up della regressione finale): testa ferma per tanto -> il corpo torna sotto; 0 = spento.
        float v = 0.4f;
        in >> v;
        g_bodyAlignHoldS = v < 0.0f ? 0.0f : (v > 5.0f ? 5.0f : v);
        return true;
    }
    if (cmd == "camownyaw" || cmd == "meshyaw") {  // "meshyaw": nome di prima (script del follow-up)
        std::string v;
        in >> v;
        g_camOwnYaw = OnOff(cmd, v, true);
        return true;
    }
    if (cmd == "armslead" || cmd == "bodyalignwall") {  // review 2 del follow-up: solo collaudi prima/dopo
        std::string v;
        in >> v;
        (cmd == "armslead" ? g_armsLead : g_bodyAlignWallStop) = OnOff(cmd, v, true);
        return true;
    }
    if (cmd == "bodyalignspeed") {
        float v = 40.0f;
        in >> v;
        g_bodyAlignDegS = v < 5.0f ? 5.0f : (v > 90.0f ? 90.0f : v);
        return true;
    }
    if (cmd == "seatedstand") {
        float r = 0.30f, h = 3.0f;
        in >> r >> h;
        g_seatedStandRiseM = r < 0.1f ? 0.1f : (r > 0.8f ? 0.8f : r);
        g_seatedStandHoldS = h < 1.0f ? 1.0f : (h > 30.0f ? 30.0f : h);
        return true;
    }
    if (cmd == "synthheight") {
        // synthheight <m>: altezza finta della testa sul pavimento (pose finte; runtime finto: la sua testa). 0 = off.
        float m = 0.0f;
        in >> m;
        g_synthHeightM = m > 0.3f && m < 2.5f ? m : 0.0f;
        if (g_synthHeightM > 0.0f && vr::rt::Active() == vr::rt::Backend::Fake) vr::fake::SetHeadHeight(m);
        // Spenta con le pose finte: altezza degli occhi di nuovo "non misurata", come prima del comando.
        if (g_synthHeightM <= 0.0f && g_mode == kModeSynthetic) g_eyeHeightAuto = -1.0f;
        return true;
    }
    if (cmd == "simevent") {
        // simevent zeropose: ricentramento del runtime simulato (gestito come l'evento vero, vr_session.cpp).
        std::string v;
        in >> v;
        if (v != "zeropose") vr::log::Line("CONFIG: simevent " + v + " not understood (zeropose).");
        else if (!vr::session::Ready()) vr::log::Line("CONFIG: simevent zeropose ignored: no VR session (headset or runtime fake).");
        else g_simEvent = 1;
        return true;
    }
    if (cmd == "fppivot") {
        // fppivot <0|1>: correzione del perno degli occhi (avanzamento fp sullo yaw del Pawn di adesso;
        // review WI-2, causa F). 0 solo per i collaudi prima/dopo.
        int v = 1;
        in >> v;
        g_fpPivotFix = v != 0;
        vr::log::Line(std::string("CONFIG: fppivot ") + (g_fpPivotFix ? "1" : "0"));
        return true;
    }
    if (cmd == "recenterhands") {
        // recenterhands <0|1>: nel tick tenuto di un salto del visore anche i controller saltati con lo spazio
        // tornano nello spazio di prima, posa dichiarata e riferimento delle mani per props.cpp senza i
        // ricentramenti del runtime assorbiti (review WI-2 round 2). 0 solo per i collaudi prima/dopo.
        int v = 1;
        in >> v;
        g_recenterHandsFix = v != 0;
        vr::log::Line(std::string("CONFIG: recenterhands ") + (g_recenterHandsFix ? "1" : "0"));
        return true;
    }
    if (cmd == "headtrace") {
        // headtrace <0|1|2>: trace della collisione della testa (collaudi, review WI-2): 0 = due passate
        // (senza Pawn e senza 0x200, piu' quella coi flag di prima), 1 = una passata con i Pawn senza 0x200,
        // 2 = le due passate con i Pawn nella prima.
        int v = 0;
        in >> v;
        g_headTraceMode = v < 0 || v > 6 || (v & 3) == 3 ? 1 : v;  // +4: muro davanti coi flag di prima (collaudi)
        vr::log::Line("CONFIG: headtrace " + std::to_string(g_headTraceMode.load()));
        return true;
    }
    if (cmd == "viewlog" || cmd == "bodylog") {
        // viewlog N: righe VIEW (composizione della vista per occhio) per N tick stereo.
        // bodylog N [ossa...]: righe BODY (ossa di Walker rispetto agli occhi e al frustum) per N tick.
        int n = 60;
        in >> n;
        if (cmd == "viewlog") {
            g_viewLogTicks = n < 0 ? 0 : n;
        } else {
            std::string rest, b;
            while (in >> b) rest += (rest.empty() ? "" : " ") + b;
            if (!rest.empty()) {
                std::lock_guard<std::mutex> lock(g_bodyLogMutex);
                g_bodyLogBones = rest;
            }
            g_bodyLogTicks = n < 0 ? 0 : n;
        }
        return true;
    }
    if (cmd == "twohandinfo") {
        // twohandinfo: una riga TWOHAND con la presa a due mani (punto dell'astina nel riferimento di synthleft).
        g_twoHandInfoRequest = true;
        return true;
    }
    if (cmd == "fake") {
        // fake <comando> ...: runtime finto (vr_fake.cpp, "runtime fake").
        std::string rest;
        std::getline(in, rest);
        if (vr::rt::Active() == vr::rt::Backend::Fake) vr::fake::Command(rest);
        else vr::log::Line("FAKE: \"fake" + rest + "\" ignored: the fake runtime is not active (runtime fake in specopsvr.ini, then restart).");
        return true;
    }
    if (cmd == "fpscap") {
        // fpscap <hz>: tick di gioco al piu' a <hz> (0 = libero). Collaudi senza visore (props.cpp).
        float v = 0.0f;
        in >> v;
        g_fpsCap = v > 1.0f && v < 1000.0f ? v : 0.0f;
        return true;
    }
    if (cmd == "grabsim") {
        // grabsim left|right on|off: grip finto (collaudi senza controller).
        std::string hand, v;
        in >> hand >> v;
        // "grabsim left off <ms>": rilascio fra <ms> (lancio durante un movimento).
        int h = hand == "right" ? 0 : 1;
        int ms = 0;
        in >> ms;
        if (v != "on" && v != "1" && ms > 0) {
            g_gripSimOffAt[h] = GetTickCount64() + static_cast<unsigned long long>(ms);
            return true;
        }
        g_gripSim[h] = (v == "on" || v == "1");
        g_gripSimOffAt[h] = 0;
        return true;
    }
    if (cmd == "synthleftrot") {
        float p = 0, y = 0, r = 0;
        in >> p >> y >> r;
        g_synthLeftRotDeg[0] = p;
        g_synthLeftRotDeg[1] = y;
        g_synthLeftRotDeg[2] = r;
        return true;
    }
    if (cmd == "synthleftvel") {
        // synthleftvel <avanti> <destra> <su> <ms>: mano sinistra finta in moto (cm/s) per <ms>.
        float f = 0, r = 0, u = 0;
        int ms = 0;
        in >> f >> r >> u >> ms;
        g_synthLeftVelCm[0] = f;
        g_synthLeftVelCm[1] = r;
        g_synthLeftVelCm[2] = u;
        g_synthLeftVelStart = GetTickCount64();
        g_synthLeftVelUntil = GetTickCount64() + static_cast<unsigned long long>(ms > 0 ? ms : 0);
        return true;
    }
    if (cmd == "synthleftjump") {
        // synthleftjump <avanti> <destra> <su> <ms>: fra <ms> la mano sinistra finta salta di (f, r, u) cm e
        // resta li' (salto del tracciamento: collaudo del filtro del lancio che ferisce, voce 1).
        float f = 0, r = 0, u = 0;
        int ms = 0;
        in >> f >> r >> u >> ms;
        g_synthJumpCm[0] = f;
        g_synthJumpCm[1] = r;
        g_synthJumpCm[2] = u;
        g_synthJumpAt = GetTickCount64() + static_cast<unsigned long long>(ms > 0 ? ms : 0);
        return true;
    }
    if (cmd == "synthleftlost") {
        // synthleftlost <ms>: la mano sinistra finta sparisce (tracciamento perso) per <ms> (collaudi della voce 8).
        int ms = 0;
        in >> ms;
        g_synthLeftLostUntil = GetTickCount64() + static_cast<unsigned long long>(ms > 0 ? ms : 0);
        return true;
    }
    if (cmd == "graspdump") {
        int n = 1;
        in >> n;
        g_graspDump = n;
        return true;
    }
    if (cmd == "fingercurl") {
        float v = -1.0f;
        in >> v;
        g_fingerCurlForce = v < 0.0f ? -1.0f : (std::min)(v, 1.0f);
        return true;
    }
    if (cmd == "fingeraxis") {
        int a = 3, t = 0;
        in >> a >> t;
        if (a != 0 && std::abs(a) <= 3) g_fingerAxis = a;
        if (t != 0 && std::abs(t) <= 3) g_thumbAxis = t;
        return true;
    }
    if (cmd == "breakdrop") {
        // Collaudo (distacco, props.cpp): "breakdrop <ms>" lascia l'oggetto (grip
        // finto spento) <ms> dopo che un distacco lo ha liberato; "breakdrop off".
        std::string v;
        in >> v;
        g_breakDropMs = (v == "off" || v.empty()) ? -1 : std::atoi(v.c_str());
        return true;
    }
    if (cmd == "grablog" || cmd == "hitlog" || cmd == "hapticlog" || cmd == "solidlog") {
        // hapticlog N: le prossime N uscite della vibrazione nel log ("HAPTIC out:", vr_input.cpp; senza visore i valori calcolati).
        // solidlog N: i prossimi N frame della mano disegnata (solidhands) nel log ("SOLID:", props.cpp).
        int n = 0;
        in >> n;
        (cmd == "grablog" ? g_grabLog : cmd == "hitlog" ? g_hitLog : cmd == "hapticlog" ? g_hapticLog : g_solidLog) = n;
        return true;
    }
    if (cmd == "propcombat") {
        // propcombat on|off: colpi degli oggetti lanciati o tenuti (props.cpp, pipeline dei colpi).
        std::string v;
        in >> v;
        g_propCombat = OnOff(cmd, v, true);
        return true;
    }
    if (cmd == "propthrowdamage") {
        // propthrowdamage on|off: gli oggetti lanciati che colpiscono un nemico lo feriscono, lo
        // fanno barcollare o lo atterrano (props.cpp ThrowDamage, voce 1 del piano; serve propcombat).
        std::string v;
        in >> v;
        g_propThrowDamage = OnOff(cmd, v, true);
        return true;
    }
    if (cmd == "propmelee") {
        // propmelee on|off: gli oggetti tenuti in mano colpiscono i nemici come la mischia di Walker (barcolla o
        // cade, danno da mischia che non uccide), la bottiglia si rompe in testa (props.cpp HeldMelee, voce 4 del
        // piano; serve propcombat).
        std::string v;
        in >> v;
        g_propMelee = OnOff(cmd, v, true);
        return true;
    }
    if (cmd == "propglass") {
        // propglass on|off: gli oggetti lanciati o tenuti rompono vetri, casse e finestre di
        // sabbia col danno del gioco (props.cpp WorldDamage, voce 2 del piano; serve propcombat).
        std::string v;
        in >> v;
        g_propGlass = OnOff(cmd, v, true);
        return true;
    }
    if (cmd == "griptune") {
        // griptune <palm|gap|finger|thumb|height|back|round|gain|relgrace|minspeed|pawnframe|force|torque|klin|krot|
        // sagcap|droopcap|brake|liftmin|pawnacc|overshoot|overtime|alphamin|fingerreach|framevel|breaktime|breakerr|
        // breakratio|breakgive|breakturn|passvolumes|wedgetime|breakkg|breakcap|regriptime|regripreach|regripmove|
        // regripsettle|stuckspeed|regriperr|hapdv|hapj|haprefr|haprearm|hapwindow|hapspherekg|hapspeed|peakwin|peakdt|
        // peakjump|energy|spinmax|spinjump|lever|peakmin|peakfull|peakout|shdead|shturn|shcap|shcapturn|shin|shout|shtele|shwall|shguard|
        // gripmin|gripturn|gripdroop|griphold|gripease|griprise|gripthrow|gripfloor|griplo|griphi> <valore>:
        // presa canonica, guida, peso, vibrazione degli urti, lancio dell'oggetto tenuto, mano che segue la fisica
        // (solidhands) e forza della presa (analoggrip) al volo (props.cpp).
        std::string name;
        float v = 0.0f;
        in >> name >> v;
        if (!vr::props::SetGripTune(name, v)) vr::log::Line("GRIP: griptune: unknown name " + name);
        return true;
    }
    if (cmd == "grabtest") {
        // grabtest <parte del nome della mesh | any>: presa di collaudo con la mano sinistra (props.cpp).
        std::string mesh;
        in >> mesh;
        if (!mesh.empty()) vr::props::QueueGrabTest(mesh);
        return true;
    }
    if (cmd == "propexplosive") {
        // propexplosive on|off: le bombole esplosive (YExplosiveActor: bombole di propano, di sub) si prendono, si
        // lanciano e esplodono dove sono col danno del gioco (props.cpp CarrierCreate / CarrierDamage, voce 5 del
        // piano); off = fuori dalla lista degli oggetti da prendere, come prima.
        std::string v;
        in >> v;
        g_propExplosive = OnOff(cmd, v, true);
        return true;
    }
    if (cmd == "fakegrenade") {
        // fakegrenade on|off: un oggetto piccolo lanciato che atterra entro 3 m da un nemico avvisa l'IA come una
        // granata vera (SendEvent_GrenadeImpact) e fa gridare "granata!" al piu' vicino; 10 s per gruppo
        // (props.cpp FakeGrenadeLanded, voce 7 del piano).
        std::string v;
        in >> v;
        g_fakeGrenade = OnOff(cmd, v, true);
        return true;
    }
    if (cmd == "propnoise") {
        // propnoise on|off: un oggetto che sbatte (lanciato, lasciato, urtato, rotto in mano) fa rumore per l'IA:
        // YAIEventPropagator.SendHearEvent_Melee nel punto dell'urto, al piu' uno al secondo (props.cpp PropNoise,
        // voce 9 del piano). Spento di default: puo' svegliare i nemici prima di quanto voglia il livello.
        std::string v;
        in >> v;
        g_propNoise = OnOff(cmd, v, false);
        return true;
    }
    if (cmd == "noiseprobe") {
        // noiseprobe on|off: solo per i test (revisione della voce 9). Conta i bot che registrano lo stimolo d'udito
        // di ogni rumore (hook su FUN_00E428A0, messo alla prima accensione; props.cpp noiseprobe).
        std::string v;
        in >> v;
        g_noiseProbe = OnOff(cmd, v, false);
        return true;
    }
    if (cmd == "grenadeback") {
        // grenadeback on|off: la sinistra prende una granata nemica ferma a terra (angoli rossi) e la rilancia;
        // la miccia continua a correre, rilanciata e' di Walker (props.cpp GrenadeGrab / GrenadeRelease, voce 8).
        std::string v;
        in >> v;
        g_grenadeBack = OnOff(cmd, v, true);
        return true;
    }
    if (cmd == "weaponpickup") {
        // weaponpickup on|off: la sinistra prende le armi lasciate dai nemici e le passa alla destra, o ne
        // prende le munizioni (props.cpp WeaponGrab / WeaponHandover / WeaponTakeAmmo, voce 6 del piano).
        std::string v;
        in >> v;
        g_weaponPickup = OnOff(cmd, v, true);
        return true;
    }
    if (cmd == "disarm") {
        // disarm on|off: colpire forte l'arma di un nemico (oggetto tenuto o lanciato, mano vuota) lo fa barcollare
        // e gliela fa cadere (props.cpp Disarm / DisarmTick, voce 10 del piano).
        std::string v;
        in >> v;
        g_disarm = OnOff(cmd, v, true);
        return true;
    }
    if (cmd == "corpsedrag") {
        // corpsedrag on|off: la sinistra afferra e trascina i nemici morti per un arto, la mano libera li spinge
        // (props.cpp CorpseGrab / CorpseDrive, voce 11 del piano).
        std::string v;
        in >> v;
        g_corpseDrag = OnOff(cmd, v, true);
        return true;
    }
    if (cmd == "corpsetest") {
        // corpsetest [bones]: collaudo, la sinistra afferra il corpo del cadavere piu' vicino al palmo (entro 2 m) senza
        // zona; "bones" scrive nel log ossa e posizioni dei corpi del cadavere piu' vicino.
        std::string v;
        in >> v;
        vr::props::QueueCorpseTest(v == "bones");
        return true;
    }
    if (cmd == "weapontest") {
        // weapontest: collaudo, la sinistra prende l'arma a terra piu' vicina (entro 3 m) senza zona.
        vr::props::QueueWeaponTest();
        return true;
    }
    if (cmd == "weapongrip") {
        // weapongrip x y z p y r: posa dell'arma raccolta nella mano sinistra (cm, gradi; collaudi).
        float off[3] = {0, 0, 0}, rot[3] = {0, 0, 0};
        in >> off[0] >> off[1] >> off[2] >> rot[0] >> rot[1] >> rot[2];
        vr::props::SetWeaponGrip(off, rot);
        return true;
    }
    if (cmd == "propshield") {
        // propshield on|off: gli oggetti tenuti fermano i colpi dei nemici, con spinta, vibrazione e
        // usura; off = i colpi li attraversano (props.cpp OnOtherShot / ShieldTick, voce 3 del piano).
        std::string v;
        in >> v;
        g_propShield = OnOff(cmd, v, true);
        return true;
    }
    if (cmd == "enemyfire") {
        // enemyfire 0x<pawn> [colpi] [x y z]: il Pawn spara con la sua arma il ciclo dei colpi del gioco
        // verso il punto, o verso l'oggetto tenuto (props.cpp EnemyFireTest, voce 3; solo collaudi).
        std::string a;
        int n = 1;
        float t[3] = {0.0f, 0.0f, 0.0f};
        in >> a >> n;
        const bool hasT = static_cast<bool>(in >> t[0] >> t[1] >> t[2]);
        const unsigned long addr = std::strtoul(a.c_str(), nullptr, 16);
        if (addr) vr::props::QueueEnemyFire(static_cast<uint32_t>(addr), n, hasT ? t : nullptr);
        return true;
    }
    if (cmd == "shieldtrace") {
        // shieldtrace on|off: off = oggetti tenuti fuori dalle trace a estensione zero (solo collaudi della
        // riscrittura dei colpi su Walker, props.cpp OnOtherShot).
        std::string v;
        in >> v;
        vr::props::SetShieldTraceTest(v != "off");
        return true;
    }
    if (cmd == "glasshit") {
        // glasshit 0x<attore> <kg> <m/s>: colpo finto su un vetro / mesh fratturabile / distruttibile
        // (props.cpp WorldDamage, voce 2; solo collaudi).
        std::string a;
        float kg = 0.0f, v = 0.0f;
        in >> a >> kg >> v;
        const unsigned long addr = std::strtoul(a.c_str(), nullptr, 16);
        if (addr && kg > 0.0f && v > 0.0f) vr::props::QueueGlassHit(static_cast<uint32_t>(addr), kg, v);
        return true;
    }
    if (cmd == "explhit") {
        // explhit <danno>: danno alla prima bombola esplosiva portata (0 = la fa esplodere; props.cpp CarrierDamage,
        // voce 5; solo collaudi).
        float d = -1.0f;
        in >> d;
        if (d >= 0.0f) vr::props::QueueExplHit(d);
        return true;
    }
    if (cmd == "spawnprop") {
        // spawnprop <mesh> [cm] [scala | sx sy sz]: oggetto di collaudo (props.cpp DoSpawn).
        std::string mesh;
        float cm = 30.0f, sc[3] = {1.0f, 1.0f, 1.0f};
        in >> mesh >> cm;
        if (in >> sc[0]) {
            if (!(in >> sc[1] >> sc[2])) sc[1] = sc[2] = sc[0];
        }
        for (float& c : sc)
            if (!(c >= 0.05f && c <= 20.0f)) c = 1.0f;
        if (!mesh.empty()) vr::props::QueueSpawn(mesh, cm, sc);
        return true;
    }
    if (cmd == "physicalcrouch") {
        std::string v;
        in >> v;
        g_physicalCrouch = OnOff(cmd, v, true);
        return true;
    }
    if (cmd == "hittest") {
        float z = 15.0f;
        in >> z;
        g_hitTestZ = z;
        return true;
    }
    if (cmd == "runtime") {
        // runtime steamvr|openxr|auto: backend VR, letto all'avvio (vr_runtime.cpp);
        // qui si registra solo la preferenza per il prossimo avvio.
        std::string v;
        in >> v;
        for (auto& c : v) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (v == "openvr") v = "steamvr";
        if (v == "fake") {
            // Runtime finto per i collaudi (vr_fake.cpp): profilo letto all'avvio da vr_runtime.cpp.
            if (vr::rt::Active() != vr::rt::Backend::Fake)
                vr::log::Line("CONFIG: runtime fake takes effect at the next game start (now: " +
                              std::string(vr::rt::BackendName()) + ").");
        } else if (v == "steamvr" || v == "openxr" || v == "auto") {
            vr::rt::Configure(v, "");
            if (vr::rt::Active() != vr::rt::Backend::None &&
                (v == "openxr") != (vr::rt::Active() == vr::rt::Backend::OpenXR) && v != "auto")
                vr::log::Line("CONFIG: runtime " + v + " takes effect at the next game start (now: " +
                              std::string(vr::rt::BackendName()) + ").");
        } else {
            vr::log::Line("CONFIG: runtime " + v + " not understood (steamvr, openxr, auto or fake).");
        }
    } else if (cmd == "openxrjson") {
        std::string rest;
        std::getline(in, rest);
        size_t a = rest.find_first_not_of(" 	");
        rest = a == std::string::npos ? "" : rest.substr(a);
        while (!rest.empty() && (rest.back() == ' ' || rest.back() == '	')) rest.pop_back();
        vr::rt::Configure("", rest == "auto" || rest == "off" ? std::string() : rest);
    } else if (cmd == "openxrgrip") {
        std::string v;
        in >> v;
        const bool fix = v != "off";
        const float pitch = !fix || v.empty() || v == "on" ? 20.6f : static_cast<float>(std::atof(v.c_str()));
        g_openXrGripFix = fix;  // telaio dell'arma (WI-4a): palmo e raggio senza misura
        g_openXrGripPitch = pitch;
        vr::xr::SetGripFix(fix, pitch);
    } else if (cmd == "audiolog") {
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
        else if (v == "cycle") {
            // vrmenu cycle <chiave> [+1|-1]: collaudo della voce come un clic (review 2 WI-1)
            std::string key;
            int dir = 1;
            in >> key >> dir;
            if (!key.empty()) vr::menu::TestCycle(key, dir);
        }
        else if (v == "dump") {
            // vrmenu dump <file.bmp> [basic|advanced|controls] [vd|novd]: la scheda in BMP (review WI-4c round 1)
            std::string path, tab, vd;
            in >> path >> tab >> vd;
            if (!path.empty()) vr::menu::TestDump(path, tab, vd);
        }
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
    } else if (cmd == "objecthints") {
        // objecthints N: suggerimenti contestuali sugli oggetti gia' visti (bit, vr_toast HintsTick);
        // 0 = si rivedono tutti.
        unsigned n = 0;
        in >> n;
        g_objectHintsShown = n;
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
    } else if (cmd == "cinemalaser") {
        // cinemalaser on|off | flipy on|off | uv band|full
        std::string v, w;
        in >> v >> w;
        if (v == "flipy") g_cinemaLaserFlipY = (w == "on");
        else if (v == "uv") g_cinemaLaserFullUv = (w == "full");
        else g_cinemaLaser = OnOff(cmd, v, true);
    } else if (cmd == "wristhud") {
        // wristhud on|off [larghezza m]
        std::string v;
        float w = 0.0f;
        in >> v >> w;
        g_wristHud = OnOff(cmd, v, true);
        if (w >= 0.04f && w <= 0.3f) g_wristHudWidthM = w;
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
        // crouchview auto|real|game (WI-3): auto = l'accucciata del tasto abbassa la
        // vista in crouchtime s, quella vera solo con la testa; real = solo la testa
        // vera (prima); game = la vista scende col gioco.
        std::string v;
        in >> v;
        g_crouchViewMode = v == "game" ? 2 : (v == "real" ? 1 : 0);
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
        // seated auto|on|off (WI-3): auto = seduti riconosciuti all'inizio della scena e a ogni ricentramento.
        std::string v;
        in >> v;
        const int before = g_seatedMode.load();
        g_seatedMode = v == "auto" ? 2 : (OnOff(cmd, v, false) ? 1 : 0);
        if (g_seatedMode == 2 && before != 2) g_seatedCaptureRequested = true;  // scelto dal menu: si misura subito
        UpdateSeatedEffective();
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
        float scale = 0.4f, depth = 100.0f;
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
        // Sempre attivo (24/10): un "autoupdate off" rimasto nell'ini (anche
        // quello scritto dal vecchio rollback.bat) non conta piu'.
        std::string v;
        in >> v;
        if (!OnOff(cmd, v, true)) vr::log::Line("CONFIG: \"autoupdate off\" ignored: updates are always on.");
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
        // fpbody full|arms (WI-3; on = full, off = arms): cosa si disegna, la gestione del corpo vale sempre.
        std::string v;
        in >> v;
        g_fpBodyMode = (v == "arms" || v == "off" || v == "0") ? 1 : 0;
        g_fpBody = g_fpBodyMode == 0;
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
        if (in >> cm && cm > 0.0f) vr::log::Line("CONFIG: leftgrip <cm> is no longer used: the drawn grip follows the two-handed grip (twohandgrip).");
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
        // twohand on|off [cm] [Hz] (sviluppo, WI-4b): presa entro <cm> dall'astina, taglio del filtro da fermi.
        // Il terzo valore era il filtro in secondi (fino a 0,5): ora Hz (0,2-10).
        std::string v;
        float cm = 0.0f, hz = -1.0f;
        in >> v >> cm >> hz;
        g_twoHand = OnOff(cmd, v, true);
        if (cm >= 3.0f && cm <= 40.0f) g_twoHandCm = cm;
        if (hz >= 0.2f && hz <= 10.0f) g_twoHandFilterHz = hz;
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
        // Chiave di prima (fino al 25/10, WI-4a): angolo della canna sul raw -Z
        // dei Touch. La migrazione della configversion 11 (10 nella copia B) la toglie dall'ini;
        // come comando resta la stessa canna di prima: calcio con stockangle
        // X + 39.4 (anche 0: canna lungo il raw, 39,4 gradi sopra il raggio).
        float v = 0.0f;
        in >> v;
        // Oltre +-45 gradi: valore della calibrazione sbagliata della 0.9.3
        // (anche -67/-96): si torna a 0.
        if (v < -45.0f || v > 45.0f) {
            vr::log::Line("CONFIG: aimpitch " + std::to_string(v) + " out of range (bad 0.9.3 calibration), using 0.");
            v = 0.0f;
        }
        g_holdMode = 1;
        g_stockAngleDeg = v - vr::pure::kAimPitchInRawDeg;
        char b[200];
        std::snprintf(b, sizeof(b), "CONFIG: aimpitch %.1f is the old setting: holdmode gunstock, stockangle %.1f (the same gun angle as before).",
                      v, g_stockAngleDeg.load());
        vr::log::Line(b);
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
        // push | off (WI-2; "on" = push, il nome di prima). push: la testa si
        // ferma sulla superficie (solo la parte che entra, il resto 1:1). "fade"
        // (nero man mano che la testa entra) richiede un'API nuova di
        // dissolvenza: per ora vale push.
        std::string v;
        in >> v;
        if (v == "push" || v == "fade") {
            g_headCollision = true;
            if (v == "fade") vr::log::Line("CONFIG: \"headcollision fade\" is not available yet, using push.");
        } else {
            g_headCollision = OnOff(cmd, v, true);
        }
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
    } else if (cmd == "squadpick") {
        // squadpick <parte della classe>: chiama la convalida del bersaglio su un Pawn vivo di quella classe (diagnostica)
        std::string v;
        in >> v;
        std::lock_guard<std::mutex> lock(g_squadPickMutex);
        g_squadPickRequest = v;
        g_squadValidateLog = 120;
    } else if (cmd == "audioroute") {
        // audioroute where: su quale dispositivo sta la sessione audio del gioco (diagnostica);
        // audioroute reopen [driver]: riapre l'uscita FMOD (0 = predefinito di Windows)
        std::string what;
        in >> what;
        if (what == "reopen") {
            int driver = 0;
            in >> driver;
            vr::audio::RequestReopen(driver);
        } else {
            vr::log::Line("AUDIO: game audio session on " + vr::audio::GameAudioDevices() + "; Windows default: " +
                          vr::audio::DefaultDeviceName() + ".");
        }
    } else if (cmd == "cuprel") {
        // cuprel <x> <y> <z>: presa avvolgente delle pistole, taratura (unita' nel riferimento della mano destra)
        float x = 0, y = 0, z = 0;
        if (in >> x >> y >> z) {
            g_cupRel[0] = x;
            g_cupRel[1] = y;
            g_cupRel[2] = z;
        }
    } else if (cmd == "uprops") {
        // uprops <oggetto> [<parte del nome>]: proprieta' per riflessione nel log (diagnostica)
        std::string rest;
        std::getline(in, rest);
        size_t a = rest.find_first_not_of(' ');
        if (a != std::string::npos) vr::script::QueueProps(rest.substr(a));
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
        g_submitPoseAuto = v == "auto";
        g_submitWithPose = v == "auto" ? true : OnOff(cmd, v, true);
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
        // Ritirato (configversion 8): gli ordini alla squadra provano sempre
        // canna, mano di supporto e sguardo. Accettato per gli ini vecchi.
        std::string v;
        in >> v;
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

// Presa effettiva: su una torretta mirata col controller quella della torretta
// ("turretgrip", WI-4c), altrimenti holdmode.
int EffectiveHoldMode() {
    const int t = g_turretHoldNow.load();
    return t >= 0 ? t : g_holdMode.load();
}
float HoldAngleDeg() { return EffectiveHoldMode() == 1 ? g_stockAngleDeg.load() : g_gunAngleDeg.load(); }

// Telaio dell'arma per la mano fisica (WI-4a). Raggio di puntamento: misurato
// dal backend OpenXR (posa aim nello spazio della mano, vr_openxr
// MeasuredAimInRaw), altrimenti la costante dei Touch (OpenVR e runtime finto)
// o openxrgrip - 60 (OpenXR prima della misura). gunyaw col segno della mano:
// + = verso l'esterno (destra per la destra, sinistra per la sinistra).
vr::pure::GunFrameParams GunFrameFor(int physicalHand, bool gunAngle, const char** source) {
    vr::pure::GunFrameParams p;
    const bool left = physicalHand == 1;
    const bool openxr = vr::rt::Active() == vr::rt::Backend::OpenXR;
    p.left = left;
    p.gripFix = !openxr || g_openXrGripFix.load();
    p.aim = vr::pure::DefaultAimInRaw(openxr, g_openXrGripFix.load(), g_openXrGripPitch.load());
    const char* src = openxr ? "OpenXR, openxrgrip model (not measured yet)" : "Touch model";
    float mp = 0.0f, my = 0.0f, mr = 0.0f;
    if (openxr && vr::xr::MeasuredAimInRaw(left ? 0 : 1, &mp, &my, &mr)) {
        p.aim.pitch = mp;
        p.aim.yaw = my;
        p.aim.roll = mr;
        src = "OpenXR aim pose, measured";
    }
    if (source) *source = src;
    if (gunAngle) {
        p.angleDeg = HoldAngleDeg();
        p.yawDeg = g_gunYawDeg.load() * (left ? -1.0f : 1.0f);
    }
    return p;
}


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

const char* DefaultIniText() { return kDefaultIni; }

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
// 7 (22/10): options that must simply work (two-handed grip, left grip, cover
// snap, ...) and the experiments (menu169, cinema3d, ham) are no longer in
// the ini: they stay as hidden commands, out of the in-headset menu.
// 8 (22/10): "reload physical" -> simple (manual reload off by default);
// "commandaim" retired: squad orders always try gun, support hand and head;
// "hud 0.5 100" -> 0.4 (tester 22/10: HUD at the edge of the view, wanted 80%).
// 9 (22/10 evening, user request): "marker off" -> on (green marker back on by
// default, can be turned off in the menu); "runtime steamvr" -> auto (OpenXR
// directly when the active runtime is not SteamVR: VDXR holds 120 Hz Godlike).
// 10 (26/10, decisione dell'orchestratore): "solidhands off" -> held (con il
// peso acceso di default la mano disegnata sul controller si chiudeva nel
// vuoto a 12-20 cm dall'oggetto pesante che restava indietro).
// 11 (27/10, merge dei fix del tester della copia B): nella copia B questi
// cambi erano la versione 10, lo stesso numero della 10 qui sopra con cambi
// diversi; un ini gia' alla 10 (quello dell'utente) non li avrebbe mai avuti.
// Ora sono la 11 (vr::pure::kTesterFixesConfigVersion). Le note della copia B:
// 11 (25/10, tester VR fixes): "headcollision on" -> push (WI-2: the value
// names the behaviour; Basic menu "Head in walls" push/off).
// WI-4a (26/10): "aimpitch X" (angle on the Touch raw -Z, 39.4 deg above the
// pointing ray) -> "holdmode normal" + "gunangle 0" for 0, or "holdmode
// gunstock" + "stockangle X+39.4" for a calibration (the same gun as before).
// WI-3 (26/10): "seated off" -> auto (sitting detected at the start of the scene
// and at every recenter), "crouchview real" -> auto (the crouch button lowers the
// view smoothly), "fpbody on/off" -> full/arms (vr::pure::kWi3ConfigChanges).
// 12 (0.10.0, 27/10): un ini piu' vecchio si mette da parte e si riparte dai
// predefiniti distribuiti (vr::pure::kResetConfigVersion, LoadConfigFile): i
// cambi qui sopra valgono solo se il salvataggio non riesce.
// 13 (29/10, issue #4 "jitterfest"): "submitpose off" -> auto (la posa quando serve);
// "hud 0.4 100" -> 0.3 (tester 29/10: icona di arma e munizioni da guardare
// all'estremo in basso a destra, col visore premuto sul viso). Senza la posa il
// compositor riproiettava ogni frame dalla posa che il runtime aveva predetto
// per quel vsync, non da quella con cui il gioco aveva disegnato: il mondo
// tremava di 0.3-0.6 deg rms appena il gioco scendeva sotto la frequenza del
// visore (runtime finto, openxr 90 Hz a 80/60/45 fps; openvr 72 Hz anche a fps
// piena); con la posa 0.004 deg. Sui PC veloci quasi non si vedeva.
constexpr int kConfigVersion = 13;
static_assert(kConfigVersion > vr::pure::kResetConfigVersion, "configversion: 12 = reset 0.10.0, 13 = submitpose auto");
// "submitpose" resta qui di proposito: la rimozione vale solo per gli ini
// prima della versione 8 (righe del comando nascosto, anche un vecchio
// "submitpose on" messo a mano); dopo arriva la riga distribuita. Dal WI-1
// (25/10) la chiave e' di nuovo nell'ini distribuito ("submitpose off",
// Advanced), non piu' un comando nascosto (review WI-1).
const char* const kRetiredKeys[] = {"twohand", "leftgrip", "menu169", "cinema3d", "ham", "mode", "gun", "arms", "muzzleaim",
                                    "aimassist", "gamecrosshair", "6dof", "bodyfollow", "posscale", "near", "viewport",
                                    "offcenter", "eyestate", "submitpose", "handpredict", "coversnap", "cinemaalign",
                                    "keepfocus", "execassist", "bodyback", "eyesmooth", "bodyfacing", "fp", "commandaim"};

void MigrateConfig(std::vector<std::string>& lines, const std::string& path) {
    // Ogni cambio vale solo per gli ini piu' vecchi della sua versione: chi era
    // gia' alla 2 e aveva rimesso a mano "submitpose on" non viene toccato.
    static const vr::pure::ConfigChange kChanges[] = {
        {2, "submitpose on", "submitpose off"}, {2, "predict off", "predict auto"}, {3, "bodyback 6 10", "bodyback -10 26"},
        {4, "bodyback -10 10", "bodyback -10 26"}, {5, "hud 0.6 150", "hud 0.5 100"},
        {6, "marker on", "marker off"},  // 21/10: mirino verde spento di default (mirini veri + laser opzionale)
        {8, "reload physical", "reload simple"}, {8, "hud 0.5 100", "hud 0.4 100"},
        {9, "marker off", "marker on"}, {9, "runtime steamvr", "runtime auto"},
        {10, "solidhands off", "solidhands held"},
        {vr::pure::kTesterFixesConfigVersion, "headcollision on", "headcollision push"},
        {13, "submitpose off", "submitpose auto"}, {13, "hud 0.4 100", "hud 0.3 100"}};
    // WI-4a: il commento di aimpitch (4 righe) diventa quello di holdmode
    // (vr::pure::kAimpitchCommentChanges), la riga dopo MigrateAimpitchLine.
    std::vector<vr::pure::ConfigChange> changes(std::begin(kChanges), std::end(kChanges));
    // WI-3: seduti auto, vista dell'accucciata auto, fpbody full/arms (vr::pure::kWi3ConfigChanges).
    changes.insert(changes.end(), std::begin(vr::pure::kWi3ConfigChanges), std::end(vr::pure::kWi3ConfigChanges));
    changes.insert(changes.end(), std::begin(vr::pure::kAimpitchCommentChanges), std::end(vr::pure::kAimpitchCommentChanges));
    // Cambi calcolati (il valore nuovo dipende dal vecchio): "aimpitch X" ->
    // presa + angolo sul raggio di puntamento (WI-4a, vr::pure::MigrateAimpitchLine).
    // Review WI-4b round 1: "twohand on <cm> [s]" / "leftgrip on <cm>" scritte a
    // mano (la riga AIM le suggeriva) cambiavano significato in silenzio (cm =
    // raggio dal palmo, secondi = Hz): restano solo on/off (vr::pure::MigrateTwoHandLine).
    static const vr::pure::ComputedChange kComputed[] = {
        {vr::pure::kTesterFixesConfigVersion, "aimpitch", &vr::pure::MigrateAimpitchLine},
        {vr::pure::kTesterFixesConfigVersion, "twohand", &vr::pure::MigrateTwoHandLine},
        {vr::pure::kTesterFixesConfigVersion, "leftgrip", &vr::pure::MigrateTwoHandLine}};
    std::string done;
    int removed = 0;
    if (vr::pure::ConfigVersionOf(lines) < 8)
        removed = vr::pure::RemoveRetiredKeys(lines, kRetiredKeys, sizeof(kRetiredKeys) / sizeof(kRetiredKeys[0]));
    if (!vr::pure::MigrateConfigLines(lines, kConfigVersion, changes.data(), changes.size(), &done, kComputed,
                                      sizeof(kComputed) / sizeof(kComputed[0])))
        return;
    if (removed) done += " " + std::to_string(removed) + " retired options removed;";
    // Review WI-4a (round 1): chi aveva un aimpitch calibrato (B + Y, anche
    // senza calcio: era l'unica correzione dei 39 gradi) ora ha la presa a
    // calcio; un avviso glielo dice (hooks.cpp, dopo 10 s di gioco).
    if (done.find("\"holdmode gunstock") != std::string::npos) g_holdMigrationNotice = true;
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
// avvio lento "updatechannel" o "protube off" arrivavano dopo (audit 19/10).
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
        if (key == "updatechannel" || key == "updaterepo") {
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
    // 0.10.0 (vr::pure::kResetConfigVersion): l'ini di una versione precedente
    // resta accanto come specopsvr.ini.bak-<data> (lo stesso nome di
    // reset_settings.bat) e si riparte dall'ini distribuito. Se la copia non
    // riesce l'ini non si tocca e lo si migra come prima.
    if (vr::pure::NeedsConfigReset(lines)) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        char stamp[32];
        std::snprintf(stamp, sizeof(stamp), "%04u%02u%02u_%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                      st.wSecond);
        const std::string bak = path + ".bak-" + stamp;
        const int oldVersion = vr::pure::ConfigVersionOf(lines);
        if (CopyFileA(path.c_str(), bak.c_str(), TRUE)) {
            std::vector<std::string> defaults;
            std::istringstream all(kDefaultIni);
            std::string l;
            while (std::getline(all, l)) {
                if (!l.empty() && l.back() == '\r') l.pop_back();
                defaults.push_back(l);
            }
            WriteIniLines(path, defaults);
            lines.swap(defaults);
            g_settingsResetNotice = true;
            vr::log::Line("CONFIG: specopsvr.ini of configversion " + std::to_string(oldVersion) +
                          " reset to the defaults of this version; your old settings are kept in " +
                          bak.substr(bak.find_last_of("\\/") + 1) + ".");
        } else {
            vr::log::Line("CONFIG: could not save a copy of specopsvr.ini (error " + std::to_string(GetLastError()) +
                          "): not reset, migrated instead.");
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
    vr::script::KeepJoystickInput();
    // Copia non Steam: avviso nel visore all'avvio e dopo 90 s (la prima volta
    // la sessione VR puo' non esserci ancora).
    if (g_nonSteamExe) {
        static int shown = 0;
        static ULONGLONG firstPoll = now;
        if ((shown == 0) || (shown == 1 && now - firstPoll > 90000)) {
            ++shown;
            const int missing = static_cast<int>(vr::addr::Count) - vr::addr::ResolvedCount();
            vr::toast::Notify(missing == 0 ? "Non-Steam copy of the game: all game addresses found"
                                           : "Non-Steam copy: " + std::to_string(missing) +
                                                 " game addresses not found, some features are off (see the log)");
        }
    }

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
