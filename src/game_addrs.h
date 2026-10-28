#pragma once
// Indirizzi dell'eseguibile del gioco usati dalla mod. Erano costanti fisse
// dell'exe Steam (build 12/07/2012); ora sono risolti all'avvio:
// 1) se all'indirizzo Steam ci sono i byte attesi, si usa quello;
// 2) altrimenti si cerca la firma (game_sigs.inc, generata da
//    tools/gensigs.py) nel codice: copie non Steam o altre build;
// 3) se non si trova (o non e' unica) il valore resta 0 e le funzioni che ne
//    dipendono si spengono invece di agganciare un punto a caso.
// Le voci della tabella degli import si trovano per nome della funzione.

#include <cstdint>
#include <string>

namespace vr::addr {

enum Id : int {
#define SIG_CODE(name, va, skip, pat) name,
#define SIG_DATA(name, va, ref, off, pat) name,
#define SIG_IAT(name, va, dll, fn, ord) name,
#include "game_sigs.inc"
#undef SIG_CODE
#undef SIG_DATA
#undef SIG_IAT
    Count
};

// Valori risolti (0 = non trovato). Le costanti k...Addr dei vari file sono
// riferimenti a queste celle, quindi il codice esistente non cambia.
extern uintptr_t g[Count];

// Da chiamare una volta, per prima cosa in DllMain. Con il file
// "specopsvr_forcesigscan" nella cartella del gioco ignora gli indirizzi Steam
// e risolve tutto dalle firme (per provare la ricerca sulla copia Steam).
void Resolve();

inline bool Ok(Id id) { return g[id] != 0; }
const char* Name(Id id);
int ResolvedCount();
bool AllAtSteamAddresses();     // stessa disposizione dell'exe Steam analizzato
std::string MissingList();      // nomi non trovati, separati da virgola ("" = nessuno)
// Limiti del codice dell'exe (per riconoscere indirizzi di ritorno del gioco sullo stack).
uintptr_t TextBegin();
uintptr_t TextEnd();

}  // namespace vr::addr
