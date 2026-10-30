#pragma once

#include <openvr.h>

// Controller Oculus Touch -> controller Xbox per il gioco.
//
// Il gioco legge il pad con XInputGetState / XInputSetState (XINPUT1_3,
// importati per ordinale 2 e 3). Sostituiamo quelle due voci della IAT:
// per il controller 0, se i Touch sono attivi in SteamVR, restituiamo il loro
// stato tradotto (layout Xbox); la vibrazione diventa un impulso aptico.
// Altrimenti si passa all'XInput vero (un pad reale continua a funzionare).
namespace vr::input {

void InstallXInputHooks();

// Riporta le pose dei controller (mano sinistra e destra) al riferimento dei
// Touch: pose = raw * tip(controller) * inverse(tip(Touch)). Da chiamare con
// il lock di sessione dopo GetDeviceToAbsoluteTrackingPose.
void CorrectControllerPoses(vr::IVRSystem* sys, vr::TrackedDevicePose_t* poses);

// Input finto per i test senza visore: valori stick in [-1,1], grilletti in
// [0,1], bottoni = maschera XINPUT_GAMEPAD_*; durata in millisecondi.
// Impulso di conferma sulla mano (0 sinistra, 1 destra) per ms millisecondi,
// tramite le azioni di SteamVR Input (l'impulso legacy viene ignorato quando
// c'e' un manifesto di azioni).
void PulseHand(int hand, unsigned ms);
// Impulso d'urto (voce 3 del piano Boneworks): ampiezza 0..1, frequenza in Hz
// (0 = 150 Hz come gli altri), durata esatta in ms. Tra due impulsi d'urto in
// corso vince il piu' forte; contro la vibrazione del gioco (mano dell'arma) e
// gli impulsi fissi qui sopra vince il piu' forte frame per frame (UpdateHaptics).
void PulseHand(int hand, unsigned ms, float amp, float freqHz = 0.0f);

void SetFakeInput(float lx, float ly, float rx, float ry, float lt, float rt, unsigned buttons, unsigned durationMs);

// Letture del pad 0 servite dalla mod finora (diagnostica: quante per tick).
unsigned PadPolls();

// Controlli sul percorso del pad 0 nel motore, ogni 0,5 s (vedi vr_input.cpp).
void WatchPadPath();
void TestPlantDirectInput(bool keep);
// Riscrive manifest e binding di SteamVR Input (specopsvr_input/) e ritorna il
// percorso del manifest ("" se fallisce): comando "inputfiles" per i test.
std::string WriteActionFiles();
// Ricarica fisica: caricatore estratto o in mano (l'osso "Magazine" dell'arma va nascosto, R-W7).
bool MagazineRemoved();
// Ricarica fisica con la mano sinistra (sul caricatore, caricatore nuovo in mano, castello): vibrazione degli urti.
bool LeftHandOnReload();

}  // namespace vr::input
