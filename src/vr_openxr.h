#pragma once
#include <openvr.h>
#include <string>

struct ID3D11Device;

// Backend OpenXR: reimplementa le interfacce OpenVR usate dal mod sopra il
// runtime OpenXR attivo (Oculus, Virtual Desktop VDXR, SteamVR, Pimax, ...),
// cosi' il resto del codice (pose, invio degli occhi, overlay, azioni) non
// cambia. Il runtime viene caricato direttamente dal suo json (registro
// Khronos\OpenXR\1, vista a 32 bit): niente openxr_loader.dll da distribuire e
// nessun API layer. Vedi docs/TECHNICAL.md "OpenXR backend".
namespace vr::xr {

// json del runtime attivo (o quello passato): false se non c'e'. name = nome
// file del json in minuscolo (es. "virtualdesktop-openxr-32.json").
bool ActiveRuntime(const std::string& overrideJson, std::string* name);

// json a 32 bit di Virtual Desktop (VDXR) se registrato tra gli AvailableRuntimes
// ed esistente, altrimenti "". VirtualDesktopRunning = streamer di VD avviato.
std::string VirtualDesktopJson();
bool VirtualDesktopRunning();

bool Init(const std::string& overrideJson, std::string* error);
// Dopo un Init fallito: dimentica il runtime caricato, cosi' un Init successivo
// puo' caricarne un altro (la dll resta in memoria: i suoi thread possono
// essere ancora vivi).
void Unload();
void Shutdown();
bool Alive();
const char* RuntimeName();

vr::IVRSystem* System();
vr::IVRCompositor* Compositor();
vr::IVROverlay* Overlay();
vr::IVRInput* Input();
vr::IVRRenderModels* RenderModels();
vr::IVRSettings* Settings();
vr::IVRApplications* Applications();

ID3D11Device* D3D11Device();
float SecondsToPhotons();
// Attesa del mutex del backend per thread (WI-1, review 2 WI-1). Il thread di
// gioco chiama EndGameTickLockWait a ogni confine del tick (fine del disegno
// stereo): torna l'attesa di quel thread nel tick (ms) e, con record, la mette
// nell'istogramma per tick (riga "OPENXR: lock wait, game thread, per tick"
// ogni 10 s); record = false per un intervallo che non e' un tick (oltre 1 s).
// Il primo richiamo da' al thread il ruolo Game. ThreadLockWaitMs: totale del
// thread chiamante (per differenza: la parte nel tracciamento della testa).
// NameLockWaitThread: nome della riga per chiamata dei thread Present e
// sessione (gli altri: "other threads"); non cambia un ruolo gia' dato.
enum class LockWaitThread { Other = 0, Game = 1, Present = 2, Session = 3 };
float EndGameTickLockWait(bool record);
double ThreadLockWaitMs();
void NameLockWaitThread(LockWaitThread role);
// Ultimo cambio dello spazio di riferimento (XrEventDataReferenceSpaceChangePending
// con poseValid): posa dell'origine del nuovo spazio nel vecchio (m, colonna 3
// in metri). true una volta per evento (WI-2, ricentramenti del runtime).
bool TakeSpaceChange(float m[3][4]);

// Correzione della posa dei controller: la posa "grip" di OpenXR riportata al
// riferimento "raw" dei Touch di SteamVR (su cui sono tarati fucile e mira).
// on/off e beccheggio in gradi (predefinito 20.6 dal modello di SteamVR).
void SetGripFix(bool on, float pitchDeg);

// Raggio di puntamento (posa aim del runtime) nello spazio della mano che la
// mod riceve (WI-4a, telaio dell'arma): angoli in gradi come ExactEuler (VDXR
// e Touch: pitch -39,4, yaw 0, roll 0). hand 0 = sinistra, 1 = destra. false
// finche' non c'e' una misura stabile e plausibile per il profilo attuale.
bool MeasuredAimInRaw(int hand, float* pitchDeg, float* yawDeg, float* rollDeg);

}  // namespace vr::xr
