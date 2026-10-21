#pragma once
#include <string>

// HUD al polso (R-U3): overlay di SteamVR agganciato al controller di
// supporto, come un orologio: munizioni (caricatore / riserva), arma, salute.
// Disegnato con GDI, aggiornato solo quando i dati cambiano. Visibile solo in
// gioco (non sullo schermo virtuale ne' nei menu). "wristhud off" lo spegne.
namespace vr::wrist {

// Dati dal thread di gioco (hooks.cpp), letti dal worker dell'uscita VR.
void SetData(bool show, int clip, int reserve, int health, const std::string& weapon);
// Dal worker dell'uscita VR a ogni frame (con il lock condiviso della sessione).
void Tick();

}  // namespace vr::wrist
