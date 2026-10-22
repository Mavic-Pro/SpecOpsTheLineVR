#include "vr_session.h"
#include "display_spoof.h"
#include "control.h"
#include "vr_toast.h"
#include "vr_menu.h"
#include "logger.h"

#include <windows.h>
#include <delayimp.h>
#include <openvr.h>
#include "vr_runtime.h"

#include <atomic>
#include <string>
#include <thread>

// openvr_api.dll caricata a richiesta (/DELAYLOAD). Se manca, ogni funzione
// esportata (anche VR_GetInitToken, chiamata dagli accessori inline di
// openvr.h come VRSystem()) diventa uno stub che restituisce 0/null: i
// chiamanti vedono "OpenVR assente" come con una sessione mai aperta. Senza
// questo gancio il delay-load sollevava un'eccezione SEH e il gioco mostrava
// "Fatal error" ~5 s dopo l'avvio (21/10, retry del collegamento).
extern "C" uintptr_t __cdecl SpecOpsOpenVrStub() { return 0; }

extern "C" FARPROC WINAPI SpecOpsDelayLoadFailure(unsigned notify, PDelayLoadInfo info) {
    if (!info || !info->szDll || _stricmp(info->szDll, "openvr_api.dll") != 0) return nullptr;
    if (notify == dliFailLoadLib) return reinterpret_cast<FARPROC>(GetModuleHandleA("kernel32.dll"));
    if (notify == dliFailGetProc) return reinterpret_cast<FARPROC>(&SpecOpsOpenVrStub);
    return nullptr;
}
extern "C" const PfnDliHook __pfnDliFailureHook2 = SpecOpsDelayLoadFailure;

