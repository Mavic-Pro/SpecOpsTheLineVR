#pragma once
#include <windows.h>

// UE3 limita la finestra (in modalita' finestra) alle dimensioni del desktop,
// lette una sola volta all'avvio con GetSystemMetrics/SystemParametersInfoW.
// Per rendere a una risoluzione piu' alta di quella del monitor (serve al
// visore, non allo schermo) sostituiamo quelle due voci nella IAT dell'exe
// in modo che riportino un desktop piu' grande. Va chiamato in DllMain:
// la nostra dll viene caricata prima che il codice del gioco parta.
namespace vr::display {

void InstallDesktopSizeSpoof();

// Windows limita comunque la finestra a schermo + bordi (WM_GETMINMAXINFO):
// dopo il primo Reset il back buffer tornava 1940x1066. Sottoclassiamo la
// finestra del gioco per alzare quel limite, poi la riportiamo alla dimensione
// client richiesta (Windows l'aveva gia' ridotta dentro CreateWindowEx, prima
// che potessimo sottoclassarla). Da chiamare in CreateDevice.
void UnclampWindow(HWND window, int clientWidth, int clientHeight);

}  // namespace vr::display
