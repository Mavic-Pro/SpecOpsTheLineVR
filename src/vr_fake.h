#pragma once
#include <openvr.h>
#include <cstdint>
#include <string>

// Runtime VR finto ("runtime fake", 25/10): le stesse interfacce OpenVR che
// danno SteamVR (openvr_api.dll) e il backend OpenXR, senza visore, senza
// SteamVR e senza Virtual Desktop. Serve ai collaudi automatici del percorso
// vero del visore (UpdateHeadTracking in "mode hmd", invio degli occhi,
// riproiezione) con pose deterministiche:
//  - vsync su una griglia QPC a 72/90/120 Hz (WaitGetPoses aspetta il "running
//    start" 2 ms prima del vsync, come SteamVR);
//  - testa e controller da traiettorie programmate (posa fissa, oscillazioni,
//    sguardo in giro, tremore, rotazione continua, file di punti chiave);
//  - due semantiche d'invio: "openvr" (posa dichiarata = quella di
//    WaitGetPoses, salvo Submit_TextureWithPose) e "openxr" (posa del layer,
//    tempo di visualizzazione fissato all'attesa del frame);
//  - riga JITTER ogni N frame: differenza tra posa dichiarata e vista
//    davvero disegnata, passo del mondo da un frame all'altro (p99), bordi
//    scoperti dalla riproiezione, frame ripetuti e in ritardo, dissolvenze.
// Comandi "fake ..." e uso: docs/TECHNICAL.md, "Tester VR fixes (25/10)".
namespace vr::fake {

// Profilo dalla riga dell'ini "runtime fake [openvr|openxr] [hz] [ms]" (prima di Init).
void Configure(const std::string& profile);
bool Init(std::string* error);
void Shutdown();
bool Alive();

vr::IVRSystem* System();
vr::IVRCompositor* Compositor();
vr::IVROverlay* Overlay();
vr::IVRInput* Input();
vr::IVRRenderModels* RenderModels();
vr::IVRSettings* Settings();
vr::IVRApplications* Applications();
float SecondsToPhotons();
// Semantica d'invio del runtime finto: true = openxr (fotoni del frame atteso), false = openvr.
bool OpenXrSemantics();

// "fake <comando> ..." (control.cpp).
void Command(const std::string& args);

// "simevent zeropose": il runtime finto ricentra come uno vero (origine dello
// spazio Standing sulla testa, yaw azzerato, altezza dal pavimento invariata)
// e manda VREvent_StandingZeroPoseReset.
void SimulateZeroPose();
// "synthheight <m>": altezza della testa sul pavimento.
void SetHeadHeight(float meters);

// Diagnostica JITTER, solo con il runtime finto attivo.
// GetPlayerViewPoint (thread di gioco): rotazione finale della vista
// dell'occhio (unita' di UE3), corpo usato e yaw di riferimento della testa.
void NoteView(uint32_t tick, int eye, const int32_t* rot, int32_t bodyYaw, float refYawRad);
// vr_output (thread di uscita), subito prima dei due Submit della coppia:
// tick del frame, posa della testa con cui e' stato disegnato, schermo virtuale.
void NoteSubmit(uint32_t tick, const vr::HmdMatrix34_t* renderPose, double sampleMs, bool cinema);

}  // namespace vr::fake
