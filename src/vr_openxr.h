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

bool Init(const std::string& overrideJson, std::string* error);
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

// Correzione della posa dei controller: la posa "grip" di OpenXR riportata al
// riferimento "raw" dei Touch di SteamVR (su cui sono tarati fucile e mira).
// on/off e beccheggio in gradi (predefinito 20.6 dal modello di SteamVR).
void SetGripFix(bool on, float pitchDeg);

}  // namespace vr::xr
