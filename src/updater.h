#pragma once

namespace vr::updater {

// Controllo dell'ultima release su GitHub in un thread a parte; se piu' recente
// installa la nuova d3d9.dll per il prossimo avvio (vedi updater.cpp).
void Start();
// Uscita normale del gioco (DLL_PROCESS_DETACH): azzera il contatore degli
// avvii falliti della versione appena installata (vedi la sentinella in
// updater.cpp).
void OnCleanExit();

}  // namespace vr::updater
