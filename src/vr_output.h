#pragma once
#include <d3d9.h>
#include <openvr.h>

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

// Thread di rendering: posa del visore con cui e' stato disegnato l'occhio del
// frame in corso (nullptr = sconosciuta). Viaggia col frame fino al Submit.
// tick: tick stereo del frame; si inviano solo coppie con lo stesso tick.
void SetRenderPose(int eye, const vr::HmdMatrix34_t* pose, uint32_t tick, bool offCenter, double sampleMs = 0.0);
// Ritardo misurato dal campionamento della testa ai fotoni (ms, 0 = non ancora).
float PhotonLatencyMs();

// Proiezione decentrata dell'occhio (tangenti esatte del visore sull'intero
// back buffer): NDC.x = tan.x * m00 + m20, NDC.y = tan.y * m11 + m21 (vista
// UE3: x a destra, y in su). false finche' le tangenti non sono note.
struct EyeProjection {
    float m00, m11, m20, m21;
};
bool OffCenterProjection(int eye, EyeProjection* out);

// Display inclinati: assi dell'occhio nella testa (componenti avanti, destra,
// su di UE3). false se l'occhio guarda dritto davanti (quasi tutti i visori).
bool EyeCant(int eye, float* fwd, float* right, float* up);

// Frequenza del visore (Hz, 0 se ignota).
float HmdRefreshHz();

// Attesa massima (ms) sul thread di rendering per inviare la coppia nello
// stesso tick in cui e' stata disegnata; 0 = invio al tick seguente.
void SetSameTickWait(float ms);

// Thread di rendering: percentuale della scena (ScreenPercentage) con cui e'
// stata disegnata l'immagine dell'occhio; il ritaglio si restringe attorno al
// centro del back buffer e il compositor la riallarga.
void SetRenderScreenPercent(int eye, int percent);

// Rettangolo (pixel del back buffer) della porzione di immagine dell'occhio.
RECT EyeCropRect(int eye);

// Dissolvenze del compositor (qualsiasi thread; eseguite dal thread di uscita).
// Blink: nero subito, poi l'immagine torna in "seconds" (nasconde un taglio:
// teletrasporto, cambio di schermo, snap turn). HoldBlack: nero finche' resta
// vero (camera del gioco lontana da Walker prima dello schermo virtuale).
void Blink(float seconds);
void HoldBlack(bool hold);

}  // namespace vr::output
