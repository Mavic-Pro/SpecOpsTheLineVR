#pragma once
#include <string>

// Menu delle impostazioni nel visore (richiesta del 21/10): un overlay di
// SteamVR davanti al giocatore, puntato con il laser dei controller (eventi
// mouse dell'overlay), con le schede Basic / Advanced / Controls. Ogni voce
// ha una riga di spiegazione (Basic: tabella interna; Advanced: i commenti
// dell'ini accanto alla dll). Le modifiche vengono eseguite subito
// (Execute) e salvate nell'ini (SaveIniValue). Si apre da solo all'avvio
// sul menu principale del gioco ("vrmenu on"), "Play" lo chiude; "vrmenu
// show" lo riapre, "vrmenu off" non lo mostra mai da solo.
namespace vr::menu {

// Dal thread della sessione (sotto il lock condiviso): eventi dell'overlay
// e apertura automatica. cinema = schermo virtuale attivo (menu del gioco).
void Tick(bool cinema);
// Richiesta di apertura/chiusura da un comando.
void Show(bool show);
// Vero mentre il menu e' aperto: il pad del gioco resta neutro.
bool IsOpen();
// Collaudo ("vrmenu cycle <chiave> [dir]"): come un clic sulla voce, al
// prossimo Tick (review 2 WI-1).
void TestCycle(const std::string& key, int dir);
// Collaudo ("vrmenu dump <file.bmp> [basic|advanced|controls] [vd|novd]",
// review WI-4c round 1): la scheda disegnata in BMP al prossimo Tick.
void TestDump(const std::string& path, const std::string& tab, const std::string& vd);

}  // namespace vr::menu