namespace vr::session {

namespace {

SRWLOCK g_lock = SRWLOCK_INIT;
std::atomic<bool> g_ready{false};
std::atomic<bool> g_scene{false};
std::atomic<unsigned> g_generation{0};
std::atomic<bool> g_reconnectPending{false};

const char* TypeName(vr::EVRApplicationType t) { return t == vr::VRApplication_Scene ? "Scene" : "Background"; }

// Da chiamare con il lock esclusivo (o prima che esistano altri utenti).
bool InitAs(vr::EVRApplicationType type) {
    // OpenXR non ha applicazioni Background: la sessione e' sempre completa.
    if (vr::rt::SceneOnly()) type = vr::VRApplication_Scene;
    std::string err;
    if (!vr::rt::Init(type, &err)) {
        vr::log::Line(std::string(vr::rt::BackendName()) + " " + TypeName(type) + " not available (" + err + ").");
        g_ready = false;
        g_scene = false;
        return false;
    }
    g_ready = true;
    ++g_generation;
    g_scene = (type == vr::VRApplication_Scene) || vr::rt::SceneOnly();  // OpenXR: sempre sessione completa
    vr::log::Line(std::string(vr::rt::BackendName()) + " initialised as " + TypeName(type) + ".");
    if (g_reconnectPending.exchange(false)) vr::toast::Notify(std::string(vr::rt::BackendName()) + " reconnected");
    if (g_scene) {
        // Visore appena indossato: direzione e altezza di riferimento da qui
        // (seduti il riferimento verticale e' questa altezza; prima restava la
        // posa del visore appoggiato, 1,0 m, 21/10). Silenzioso.
        vr::control::g_recenterSilent = true;
        vr::control::g_recenterRequested = true;
    }
    return true;
}

// --- ASW di Meta (Quest via Link) --------------------------------------------
//
// 18/10, prova col visore: 36 fps fissi con la GPU a 6 ms su 13,9; l'attesa
// del compositor era di 17-20 ms per frame. Spento l'ASW con l'Oculus Debug
// Tool il gioco e' tornato subito a 70-72 fps. L'ASW non serve: la
// risoluzione dinamica tiene il frame e le immagini partono con la loro posa.
// OculusDebugToolCLI.exe -f <file> con "server: asw.Off"; un PowerShell
// nascosto aspetta la chiusura del gioco e lo rimette su Auto (l'impostazione
// vale fino al riavvio del servizio Oculus e toccherebbe anche gli altri giochi).
void RunOdtCommands(const std::string& odt, const std::string& dir, const char* name, const char* commands) {
    std::string file = dir + name;
    HANDLE f = CreateFileA(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    WriteFile(f, commands, static_cast<DWORD>(strlen(commands)), &put, nullptr);
    CloseHandle(f);
    std::string cmd = "\"" + odt + "\" -f \"" + file + "\"";
    STARTUPINFOA si = {sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (CreateProcessA(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 15000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

// Ricostruzione dei frame mancanti per piattaforma (stesso problema dell'ASW:
// puo' tenere il gioco a meta' frequenza anche con margine):
// - SteamVR nativo (Index, Vive, Pimax, WMR): Motion Smoothing, spento solo
//   per questo gioco con l'override per applicazione di SteamVR
//   (motionSmoothingOverride: 0 globale, 1 forzato acceso, 2 spento, 3 sempre
//   acceso: enum della dashboard di SteamVR, chunk~8012d0c89.js);
//   resta salvato in steamvr.vrsettings solo per Spec Ops.
// - Virtual Desktop: l'SSW si imposta solo dallo Streamer di VD: lo si segnala
//   nel log.
// - Quest via Link: ASW di Meta (sotto).
void ConfigureReprojection() {
    static bool done = false;
    if (done) return;
    char system[64] = {}, model[128] = {}, appKey[128] = {};
    {
        Shared s;
        if (!vr::rt::System()) return;
        vr::rt::System()->GetStringTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_TrackingSystemName_String,
                                                       system, sizeof(system));
        vr::rt::System()->GetStringTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_ModelNumber_String, model,
                                                       sizeof(model));
        // Gioco avviato senza SteamVR: al primo controllo il driver del visore
        // non ha ancora pubblicato le proprieta' e "system" e' vuoto; si
        // riprova (prima ASW di Link e avviso per VD saltavano; audit 20/10).
        if (!system[0]) return;
        done = true;
        vr::log::Line(std::string("VR: headset ") + model + ", tracking system " + system + ".");
        if (vr::control::g_motionSmoothingOff && vr::rt::Settings() && vr::rt::Applications() &&
            vr::rt::Applications()->GetApplicationKeyByProcessId(GetCurrentProcessId(), appKey, sizeof(appKey)) ==
                vr::VRApplicationError_None &&
            appKey[0]) {
            vr::EVRSettingsError e = vr::VRSettingsError_None;
            int32_t cur = vr::rt::Settings()->GetInt32(appKey, vr::k_pch_SteamVR_MotionSmoothingOverride_Int32, &e);
            if (e != vr::VRSettingsError_None || cur != 2) {
                vr::rt::Settings()->SetInt32(appKey, vr::k_pch_SteamVR_MotionSmoothingOverride_Int32, 2, &e);
                vr::log::Line(std::string("VR: SteamVR Motion Smoothing forced off for ") + appKey +
                              (e == vr::VRSettingsError_None ? "." : " FAILED (" + std::to_string(e) + ")."));
            }
        }
    }
    std::string sys(system);
    for (char& c : sys) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    // Virtual Desktop si presenta come "oculus" / "Meta Quest 3": il driver
    // vero sta in steamvr.vrsettings, LastKnown/ActualHMDDriver =
    // "oculus_virtualdesktop" (21/10). L'SSW di VD si spegne solo dal menu di
    // VD nel visore (Streaming > Synchronous Spacewarp): avviso nel visore.
    bool vd = sys.find("virtual") != std::string::npos || std::string(model).find("Virtual Desktop") != std::string::npos;
    {
        Shared s;
        char drv[128] = {};
        vr::EVRSettingsError e = vr::VRSettingsError_None;
        if (vr::rt::Settings()) vr::rt::Settings()->GetString("LastKnown", "ActualHMDDriver", drv, sizeof(drv), &e);
        std::string d(e == vr::VRSettingsError_None ? drv : "");
        for (char& c : d) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (d.find("virtualdesktop") != std::string::npos) vd = true;
        if (d[0]) vr::log::Line("VR: SteamVR HMD driver " + d + ".");
    }
    if (vd) {
        vr::control::g_vdDetected = true;
        vr::log::Line("VR: Virtual Desktop detected. If the game stays at half the headset refresh rate, set "
                      "\"Synchronous Spacewarp\" to Disabled in the Virtual Desktop menu (Streaming tab).");
        vr::toast::Notify("Virtual Desktop: set Synchronous Spacewarp to Disabled (VD menu, Streaming) for full frame rate");
        return;  // niente ASW di Link
    }
    if (sys != "oculus" || !vr::control::g_linkAswOff) return;  // ASW: solo Link
    // Il gioco e' a 32 bit: qui "ProgramFiles" e' Program Files (x86). Prima
    // OculusBase (impostata dall'installazione di Meta), poi ProgramW6432.
    std::string odt;
    char buf[MAX_PATH] = {};
    if (GetEnvironmentVariableA("OculusBase", buf, MAX_PATH)) {
        odt = buf;
        if (!odt.empty() && odt.back() != '\\') odt += '\\';
        odt += "Support\\oculus-diagnostics\\OculusDebugToolCLI.exe";
    }
    if ((odt.empty() || GetFileAttributesA(odt.c_str()) == INVALID_FILE_ATTRIBUTES) &&
        GetEnvironmentVariableA("ProgramW6432", buf, MAX_PATH))
        odt = std::string(buf) + "\\Oculus\\Support\\oculus-diagnostics\\OculusDebugToolCLI.exe";
    if (odt.empty() || GetFileAttributesA(odt.c_str()) == INVALID_FILE_ATTRIBUTES) {
        vr::log::Line("ASW: Oculus Debug Tool not found, Meta ASW left as it is.");
        return;
    }
    char tmp[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, tmp);
    std::string dir(tmp);
    std::thread([odt, dir] {
        RunOdtCommands(odt, dir, "specopsvr_asw_off.txt", "server: asw.Off\nexit\n");
        vr::log::Line("ASW: Meta ASW turned off for this session (\"linkasw keep\" in specopsvr.ini to leave it).");
        // Rimesso su Auto quando il gioco si chiude.
        std::string restore = dir + "specopsvr_asw_auto.txt";
        HANDLE f = CreateFileA(restore.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            const char* c = "server: asw.Auto\nexit\n";
            DWORD put = 0;
            WriteFile(f, c, static_cast<DWORD>(strlen(c)), &put, nullptr);
            CloseHandle(f);
        }
        // Apostrofi nei percorsi raddoppiati (stringa PowerShell a virgolette singole; L26).
        auto psq = [](std::string s) {
            for (size_t i = 0; (i = s.find('\'', i)) != std::string::npos; i += 2) s.insert(i, "'");
            return s;
        };
        std::string ps = "powershell -NoProfile -WindowStyle Hidden -Command \"Wait-Process -Id " +
                         std::to_string(GetCurrentProcessId()) + "; & '" + psq(odt) + "' -f '" + psq(restore) + "'\"";
        STARTUPINFOA si = {sizeof(si)};
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi = {};
        // Niente DETACHED_PROCESS: senza console PowerShell esce subito senza
        // eseguire nulla (provato il 18/10). Fuori dal job di Steam se
        // possibile, cosi' non viene chiuso insieme al gioco.
        std::string ps2 = ps;
        bool ok = CreateProcessA(nullptr, &ps[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB,
                                 nullptr, nullptr, &si, &pi) != FALSE;
        if (!ok)
            ok = CreateProcessA(nullptr, &ps2[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) !=
                 FALSE;
        if (ok) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        vr::log::Line(ok ? "ASW: Meta ASW goes back to Auto when the game closes."
                         : "ASW: could not start the restore helper (" + std::to_string(GetLastError()) +
                               "): run the Oculus Debug Tool to set ASW back to Auto.");
    }).detach();
}

void Switch(vr::EVRApplicationType type) {
    AcquireSRWLockExclusive(&g_lock);
    vr::log::Line(std::string("OpenVR: switching to ") + TypeName(type) + "...");
    if (g_ready) vr::rt::Shutdown();
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

// Eventi di SteamVR (audit 19/10: nessuno era gestito).
// - Dashboard aperta / focus dell'input a un'altra app: il gioco va in pausa
//   (Start una volta, vr_input) e i gesti si spengono.
// - Ricentramento del sistema (Quest: tasto Meta tenuto) o area di gioco
//   cambiata: anche la mod ricentra.
// - IPD o proprieta' del visore cambiate: proiezione degli occhi ricalcolata.
// - SteamVR che chiude: si conferma l'uscita della sessione VR (non del
//   gioco) e si riprova a collegarsi ogni 5 s finche' SteamVR non torna.
// Ritorna false se la sessione va chiusa.
bool PollEvents() {
    if (!g_ready) return true;
    vr::IVRSystem* sys = vr::rt::System();
    if (!sys) return true;
    vr::VREvent_t ev;
    bool keep = true;
    while (sys->PollNextEvent(&ev, sizeof(ev))) {
        switch (ev.eventType) {
        case vr::VREvent_DashboardActivated:
            if (!vr::control::g_vrDashboard.exchange(true)) {
                vr::control::g_vrPauseRequest = true;
                vr::log::Line("OpenVR: dashboard open, game paused.");
            }
            break;
        case vr::VREvent_DashboardDeactivated:
            if (vr::control::g_vrDashboard.exchange(false)) vr::log::Line("OpenVR: dashboard closed.");
            break;
        case vr::VREvent_InputFocusCaptured:
        case vr::VREvent_InputFocusReleased:
            // Un'altra app prende il focus dell'input: su Link succede di
            // continuo (22/10: catturato/rilasciato a raffica all'avvio, poi
            // catturato per sempre con la dashboard invisibile: comandi morti
            // per tutta la partita). Non e' la dashboard: SteamVR smette da
            // solo di dare le azioni finche' dura, qui si logga e basta.
            {
                char b[120];
                snprintf(b, sizeof(b), "OpenVR: input focus %s (pid %u, old pid %u).",
                         ev.eventType == vr::VREvent_InputFocusCaptured ? "captured by another app" : "released",
                         ev.data.process.pid, ev.data.process.oldPid);
                vr::log::Line(b);
            }
            break;
        case vr::VREvent_ChaperoneUniverseHasChanged:
        case vr::VREvent_StandingZeroPoseReset:
        case vr::VREvent_SeatedZeroPoseReset: {
            // Virtual Desktop li manda a raffica (anche 4 per secondo, 21/10):
            // uno ogni 10 s al massimo, e senza avviso nel visore (il toast
            // "Recentered" a 1 m davanti allo schermo virtuale era un riquadro
            // che spuntava di continuo: nausea sulla bandiera iniziale).
            static ULONGLONG lastAuto = 0;
            ULONGLONG nowA = GetTickCount64();
            if (nowA - lastAuto > 10000) {
                lastAuto = nowA;
                vr::control::g_recenterSilent = true;
                vr::control::g_recenterRequested = true;
                vr::log::Line("OpenVR: play area / zero pose changed, recentering.");
            }
            break;
        }
        case vr::VREvent_IpdChanged:
            vr::control::g_vrProjectionDirty = true;
            break;
        case vr::VREvent_PropertyChanged:
            // Solo proprieta' che cambiano la proiezione: Quest e VD mandano
            // spesso batteria e simili, e ogni volta si ricalcolava tutto
            // (audit 20/10).
            if (ev.trackedDeviceIndex == vr::k_unTrackedDeviceIndex_Hmd) {
                vr::ETrackedDeviceProperty prop = ev.data.property.prop;
                if (prop == vr::Prop_UserIpdMeters_Float || prop == vr::Prop_DisplayFrequency_Float ||
                    prop == vr::Prop_UserHeadToEyeDepthMeters_Float || prop == vr::Prop_LensCenterLeftU_Float ||
                    prop == vr::Prop_LensCenterLeftV_Float || prop == vr::Prop_LensCenterRightU_Float ||
                    prop == vr::Prop_LensCenterRightV_Float || prop == vr::Prop_ScreenshotHorizontalFieldOfViewDegrees_Float ||
                    prop == vr::Prop_DisplayMCImageWidth_Int32 || prop == vr::Prop_DisplayMCImageHeight_Int32)
                    vr::control::g_vrProjectionDirty = true;
            }
            break;
        case vr::VREvent_Quit:
        case vr::VREvent_DriverRequestedQuit:
            vr::log::Line("OpenVR: SteamVR is closing, VR session ended (the game keeps running).");
            if (ev.eventType == vr::VREvent_Quit) sys->AcknowledgeQuit_Exiting();
            keep = false;
            break;
        default:
            break;
        }
    }
    return keep;
}

void MonitorLoop() {
    ULONGLONG inactiveSince = 0, lastActive = 0, standbySince = 0, lastRetry = 0;
    for (;;) {
        Sleep(500);
        if (!g_ready) {
            // SteamVR assente all'avvio o chiuso: nuovo tentativo ogni 5 s.
            ULONGLONG now = GetTickCount64();
            if (now - lastRetry < 5000) continue;
            lastRetry = now;
            AcquireSRWLockExclusive(&g_lock);
            bool ok = InitAs(vr::VRApplication_Background);
            ReleaseSRWLockExclusive(&g_lock);
            if (ok) {
                vr::control::g_vrProjectionDirty = true;
                vr::log::Line("OpenVR: connected again.");
            }
            continue;
        }
        bool quit = false;
        {
            Shared s;
            quit = !PollEvents();
            if (!quit) vr::menu::Tick(vr::control::g_cinemaActive);  // menu nel visore (stesso thread dello switch)
            // Lo stato della dashboard viene dagli eventi, ma un evento perso
            // (cambio Background/Scene) lasciava la mod convinta che fosse
            // aperta, con tutti i comandi bloccati: ogni secondo si riallinea
            // allo stato vero (audit 20/10). Solo la dashboard visibile: su
            // Link IsInputAvailable() resta falso per tutta la partita (22/10).
            static ULONGLONG lastSync = 0;
            if (!quit && g_scene && vr::rt::Overlay() && vr::rt::System() && GetTickCount64() - lastSync > 1000) {
                lastSync = GetTickCount64();
                bool open = vr::rt::Overlay()->IsDashboardVisible();
                if (vr::control::g_vrDashboard.exchange(open) != open)
                    vr::log::Line(open ? "OpenVR: dashboard open (state resync)." : "OpenVR: dashboard closed (state resync).");
                static bool inputWas = true;
                bool inputNow = vr::rt::System()->IsInputAvailable();
                if (inputNow != inputWas) {
                    inputWas = inputNow;
                    if (inputNow) vr::display::BringToForeground("VR input focus back");
                    vr::log::Line(inputNow ? "OpenVR: input available again." : "OpenVR: input not available (another app has the focus).");
                }
            }
        }
        // SteamVR morto senza l'evento di uscita (chiuso dal Task Manager,
        // crash del driver di Link o VD): vrserver.exe sparito per 5 s = come
        // un'uscita, e ci si ricollega quando torna (audit 20/10).
        {
            static ULONGLONG serverMissingSince = 0, lastCheck = 0;
            ULONGLONG t = GetTickCount64();
            if (t - lastCheck > 2000) {
                lastCheck = t;
                if (vr::rt::Alive()) {
                    serverMissingSince = 0;
                } else {
                    if (!serverMissingSince) serverMissingSince = t;
                    if (t - serverMissingSince > 5000) {
                        vr::log::Line(std::string(vr::rt::BackendName()) + ": runtime gone (crashed or closed), VR session ended.");
                        serverMissingSince = 0;
                        quit = true;
                    }
                }
            }
        }
        if (quit) {
            AcquireSRWLockExclusive(&g_lock);
            vr::rt::Shutdown();
            g_ready = false;
            g_scene = false;
            ++g_generation;
            ReleaseSRWLockExclusive(&g_lock);
            g_reconnectPending = true;  // al ritorno di SteamVR: avviso nel visore (R-U1)
            vr::control::g_vrDashboard = false;
            // Stato del visore da rifare da capo: con il vecchio "indossato"
            // e la Scene che non si lascia piu', al ritorno di SteamVR col
            // visore sul tavolo si restava in Scene (audit 20/10).
            g_present = false;
            inactiveSince = lastActive = standbySince = 0;
            lastRetry = GetTickCount64();
            continue;
        }
        bool active = false;
        {
            Shared s;
            vr::IVRSystem* sys = vr::rt::System();
            if (!sys) continue;
            vr::EDeviceActivityLevel level = sys->GetTrackedDeviceActivityLevel(vr::k_unTrackedDeviceIndex_Hmd);
            ULONGLONG t = GetTickCount64();
            bool interacting = level == vr::k_EDeviceActivityLevel_UserInteraction ||
                               level == vr::k_EDeviceActivityLevel_UserInteraction_Timeout;
            bool standby = level == vr::k_EDeviceActivityLevel_Standby || level == vr::k_EDeviceActivityLevel_Idle_Timeout;
            // Testa in movimento = visore indossato, qualunque sia lo stato:
            // con Virtual Desktop, dopo uno standby lo stato resta Idle/Standby
            // anche col visore di nuovo in testa, e la mod smetteva di mandare
            // le immagini e bloccava i comandi (20/10). Chi lo indossa si muove
            // sempre di qualche millimetro; un visore appoggiato o in standby ha
            // la posa ferma.
            {
                vr::TrackedDevicePose_t pose;
                sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.0f, &pose, 1);
                static float lastPos[3] = {}, lastFwd[3] = {};
                static bool haveLast = false;
                if (pose.bPoseIsValid && pose.eTrackingResult == vr::TrackingResult_Running_OK) {
                    const auto& m = pose.mDeviceToAbsoluteTracking.m;
                    float pos[3] = {m[0][3], m[1][3], m[2][3]}, fwd[3] = {m[0][2], m[1][2], m[2][2]};
                    float dp = 0.0f, df = 0.0f;
                    for (int i = 0; i < 3; ++i) {
                        dp += (pos[i] - lastPos[i]) * (pos[i] - lastPos[i]);
                        df += (fwd[i] - lastFwd[i]) * (fwd[i] - lastFwd[i]);
                        lastPos[i] = pos[i];
                        lastFwd[i] = fwd[i];
                    }
                    bool moving = haveLast && (dp > 0.002f * 0.002f || df > 0.003f * 0.003f);
                    haveLast = true;
                    if (moving && !interacting) {
                        static int logged = 0;
                        if (!g_present && logged++ < 10)
                            vr::log::Line("OpenVR: headset state " + std::to_string(level) +
                                          " but the head is moving: treated as worn.");
                        interacting = true;
                        standby = false;
                    }
                }
            }
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
            {
                // Visore appena indossato: il gioco davanti, o il pad non arriva.
                static bool wasActive = false;
                if (active && !wasActive) vr::display::BringToForeground("headset worn");
                wasActive = active;
            }
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
            // Dopo un passaggio a Scene fallito si riprova ogni 5 s, non ogni
            // 500 ms (ogni tentativo e' Shutdown + due Init sotto il lock
            // esclusivo: testa e comandi fermi; audit 20/10).
            static ULONGLONG lastSceneTry = 0;
            if (!g_scene && now - lastSceneTry > 5000) {
                lastSceneTry = now;
                Switch(vr::VRApplication_Scene);
            }
            if (g_scene) ConfigureReprojection();
        }
        // (Il ritorno a Background dopo 20 s di inattivita' non esiste piu':
        // wantScene include g_scene, "una volta in Scene si resta in Scene"; L23.)
        (void)inactiveSince;
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
unsigned Generation() { return g_generation; }

bool Init() {
    // Senza SteamVR in esecuzione un'app Background non puo' partire: in quel
    // caso Scene, che avvia SteamVR (il controllo lo riporta poi a
    // Background se il visore resta inattivo).
    AcquireSRWLockExclusive(&g_lock);
    bool ok = InitAs(vr::VRApplication_Background) || InitAs(vr::VRApplication_Scene);
    ReleaseSRWLockExclusive(&g_lock);
    // Il controllo parte comunque: senza SteamVR riprova a collegarsi.
    if (!ok) vr::log::Line("OpenVR: not available yet, retrying every 5 s.");
    std::thread(MonitorLoop).detach();
    return ok;
}

}  // namespace vr::session
