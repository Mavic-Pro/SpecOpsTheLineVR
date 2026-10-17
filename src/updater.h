#pragma once

namespace vr::updater {

// Controllo dell'ultima release su GitHub in un thread a parte; se piu' recente
// installa la nuova d3d9.dll per il prossimo avvio (vedi updater.cpp).
void Start();

}  // namespace vr::updater
