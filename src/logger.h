#pragma once
#include <string>

namespace vr::log {

// Apre specopsvr.log accanto alla dll (le tre sessioni precedenti restano come
// specopsvr.1/2/3.log) e scrive l'intestazione: versione, gioco, Windows, ini.
void Init();
// Crash del gioco: riga CRASH (codice, modulo +offset) e minidump accanto al log.
void InstallCrashHandler();
void Line(const std::string& message);
// Cartella della dll (= cartella del gioco).
std::string Directory();

}
