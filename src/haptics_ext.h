#pragma once
#include <string>

// Feedback aptico esterno: ProTubeVR (ForceTube / ProVolver, tramite
// ForceTubeVR_API_x32.dll accanto al gioco) e bHaptics (giubbotto TactSuit e
// braccia Tactosy tramite il bHaptics Player, websocket locale). Entrambi
// facoltativi: senza dispositivo/dll/Player non fanno nulla.
namespace vr::haptics {

void Start();  // all'avvio (thread propri, non blocca)

// Colpo sparato dall'arma del giocatore. strength 0..1 (dipende dall'arma).
void Shot(const std::string& weaponClass, bool twoHanded);
// Danno subito (salute persa); front = colpo da davanti (se noto).
void Damage(int amount);
// Gesti e azioni.
void GrenadeThrow();
void Melee();
void Reload();

}  // namespace vr::haptics
