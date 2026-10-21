#pragma once
#include <string>

namespace vr::log {

// Apre specopsvr.log accanto alla dll (le tre sessioni precedenti restano come
// specopsvr.1/2/3.log) e scrive l'intestazione: versione, gioco, Windows, ini.
void Init();
// Crash del gioco: riga CRASH (codice, modulo +offset) e minidump accanto al log.
void InstallCrashHandler();
// Rapporto di crash (riga CRASH + minidump, una volta sola) per un'eccezione
// intercettata altrove, p.es. dal filtro del motore agganciato (hooks.cpp).
void ReportCrash(struct _EXCEPTION_POINTERS* info);
void Line(const std::string& message);
// Cartella della dll (= cartella del gioco).
std::string Directory();

}
