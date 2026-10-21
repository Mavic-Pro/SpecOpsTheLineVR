#pragma once
#include <string>

// Avvisi brevi nel visore (R-U1): overlay di SteamVR agganciato alla testa,
// testo disegnato con GDI, 3,5 s per avviso, coda di 4. Notify() da qualsiasi
// thread; Tick() dal worker dell'uscita VR a ogni frame. "toasts off" li spegne.
namespace vr::toast {

void Notify(const std::string& text);
void Tick();
// Suggerimenti sui controller (R-U2): nelle prime 3 sessioni di gioco, dopo
// 15 s di gameplay in 3D, una serie di avvisi (uno ogni 7 s) con i comandi
// principali; "hints on" li mostra sempre, "hints off" mai, "hints auto" (3
// sessioni, contate in "hintsshown" nell'ini). Chiamata dal thread di gioco.
void HintsTick(bool gameplay);

}  // namespace vr::toast
