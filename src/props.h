#pragma once

#include <cstdint>
#include <string>

// Oggetti di scena da prendere con le mani (piano "mondo interagibile", 24/10).
// Fase 1: gli oggetti gia' fisici del gioco (KActor e derivati: cartelli,
// cappelli, portiere, oggetti creati per i test) si prendono col grip, restano
// in mano guidati dalla fisica (velocita' verso la mano, quindi urtano muri e
// oggetti invece di attraversarli) e al rilascio partono con la velocita' della
// mano. Tutto sul thread di gioco (Draw di FViewport).
namespace vr::props {

// Una volta per tick di gioco (le chiamate in piu' dello stesso tick, una per
// occhio, non fanno nulla). hands: [0] destra, [1] sinistra, posizione nel mondo
// (bersaglio IK) e assi della mano (righe X, Y, Z), valid se il controller c'e'.
void Tick(void* pawn, const float handPos[2][3], const float handAxes[2][3][3], const bool valid[2]);

// Riferimento delle mani di questo tick (UpdateArms, subito prima di Tick):
// le mani stanno in center + Rz(posYaw) x scostamento del controller, i loro
// assi girano con axesYaw (unita' di rotatore). refYaw = yaw del riferimento
// della testa (x g_yawSign) con cui sono calcolati gli scostamenti, originOff
// = scostamento dell'origine del tracciamento (riferimento del corpo,
// unita'): un punto fermo nella stanza si muove nel mondo solo se cambiano
// center, l'origine o posYaw - refYaw. Un salto (giro a scatti, spostamento
// scriptato, ricentramento) porta con se' l'oggetto tenuto e le sfere delle
// mani (voce 2 del piano, solo con "objectweight" acceso).
void SetHandFrame(const float center[3], int32_t posYaw, int32_t axesYaw, int32_t refYaw, const float originOff[3]);

// Ogni Draw: se Tick non arriva da 0,3 s (scena, menu, braccia spente) gli
// oggetti tenuti cadono.
void Watchdog();

// L'attore e' un oggetto tenuto in mano o la sfera di una mano (le trace
// della vista lo ignorano: non e' un muro). Thread di gioco.
bool IsHeldActor(const void* actor);

// L'attore e' la sfera invisibile di una mano (mani solide). Le sfere non
// prendono parte alle trace (bit BlockZeroExtent/BlockNonZeroExtent spenti in
// ProxyCreate): se una trace della mod ne colpisce una lo stesso, la salta e
// chiama HandSphereTraced (nel log; i bit si rispengono). Thread di gioco.
bool IsHandSphere(const void* actor);
void HandSphereTraced(const void* actor, const char* who);

// Dita della mano (lato 1 = mano di supporto) chiuse sull'oggetto tenuto, a
// pugno col grip stretto a vuoto: dopo la composizione dello scheletro
// (UpdateSkelPose), sulle SpaceBases. handSuffix: "Left" o "Right" (ossa
// Pinky_Left, Index_Left...).
void PoseFingers(void* mesh, int side, const char* handSuffix);

// Mano libera disegnata che segue la fisica (voce 5 del piano, "solidhands";
// solo side 1, la mano di supporto). Da UpdateArms subito prima di
// WriteHand(1), con la sinistra fuori dall'arma (gripBlend 0) e il
// controller presente. tgtPos/tgtAxes = bersaglio della mano (quello della
// fisica, g_handWorld[1]: resta lui); outPos/outAxes = la posa da disegnare:
// sull'oggetto tenuto (dove la presa senza peso lo vuole rispetto alla mano),
// sul punto afferrato di portiere e cartelli, con "full" sulla sfera della
// mano vuota; zona morta, tetto e filtro (40 ms in entrata, 100 in uscita).
// shoulder/reach (ArmReach, reach <= 0 = sconosciuta): la posa resta entro la
// portata del braccio (armstretch). Ritorna la distanza disegnata - bersaglio
// (cm). Con solidhands off la posa e' il bersaglio, identica. Thread di gioco.
float RenderHandPose(int side, const float tgtPos[3], const float tgtAxes[3][3], const float* shoulder, float reach, float outPos[3],
                     float outAxes[3][3]);
// Questo frame la mano disegnata e' un'altra (sull'arma, senza controller):
// niente scostamento, filtro azzerato.
void RenderHandNone(int side);
// Posa scritta nell'IK oltre la portata del braccio (hooks.cpp, solo con
// solidhands e ArmReach noto): le ossa non ci arrivano e MeasurePalm non
// misura (la distanza mancante entrava in g_palmLocal). Thread di gioco.
void SetHandBeyondReach(int side, bool beyond);

// Oggetto da evidenziare (puntato coi guanti o a portata di mano): box nel
// mondo e stato (1 puntato / vicino, 2 agganciato; voce 8: 3 granata nemica a
// terra, 4 granata nella zona della mano sinistra). Thread di gioco.
bool Highlight(float mn[3], float mx[3], int* state);

// Suggerimenti contestuali sugli oggetti (vr_toast HintsTick, regressione
// finale del gameplay 27/10): bit 1 << vr::ui::ObjHint delle situazioni
// dell'ultimo tick (oggetto nella zona della sinistra, oggetto in mano, nemico a
// portata con un oggetto in mano, granata nemica con gli angoli rossi, arma a
// terra nella zona della sinistra, bombola esplosiva in mano). Thread di gioco.
unsigned HintSituations();

// Granate nemiche rilanciate (voce 8, "grenadeback"): true per la granata
// nella mano sinistra e per quelle che la mano ha rilanciato o lasciato (ora
// hanno Walker come Instigator): il reindirizzamento dei proiettili di Walker
// (hooks.cpp, granata del gesto, RPG e Milkor) non le tocca. Thread di gioco.
bool IsThrownBackGrenade(const void* actor);

// Comando "spawnprop <mesh> [distanza_cm]": crea un oggetto fisico con quella
// StaticMesh davanti alla mano sinistra (collaudi e fase 2); scale3 (x, y, z)
// opzionale: la massa di un'istanza scalata (revisione della voce 1).
void QueueSpawn(const std::string& mesh, float distCm, const float* scale3 = nullptr);

// Colpo dell'arma di Walker (hook di CalcWeaponFire, thread di gioco): un
// oggetto di scena colpito diventa fisico e riceve l'impulso (fase 4).
// hitComp e' ImpactInfo.HitInfo.HitComponent. weapon = l'arma (YWeapon): il
// suo danno va a una bombola esplosiva portata (voce 5).
void OnShot(const float* start, const float* hit, void* hitActor, void* hitComp, void* weapon = nullptr);

// Scudi con oggetti tenuti (voce 3 del piano dei combattimenti, 26/10):
// colpo istantaneo di un'arma che non e' quella di Walker (nemici, squadra),
// dall'hook di CalcWeaponFire subito dopo la trace del gioco (thread di
// gioco, dentro il ciclo dei colpi FUN_0093F510: danno ed effetti del colpo
// vengono dopo, da impact). walker = il Pawn di Walker. Se il colpo ha preso
// un oggetto tenuto: vibrazione, spinta e usura (nel tick dopo). Se ha preso
// Walker ma il segmento start -> punto colpito attraversa la scatola di un
// oggetto tenuto: impact riscritto sull'oggetto (ritorna 1). Con "propshield
// off" e il colpo su un oggetto tenuto ritorna 2: chi chiama rifa' la trace
// con gli oggetti tenuti fuori (HeldTraceable). Altrimenti 0. Solo i colpi dei
// nemici (Instigator ostile, PawnHostile) hanno effetti: quelli dei compagni li
// ferma il gioco e basta. bullet = false: chiamata da uno script
// (AYWeapon_execCalcWeaponFire), non un proiettile: contata e ignorata.
int OnOtherShot(void* weapon, void* walker, const float* start, const float* end, void* impact, bool bullet);
// Posizione della vista dell'occhio appena disegnato (testa), prima di Tick:
// l'oggetto colpito vola via lontano da li'.
void SetViewOrigin(const float* loc);
// Bit delle trace a estensione zero dei componenti degli oggetti tenuti
// (PrimitiveComponent +0x14C, BlockZeroExtent 0x8): off li spegne e salva i
// valori in saved[2], on li rimette. Solo attorno a una trace (propshield off).
void HeldTraceable(bool on, uint32_t saved[2]);
// Collaudo "enemyfire 0x<pawn> <colpi> [x y z]": il Pawn spara con la sua arma
// (m_currentWeapon) il ciclo dei colpi istantanei del gioco (FUN_0093F510,
// lo stesso di ogni YWeapon) da 20 unita' davanti al suo petto verso (x, y,
// z), o verso l'oggetto tenuto (senza punto). Nel tick dopo.
void QueueEnemyFire(uint32_t pawn, int shots, const float* target);
// Collaudo "shieldtrace on|off": off = gli oggetti tenuti restano fuori dalle
// trace a estensione zero (come un oggetto che la trace del gioco non prende),
// per provare la riscrittura del colpo su Walker. on = come sempre: il bit
// torna nel tick dopo (e a ogni oggetto appena lasciato, anche durante il
// collaudo).
void SetShieldTraceTest(bool traceable);

// Armi dei nemici raccolte con la sinistra (voce 6 dei combattimenti, 27/10).
// Collaudo "weapontest": la sinistra prende l'arma a terra piu' vicina (entro
// 3 m) senza zona di presa, o ne prende le munizioni se Walker ha gia' quell'arma.
void QueueWeaponTest();
// Collaudo "corpsetest [bones]" (voce 11): la sinistra afferra il corpo del cadavere piu' vicino (entro 2 m),
// senza zona; "bones" scrive nel log i corpi del cadavere (osso e posizione).
void QueueCorpseTest(bool bones);
// "weapongrip x y z p y r": posa della radice dell'arma negli assi del
// bersaglio della mano sinistra (cm lungo X avanti, Y su, Z a sinistra; gradi
// beccheggio attorno a Z, imbardata attorno a Y, rollio attorno a X).
// Predefinita 6 4 0 0 0 -90. Messa in coda: la applica il tick (thread di gioco).
void SetWeaponGrip(const float off[3], const float rot[3]);
// true se la mano h prende da sola questa pickup d'arma (grabuse non preme A
// per lei): quella tenuta, con qualsiasi mano; per la sinistra (h 1) una
// YPickup_WeaponSpawnable (o sottoclasse) con il PhysicsAsset e non temporanea,
// con weaponpickup e grabprops accesi e il tick delle prese attivo (non in
// cinema ne' a gioco bloccato). Thread di gioco.
bool WeaponForHand(void* actor, int h);
// true se e' una YPickup_WeaponSpawnable tornata nel pool del gioco
// (m_pickupMesh None, m_pickupHidden o attore nascosto): non e' piu'
// un'interazione. Quella appena raccolta (passaggio alla destra, DoPickup)
// resta dov'era, accanto alla mano dell'arma. Thread di gioco.
bool WeaponPickupGone(void* actor);

// Collaudo: la mano sinistra prende l'oggetto (fisico o di scena) piu' vicino
// la cui mesh contiene meshPart, senza zona di presa ("grabtest <nome>").
void QueueGrabTest(const std::string& meshPart);
// "glasshit 0x<attore> <kg> <m/s>" (solo collaudi, voce 2): un colpo finto di un
// oggetto lanciato contro un bersaglio del mondo (mesh fratturabile, finestra
// di sabbia, distruttibile), da Walker verso il centro del suo box, passato a
// WorldDamage come un evento della pipeline. Prova il danno senza una traiettoria.
void QueueGlassHit(uint32_t actor, float kg, float speed);
// "explhit <danno>" (solo collaudi, voce 5): danno alla prima bombola portata,
// come un colpo (0 = quanto basta per farla esplodere), da Walker: prova lo
// spostamento della YExplosiveActor e la sua esplosione senza mirare.
void QueueExplHit(float damage);

// Comando "griptune <nome> <valore>": misure della presa canonica al volo
// (palm, gap, finger, thumb in cm; height, back frazioni; round 0..1 o -1
// automatico; gain 0,1..1 = frazione dell'errore corretta per tick dalla
// guida; relgrace s di esenzione delle sfere dopo il rilascio; minspeed m/s
// minimi dell'oggetto tenuto; pawnframe 1/0 limiti rispetto a Walker o nel
// mondo; peso, voce 2: force N, torque N m, klin N/m, krot N m/rad, sagcap
// cm, droopcap gradi, brake frazione, liftmin m/s^2, pawnacc m/s^2, overshoot cm, overtime ms,
// alphamin rad/s^2 = pavimento della rotazione, fingerreach cm = dita verso l'oggetto vero,
// framevel 1/0 = velocita' di Walker dal centro del riferimento delle mani o da Actor.Velocity;
// distacco degli oggetti incastrati: breaktime ms di raddoppio del budget (0 = spento), breakerr cm di
// errore minimo, breakratio frazione misurata/comandata sotto cui e' bloccato, breakgive ms alla guida
// piena prima di rinunciare, breakturn 1/0 = anche la coppia cresce o resta quella del peso, breakkg kg =
// oltre, niente distacco, breakcap m/s = comando dello strappo al piu' la mano piu' tanto; passvolumes
// 1/0 = l'oggetto tenuto con il peso attraversa i muri invisibili o no; wedgetime s = incastrato senza
// via d'uscita a 16 cm dal palmo, il palmo fuori portata: la mano lo lascia dopo tanto (1, 0 = mai);
// ripresa dove si trova: regriptime s bloccato con la mano ferma prima della ripresa (0 = mai),
// regriperr cm = errore della presa oltre cui e' bloccato, regripreach cm = palmo al piu' a tanto
// dall'oggetto, regripmove cm e regripspeed cm/s = il palmo si sposta di tanto o va piu' veloce: la mano
// si muove, regripsettle s = assestamento nella posa nuova, regriprest s = alla prima mossa della mano
// l'oggetto torna sul palmo in tanto (0 = resta dove l'ha ripreso), stuckspeed m/s = bloccato con la
// mano ferma: al piu' tanto verso il bersaglio (0 = guida piena); vibrazione degli urti, voce 3: hapdv
// m/s = velocita' tolta dal contatto oltre cui c'e' un urto, hapj N s = impulso dell'ampiezza piena,
// haprefr ms = al piu' un impulso ogni tanto per mano, haprearm ms = il contatto finisce per tanto prima
// del prossimo, hapwindow ms = un contatto somma la velocita' tolta per tanto, hapspherekg kg = massa della mano vuota, hapspeed m/s = schiaffo solo oltre tanto;
// lancio, voce 4 (throwmode peak): peakwin ms = il picco negli ultimi tanti ms, peakdt ms = velocita' di un campione da
// quello piu' vecchio ad almeno tanto, peakjump m/s = campioni piu' veloci (rispetto a Walker) scartati, energy J = energia
// massima del lancio (0 = nessun limite), spinmax rad/s = rotazione massima, spinjump rad/s = velocita' angolare di un
// campione oltre cui e' un salto, lever 1/0 = leva del baricentro o solo la mano, peakmin / peakfull m/s = sotto peakmin
// di picco (rispetto a Walker) l'oggetto e' posato con la media al rilascio, da peakfull lanciato col picco (in mezzo
// un passaggio morbido), peakout m/s = un campione che dista dalla mediana dei suoi 3 piu' di max(peakout, 20%)
// la prende (0 = mai); mano che segue la fisica,
// voce 5 (solidhands): shdead cm e shturn gradi = zona morta (smoothstep), shcap cm e shcapturn gradi = tetto,
// shin / shout ms = filtro in entrata e in uscita, shtele cm = con un muro tra sfera e palmo la sfera torna sul
// palmo solo oltre tanto (full), shwall cm = raggio minimo di cio' che conta come muro per quella trace,
// shguard 1/0 = MeasurePalm e le dita misurano sulla posa disegnata (0 solo per il confronto: il palmo deriva);
// forza della presa, voce 6 (analoggrip): gripmin = frazione di forza e coppia alla presa piu' lenta, griplo /
// griphi = valori del grip della presa piu' lenta e piu' stretta, gripturn = guadagno della rotazione alla piu'
// lenta (frazione), gripdroop = piega alla piu' lenta x tanto (mai oltre droopcap), griphold ms = una presa piu' lenta vale dopo tanto,
// gripease ms = poi ci arriva con questa costante, griprise ms = costante di una presa piu' stretta (niente
// scalini), gripthrow ms = il lancio usa la presa piu' stretta di tanto prima (0 = quella di adesso),
// gripfloor g = accelerazione minima con la presa lenta (o quella della presa stretta, se minore).
// Valgono dalla presa successiva (gain, peso e vibrazione subito). false = nome sconosciuto.
bool SetGripTune(const std::string& name, float v);

}  // namespace vr::props
