#pragma once
#include <cstdint>
#include <string>

// Chiamate a funzioni UnrealScript dalla mod, tramite UObject::ProcessEvent
// (voce 66 della vtable, 0x005AAF20: l'unica con firma (funzione, parametri,
// risultato) che legge FunctionFlags +0x94 e ParmsSize +0xA6). Solo sul
// thread di gioco.
namespace vr::script {

// Nome (testo FName) di un oggetto UE3, vuoto se il puntatore non e' valido.
std::string ObjectName(void* obj);
// Nome della classe di un oggetto.
std::string ClassName(void* obj);
// true se la classe dell'oggetto e' "base" o deriva da essa.
bool IsA(void* obj, const char* base);

// Comandi in coda dal thread di controllo, eseguiti in Tick() sul thread di
// gioco: "giveweapon <ClasseArma>".
void QueueGiveWeapon(const std::string& weaponClass);
// "console <comando>": PlayerController.ConsoleCommand (es. "open SP_Gorge01_P").
void QueueConsole(const std::string& command);
// Thread di gioco: esegue subito un comando da console (PlayerController).
void ExecConsole(void* pc, const std::string& command);
// Oggetto ancora vivo: il suo posto in GObjects punta ancora a lui (un
// puntatore del livello precedente non passa).
bool LiveObject(void* obj);
// Indice FName di un testo (senza distinzione maiuscole), -1 se non esiste.
// Il risultato e' messo in cache.
int32_t NameIndex(const std::string& text);
// Testo di un indice FName.
std::string NameFromIndex(int32_t idx);
// Chiama per nome una funzione UnrealScript (anche nativa) dell'oggetto, con
// i parametri gia' preparati nel layout della funzione. Solo thread di gioco.
bool CallFunction(void* obj, const char* name, void* parms);
// Azzera le esclusioni per oggetto delle interrogazioni delle ossa (WI-3 fix 1:
// al cambio di Pawn, cosi' un oggetto nuovo allo stesso indirizzo non le eredita).
void ClearObjectFaults();

// "ucall <oggetto> <Funzione> [argomenti]": chiamata generica a una funzione
// UnrealScript (oggetto = pc|pawn|cheat|world|0x...|Classe[#k]).
void QueueCall(const std::string& line);
// "uprops <oggetto> [<parte del nome>]": proprieta' della classe dell'oggetto
// (nome, tipo, offset, valore) nel log, per riflessione. Diagnostica.
void QueueProps(const std::string& line);
void Tick(void* pc, void* pawn);

}  // namespace vr::script
