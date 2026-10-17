#include "vr_session.h"
#include "control.h"
#include "logger.h"

#include <windows.h>
#include <openvr.h>

#include <atomic>
#include <string>
#include <thread>

namespace vr::session {

namespace {

SRWLOCK g_lock = SRWLOCK_INIT;
std::atomic<bool> g_ready{false};
std::atomic<bool> g_scene{false};

const char* TypeName(vr::EVRApplicationType t) { return t == vr::VRApplication_Scene ? "Scene" : "Background"; }

// Da chiamare con il lock esclusivo (o prima che esistano altri utenti).
bool InitAs(vr::EVRApplicationType type) {
    vr::EVRInitError err = vr::VRInitError_None;
    vr::VR_Init(&err, type);
    if (err != vr::VRInitError_None) {
        vr::log::Line(std::string("OpenVR ") + TypeName(type) + " not available (" +
                      vr::VR_GetVRInitErrorAsEnglishDescription(err) + ").");
        g_ready = false;
        g_scene = false;
        return false;
    }
    g_ready = true;
    g_scene = (type == vr::VRApplication_Scene);
    vr::log::Line(std::string("OpenVR initialised as ") + TypeName(type) + ".");
    return true;
}

void Switch(vr::EVRApplicationType type) {
    AcquireSRWLockExclusive(&g_lock);
    vr::log::Line(std::string("OpenVR: switching to ") + TypeName(type) + "...");
    if (g_ready) vr::VR_Shutdown();
    g_ready = false;
    if (!InitAs(type) && type == vr::VRApplication_Scene) InitAs(vr::VRApplication_Background);
    // Visore appena indossato: "avanti" = dove si sta guardando adesso.
    if (g_scene) vr::control::g_recenterRequested = true;
    ReleaseSRWLockExclusive(&g_lock);
}

// Controlla lo stato del visore e cambia tipo di applicazione quando serve.
// Visore "presente" con isteresi. Con Quest Link lo stato 0 (Idle) arriva
// anche col visore in testa quando si sta fermi qualche secondo (17/10:
// l'immagine si fermava subito dopo la cinematica iniziale): Idle toglie il
// visore solo dopo 2 minuti senza movimento; lo standby vero (3, 4) dopo 3 s.
std::atomic<bool> g_present{false};

void MonitorLoop() {
    ULONGLONG inactiveSince = 0, lastActive = 0, standbySince = 0;
    for (;;) {
        Sleep(500);
        if (!g_ready) continue;
        bool active = false;
        {
            Shared s;
            vr::IVRSystem* sys = vr::VRSystem();
            if (!sys) continue;
            vr::EDeviceActivityLevel level = sys->GetTrackedDeviceActivityLevel(vr::k_unTrackedDeviceIndex_Hmd);
            ULONGLONG t = GetTickCount64();
            bool interacting = level == vr::k_EDeviceActivityLevel_UserInteraction ||
                               level == vr::k_EDeviceActivityLevel_UserInteraction_Timeout;
            bool standby = level == vr::k_EDeviceActivityLevel_Standby || level == vr::k_EDeviceActivityLevel_Idle_Timeout;
            if (interacting) lastActive = t;
            if (standby) {
                if (!standbySince) standbySince = t;
            } else {
                standbySince = 0;
            }
            if (interacting) g_present = true;
            else if (standby && t - standbySince > 3000) g_present = false;
            else if (level == vr::k_EDeviceActivityLevel_Idle && (!lastActive || t - lastActive > 120000)) g_present = false;
            active = g_present;
            static int lastLevel = -99;
            if (level != lastLevel) {
                lastLevel = level;
                vr::log::Line("OpenVR: headset state " + std::to_string(level) +
                              (active ? " (worn)" : " (not worn / standby)"));
            }
        }
        // Una volta in Scene si resta in Scene: con Quest Link lo stato del
        // visore indossato oscilla tra 1, 0 e 3 (standby) e in Background
        // SteamVR non passa i controller al gioco (17/10, prima prova reale).
        bool wantScene = active || vr::control::g_vrOutputForce || g_scene;
        ULONGLONG now = GetTickCount64();
        if (wantScene) {
            inactiveSince = 0;
            if (!g_scene) Switch(vr::VRApplication_Scene);
        } else if (g_scene) {
            if (!inactiveSince) inactiveSince = now;
            if (now - inactiveSince > 20000) {
                vr::log::Line("OpenVR: headset idle for 20 s.");
                Switch(vr::VRApplication_Background);
                inactiveSince = 0;
            }
        }
    }
}

}  // namespace

Shared::Shared() { AcquireSRWLockShared(&g_lock); }
Shared::Shared(bool tryOnly) {
    if (tryOnly) m_ok = TryAcquireSRWLockShared(&g_lock) != FALSE;
    else AcquireSRWLockShared(&g_lock);
}
Shared::~Shared() {
    if (m_ok) ReleaseSRWLockShared(&g_lock);
}

bool Ready() { return g_ready; }
bool IsScene() { return g_scene; }
bool HmdPresent() { return g_present; }

bool Init() {
    // Senza SteamVR in esecuzione un'app Background non puo' partire: in quel
    // caso Scene, che avvia SteamVR (il controllo lo riporta poi a
    // Background se il visore resta inattivo).
    AcquireSRWLockExclusive(&g_lock);
    bool ok = InitAs(vr::VRApplication_Background) || InitAs(vr::VRApplication_Scene);
    ReleaseSRWLockExclusive(&g_lock);
    if (ok) std::thread(MonitorLoop).detach();
    return ok;
}

}  // namespace vr::session
