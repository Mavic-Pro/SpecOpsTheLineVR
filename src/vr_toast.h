#pragma once
#include <string>

// Avvisi brevi nel visore (R-U1): overlay di SteamVR agganciato alla testa,
// testo disegnato con GDI a capo automatico (fino a tre righe, ui_text.h),
// 3,5-7 s per avviso secondo la lunghezza, coda di 4. Notify() da qualsiasi
// thread; Tick() dal worker dell'uscita VR a ogni frame. "toasts off" li spegne.
namespace vr::toast {

void Notify(const std::string& text);
void Tick();
// Suggerimenti sui controller (R-U2): nelle prime 3 sessioni di gioco, dopo
// 15 s di gameplay in 3D, una serie di avvisi (uno ogni 7 s) con i comandi
// principali; "hints on" li mostra sempre, "hints off" mai, "hints auto" (3
// sessioni, contate in "hintsshown" nell'ini). Chiamata dal thread di gioco.
// turret: -1 a piedi, 0 torretta con la presa NORMAL, 1 GUNSTOCK, 2 torretta
// mirata in altro modo: sulla torretta la serie dell'arma aspetta (ui_text.h).
// objectSituations: bit delle situazioni degli oggetti dell'ultimo tick
// (props::HintSituations) per i suggerimenti contestuali sugli oggetti
// (ui_text.h ObjHintStep; regressione finale del gameplay, 27/10).
void HintsTick(bool gameplay, int turret, unsigned objectSituations = 0);
// Collaudo senza visore ("toastdump <cartella>"): i suggerimenti, le guide
// della torretta e gli avvisi del calcio disegnati in BMP con l'impaginazione
// nel log (TOASTDUMP).
void DumpForTest(const std::string& dir);

}  // namespace vr::toast
