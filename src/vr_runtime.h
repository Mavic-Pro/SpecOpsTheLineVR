#pragma once
#include <openvr.h>
#include <string>

struct ID3D11Device;

// Runtime VR: le interfacce OpenVR che il mod usa (IVRSystem, IVRCompositor,
// IVROverlay, IVRInput, ...) arrivano o da SteamVR (openvr_api.dll) o dal
// backend OpenXR (vr_openxr.cpp), che le reimplementa sopra il runtime OpenXR
// attivo (Oculus, Virtual Desktop VDXR, SteamVR, Pimax...). Il resto del codice
// chiama sempre e solo questi accessori. Scelta con "runtime steamvr|openxr|auto"
// nell'ini, letta prima di tutto il resto (la sessione parte da CreateDevice,
// prima del caricamento dell'ini). "runtime fake [openvr|openxr] [hz] [ms]"
// (collaudi, 25/10): runtime finto senza visore (vr_fake.cpp), anche con il
// file specopsvr_novr.
namespace vr::rt {

enum class Backend { None, OpenVR, OpenXR, Fake };

// Backend in uso (None prima del primo Init).
Backend Active();
const char* BackendName();

// Da vr::session, sotto il suo lock esclusivo. type: Background o Scene.
bool Init(vr::EVRApplicationType type, std::string* error);
void Shutdown();
// Il processo del runtime esiste ancora (vrserver.exe) / l'istanza OpenXR non e' persa.
bool Alive();
// OpenXR non ha applicazioni "Background": ogni Init da' una sessione completa.
bool SceneOnly();
// Nome dell'ini: "steamvr", "openxr", "auto" o "fake"; openxrjson = percorso json del runtime (vuoto = quello attivo).
void Configure(const std::string& choice, const std::string& openxrJson);
std::string Choice();
// L'ini chiede il runtime finto ("runtime fake ..."): con specopsvr_novr si avvia solo quello.
bool FakeChosen();

// Interfacce (nullptr se il backend non e' inizializzato).
vr::IVRSystem* System();
vr::IVRCompositor* Compositor();
vr::IVROverlay* Overlay();
vr::IVRInput* Input();
vr::IVRRenderModels* RenderModels();
vr::IVRSettings* Settings();
vr::IVRApplications* Applications();

// Device D3D11 creato dal backend sulla scheda del visore (OpenXR), nullptr
// altrimenti: vr_output lo riusa invece di crearne uno suo.
ID3D11Device* SharedD3D11();

// Secondi da adesso a quando il frame che si sta preparando arriva sul display
// (prossimo vsync + vsync->fotoni); 0 se ignoto.
float SecondsToPhotons();

// Da chiamare subito dopo l'attesa del compositor (WaitGetPoses): secondi da
// adesso ai fotoni del frame che quell'attesa ha aperto (WI-1). Con SteamVR
// (e il runtime finto con semantica openvr) WaitGetPoses torna 2-3 ms prima
// del vsync ("running start") e il frame va sul vsync DOPO: SecondsToPhotons
// (fino al prossimo vsync) era un frame corto e "predict auto" prediceva un
// frame in meno. OpenXR: il tempo previsto del frame iniziato.
float FramePhotonsSeconds();

// Nome del thread chiamante per le righe dell'attesa del mutex OpenXR (review 2
// WI-1): Present (OnPresent) e sessione (MonitorLoop). Solo un thread_local:
// innocuo con ogni backend.
enum class ThreadRole { Present, Session };
void NameThread(ThreadRole role);

}  // namespace vr::rt
