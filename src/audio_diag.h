#pragma once
#include <string>
// Audio: l'uscita del gioco (FMOD, WASAPI) segue il dispositivo di
// riproduzione predefinito di Windows (Virtual Desktop partito dopo il gioco).
namespace vr::audio {
std::string DefaultDeviceName();
// Scrive nel log il nome del dispositivo, solo quando cambia rispetto all'ultima chiamata.
void LogDefaultDevice(const char* when);
// Dopo MH_Initialize, prima che il motore inizializzi FMOD: hook sul client
// WASAPI (formato fisso 48 kHz stereo float) e su FMOD System::init.
void InstallAudioHooks();
// Ogni 2 s (thread di uscita VR): predefinito cambiato -> richiesta di riapertura.
void Tick();
// Riapertura dell'uscita FMOD su un driver (0 = predefinito di Windows): la
// richiesta si fa da qualsiasi thread ("audioroute reopen"), l'esecuzione dal
// thread del gioco (hook dell'ascoltatore 3D, che passa l'EventSystem).
void RequestReopen(int driver);
bool ReopenPending();
void ReopenOutput(void* eventSystem);
// Dispositivi con una sessione audio attiva di questo processo ("audioroute where").
std::string GameAudioDevices();
}  // namespace vr::audio
