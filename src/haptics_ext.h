#pragma once
#include <string>

// Feedback aptico esterno: ProTubeVR (ForceTube / ProVolver, tramite il ponte
// a 64 bit o ForceTubeVR_API_x32.dll accanto al gioco) e bHaptics (giubbotto TactSuit e
// braccia Tactosy tramite il bHaptics Player, websocket locale). Entrambi
// facoltativi: senza dispositivo/dll/Player non fanno nulla.
namespace vr::haptics {

void Start();  // all'avvio (thread propri, non blocca)

// Colpo sparato dall'arma del giocatore. strength 0..1 (dipende dall'arma).
void Shot(const std::string& weaponClass, bool twoHanded);
// Danno subito (salute persa). angleRad: direzione da cui arriva il colpo
// rispetto alla testa (0 = davanti, +pi/2 = destra, +-pi = dietro); NAN se
// non nota (tutto il giubbotto).
void Damage(int amount, float angleRad);
// Gesti e azioni.
void GrenadeThrow();
void Melee();
void Reload();

}  // namespace vr::haptics
