#pragma once
#include <d3d9.h>

// Uscita verso il visore: il back buffer D3D9 del gioco viene ritagliato per
// occhio, copiato via CPU (lettura in pipeline) in texture Direct3D 11 create
// sulla scheda del visore e inviato al compositor di SteamVR. Vedi
// vr_output.cpp per la storia dei tentativi (interop NVIDIA, OpenGL).
namespace vr::output {

// Thread di rendering, in Present prima dell'originale. Dopo l'invio chiama
// WaitGetPoses, che sincronizza il ritmo del gioco con il visore.
// eye: 0/1 = occhio del frame (stereo), -1 = mono/sconosciuto.
// In stereo l'invio a SteamVR avviene dopo l'occhio destro.
void OnPresent(IDirect3DDevice9* device, int eye);

// Da chiamare prima di IDirect3DDevice9::Reset: le risorse D3DPOOL_DEFAULT
// vanno rilasciate o il Reset fallisce.
void OnBeforeReset();

// Rettangolo (pixel del back buffer) della porzione di immagine dell'occhio.
RECT EyeCropRect(int eye);

}  // namespace vr::output
