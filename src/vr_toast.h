#pragma once
#include <string>

// Avvisi brevi nel visore (R-U1): overlay di SteamVR agganciato alla testa,
// testo disegnato con GDI, 3,5 s per avviso, coda di 4. Notify() da qualsiasi
// thread; Tick() dal worker dell'uscita VR a ogni frame. "toasts off" li spegne.
namespace vr::toast {

void Notify(const std::string& text);
void Tick();

}  // namespace vr::toast
