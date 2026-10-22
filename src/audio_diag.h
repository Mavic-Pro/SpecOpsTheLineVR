#pragma once
#include <string>
// Audio: dispositivo di riproduzione predefinito di Windows e instradamento
// dell'audio del gioco quando il predefinito cambia (Virtual Desktop partito
// dopo il gioco).
namespace vr::audio {
std::string DefaultDeviceName();
// Scrive nel log il nome del dispositivo, solo quando cambia rispetto all'ultima chiamata.
void LogDefaultDevice(const char* when);
// Ogni 2 s (thread di uscita VR): predefinito cambiato -> il processo del gioco viene spostato li'.
void Tick();
// All'uscita: toglie la preferenza per app.
void Shutdown();
// Diagnostica ("audioroute test"): instrada il gioco sul predefinito di adesso e logga l'esito.
void RouteTest();
}  // namespace vr::audio
