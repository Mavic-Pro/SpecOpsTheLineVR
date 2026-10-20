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

// Il gioco sostituisce la procedura della finestra dopo CreateDevice senza
// richiamare la nostra: va ricontrollato a ogni frame (EndScene).
void KeepSubclass();

// Dimensione client riportata da GetClientRect: aggiornata a ogni Reset del
// device (setres della risoluzione automatica; prima restava quella di
// CreateDevice e UE3 poteva tornare alla vecchia; audit 20/10).
void SetWantedClientSize(int w, int h);

// Diagnostica ("wndinfo"): procedura di finestra attiva e messaggi ricevuti.
void LogWindowInfo();

}  // namespace vr::display
