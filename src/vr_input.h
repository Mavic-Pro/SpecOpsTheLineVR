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

void SetFakeInput(float lx, float ly, float rx, float ry, float lt, float rt, unsigned buttons, unsigned durationMs);

}  // namespace vr::input
