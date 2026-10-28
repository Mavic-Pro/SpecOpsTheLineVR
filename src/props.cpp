#include "game_addrs.h"
#include "props.h"

#include "control.h"
#include "logger.h"
#include "script_call.h"
#include "vr_input.h"
#include "pure_utils.h"
#include "ui_text.h"

#include <windows.h>
#include <MinHook.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace vr::props {
namespace {

// Letture/scritture protette (gli oggetti del gioco possono sparire tra un tick e l'altro).
bool SafeCopy(void* dst, const void* src, size_t n) {
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool TrySafeReadUint32(void* addr, uint32_t* out) { return SafeCopy(out, addr, 4); }

double ProfMs() {
    static LARGE_INTEGER f = {};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}

const uintptr_t& kGObjects = vr::addr::g[vr::addr::GObjects];
constexpr uintptr_t kOffName = 0x2C, kOffClass = 0x34, kOffSuper = 0x3C;
constexpr uintptr_t kOffActorLocation = 0x54;
constexpr uintptr_t kOffActorPhysics = 0x94;          // Actor.Physics (10 = PHYS_RigidBody)
constexpr uintptr_t kOffActorFlags9C = 0x9C;          // bHidden 0x4, bDeleteMe 0x10, bIgnoreRigidBodyPawns 0x200
constexpr uintptr_t kOffActorFlagsA0 = 0xA0;          // bBlockActors 0x40000000
constexpr uintptr_t kOffActorWorldInfo = 0xEC;
constexpr uintptr_t kOffActorCollisionComp = 0x194;
constexpr uintptr_t kOffWorldTimeSeconds = 0x454;     // WorldInfo.TimeSeconds
constexpr uintptr_t kOffCompLocalToWorld = 0x70;      // PrimitiveComponent.LocalToWorld (righe X, Y, Z, T)
constexpr uintptr_t kOffCompBounds = 0xEC;            // FBoxSphereBounds: Origin, BoxExtent, SphereRadius
constexpr uintptr_t kOffCompBodyInstance = 0x170;     // PrimitiveComponent.BodyInstance
constexpr uintptr_t kOffKActorSmc = 0x1F8;            // DynamicSMActor.StaticMeshComponent
constexpr uint8_t kPhysRigidBody = 10;
constexpr uint32_t kHidden = 0x4, kDeleteMe = 0x10, kIgnoreRbPawns = 0x200;
constexpr uint32_t kBlockActors = 0x40000000;
constexpr uint32_t kWorldGeometry = 0x100;            // Actor.bWorldGeometry (+0x9C)
constexpr uintptr_t kOffCompOwner = 0x4C;             // ActorComponent.Owner
constexpr uintptr_t kOffCompVisFlags = 0x134;         // PrimitiveComponent: HiddenGame 0x4
constexpr uint32_t kCompHiddenGame = 0x4;
constexpr uintptr_t kOffCompAttached = 0x50;          // UActorComponent::bAttached (bit 0), richiesto da InitRBPhys
constexpr uintptr_t kOffSmcStaticMesh = 0x214;        // StaticMeshComponent.StaticMesh
constexpr uintptr_t kOffMeshBodySetup = 0x90;         // StaticMesh.BodySetup (collisione semplice: serve al corpo rigido)
constexpr uintptr_t kOffMeshCanBecomeDynamic = 0xE8;  // StaticMesh.bCanBecomeDynamic (bit 0): le bottiglie del gioco

struct Prop {
    void* actor;  // KActor; per un oggetto statico l'attore di geometria che lo contiene
    void* comp;   // identita' dell'oggetto (una collezione contiene molti componenti)
    float boxMin[3], boxMax[3];
    float radius;
    bool isStatic = false;  // ancora statico: diventa fisico alla presa (MakePropDynamic)
    bool constrained = false;  // su cerniera / vincolo (portiere, cartelli: fase 5)
    bool explosive = false;  // mesh di una YExplosiveActor (bombola): alla presa la porta un KActorSpawnable (voce 5)
};

// Palmo della mano (misurato dalle ossa in PoseFingers): centro e normale
// (verso cui si chiudono le dita) negli assi della mano, rispetto al bersaglio
// IK. Ultima posa della mano passata a Tick, per la conversione.
float g_palmLocal[2][3] = {}, g_palmNormalLocal[2][3] = {}, g_palmAxisLocal[2][3] = {};
bool g_palmValid[2] = {false, false};
// Posa scritta oltre la portata del braccio (SetHandBeyondReach, voce 5):
// MeasurePalm non misura.
bool g_beyondReach[2] = {false, false};
float g_lastHandPos[2][3] = {}, g_lastHandAxes[2][3][3] = {};
bool g_lastHandValid[2] = {false, false};
// Velocita' angolare del polso (rad/s) calcolata da Drive per l'oggetto tenuto:
// la pipeline dei colpi ci ricava la velocita' dei punti dell'oggetto tenuto.
float g_heldHandW[2][3] = {};

// Presa canonica (utente 24/10: "se fai ruotare durante la presa l'oggetto in
// modo che arrivi alla mano sempre nella stessa posa e' tutto piu' facile"):
// terna delle nocche negli assi della mano, misurata in MeasurePalm dalle basi
// delle 4 dita, rigide con la mano (non dipende dall'animazione).
// o = media delle nocche; f = verso le dita; a = dal mignolo all'indice;
// p = verso cui si chiudono le dita (palmo). span = nocca del mignolo e
// dell'indice lungo a, da o (unita').
struct GripFrame {
    bool valid = false;
    float o[3] = {}, f[3] = {}, a[3] = {}, p[3] = {};
    float span[2] = {};
};
GripFrame g_grip[2];

// Misure della presa (cm); "griptune <nome> <valore>" le cambia al volo.
struct GripTune {
    float palm = 1.5f;     // centro delle nocche -> pelle del palmo
    float gap = 0.3f;      // aria tra palmo e oggetto
    float finger = 0.9f;   // raggio di un dito
    float thumb = 1.0f;    // raggio del pollice
    float height = 0.30f;  // centro della mano dalla base (frazione della lunghezza; base nota)
    float back = 0.6f;     // centro dell'oggetto verso il polso: 0,4 cm + back * mezzo spessore
    float round = -1.0f;   // arrotondamento della sezione 0..1 (-1 = dalla sfera dei limiti)
    float gain = 0.5f;     // frazione dell'errore corretta per tick nella guida dell'oggetto (Drive)
    float relGrace = 0.3f;  // s: oggetto appena lasciato ignorato dalle sfere delle mani (0 = come prima)
    float minSpeed = 5.0f;  // m/s: velocita' massima dell'oggetto tenuto mai sotto questa, qualunque sia la massa (HeldMaxSpeed)
    float pawnFrame = 1.0f;  // 1: limiti di velocita' dell'oggetto tenuto rispetto a Walker (PawnVelocity); 0 = nel mondo, come prima
    // Peso (voce 2 del piano, "objectweight"; valori di "full": light raddoppia
    // forza e coppia e dimezza abbassamento e piega). Senza fonte (SLZ non ha
    // pubblicato i suoi), tarati con grablog nei collaudi del 25/10. Revisione
    // del 25/10: forza e coppia dimezzate (400 -> 200 N, 25 -> 12,5 N m),
    // cosi' il predefinito light da' i valori del piano (radio 60-120 ms,
    // barile al piu' 8 rad/s^2); prima light li dava solo con full.
    float force = 200.0f;    // N: forza massima della mano sull'oggetto, gravita' compresa (WeightOf)
    float torque = 12.5f;    // N m: coppia massima (accelerazione angolare = coppia / inerzia dei limiti)
    float alphaMin = 3.0f;   // rad/s^2: accelerazione angolare minima, qualunque siano massa e misure (mai un rifiuto; light x2)
    float fingerReach = 3.0f;  // cm (x 5 = gradi): le dita seguono l'oggetto vero fin qui dalla posa voluta (HeldShape, con il peso)
    float kLin = 2000.0f;    // N/m: rigidezza verticale della presa (abbassamento = kg x 9,81 / kLin)
    float kRot = 60.0f;      // N m/rad: rigidezza della presa alla rotazione (piega = coppia del peso / kRot)
    float sagCap = 6.0f;     // cm: abbassamento massimo
    float droopCap = 20.0f;  // gradi: piega massima
    float brake = 0.7f;      // frazione dell'accelerazione disponibile usata per frenare in arrivo (margine per il ritardo di un tick)
    float overshoot = 2.0f;  // cm (x 2,5 = gradi): quanto puo' oltrepassare il bersaglio se la mano si ferma di colpo (arresto sicuro)
    float overTime = 15.0f;  // ms: o quanto fa in questo tempo, se di piu' (oggetti leggeri e veloci)
    float liftMin = 3.0f;    // m/s^2: accelerazione verso l'alto minima al netto della gravita', qualunque sia la massa (mai un rifiuto; light x2)
    float pawnAcc = 60.0f;   // m/s^2: accelerazione di Walker data gratis all'oggetto (Actor.Velocity ha picchi di un tick)
    float frameVel = 1.0f;   // 1: velocita' di Walker per il peso dal centro del riferimento delle mani (FrameVelocity); 0 = Actor.Velocity, come prima
    // Distacco (seguito della voce 2, 25/10: oggetti incastrati; Drive,
    // BreakawayStep). Un oggetto bloccato dai contatti mentre la mano tira via
    // riceve un budget di forza che raddoppia ogni breakTime ms, fino alla
    // guida senza peso; libero, torna subito al budget del peso.
    float breakTime = 30.0f;   // ms: raddoppio del budget (0 = niente distacco, come prima)
    float breakErr = 8.0f;     // cm: errore minimo perche' un oggetto bloccato conti come incastrato (sotto: appoggiato a qualcosa)
    float breakRatio = 0.35f;  // bloccato: velocita' misurata sotto questa frazione di quella comandata
    float breakGive = 100.0f;  // ms alla guida piena ancora bloccato: rinuncia (muro, oggetto inamovibile)
    float breakTurn = 0.0f;    // 1: anche la coppia cresce nel distacco; 0 = la rotazione resta col budget del peso
    float passVolumes = 1.0f;  // 1: l'oggetto tenuto (con il peso) attraversa i muri invisibili (BlockingVolume); 0 = come prima
    // Revisione del seguito (25/10 pomeriggio). Incastrato senza via d'uscita
    // (posa voluta dentro la cassa, mano ferma): la mano lo lascia (WedgeStep).
    // Seguito 2 (25/10 sera): con il palmo sull'oggetto la presa si riprende
    // (Regrip in WedgeStep) e il rilascio vale solo con il palmo fuori
    // portata. Revisione del seguito 2: di nuovo 1 s (con 0 un oggetto
    // bloccato lontano dal palmo restava "in mano" per sempre, la mano chiusa
    // sul vuoto a 20-60 cm: smR4, 3 s a 24 cm finche' la mano non scendeva).
    float wedgeTime = 1.0f;    // s: fermo, il palmo fuori portata e l'errore della presa 2 x breakErr per tanto: lo lascia; piu' lontano prima (0 = mai)
    float breakKg = 30.0f;     // kg: oltre, niente distacco (barile trascinato: l'attrito non e' un incastro, il peso resta)
    float breakCap = 4.0f;     // m/s: comando dello strappo al piu' la mano piu' tanto, rispetto a Walker (prima maxSpeed, 30 m/s)
    // Seguito 2 (25/10 sera): l'oggetto bloccato con la mano ferma che lo tocca
    // non viene piu' lasciato: la presa si riprende dove si trova (Regrip in WedgeStep).
    float regripTime = 0.2f;   // s: bloccato (senza avvicinarsi) con la mano ferma e il palmo sull'oggetto per tanto: presa ripresa li' (0 = mai, come prima)
    float regripErr = 4.0f;    // cm: errore della presa (media 0,05 s) oltre cui l'oggetto fermo conta come bloccato (sotto breakErr: una bottiglia vibrava a 6-8 cm per 12 s)
    // Revisione del seguito 2: 16 cm (era 8, la zona di presa). La presa canonica
    // che gira la bottiglia della cassa con la mano ferma la puo' lasciare a
    // 9 cm dal palmo (suite, presa statica): con 8 cm e weightwedge 1 la mano la
    // lasciava dopo 1,85 s, la regressione del seguito. Dopo la ripresa l'appoggio
    // la riporta sul palmo alla prima mossa; il rilascio resta per la mano
    // davvero lontana (18-21 cm nello smash loop, 19,5 in smR4).
    float regripReach = 16.0f; // cm: palmo al piu' a tanto dalla scatola dell'oggetto (oltre la mano non lo tiene piu': niente ripresa, rilascio dopo weightwedge)
    float regripMove = 2.0f;   // cm: il palmo si sposta di tanto nel mondo = la mano si muove (strappo possibile, niente ripresa)
    float regripSettle = 0.15f; // s: dopo la ripresa la posa voluta segue quella vera per tanto (l'oggetto si assesta tra i contatti senza spinte), poi resta li'
    float stuckSpeed = 0.1f;   // m/s: bloccato con la mano ferma (il palmo fuori portata, o prima della ripresa): al piu' tanto verso il bersaglio (0 = guida piena, come prima)
    // Revisione del seguito 2: la mano lenta non e' ferma (tirando via piano un
    // oggetto incastrato la ripresa scattava a ogni 4 cm e lo strappo non
    // partiva mai), e dopo la ripresa l'oggetto torna ad appoggiarsi al palmo
    // quando la mano si muove (come l'appoggio della presa).
    float regripSpeed = 3.0f;  // cm/s: palmo piu' veloce di tanto (media di 0,1 s, nel mondo) = la mano si muove
    float regripRest = 0.25f;  // s: dopo la ripresa, alla prima mossa della mano, l'oggetto scivola sul palmo in tanto (0 = resta dove l'ha ripreso)
    // Vibrazione degli urti (voce 3 del piano, "impacthaptics"): soglie del
    // rilevatore (Drive per l'oggetto tenuto, ProxyTick per la sfera della
    // mano vuota) e scala dell'ampiezza. Valori del piano, senza fonte.
    float hapDv = 0.8f;        // m/s: velocita' tolta dal contatto (oltre la gravita' di un passo) oltre cui c'e' un urto
    float hapJ = 5.0f;         // N s: impulso per l'ampiezza piena (ampiezza = 0,2 + J / hapj)
    float hapRefr = 80.0f;     // ms: al piu' un impulso d'urto ogni tanto per mano (tenuto, schiaffo, oggetto urtato)
    float hapRearm = 40.0f;    // ms: dopo un urto il contatto deve finire (sotto meta' soglia) per tanto prima del prossimo
    float hapWindow = 50.0f;   // ms: un contatto somma la velocita' tolta per tanto (spigolo che tocca e fa ruotare: 8 tick)
    float hapSphereKg = 5.0f;  // kg: massa della mano (massa ridotta con l'oggetto: tenuto, colpo, oggetto urtato, presa al volo)
    float hapSpeed = 0.5f;     // m/s: lo schiaffo conta solo con la mano piu' veloce di tanto
    // Revisione della voce 3 (26/10). Schiaffo: J = hapslapkg x dv (con i 5 kg
    // della sfera ogni schiaffo oltre soglia era pieno, 1,00); storia
    // dell'avvicinamento a tempo (prima 3 tick: 21 ms a 144 Hz, 67 a 45).
    float hapSlapKg = 2.0f;    // kg: massa della mano vuota che colpisce [senza fonte]: 0,8 m/s -> 0,52, 2 m/s -> pieno
    float hapApproach = 25.0f; // ms: velocita' d'avvicinamento dai tick di tanto prima del contatto
    // Lancio (voce 4 del piano, "throwmode peak"): picco della velocita' della
    // mano nella storia (come Half-Life: Alyx e SteamVR), polso, leva e limite
    // di energia. Valori del piano; l'energia senza fonte (il piano diceva
    // 150 J per un barile da 60 kg a 2,3 m/s; con masskg 40 il barile pesa
    // 33,2 kg e a 150 J partiva a 3,0: 85 J lo riportano a 2,26, revisione 26/10).
    float peakWin = 110.0f;     // ms: il picco si cerca negli ultimi tanti ms (10 frame a 90 Hz, come Alyx)
    float peakDt = 6.0f;        // ms: velocita' di un campione da quello piu' vecchio ad almeno tanto (un frame fino a 144 Hz; senza visore a 225 tick/s, due)
    float peakJump = 12.0f;     // m/s: campioni piu' veloci di tanto rispetto a Walker scartati come salti del tracciamento
    float throwEnergy = 85.0f;  // J: energia cinetica massima del lancio, rispetto a Walker: |v| <= sqrt(2 E / kg) (0 = nessun limite)
    // Lancio o oggetto posato (revisione 26/10): sotto peakmin m/s di picco
    // (rispetto a Walker) l'oggetto parte con la media al rilascio, come
    // classic; da peakfull in su col picco; in mezzo un passaggio morbido.
    float peakMin = 1.0f;       // m/s [senza fonte]
    float peakFull = 2.0f;      // m/s [senza fonte]
    // Campioni isolati sbagliati (revisione 26/10): uno che dista dalla
    // mediana dei suoi 3 piu' di max(peakout, 20% della sua velocita') prende
    // la mediana (un controller che torna in vista: 5 cm in un frame a 90 Hz =
    // 4,5 m/s). 0 = nessuna sostituzione (media 1:2:1 sui campioni grezzi).
    float peakOut = 0.5f;       // m/s [senza fonte]
    float spinMax = 55.0f;      // rad/s: rotazione massima data al lancio (sotto i 60 di RaiseMaxAngVel)
    float spinJump = 50.0f;     // rad/s: velocita' angolare di un campione oltre cui e' un salto del tracciamento (scartata)
    float lever = 1.0f;         // 1: il baricentro parte a mano + w x (baricentro - mano); 0 = solo la mano (confronto)
    // Mano disegnata che segue la fisica (voce 5 del piano, "solidhands";
    // RenderHandPose). Zona morta di 2,5 cm (soglia di percezione di Rietzler)
    // e 8 gradi; tetto e filtro senza fonte.
    float shDead = 2.5f;      // cm: scostamenti piu' piccoli attenuati (smoothstep), da qui interi
    float shTurn = 8.0f;      // gradi: lo stesso per la rotazione
    float shCap = 25.0f;      // cm: scostamento massimo disegnato
    float shCapTurn = 45.0f;  // gradi: rotazione massima disegnata
    float shIn = 40.0f;       // ms: filtro quando lo scostamento cresce
    float shOut = 100.0f;     // ms: filtro quando cala
    float shTele = 45.0f;     // cm (full): con un muro tra la sfera e il palmo, la sfera torna sul palmo solo oltre tanto
    float shWall = 15.0f;     // unita' (raggio dei limiti): mesh del livello piu' piccole non sono un muro per quella trace
    float shGuard = 1.0f;     // 1: MeasurePalm e le dita sulla posa disegnata; 0 = sul bersaglio come prima (solo confronto)
    // Forza della presa (voce 6 del piano, "analoggrip"; GripStep, WeightOf).
    // Con il peso, forza e coppia della mano scalano in linea retta dal 40%
    // a grip 0,6 (la soglia della presa) al 100% a grip 1 (valori del piano,
    // la curva senza fonte: SLZ lega la presa alla forza ma non ha pubblicato
    // come). Mai un rifiuto: la forza non scende sotto il pavimento di WeightOf.
    float gripMin = 0.4f;     // frazione di forza e coppia alla presa piu' lenta
    float gripLo = 0.6f;      // valore del grip della presa piu' lenta (sotto: uguale)
    // Valore del grip della presa piu' stretta (sopra: uguale). 0,9 e non 1
    // (revisione 26/10): un Touch consumato o l'Index (squeeze/value di OpenXR
    // dal sensore di forza) possono non arrivare a 1, e la presa a fondo
    // normale deve gia' dare tutta la forza (niente stretta continua e
    // faticosa per avere il controllo).
    float gripHi = 0.9f;
    float gripTurn = 0.5f;    // guadagno della rotazione verso la posa voluta alla presa piu' lenta (frazione) [senza fonte]
    float gripDroop = 2.0f;   // piega alla presa piu' lenta: rigidezza alla rotazione / tanto (al piu' droopCap, come prima) [senza fonte]
    float gripHold = 100.0f;  // ms: una presa piu' lenta vale dopo tanto dall'ultimo massimo (il tremolio del grip non entra)
    float gripEase = 80.0f;   // ms: poi ci arriva con questa costante di tempo (niente scalini di forza e piega)
    float gripRise = 25.0f;   // ms: una presa piu' stretta ci arriva con questa costante (revisione 26/10: prima in un tick, scalino di forza e piega)
    float gripThrow = 300.0f; // ms: il lancio usa la presa piu' stretta di tanto prima (aprire la mano lanciando, anche piano, non lo indebolisce); 0 = quella di adesso
    float gripFloor = 1.3f;   // g: con la presa lenta l'accelerazione massima non scende sotto tanto (o sotto quella della presa stretta, se minore)
} g_tune;

// Mano disegnata (voce 5, "solidhands"; RenderHandPose): scostamento dal
// bersaglio negli assi del bersaglio, filtrato. dPos/dRot: quello scritto
// davvero nell'IK in questo frame (dopo la portata del braccio). Le ossa
// seguono la posa scritta, non il bersaglio: MeasurePalm e le dita misurano
// rispetto a quella (RefHand), altrimenti lo scostamento entrerebbe in
// g_palmLocal e nella terna della presa e li farebbe derivare (anello chiuso).
struct RenderHand {
    float fp[3] = {0, 0, 0};       // scostamento filtrato del palmo (assi del bersaglio, unita')
    float fq[4] = {0, 0, 0, 1};    // rotazione filtrata (quaternione x, y, z, w negli assi del bersaglio)
    double lastT = 0.0;
    bool active = false;           // scostamento in corso (o in uscita dal filtro)
    bool deltaValid = false;       // dPos/dRot validi per questo frame (false: posa scritta = bersaglio)
    float dPos[3] = {0, 0, 0};     // posa scritta - bersaglio (assi del bersaglio, unita')
    float dRot[3][3] = {};         // assi scritti negli assi del bersaglio (righe)
    float dCut = 0.0f;             // quanto la portata del braccio ha tolto alla posa disegnata (unita'): le dita lo recuperano (HeldShape)
    int src = 0;                   // 0 niente, 1 oggetto tenuto, 2 punto afferrato (vincolato), 3 sfera della mano vuota
};
RenderHand g_render[2];

// Posa della mano che le ossa hanno davvero: l'ultimo bersaglio dato a Tick
// con lo scostamento scritto in quel frame (voce 5). Senza scostamento (o
// "griptune shguard 0", confronto) il bersaglio, identico a prima.
void RefHand(int side, float hp[3], float ha[3][3]) {
    std::memcpy(hp, g_lastHandPos[side], 12);
    std::memcpy(ha, g_lastHandAxes[side], 36);
    const RenderHand& rh = g_render[side];
    if (!rh.deltaValid || g_tune.shGuard < 0.5f) return;
    const float(*ta)[3] = g_lastHandAxes[side];
    for (int c = 0; c < 3; ++c) {
        hp[c] = g_lastHandPos[side][c] + rh.dPos[0] * ta[0][c] + rh.dPos[1] * ta[1][c] + rh.dPos[2] * ta[2][c];
        for (int j = 0; j < 3; ++j) ha[j][c] = rh.dRot[j][0] * ta[0][c] + rh.dRot[j][1] * ta[1][c] + rh.dRot[j][2] * ta[2][c];
    }
}

struct Held {
    void* actor = nullptr;
    void* comp = nullptr;
    float relPos[3] = {0, 0, 0};      // origine dell'oggetto negli assi della mano
    float relPosFrom[3] = {0, 0, 0};  // dove era alla presa: scivola sul palmo in 0,12 s
    double grabAt = 0.0;
    float relAxes[3][3] = {};         // assi dell'oggetto negli assi della mano
    float relAxesFrom[3][3] = {};     // alla presa: ruota nella presa canonica in blendT
    float anchor[3] = {0, 0, 0};      // punto dell'oggetto che finisce in mano (assi dell'oggetto, unita'): perno della rotazione
    float blendT = 0.12f;             // durata dell'arrivo nella presa (s)
    // Peso: abbassamento e piega entrano dalla presa in weightBlendT, con un
    // orologio loro (WeightBlend). La ripresa (Regrip, il suo assestamento,
    // l'appoggio sul palmo) cambia grabAt e blendT, non questo: con lo stesso
    // orologio la posa voluta tornava a una frazione dell'abbassamento e della
    // piega a ogni tick dell'assestamento e l'oggetto saliva (revisione del seguito 2).
    double weightAt = 0.0;
    float weightBlendT = 0.12f;
    float mass = 0.2f;                // GetBodyMass, unita' del motore (per gli impulsi); in kg: MassKg
    float radiusCm = 10.0f;
    float halfThickCm = 0.0f;  // mezzo spessore tra palmo e dita (0 = non misurato)
    bool powerGrip = false;    // presa canonica (CanonicalGrip)
    int gripKind = 0;          // 0 sul palmo, 1 di forza, 2 a pinza (oggetti piatti)
    bool wantCanonical = false;  // terna delle nocche non ancora misurata alla presa: la posa canonica al primo tick utile
    float lastCmdV[3] = {0, 0, 0};  // velocita' data al tick prima (oggetto sbattuto: fase 4)
    vr::pure::ImpactEdge hapEdge;   // vibrazione degli urti (voce 3): fronte di salita del contatto (HeldImpact)
    double hapBrkAt = -10.0;        // ultimo tick con un distacco in corso (vibrazione muta fino a 0,3 s dopo)
    bool constrained = false;       // vincolato: si tira il punto afferrato, non l'oggetto (fase 5)
    float grabLocal[3] = {0, 0, 0};  // punto afferrato negli assi dell'oggetto, dall'origine
    float prevPt[3] = {0, 0, 0};     // punto afferrato al tick prima (la sua velocita' misurata)
    bool hasPrevPt = false;
    int stoppedTicks = 0;           // tick di fila con l'oggetto fermato mentre la mano corre
    float maxSpeed = 1500.0f;
    double stuckSince = -1.0;
    uint32_t savedA0 = 0, saved9C = 0;
    uint32_t walkerSig = 0;  // corpi di Walker ignorati (IgnoreWalker)
    bool wristInit = false;  // Drive ha gia' la posa della mano del tick prima per questa presa
    int ticks = 0;           // tick di Drive dalla presa (massa riletta a 5 e 30: MassReread)
    // Peso (voce 2, "objectweight"): modo fissato alla presa (0 = la guida di
    // prima, identica; 1 light, 2 full), centro dei limiti negli assi
    // dell'oggetto (unita', con la scala: baricentro stimato) e raggi d'inerzia
    // al quadrato della scatola dei limiti sugli assi dell'oggetto (m^2 per kg:
    // inerzia = MassKg x inertia, WeightSetup).
    int weight = 0;
    // Mano che segue la fisica (voce 5, "solidhands" non off): fissato alla
    // presa come il peso. La mano disegnata sta sull'oggetto (RenderHandPose).
    // Vale solo finche' solidhands resta acceso (SolidHeld): spento dal menu
    // con l'oggetto in mano, mano disegnata, ripresa e dita tornano insieme
    // come off (revisione 26/10).
    bool solid = false;
    int hand = 1;
    float comLocal[3] = {0, 0, 0};
    float inertia[3] = {0, 0, 0};
    float lastCmdW[3] = {0, 0, 0};  // velocita' angolare data al tick prima
    bool hasCmd = false;            // lastCmdV / lastCmdW valgono (dal secondo tick di Drive)
    float prevPawnV[3] = {0, 0, 0};  // velocita' di Walker al tick prima (accelerazione data gratis)
    double jumpUntil = 0.0;          // salto del riferimento appena fatto (FrameJump): velocita' misurata ignorata fino a qui
    float smashRef[3] = {0, 0, 0};   // comando di prima dell'urto (oggetto sbattuto, con il peso)
    // Distacco (seguito della voce 2): comando del peso senza la forza del
    // distacco (uguale a lastCmdV/W fuori dal distacco). Rilascio, oggetto
    // sbattuto e colpi in mano partono da questo: il comando gonfiato per
    // strappare un oggetto incastrato non diventa mai un lancio o un colpo.
    float weightCmdV[3] = {0, 0, 0};
    float weightCmdW[3] = {0, 0, 0};
    double brkSince = -1.0;  // primo tick bloccato di fila (candidato), -1 = non bloccato
    int brkTicks = 0;        // tick nella finestra
    float brkSumCe = 0.0f, brkSumProg = 0.0f;  // somme nella finestra: velocita' comandata e misurata lungo il comando
    float brkErr0 = 0.0f;    // errore al primo tick bloccato (la mano tira via: l'errore cresce)
    double brkStart = 0.0;   // inizio del distacco (0 = nessuno)
    double brkSat = -1.0;    // da quando il comando del distacco e' la guida piena (senza peso)
    float brkErrMax = 0.0f;  // errore massimo dal distacco (cala di 2 cm: libero)
    int brkMoving = 0;       // tick di fila in cui segue almeno meta' del comando (libero: >= 2 e 8 ms)
    double brkMovingSince = -1.0;  // primo di quei tick
    int brkThrash = 0;       // tick di fila in cui si muove di lato o contro il comando oltre la vibrazione di prima
    double brkThrashSince = -1.0;  // primo di quei tick (rinuncia: >= 2 tick e 30 ms, forza almeno x4)
    float brkSumOff = 0.0f;  // somma nella finestra: velocita' misurata di lato o all'indietro (vibrazione del peso)
    float brkVib = 0.0f;     // la sua media all'inizio del distacco (soglia del "si dimena")
    double brkGiveAt = -1.0; // ultima rinuncia (nuovo tentativo comunque dopo 0,5 s)
    int brkAgainN = 0;       // riprese di fila ("di nuovo bloccato" entro 0,3 s): alla terza, rinuncia
    // Incastrato senza via d'uscita (WedgeStep): velocita' del punto della
    // presa dalle posizioni (media esponenziale, 0,15 s: la vibrazione sul
    // posto si annulla), tempo accumulato fermo e lontano dal palmo.
    float wedgeVel[3] = {0, 0, 0};
    float wedgePrev[3] = {0, 0, 0};
    bool wedgeHasPrev = false;
    double wedgeT = 0.0;
    // Ripresa dove si trova (Regrip in WedgeStep): palmo nel mondo all'ultimo moto della
    // mano e quando (la mano e' ferma se non si sposta di regripMove), tempo
    // accumulato bloccato con la mano ferma e il palmo sull'oggetto, riprese in
    // questa presa. Scatola dei limiti dell'oggetto (mezze estensioni negli
    // assi dell'oggetto, unita', con la scala; centro = comLocal): distanza del palmo.
    float palmRefW[3] = {0, 0, 0};
    double handMovedAt = -1.0;  // -1 = non ancora misurato (primo tick col peso)
    double regripT = 0.0;
    float regripErr0 = 0.0f;  // errore della presa all'inizio della finestra (cala di 2 cm: si avvicina, finestra nuova)
    float regripErrF = -1.0f; // errore della presa in media esponenziale (0,05 s; -1 = da riprendere): la vibrazione non e' progresso
    double settleUntil = 0.0;  // dopo la ripresa: fin qui la posa voluta segue quella vera (assestamento)
    bool stuck = false;        // bloccato con la mano ferma: la guida non lo spinge contro l'ostacolo (Drive)
    int regrips = 0;
    // Velocita' del palmo nel mondo (media esponenziale, 0,1 s): oltre
    // regripSpeed la mano si muove anche senza spostarsi di regripMove.
    float palmPrevW[3] = {0, 0, 0};
    float palmVelF[3] = {0, 0, 0};
    bool palmHasPrev = false;
    // Appoggio sul palmo dopo la ripresa (Regrip): spostamento dell'oggetto
    // negli assi della mano, applicato alla prima mossa della mano (WedgeStep).
    float restShift[3] = {0, 0, 0};
    bool restPending = false;
    float restRaw = 0.0f, restAlongN = 0.0f;  // appoggio calcolato (anche se scartato) e la sua parte lungo la normale del palmo (unita', log)
    // Contro un Pawn (TouchingPawn) letto al piu' ogni 0,1 s, e solo quando decide.
    double pawnChkAt = 0.0;
    bool pawnTouch = false;
    float boxExt[3] = {0, 0, 0};
    bool hasBox = false;
    float slipCloseSum = 0.0f;  // scivolamento: avvicinamento misurato sommato (x dt) dall'inizio della finestra
    float brkPos0[3] = {0, 0, 0};  // centro dell'oggetto all'inizio del distacco (ribloccato li': non e' servito)
    double brkEnd = -1.0;    // fine dell'ultimo distacco riuscito (di nuovo bloccato entro 0,3 s: si riprende)
    float brkEndMul = 1.0f;  // moltiplicatore a quella fine
    float brkCoolErr = 0.0f; // dopo una rinuncia: niente nuovo distacco finche' l'errore non cambia di molto
    double brkCoolUntil = 0.0;  // e non prima di 0,25 s dalla rinuncia
    float slipErr0 = 0.0f;   // errore all'inizio della finestra dello scivolamento (con il peso)
    double brkTraceAt = 0.0;  // prossimo controllo del muro tra oggetto e bersaglio
    bool brkWallLogged = false;
    // Muri invisibili (BlockingVolume) che l'oggetto tenuto attraversa
    // (HeldVolumes): componente, NxActor e attore del muro (ricontrollati
    // prima di ogni chiamata a PhysX: VolumeAlive).
    void* volIgn[8] = {};
    void* volNx[8] = {};
    void* volActor[8] = {};
    int nVolIgn = 0;
    // Muri diventati pavimento in questa presa: restano solidi fino al
    // rilascio (senza, a cavallo della cima la coppia cambiava a ogni tick).
    void* volFloor[8] = {};
    int nVolFloor = 0;
    // Forza della presa (voce 6, "analoggrip"; GripStep): frazione di forza e
    // coppia in uso (1 = stretta; filtrata, vr::pure::GripFilterStep: piu'
    // stretta con gripRise ms, piu' lenta dopo gripHold ms e con gripEase ms),
    // la piu' alta degli ultimi gripThrow ms (il lancio, Release), valore del
    // grip letto, stato del filtro e ultimo valore scritto nel log.
    float gripF = 1.0f;
    float gripThrowF = 1.0f;
    float gripV = 1.0f;
    vr::pure::GripFilterState gripFilt;
    float gripLogF = 1.0f;
    double gripStableAt = 0.0;  // ultimo tick in cui gripF e' cambiato (riga nel log solo dopo 0,2 s fermo)
    // Scudo (voce 3 dei combattimenti, ShieldKick in Drive): un proiettile
    // sull'oggetto tenuto sposta la posa voluta lungo il colpo e la riporta
    // (ShieldKickShape): ora del colpo (-1 = nessuno), direzione, ampiezza
    // (unita'), asse e angolo massimo della rotazione (colpo fuori centro).
    double kickAt = -1.0;
    float kickDir[3] = {0, 0, 0};
    float kickAmp = 0.0f;
    float kickAxis[3] = {0, 0, 0};
    float kickAng = 0.0f;
    // Mischia (voce 4 dei combattimenti, HeldMelee): dopo un colpo contato la
    // guida frena l'oggetto lungo il colpo per meleeBrakeTicks tick (e almeno
    // fino a meleeBrakeUntil): si ferma sul corpo invece di attraversarlo.
    int meleeBrakeTicks = 0;
    double meleeBrakeUntil = 0.0;
    float meleeBrakeDir[3] = {0, 0, 0};
};

// Massa in kg dalle unita' di GetBodyMass: "masskg" kg per unita' (40). Lette
// il 24/10 sugli oggetti veri del livello 1: bottiglie 0,041-0,042 (1,7 kg),
// radio e sacco di sabbia 0,200 (8 kg), borsa da viaggio 0,400, valigia 0,419
// (16-17 kg), sacco a pelo 0,335, sedile del bus 0,57 (23 kg), barile di
// carburante 0,831 (33 kg); la media geometrica dei kg credibili da' ~37
// (tabella in docs/TECHNICAL.md). La densita' e' uniforme, quindi nessun
// fattore va bene per tutti (il sacco a pelo pesa 13 kg). Prima Grab
// confrontava le unita' con 15 kg: il limite di velocita' per gli oggetti
// pesanti non scattava mai. I due 0,200 sono masse vere, non il valore di
// riserva: rilette uguali 5 e 30 tick dopo la conversione (MassReread), e
// create con spawnprop alla scala dell'istanza (Scale3D del componente: radio
// 1,25; sacco 1 x 0,8 x 1,519) danno 0,200 anche loro (a scala 1: 0,128 e
// 0,181). La massa del motore cresce circa col quadrato della scala, non col
// cubo: radio x1,25 -> x1,5625; x2 -> x4,1.
float MassKg(const Held& held) { return held.mass * vr::control::g_massKgPerUnit.load(); }

// Mano che segue l'oggetto tenuto (voce 5): fissata alla presa (Held.solid) e
// solidhands ancora acceso. Mano disegnata (SolidSource), ripresa (WedgeStep)
// e dita (HeldShape) leggono tutte questa: prima solo SolidSource guardava il
// modo attuale, e spento dal menu durante la presa la mano tornava sul
// controller mentre ripresa e dita restavano quelle di solidhands (la mano
// sprofondava nel tavolo con l'oggetto sopra, revisione 26/10).
bool SolidHeld(const Held& held) { return held.solid && vr::control::g_solidHands.load() > 0; }

// Velocita' massima dell'oggetto tenuto rispetto a Walker (unita'/s): 30 m/s
// fino a 15 kg, poi 30 x 15 / kg (barile di carburante, 33 kg: 13,5 m/s), ma
// mai sotto "griptune minspeed" (5 m/s). Senza il pavimento un oggetto molto
// pesante (o un masskg alto: il menu arriva a 500) restava indietro gia' alla
// velocita' normale della mano e la regola dello scivolamento (60 cm per
// 0,3 s, Drive) lo lasciava cadere: un rifiuto, non un peso (regola
// dell'utente: con una sola mano libera tutto si deve poter sollevare).
float HeldMaxSpeed(const Held& held) {
    const float s = vr::control::g_posScale;
    const float kg = (std::max)(0.01f, MassKg(held));
    return (std::max)(30.0f * s * (std::min)(1.0f, 15.0f / kg), (std::max)(0.5f, g_tune.minSpeed) * s);
}

// Oggetto appena lasciato: le collisioni con Walker tornano dopo 0,5 s
// (un lancio non deve urtare la sua stessa capsula). I flag dell'attore
// (bIgnoreRigidBodyPawns, bBlockActors) tornano invece appena l'oggetto e'
// fuori dalla capsula di Walker (pipeline dei colpi, 24/10); contro Walker
// resta solo NX_IGNORE_PAIR con i suoi corpi (IgnoreWalker). Nota: in questo
// eseguibile bIgnoreRigidBodyPawns (+0x9C bit 0x200) lo legge solo
// APawn::IgnoreBlockingBy (FUN_008ba520: Pawn in PHYS_RigidBody contro
// l'attore); la capsula cinematica dei nemici (YPawnKinematicRBCollider,
// NxActor senza userData) ferma l'oggetto lanciato anche col flag acceso
// (manichino, 24/10: radio lasciata sulla testa, ferma sulla testa in tutti e due i casi).
struct Restore {
    void* actor;
    double at;
    uint32_t savedA0, saved9C;
    uint32_t walkerSig = 0;  // corpi di Walker ignorati da questo oggetto (IgnoreWalker)
    bool flagsBack = false;  // flag dell'attore gia' rimessi (oggetto uscito dalla capsula di Walker)
    double since = 0.0;      // ora del rilascio (diagnostica nel log)
};

// Oggetto appena lasciato, ancora ignorato dalle sfere delle mani (ProxyTick):
// per g_tune.relGrace secondi, o finche' esce dalla sfera piu' 6 unita'. Prima
// la coppia tornava solida subito sopra 1 m/s (regola dello schiaffo): la
// sfera, dentro l'oggetto appoggiato al palmo, lo spingeva fuori al primo
// passo di PhysX e il lancio deviava. Vale per tutte e due le sfere (anche
// quella della mano dell'arma: un lancio che parte accanto all'impugnatura a
// due mani), ognuna con la sua distanza dal suo palmo: [sfera][mano che l'ha lasciato].
struct ReleaseGrace {
    void* comp = nullptr;
    double since = 0.0, until = 0.0;
    bool used = false;  // ha tenuto ignorata una coppia che la regola dello schiaffo avrebbe reso solida
} g_relGrace[2][2];

std::vector<Prop> g_props;
Held g_held[2];
std::vector<Restore> g_restore;
std::unordered_map<uint32_t, uint8_t> g_grabbableClass;  // 0 niente, 1 KActor, 2 vincolo (RB_ConstraintActor)
bool g_rescan = false;
void* g_proxyActor[2] = {nullptr, nullptr};  // corpi delle mani (fase 3): non sono oggetti da prendere
void* g_tickPawn = nullptr;  // Walker dell'ultimo Tick (per il corpo-spinta in Grab)
// Arma dei nemici raccolta con la sinistra (voce 6 dei combattimenti, 27/10):
// SkeletalMeshComponent del YPickup_WeaponSpawnable tenuto [0] e lasciato da
// poco [1]. Le SkeletalMeshComponent non hanno un BodyInstance a +0x170: il
// corpo sta nel PhysicsAssetInstance (WeaponRootBody). CompNx lo legge solo
// per questi due componenti, cosi' coppie ignorate con Walker e con le sfere
// delle mani e la rotazione massima valgono anche per l'arma.
void* g_wpnComp[2] = {nullptr, nullptr};
// Cadavere trascinato (voce 11 dei combattimenti, 27/10): la mesh del pawn
// morto tenuta. Come per l'arma, i corpi del ragdoll stanno nel
// PhysicsAssetInstance: CompNxAll li elenca tutti per questa mesh e per
// quelle di g_corpsePairMeshes (coppie con la sfera della sinistra ignorate e
// poi rimesse).
void* g_corpseComp = nullptr;
// Cadaveri le cui coppie con una sfera delle mani sono (o sono state da poco)
// ignorate: mentre la mano libera si avvicina piano per prenderli (come gli
// oggetti nella zona di presa sotto 1 m/s: prima la sfera spingeva via
// l'arto da 0,4 kg prima della stretta, 27/10), tenuti e appena lasciati.
// CompNxAll ne elenca tutti i corpi finche' qualche coppia resta da rimettere
// (pulizia a fine tick). Revisione della voce 11: prima il cadavere lasciato
// stava in un posto solo (g_corpseComp[1]); preso un secondo cadavere, le
// coppie del primo ancora da rimettere non trovavano piu' i suoi corpi e
// restavano ignorate (la sfera passava attraverso i suoi arti).
std::vector<void*> g_corpsePairMeshes;
bool CorpsePairMesh(void* comp) {
    return comp && (comp == g_corpseComp || std::find(g_corpsePairMeshes.begin(), g_corpsePairMeshes.end(), comp) != g_corpsePairMeshes.end());
}
// Lancio appena fatto: velocita' vera dell'oggetto 3 tick dopo e a 0,1 s, con
// lo scarto di direzione da quella data (diagnostica nel log).
struct ThrowCheck {
    void* actor = nullptr;
    void* comp = nullptr;
    int ticks = 0;
    int stage = 0;          // 0 aspetta i 3 tick, 1 aspetta 0,1 s
    float wanted = 0.0f;
    float wantV[3] = {0, 0, 0};  // velocita' data al rilascio (unita'/s)
    float wantW[3] = {0, 0, 0};  // rotazione data al rilascio (rad/s; throwmode peak)
    bool hasW = false;           // rotazione data (altrimenti resta quella della guida)
    float from[3] = {0, 0, 0};   // centro dei limiti al rilascio
    double at = 0.0;        // ora del rilascio
} g_throwCheck;  // oggetto appena creato: scansione al tick dopo
int32_t g_kactorName = -1, g_constraintName = -1, g_smcName = -1;

// Storia delle mani: 16 campioni (voce 4 del piano: il lancio cerca il picco
// negli ultimi ~110 ms, 16 tick a 144 Hz; prima 8). Le letture di prima
// (HistVelocity, HandJumped) restano sugli 8 piu' recenti, identiche. Gli assi
// e l'ora della posa servono solo a g_hist (lancio: velocita' angolare del
// polso anche a mano vuota; velocita' con l'ora in cui la posa e' stata letta,
// non quella del tick, che arriva piu' tardi nel Draw dopo lavoro variabile).
constexpr int kHistN = 16;
struct HandHist {
    float pos[kHistN][3];
    float ax[kHistN][3][3];
    double t[kHistN];
    double pt[kHistN];  // ora della posa (s; g_poseStampMs), solo g_hist
    // Centro del riferimento delle mani (SetHandFrame) dello stesso tick, solo
    // g_hist (revisione 26/10): il lancio cerca il picco della mano rispetto a
    // Walker (pos - fc), non nel mondo. fok: centro di questo tick.
    float fc[kHistN][3];
    bool fok[kHistN];
    int n = 0, head = 0;
} g_hist[2];
// Indice del k-esimo campione dal piu' recente (k = 0 il piu' recente).
int HistIdx(const HandHist& hh, int k) { return (hh.head - 1 - k + 2 * kHistN) % kHistN; }
void HistPush(HandHist& hh, const float* pos, double t, const float (*ax)[3] = nullptr, double poseT = 0.0, const float* frameC = nullptr) {
    std::memcpy(hh.pos[hh.head], pos, sizeof(hh.pos[0]));
    if (ax) std::memcpy(hh.ax[hh.head], ax, sizeof(hh.ax[0]));
    hh.fok[hh.head] = frameC != nullptr;
    if (frameC) std::memcpy(hh.fc[hh.head], frameC, sizeof(hh.fc[0]));
    hh.t[hh.head] = t;
    // Ora della posa: quella data se plausibile (al piu' 50 ms prima del tick), altrimenti il tick.
    hh.pt[hh.head] = poseT > 0.0 && poseT <= t + 1e-4 && t - poseT < 0.05 ? poseT : t;
    hh.head = (hh.head + 1) % kHistN;
    if (hh.n < kHistN) ++hh.n;
}
HandHist g_pawnHist;  // posizione di Walker negli stessi tick (solo diagnostica: GRABLOG di Drive)
// Centro del riferimento delle mani (UpdateArms -> SetHandFrame) negli stessi
// tick: la sua velocita' e' quella con cui Walker porta le mani (FrameVelocity).
HandHist g_frameHist;

std::mutex g_spawnMutex;
std::string g_spawnMesh;
float g_spawnDistCm = 30.0f;
float g_spawnScale[3] = {1.0f, 1.0f, 1.0f};  // Scale3D del componente creato (prima della mesh: il corpo nasce scalato)
std::string g_grabTestMesh;  // "grabtest": la mano sinistra prende l'oggetto piu' vicino con questa parte di nome

uint32_t U32(const void* base, uintptr_t off) {
    uint32_t v = 0;
    TrySafeReadUint32(static_cast<char*>(const_cast<void*>(base)) + off, &v);
    return v;
}

bool ClassIsA(uint32_t cls, int32_t nameIndex) {
    for (int i = 0; cls && i < 32; ++i) {
        uint32_t name = 0;
        if (!TrySafeReadUint32(reinterpret_cast<char*>(cls) + kOffName, &name)) return false;
        if (static_cast<int32_t>(name) == nameIndex) return true;
        if (!TrySafeReadUint32(reinterpret_cast<char*>(cls) + kOffSuper, &cls)) return false;
    }
    return false;
}

bool ReadMatrix(void* comp, float m[4][4]) { return SafeCopy(m, static_cast<char*>(comp) + kOffCompLocalToWorld, 64); }

// Assi senza scala (righe) e origine del componente.
bool CompPose(void* comp, float pos[3], float axes[3][3]) {
    float m[4][4];
    if (!ReadMatrix(comp, m)) return false;
    for (int r = 0; r < 3; ++r) {
        float l = std::sqrt(m[r][0] * m[r][0] + m[r][1] * m[r][1] + m[r][2] * m[r][2]);
        if (!(l > 1e-6f)) return false;
        for (int c = 0; c < 3; ++c) axes[r][c] = m[r][c] / l;
    }
    for (int c = 0; c < 3; ++c) pos[c] = m[3][c];
    return std::isfinite(pos[0]) && std::isfinite(pos[1]) && std::isfinite(pos[2]);
}

// --- Matematica della presa (GRIPMATH_BEGIN) ----------------------------------
// Quaternioni (spostati qui da namespace fingers: servono anche a Drive).
struct Quat {
    float x, y, z, w;
};

Quat Mul(const Quat& a, const Quat& b) {  // prima b, poi a
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

void Rotate(const Quat& q, const float* v, float* out) {
    // v' = v + 2w (u x v) + 2 u x (u x v)
    float u[3] = {q.x, q.y, q.z};
    float t[3] = {2.0f * (u[1] * v[2] - u[2] * v[1]), 2.0f * (u[2] * v[0] - u[0] * v[2]), 2.0f * (u[0] * v[1] - u[1] * v[0])};
    out[0] = v[0] + q.w * t[0] + (u[1] * t[2] - u[2] * t[1]);
    out[1] = v[1] + q.w * t[1] + (u[2] * t[0] - u[0] * t[2]);
    out[2] = v[2] + q.w * t[2] + (u[0] * t[1] - u[1] * t[0]);
}

Quat AxisAngle(const float* axis, float rad) {
    float s = std::sin(rad * 0.5f);
    return {axis[0] * s, axis[1] * s, axis[2] * s, std::cos(rad * 0.5f)};
}

Quat Conj(const Quat& q) { return {-q.x, -q.y, -q.z, q.w}; }

float Dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void Cross3(const float* a, const float* b, float* out) {
    float r[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    std::memcpy(out, r, sizeof(r));
}

bool Norm3(float* v) {
    float l = std::sqrt(Dot3(v, v));
    if (!(l > 1e-6f)) return false;
    for (int i = 0; i < 3; ++i) v[i] /= l;
    return true;
}

float Det3(const float m[3][3]) {
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

// Righe = immagini degli assi X, Y, Z (come relAxes; Rotate(q, asse i) = riga i).
Quat RowsToQuat(const float m[3][3]) {
    float t = m[0][0] + m[1][1] + m[2][2], s;
    Quat q;
    if (t > 0.0f) {
        s = 2.0f * std::sqrt(1.0f + t);
        q = {(m[1][2] - m[2][1]) / s, (m[2][0] - m[0][2]) / s, (m[0][1] - m[1][0]) / s, 0.25f * s};
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        s = 2.0f * std::sqrt((std::max)(1e-12f, 1.0f + m[0][0] - m[1][1] - m[2][2]));
        q = {0.25f * s, (m[0][1] + m[1][0]) / s, (m[2][0] + m[0][2]) / s, (m[1][2] - m[2][1]) / s};
    } else if (m[1][1] > m[2][2]) {
        s = 2.0f * std::sqrt((std::max)(1e-12f, 1.0f + m[1][1] - m[0][0] - m[2][2]));
        q = {(m[0][1] + m[1][0]) / s, 0.25f * s, (m[1][2] + m[2][1]) / s, (m[2][0] - m[0][2]) / s};
    } else {
        s = 2.0f * std::sqrt((std::max)(1e-12f, 1.0f + m[2][2] - m[0][0] - m[1][1]));
        q = {(m[2][0] + m[0][2]) / s, (m[1][2] + m[2][1]) / s, 0.25f * s, (m[0][1] - m[1][0]) / s};
    }
    float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return {q.x / l, q.y / l, q.z / l, q.w / l};
}

void QuatToRows(const Quat& q, float m[3][3]) {
    for (int i = 0; i < 3; ++i) {
        float e[3] = {0, 0, 0};
        e[i] = 1.0f;
        Rotate(q, e, m[i]);
    }
}

Quat Slerp(Quat a, Quat b, float t) {
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0.0f) {
        b = {-b.x, -b.y, -b.z, -b.w};
        d = -d;
    }
    float wa = 1.0f - t, wb = t;
    if (d < 0.9995f) {
        float th = std::acos(d), s = std::sin(th);
        wa = std::sin((1.0f - t) * th) / s;
        wb = std::sin(t * th) / s;
    }
    Quat q = {a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb};
    float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return {q.x / l, q.y / l, q.z / l, q.w / l};
}

// Sezione tonda o squadrata dalla sfera dei limiti della StaticMesh (UE3: la
// distanza massima di un vertice dal centro del box). Spigoli vivi in fondo o
// in cima (bottiglia quadrata): sfera^2 = eL^2 + ea^2 + eb^2; sezione tonda:
// eL^2 + max(ea, eb)^2. Ritorna il raggio degli spigoli come frazione del mezzo
// spessore minore (0,15 spigoli vivi ... 1 cilindro).
float SectionRound(const float le[3], float sphere, int L, float force) {
    if (force >= 0.0f) return (std::min)(1.0f, force);
    int ia = (L + 1) % 3, ib = (L + 2) % 3;
    float sq = le[ia] * le[ia] + le[ib] * le[ib];
    if (!(sq > 1e-6f) || !(sphere > 0.0f)) return 0.35f;
    float q = (sphere * sphere - le[L] * le[L]) / sq;  // 1 = spigoli, 0,5 = cerchio
    if (!(q > 0.3f) || q > 1.05f) return 0.35f;        // sfera non dai vertici: compromesso
    return (std::max)(0.15f, (std::min)(1.0f, 0.15f + (1.0f - q) / 0.4f * 0.85f));
}

// Posa canonica dell'oggetto nella mano (assi della mano, unita'): l'asse
// lungo su a (la cima, se la base e' nota, verso l'indice), il mezzo spessore
// minore su p (una faccia piatta sul palmo), l'altro su f; tra le pose
// equivalenti (segni, sezione quasi quadrata) quella piu' vicina alla posa
// alla presa. Presa di forza (kind 1): centro dentro l'anello delle dita,
// all'altezza canonica (30% dalla base) o dove ha toccato (base ignota);
// a pinza (kind 2, oggetti piatti larghi): bordo nel palmo, lastra tra dita e
// pollice. 0 = troppo grande: resta la presa sul palmo.
// In: g terna della presa; lcs/e centro e mezze estensioni scalate (unita');
// R0/c0 assi e centro dell'oggetto alla presa (assi della mano); cm = unita'
// per cm. Out: assi (righe), centro, ancora (punto che finisce in mano, negli
// assi dell'oggetto, dall'origine), mezzo spessore sul palmo.
int CanonicalPose(const GripFrame& g, const float lcs[3], const float e[3], const float R0[3][3],
                  const float c0[3], float cm, const GripTune& tu, float na[3][3], float nc[3], float anc[3], float* halfThick) {
    int L = e[0] >= e[1] ? (e[0] >= e[2] ? 0 : 2) : (e[1] >= e[2] ? 1 : 2);
    int i1 = (L + 1) % 3, i2 = (L + 2) % 3;
    int Ax = e[i1] <= e[i2] ? i1 : i2, Bx = 3 - L - Ax;  // Ax piu' sottile
    int kind = 0;
    // Di forza fino a 14 x 16 cm di sezione (bottiglie grandi del livello 1,
    // bottle_quad01 11,7 cm: con 10 cm finivano sul palmo): le dita si chiudono
    // fin dove toccano (PoseFingers), anche senza arrivare dall'altra parte.
    if (e[Ax] <= 7.0f * cm && e[Bx] <= 8.0f * cm) kind = 1;
    else if (e[Ax] <= 2.5f * cm && e[Bx] <= 30.0f * cm) kind = 2;
    if (!kind) return 0;
    bool topKnown = std::fabs(lcs[L]) > 0.3f * e[L];  // origine su un'estremita' (bottiglie, lattine)
    float topSign = lcs[L] > 0.0f ? 1.0f : -1.0f;
    bool square = kind == 1 && e[Bx] - e[Ax] < 0.15f * e[Bx];
    float d0 = Det3(R0), best = -10.0f;
    int bestSw = 0;
    float bestSL = 1.0f;
    for (int sw = 0; sw < (square ? 2 : 1); ++sw)
        for (int sg = 0; sg < 8; ++sg) {
            float sL = (sg & 1) ? -1.0f : 1.0f, sA = (sg & 2) ? -1.0f : 1.0f, sB = (sg & 4) ? -1.0f : 1.0f;
            if (topKnown && sL != topSign) continue;
            const float* toA = sw ? g.f : g.p;
            const float* toB = sw ? g.p : g.f;
            float cand[3][3];
            for (int i = 0; i < 3; ++i) {
                cand[L][i] = sL * g.a[i];
                cand[Ax][i] = sA * toA[i];
                cand[Bx][i] = sB * toB[i];
            }
            if (Det3(cand) * d0 < 0.0f) continue;  // stessa chiralita' della mesh
            float tr = Dot3(cand[0], R0[0]) + Dot3(cand[1], R0[1]) + Dot3(cand[2], R0[2]);
            if (tr > best) {
                best = tr;
                bestSw = sw;
                bestSL = sL;
                std::memcpy(na, cand, sizeof(cand));
            }
        }
    if (best < -5.0f) return 0;
    float Ef = bestSw ? e[Ax] : e[Bx], Ep = bestSw ? e[Bx] : e[Ax], Ea = e[L];
    float kp = (std::min)(g.span[0], g.span[1]), ki = (std::max)(g.span[0], g.span[1]);
    float ac = 0.5f * (kp + ki), rad = tu.finger * cm;
    float hL = ac - kp + rad, hI = ki - ac + rad;  // la mano lungo a, dal suo centro
    float u, v = (tu.palm + tu.gap) * cm + Ep, al = ac, ua = 0.0f;
    if (kind == 1) {
        u = -(std::max)(1.2f * cm, (std::min)(3.5f * cm, 0.4f * cm + tu.back * Ef));
        ua = u;
        float len = 2.0f * Ea;
        if (len < hL + hI + 2.0f * cm) {
            al = ac;  // corto: centrato sulla mano
        } else if (topKnown) {
            float db = (std::max)(hL + 1.0f * cm, (std::min)(len - hI - 1.0f * cm, tu.height * len));  // centro della mano dalla base
            al = ac - db + Ea;
        } else {
            // Base ignota: lungo l'asse resta il punto toccato (sull'asse dell'oggetto,
            // non su quello voluto), con tutta la mano sull'oggetto.
            float xh[3], along = 0.0f;
            for (int i = 0; i < 3; ++i) xh[i] = g.o[i] + ac * g.a[i] + tu.palm * cm * g.p[i] - c0[i];
            along = Dot3(xh, R0[L]);
            al = ac - bestSL * along;
            float lo = ac + hI + 0.5f * cm - Ea, hi = ac - hL - 0.5f * cm + Ea;
            al = lo <= hi ? (std::max)(lo, (std::min)(hi, al)) : ac;
        }
    } else {
        u = -1.5f * cm + Ef;  // bordo 1,5 cm prima delle nocche, la lastra sotto le dita
    }
    for (int i = 0; i < 3; ++i) nc[i] = g.o[i] + u * g.f[i] + v * g.p[i] + al * g.a[i];
    // Ancora: il punto dell'oggetto all'altezza del centro della mano (pinza: sotto le nocche).
    float d[3];
    for (int i = 0; i < 3; ++i) d[i] = (ua - u) * g.f[i] + (ac - al) * g.a[i];
    for (int i = 0; i < 3; ++i) anc[i] = lcs[i] + Dot3(d, na[i]);
    *halfThick = Ep;
    return kind;
}
// --- GRIPMATH_END --------------------------------------------------------------

bool ActorUsable(void* actor) {
    uint32_t f9c = 0;
    uint8_t phys = 0;
    if (!TrySafeReadUint32(static_cast<char*>(actor) + kOffActorFlags9C, &f9c) || (f9c & (kHidden | kDeleteMe))) return false;
    if (!SafeCopy(&phys, static_cast<char*>(actor) + kOffActorPhysics, 1) || phys != kPhysRigidBody) return false;
    return true;
}

void* PropComponent(void* actor) {
    uint32_t comp = U32(actor, kOffActorCollisionComp);
    if (!comp || !U32(reinterpret_cast<void*>(comp), kOffCompBodyInstance)) return nullptr;
    return reinterpret_cast<void*>(comp);
}

bool ReadBounds(void* comp, Prop* p) {
    float b[7];
    if (!SafeCopy(b, static_cast<char*>(comp) + kOffCompBounds, sizeof(b))) return false;
    for (int i = 0; i < 3; ++i) {
        p->boxMin[i] = b[i] - b[3 + i];
        p->boxMax[i] = b[i] + b[3 + i];
    }
    p->radius = b[6];
    return std::isfinite(b[6]) && b[6] > 0.0f;
}

// Oggetti di scena statici che diventano fisici (fase 2): da 4 a 110 cm di
// diagonale, con collisione semplice (StaticMesh.BodySetup). Esclusi per nome
// i pezzi fissati o che non sono oggetti; elenco rivisto a mano sulle 422 mesh
// piccole dei 18 livelli (24/10): parti di veicoli (portiere, ruote, cofani,
// sedili; le portiere sono la fase 5), luci e lampade (la luce resterebbe
// dov'era), condotte, cornici, colonne, pannelli, vetrate e schegge, insegne,
// oblo', tavoli, scaffali, armadi, rastrelliere, banconi, carrelli, macchinari
// fissi, piante e rocce, mucchi e gruppi di oggetti fusi in una mesh, detriti
// sparsi, cadaveri, oggetti delle cinematiche. Le mesh che il gioco stesso
// puo' rendere dinamiche (bCanBecomeDynamic: le bottiglie a cui si spara)
// valgono sempre. Restano prendibili bottiglie, lattine, borse, valigie,
// scatole e casse, radio, libri, piatti, bicchieri, sedie, barili, sacchi,
// cuscini, giocattoli, attrezzi.
constexpr float kStaticMinRadius = 2.0f;   // unita' (raggio della sfera dei limiti)
constexpr float kStaticMaxRadius = 55.0f;
const char* const kStaticExcluded[] = {
    // veicoli
    "door", "wheel", "tire", "tyre", "hood", "seat", "bumper", "frontend", "trunk", "mirror", "bus_", "chassis",
    "win_car", "fender", "hull", "wreck", "yacht_block", "radome", "_bar_0", "sofa",
    // strutture e impianti
    "pole", "pipe", "cable", "wire", "fix", "rappel", "rail", "fence", "beam", "frame", "window", "wall", "floor",
    "roof", "stair", "ceiling", "duct", "vent", "trim", "column", "pillar", "panel", "grid", "edge", "corner",
    "capstone", "plaza", "post", "barrier", "hanging", "hook", "anchor", "hose", "pump", "generator", "server",
    "booth", "terminal", "parking", "elevator", "button", "socket", "mount", "porthole", "sink", "kitchen",
    "anti_thief", "copy_machine",
    // luci, schermi fissi, insegne
    "light", "lamp", "spot", "sign", "billboard", "logo", "display", "on_air", "prompter", "dolly", "mobile",
    // vetri
    "shard", "security_glass", "destroyed_glass", "glass_art",
    // mobili grandi e appoggi
    "table", "bench", "shelf", "cabinet", "rack", "lectern", "drawer", "cubicle", "mattress", "armchair",
    "lounge_chair", "piano_0", "stand", "cart", "tarp", "cover", "destructible",
    // mucchi, sparsi, gruppi, decorazioni fuse con l'ambiente
    "cluster", "pile", "heap", "scattered", "looted", "debris", "notes", "pages", "wax", "trash_01", "_straight",
    "laptop_main", "laptop_screen", "cinematic",
    // corpi, piante, terreno
    "pose", "dead", "corpse", "body", "grass", "foliage", "plant", "tree", "bush", "leaf", "leaves", "agave",
    "bamboo", "feather", "peacock", "coral", "boulder", "stone_", "rock_", "cliff", "terrain", "sandmesh", "sand_",
    "dune", "decal", "blood",
};
std::unordered_map<uint32_t, bool> g_meshAllowed;

// Superfici statiche senza corpo PhysX (BlockRigidBody spento: quasi tutti gli
// oggetti piccoli e molti mobili): un oggetto fisico le attraversava (la radio
// del livello 1 resa fisica e' caduta di 72 cm dal mobile, 24/10). Raccolte da
// Scan vicino a Walker; SupportsNear le rende solide al passaggio di un oggetto
// tenuto, in volo o appena lasciato.
struct NoBody {
    void* comp;
    float mn[3], mx[3];
};
std::vector<NoBody> g_noBody;
// Da dove viene un oggetto che si muove (pipeline dei colpi, revisione 24/10):
// le voci 1, 2, 4 filtrano su questo. Prima ogni oggetto in g_falling contava
// come lanciato, anche quello spinto via da un colpo di Walker (che OnShot ha
// gia' gestito) o urtato dalla sfera della mano.
enum class HitSource : uint8_t {
    kHeld,     // in mano
    kThrown,   // lasciato dalla mano a 1,5 m/s o piu' (Release col grip)
    kDropped,  // lasciato piano o per un altro motivo (scivolato, disattivato)
    kGlove,    // agganciato, in volo o sospeso col guanto e poi lasciato
    kShot,     // reso fisico e spinto da un colpo di Walker (OnShot)
    kKnocked,  // urtato dalla sfera della mano (ProxyKnockStatics)
    kOther,    // diventato fisico in un altro modo (presa non riuscita)
};
// Oggetti appena lasciati o caduti dal guanto: per 4 s le superfici che
// attraversano diventano solide (SupportsAround).
struct Falling {
    void* actor;
    void* comp;
    double until;
    // Assestamento dopo la conversione: gli oggetti di scena sono spesso
    // compenetrati tra loro o nel terreno e PhysX li espelleva (un barile
    // reso fisico saltava via); fino a clampUntil la velocita' resta sotto
    // maxSpeed (unita'/s), la rotazione sotto 10 rad/s.
    double clampUntil = 0.0;
    float maxSpeed = 0.0f;
    // Velocita' degli ultimi tick con l'ora (urti: fase 4). Il cambio si misura
    // su 50 ms: a 250 Hz PhysX distribuisce un urto su piu' passi.
    float histV[6][3] = {};
    double histT[6] = {};
    int histN = 0;
    float maxDv = 0.0f;  // cambio di velocita' massimo visto (diagnostica nel log)
    // Origine (FallingAdd): l'ultima vale per tutte le voci dello stesso componente.
    HitSource source = HitSource::kOther;
    int8_t hand = -1;  // mano che l'ha lasciato, agganciato o urtato (-1 nessuna)
    uint32_t gen = 0;  // cambia a ogni origine nuova: per la pipeline e' un lancio nuovo
    double gloveUntil = 0.0;  // voce 5: fino a quando la bombola conta ancora come mossa dal guanto (niente urto)
    bool gloveLogged = false;  // voce 5: cambio ignorato gia' nel log
    // Voce 7 (finta granata): il primo urto di questo lancio e' gia' stato
    // visto (o l'oggetto ha colpito un nemico): niente altri avvisi all'IA.
    bool fakeDone = false;
    // Voce 9 (rumore), revisione: un urto resta visibile per ~50 ms (la
    // finestra della storia), cioe' per piu' tick. noiseLatch = questo urto e'
    // gia' stato preso (si spegne quando la finestra non lo vede piu');
    // noiseDv > 0 = rumore in attesa: il cambio piu' grande della finestra,
    // mandato a finestra chiusa, a 0,1 s dal primo tick o subito se l'urto
    // rompe o fa esplodere l'oggetto.
    bool noiseLatch = false;
    float noiseDv = 0.0f;
    double noiseT = 0.0;
};
std::vector<Falling> g_falling;
uint32_t g_fallingGen = 0;

// Un oggetto in piu' da seguire per 4 s. Lo stesso componente puo' avere gia'
// una voce (reso fisico alla presa, poi lasciato): restano tutte e due (ognuna
// col suo assestamento), ma l'origine nuova va su entrambe.
void FallingAdd(Falling f, HitSource source, int hand) {
    f.source = source;
    f.hand = static_cast<int8_t>(hand);
    f.gen = ++g_fallingGen;
    for (Falling& o : g_falling)
        if (o.comp == f.comp) {
            o.source = f.source;
            o.hand = f.hand;
            o.gen = f.gen;
            o.fakeDone = false;  // lancio nuovo: puo' fare di nuovo da finta granata (voce 7)
        }
    g_falling.push_back(f);
}
// Fino a 4 m di raggio (mobili, casse, parti di veicoli). Le mesh grandi hanno
// spesso una collisione semplice grossolana, lasciata spenta apposta: rese
// solide creavano ostacoli invisibili attorno a Walker (la bottiglia tenuta
// vicino al corpo scivolava via, 24/10). Escluse le mesh solo visive.
constexpr float kSupportMaxRadius = 200.0f;
const char* const kSupportExcluded[] = {"alpha", "decal", "edge", "sand", "dune", "terrain", "grass", "foliage", "leaf",
                                        "leaves", "blend", "fx_", "sky", "bkg", "vista", "plate", "shadow", "light",
                                        "cable", "wire", "smoke", "dust", "cloud", "water", "glass"};
std::unordered_map<uint32_t, bool> g_supportAllowed;

bool SupportMeshAllowed(uint32_t mesh) {
    auto it = g_supportAllowed.find(mesh);
    if (it != g_supportAllowed.end()) return it->second;
    std::string n = vr::script::ObjectName(reinterpret_cast<void*>(mesh));
    for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    bool ok = true;
    for (const char* k : kSupportExcluded)
        if (n.find(k) != std::string::npos) ok = false;
    g_supportAllowed[mesh] = ok;
    return ok;
}

bool StaticMeshAllowed(uint32_t mesh) {
    if (!mesh) return false;
    auto it = g_meshAllowed.find(mesh);
    if (it != g_meshAllowed.end()) return it->second;
    bool ok = U32(reinterpret_cast<void*>(mesh), kOffMeshBodySetup) != 0;
    if (ok && !(U32(reinterpret_cast<void*>(mesh), kOffMeshCanBecomeDynamic) & 1)) {
        std::string n = vr::script::ObjectName(reinterpret_cast<void*>(mesh));
        for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (const char* k : kStaticExcluded)
            if (n.find(k) != std::string::npos) ok = false;
    }
    g_meshAllowed[mesh] = ok;
    return ok;
}

// Oggetti del motore che servono alla prima presa (oggetto di default e classe
// di KActorFromStatic, tipo di danno, classe e mesh della sfera della mano):
// cercarli in tutto GObjects (~200.000 oggetti) costava un picco di ~5 ms
// alla prima presa di ogni livello; li riconosce per indice di nome la
// scansione a pezzi, che vede comunque ogni oggetto. Le funzioni sotto
// ripiegano sul giro intero solo se non sono ancora stati visti.
enum { kPfKafsDefault, kPfKafsClass, kPfDamageMelee, kPfSpawnableClass, kPfSphereMesh, kPfDamageStun, kPfDamageExplosive, kPfAiPropagator, kPfCount };
struct Prefetch {
    bool init = false;
    int32_t name[kPfCount] = {};
    void* obj[kPfCount] = {};
} g_pf;

void PrefetchCheck(uint32_t o) {
    if (!g_pf.init) {
        const char* names[kPfCount] = {"Default__KActorFromStatic", "KActorFromStatic", "YDamageType_Melee", "KActorSpawnable", "Sphere",
                                       "YDamageType_Stun", "YDamageType_Explosive", "Default__YAIEventPropagator"};
        for (int k = 0; k < kPfCount; ++k) g_pf.name[k] = vr::script::NameIndex(names[k]);
        g_pf.init = true;
    }
    int32_t n = static_cast<int32_t>(U32(reinterpret_cast<void*>(o), kOffName));
    for (int k = 0; k < kPfCount; ++k) {
        if (n != g_pf.name[k] || n < 0 || g_pf.obj[k]) continue;
        void* obj = reinterpret_cast<void*>(o);
        bool ok = false;
        if (k == kPfKafsDefault) ok = true;
        else if (k == kPfAiPropagator) ok = vr::script::ClassName(obj) == "YAIEventPropagator";  // voce 7: il singleton e' l'oggetto di default
        else if (k == kPfSphereMesh)
            ok = vr::script::ClassName(obj) == "StaticMesh" &&
                 vr::script::ObjectName(reinterpret_cast<void*>(U32(obj, 0x28))) == "PawnCollisionTriggerMesh";  // Outer
        else ok = vr::script::ClassName(obj) == "Class";
        if (ok) g_pf.obj[k] = obj;
    }
}

void* Prefetched(int k) {
    void* o = g_pf.obj[k];
    if (o && static_cast<int32_t>(U32(o, kOffName)) == g_pf.name[k]) return o;
    g_pf.obj[k] = nullptr;
    return nullptr;
}

// L'oggetto di default di KActorFromStatic (le funzioni statiche si chiamano su
// quello), cercato una volta per nome in GObjects.
void* KafsDefault() {
    if (void* pf = Prefetched(kPfKafsDefault)) return pf;
    static void* cached = nullptr;
    static int32_t want = -1;
    if (want < 0) want = vr::script::NameIndex("Default__KActorFromStatic");
    if (want < 0) return nullptr;
    if (cached && static_cast<int32_t>(U32(cached, kOffName)) == want) return cached;
    cached = nullptr;
    uint32_t data = U32(reinterpret_cast<void*>(kGObjects), 0), num = U32(reinterpret_cast<void*>(kGObjects), 4);
    for (uint32_t i = 0; data && i < num && i < 2000000; ++i) {
        uint32_t o = 0;
        if (!TrySafeReadUint32(reinterpret_cast<void*>(data + 4 * i), &o) || !o) continue;
        if (static_cast<int32_t>(U32(reinterpret_cast<void*>(o), kOffName)) == want) {
            cached = reinterpret_cast<void*>(o);
            break;
        }
    }
    return cached;
}

// Rende solide per i corpi rigidi (SetBlockRigidBody + InitRBPhys: corpo
// PhysX fisso) le superfici statiche senza corpo entro r dal punto c; al piu'
// maxMade per chiamata (creare un corpo costa). Una volta solide restano tali.
void SupportsNear(const float* c, float r, int maxMade = 6) {
    static int logged = 0;
    int made = 0;
    for (size_t i = 0; i < g_noBody.size() && made < maxMade;) {
        const NoBody& nb = g_noBody[i];
        float d2 = 0.0f;
        for (int k = 0; k < 3; ++k) {
            float v = c[k] < nb.mn[k] ? nb.mn[k] - c[k] : (c[k] > nb.mx[k] ? c[k] - nb.mx[k] : 0.0f);
            d2 += v * v;
        }
        if (d2 > r * r) {
            ++i;
            continue;
        }
        void* comp = nb.comp;
        g_noBody[i] = g_noBody.back();
        g_noBody.pop_back();
        // Ancora un StaticMeshComponent attaccato e senza corpo (Scan e' di al piu' 0,5 s fa).
        uint32_t cls = U32(comp, kOffClass);
        if (!cls || static_cast<int32_t>(U32(reinterpret_cast<void*>(cls), kOffName)) != g_smcName) continue;
        if (U32(comp, kOffCompBodyInstance) || !(U32(comp, kOffCompAttached) & 1)) continue;
        struct {
            uint32_t b;
        } blk = {1};
        vr::script::CallFunction(comp, "SetBlockRigidBody", &blk);
        uint32_t none[2] = {0, 0};
        vr::script::CallFunction(comp, "InitRBPhys", none);
        ++made;
        if (logged < 8) {
            ++logged;
            vr::log::Line("PROPS: " + vr::script::ObjectName(reinterpret_cast<void*>(U32(comp, kOffSmcStaticMesh))) +
                          (U32(comp, kOffCompBodyInstance) ? " now solid for physics objects" : " could not get a physics body"));
        }
    }
}

// Superfici attorno a un oggetto fisico (centro dei limiti, raggio + margine).
void SupportsAround(void* comp, float speed, int maxMade = 6) {
    Prop cur;
    if (!comp || !ReadBounds(comp, &cur)) return;
    float c[3];
    for (int i = 0; i < 3; ++i) c[i] = 0.5f * (cur.boxMin[i] + cur.boxMax[i]);
    SupportsNear(c, cur.radius + 40.0f + 0.15f * speed, maxMade);
}

// Oggetto statico -> KActorFromStatic con lo stesso componente (mesh,
// materiali, luce restano), con il meccanismo del gioco per le bottiglie a cui
// si spara (KActorFromStatic.MakeDynamic, nativa: sposta il componente su un
// KActor nuovo e lo rende corpo rigido). Senza corpo PhysX (BlockRigidBody
// spento: quasi tutti gli oggetti piccoli) MakeDynamic non fa nulla: prima
// SetBlockRigidBody(true) e InitRBPhys lo creano (24/10, verificato sulla
// radio del livello 1). Quando l'oggetto si ferma il gioco lo rende di nuovo
// statico (OnSleepRBPhysics -> MakeStatic) nella posa nuova. source e hand:
// chi l'ha mosso (origine dei colpi in g_falling).
// Bombole esplosive (voce 5 del piano dei combattimenti, 26/10): le
// YExplosiveActor (YExpl_PropaneBottle, YExpl_ScubaTank...) sono
// bWorldGeometry e bMovable spento, e MakeDynamic le rifiuta (vuole un
// proprietario StaticMeshActorBase): erano evidenziate ma non si prendevano.
// Ora alla presa (o all'aggancio del guanto) la mesh la porta un
// KActorSpawnable con la stessa mesh, scala e materiali, e l'originale si
// nasconde senza collisione (CarrierCreate, piu' avanti); compOut = il
// componente che si muove davvero (quello del portatore).
constexpr uintptr_t kOffExplHealth = 0x200;     // YExplosiveActor.m_currentHealth (float; m_defaultHealth +0x1FC: 50 la bombola)
constexpr uintptr_t kOffExplNumHits = 0x214;    // m_numHits (int): 0 = la prima esplosione aspetta 0,15 s (SetTimer)
constexpr uintptr_t kOffExplFlags = 0x224;      // m_exploded 0x1
constexpr uintptr_t kOffExplSmc = 0x22C;        // m_statMeshComp (la bombola intera; m_destroyedStatMeshComp +0x230)
constexpr uint32_t kExplExploded = 0x1;
// Nome completo di un oggetto (FName: indice +0x2C e numero +0x30). Con
// l'indirizzo dice se e' ancora lui: GObjects riusa gli slot liberi e
// l'allocatore i blocchi della stessa misura, e KActorSpawnable_3 e
// KActorSpawnable_4 hanno lo stesso indice (revisione della voce 5: prima
// solo l'indice, e un portatore nuovo nello stesso slot passava per il vecchio).
struct ObjId {
    int32_t index = 0, number = 0;
};
struct Carrier {
    void* expl = nullptr;   // YExplosiveActor (resta nel livello: e' lei che esplode)
    void* orig = nullptr;   // il suo m_statMeshComp, nascosto e senza collisione
    void* actor = nullptr;  // KActorSpawnable che porta la mesh
    void* comp = nullptr;   // il suo StaticMeshComponent
    ObjId explId, actorId;  // gli oggetti sono ancora quelli?
    float home[3] = {};       // Location originale dello YExpl
    float centerOff[3] = {};  // centro dei limiti della mesh meno Location, a casa
    uint32_t origColl = 0;    // PrimitiveComponent +0x14C dell'originale prima della presa
    bool moved = false;       // componenti dello YExpl spostati almeno una volta (ForceUpdateComponents)
    float syncC[3] = {};      // centro del portatore all'ultimo spostamento dei componenti
    double since = 0.0;
};
std::vector<Carrier> g_carriers;
void* CarrierCreate(const Prop& p, HitSource source, int hand, void** compOut);  // piu' avanti

void* MakePropDynamic(const Prop& p, HitSource source, int hand, void** compOut = nullptr) {
    if (compOut) *compOut = p.comp;
    if (!p.isStatic) return p.actor;
    if (p.explosive) return CarrierCreate(p, source, hand, compOut);
    void* comp = p.comp;
    uint32_t own = U32(comp, kOffCompOwner);
    if (own && own != static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p.actor))) {
        // Gia' convertito nel frattempo (dal gioco o da un'altra mano).
        void* a = reinterpret_cast<void*>(own);
        return ActorUsable(a) ? a : nullptr;
    }
    if (!(U32(comp, kOffCompAttached) & 1)) return nullptr;
    void* kafs = KafsDefault();
    if (!kafs) return nullptr;
    if (!U32(comp, kOffCompBodyInstance)) {
        struct {
            uint32_t b;
        } blk = {1};
        vr::script::CallFunction(comp, "SetBlockRigidBody", &blk);
        uint32_t none[2] = {0, 0};
        vr::script::CallFunction(comp, "InitRBPhys", none);
    }
    struct {
        void* mesh;
        void* ret;
    } md = {comp, nullptr};
    bool called = U32(comp, kOffCompBodyInstance) && vr::script::CallFunction(kafs, "MakeDynamic", &md);
    void* a = called ? md.ret : nullptr;
    std::string mesh = vr::script::ObjectName(reinterpret_cast<void*>(U32(comp, kOffSmcStaticMesh)));
    if (!a || !ActorUsable(a)) {
        vr::log::Line("GRAB: static " + mesh + " could not become physical (" + (called ? "MakeDynamic returned None" : "no body") + ")");
        g_meshAllowed[U32(comp, kOffSmcStaticMesh)] = false;  // non riproviamo con questa mesh
        return nullptr;
    }
    vr::log::Line("GRAB: static " + mesh + " became physical (" + vr::script::ObjectName(a) + ")");
    SupportsAround(comp, 0.0f, 8);  // non cade dal mobile (o dal suolo) senza collisione su cui stava; poi 6 per tick
    FallingAdd({a, comp, ProfMs() * 0.001 + 4.0, ProfMs() * 0.001 + 0.5, 1.0f * vr::control::g_posScale}, source,
               hand);  // e per 4 s quello che attraversa
    return a;
}

// --- Rotture (fase 4, 24/10) -------------------------------------------------
// Il gioco rompe gia' le sue bottiglie quando gli si spara: StaticMesh con
// bCanBecomeDynamic, "Health for KActorFromStatic" (+0x100: 30 le bottiglie,
// 1 le bottiglie di liquore, 20 i teschi) e una classe template
// (KActorForStaticClass +0x104: YKAFST_GlassMedium = particelle
// FX_vaporizing_glassMedium e suono sfx_VaporBottle, YKAFST_Bottle per la
// plastica, YKAFST_CorpseHead); MakeDynamic li copia nel KActorFromStatic
// (+0x2F0 vita, +0x2F4 template) e TakeDamage oltre la vita lo distrugge con
// l'effetto (verificato con ucall). La mod aggiunge la rottura negli urti:
// lanciato o caduto (cambio di velocita' oltre 5 m/s in un tick partendo da
// oltre 3 m/s) o sbattuto tenendolo in mano (mano oltre 3 m/s e oggetto
// fermato a meno del 40% della velocita' data). E i colpi della mano di
// Walker sugli oggetti di scena che il gioco non rende dinamici: diventano
// fisici e ricevono l'impulso nel punto colpito (e si rompono se hanno il template).
constexpr uintptr_t kOffKafsHealth = 0x2F0, kOffKafsTemplate = 0x2F4;

void* ClassByName(const char* name, void** cache) {
    if (std::strcmp(name, "KActorFromStatic") == 0) {
        if (void* pf = Prefetched(kPfKafsClass)) return pf;
    } else if (std::strcmp(name, "YDamageType_Melee") == 0) {
        if (void* pf = Prefetched(kPfDamageMelee)) return pf;
    } else if (std::strcmp(name, "YDamageType_Stun") == 0) {
        if (void* pf = Prefetched(kPfDamageStun)) return pf;
    } else if (std::strcmp(name, "YDamageType_Explosive") == 0) {
        if (void* pf = Prefetched(kPfDamageExplosive)) return pf;
    }
    if (*cache && vr::script::ObjectName(*cache) == name) return *cache;
    *cache = nullptr;
    uint32_t data = U32(reinterpret_cast<void*>(kGObjects), 0), num = U32(reinterpret_cast<void*>(kGObjects), 4);
    int32_t want = vr::script::NameIndex(name);
    if (want < 0) return nullptr;
    for (uint32_t i = 0; data && i < num && i < 2000000; ++i) {
        uint32_t o = 0;
        if (!TrySafeReadUint32(reinterpret_cast<void*>(data + 4 * i), &o) || !o) continue;
        void* obj = reinterpret_cast<void*>(o);
        if (static_cast<int32_t>(U32(obj, kOffName)) == want && vr::script::ClassName(obj) == "Class") {
            *cache = obj;
            break;
        }
    }
    return *cache;
}

// KActorFromStatic con un template di rottura e la vita: si rompe.
bool Breakable(void* actor) {
    static void* kafsClass = nullptr;
    void* cls = ClassByName("KActorFromStatic", &kafsClass);
    if (!actor || !cls || reinterpret_cast<void*>(U32(actor, kOffClass)) != cls) return false;
    int32_t health = static_cast<int32_t>(U32(actor, kOffKafsHealth));
    return U32(actor, kOffKafsTemplate) && health > 0 && health < 100000;
}

void ProxyForget(void* comp);  // piu' avanti, con le sfere delle mani

void BreakProp(void* actor, void* comp, const char* why) {
    static void* dmgClass = nullptr;
    Prop cur;
    if (!ReadBounds(comp, &cur)) return;
    struct {
        int32_t damage;
        void* instigator;
        float loc[3];
        float momentum[3];
        void* damageType;
        uint8_t hitInfo[0x1C];
        void* causer;
    } td = {};
    td.damage = static_cast<int32_t>(U32(actor, kOffKafsHealth)) + 1;
    for (int i = 0; i < 3; ++i) td.loc[i] = 0.5f * (cur.boxMin[i] + cur.boxMax[i]);
    td.damageType = ClassByName("YDamageType_Melee", &dmgClass);
    std::string mesh = vr::script::ObjectName(reinterpret_cast<void*>(U32(comp, kOffSmcStaticMesh)));
    // Il gioco distrugge attore e componente: via da tutte le liste della mod.
    g_props.erase(std::remove_if(g_props.begin(), g_props.end(), [&](const Prop& p) { return p.comp == comp; }), g_props.end());
    ProxyForget(comp);
    vr::script::CallFunction(actor, "TakeDamage", &td);
    vr::log::Line("PROPS: " + mesh + " broke (" + why + ")");
}

// Il gioco renderebbe dinamico da se' questo componente (stessa prova della
// nativa CanBecomeDynamic, vtable +0x2A0): ai suoi colpi pensa il gioco.
bool GameMakesDynamic(void* comp) {
    uint32_t mesh = U32(comp, kOffSmcStaticMesh), fl = U32(comp, 0x14C);
    return mesh && (U32(reinterpret_cast<void*>(mesh), kOffMeshCanBecomeDynamic) & 1) && !(U32(comp, 0x23C) & 1) && (fl & 0x40) &&
           !(fl & 0x80);
}

struct ShotEvent {
    void* actor;
    void* comp;
    float hit[3], dir[3];
    // Voce 5: danno dell'arma (YWeapon.m_damage +0x704) e chi spara (il suo
    // Controller), per una bombola portata (CarrierHit). other = colpo di
    // un'altra arma (OnOtherShot): conta solo su una bombola portata.
    float damage = 0.0f;
    void* ctrl = nullptr;
    bool other = false;
    void* dmgType = nullptr;  // YWeapon.m_damageType (+0x248, classe; revisione della voce 5)
    const char* why = nullptr;  // per il log: come e' arrivato il colpo (OnOtherShot)
};
std::mutex g_shotMutex;
std::vector<ShotEvent> g_shots;

// Scudi con oggetti tenuti (voce 3 del piano dei combattimenti, 26/10): colpi
// istantanei di altre armi (nemici, squadra) su un oggetto tenuto, raccolti
// dall'hook di CalcWeaponFire (OnOtherShot, dentro il ciclo dei colpi del
// gioco) e gestiti nel tick (ShieldTick: spinta, vibrazione, usura). Stesso
// mutex dei colpi di Walker.
struct ShieldEvent {
    int hand = 1;
    void* actor = nullptr;
    void* comp = nullptr;
    void* weapon = nullptr;
    float hit[3] = {}, dir[3] = {};
    float damage = 0.0f;   // YWeapon.m_damage (+0x704)
    bool redirected = false;  // il gioco aveva preso Walker: riscritto sull'oggetto
    double at = 0.0;
};
std::vector<ShieldEvent> g_shieldEvents;
// Usura per oggetto (componente: resta anche se l'oggetto torna statico e
// poi fisico), per tutta la permanenza nel livello.
struct ShieldWearState {
    void* actor = nullptr;
    int hits = 0, cap = 0;
    int material = 0;  // vr::pure::ShieldMaterial
};
std::unordered_map<void*, ShieldWearState> g_shieldWear;
// Ultimo colpo contato (i pallini di un fucile a pompa sono piu' trace della
// stessa arma nello stesso istante: usura e vibrazione una volta).
struct ShieldVolley {
    void* weapon = nullptr;
    void* comp = nullptr;
    double at = -10.0;
} g_shieldVolley;
// Riga ogni 30 s (Tick): colpi delle altre armi visti, su Walker, sugli
// oggetti tenuti (fermati dal gioco), riscritti sull'oggetto, lasciati passare
// (propshield off), oggetti volati via o rotti.
// friendly = colpi di Adams e Lugo (o di chi non e' nemico) che toccavano un
// oggetto tenuto, senza effetti della mod; script = chiamate a CalcWeaponFire
// dagli script (non proiettili, ignorate).
struct ShieldStats {
    uint32_t shots = 0, onWalker = 0, onHeld = 0, redirected = 0, passed = 0, knocked = 0, broke = 0, pellets = 0;
    uint32_t friendly = 0, script = 0;
} g_shieldStats;
// Collaudo "shieldtrace off": gli oggetti tenuti restano fuori dalle trace a
// estensione zero (BlockZeroExtent spento), come un oggetto che la trace del
// gioco non prende: i colpi arrivano a Walker e la riscrittura sull'oggetto
// (OnOtherShot) si puo' provare. Scritto da control::Poll, letto nel tick.
// I componenti a cui il collaudo ha spento il bit (con il valore di prima)
// lo riavranno appena non sono piu' tenuti o il collaudo finisce
// (ShieldTraceTestTick; revisione della voce 3: prima restavano senza per il
// resto del livello).
std::atomic<bool> g_shieldNoTraceTest{false};
struct NoTraceComp {
    void* actor = nullptr;
    void* comp = nullptr;
    uint32_t bit = 0;  // BlockZeroExtent (0x8) prima del collaudo
};
std::vector<NoTraceComp> g_noTraceComps;
// Testa (posizione della vista dell'occhio appena disegnato, hooks.cpp prima
// di Tick): l'oggetto colpito vola via lontano da li' (ShieldTick).
float g_viewOrigin[3] = {0, 0, 0};
double g_viewOriginAt = -10.0;
// Collaudo "enemyfire".
struct EnemyFireReq {
    uint32_t pawn = 0;
    int shots = 0;
    bool hasTarget = false;
    float target[3] = {};
} g_enemyFireReq;

// Oggetti fisici entro 12 m da Walker e oggetti di scena statici entro 8,5 m
// (GObjects; le classi gia' viste si decidono una volta sola). Il giro
// completo di GObjects (~190.000 oggetti) costava ~5 ms in un solo tick ogni
// 0,5 s: un frame perso a 90 Hz. Ora e' a pezzi, ~1/45 a ogni tick (giro
// completo in ~0,5 s a 90 Hz); le liste usate (g_props, g_noBody) sono
// quelle dell'ultimo giro completo. Dopo spawnprop (g_rescan) un giro intero
// subito, come prima (solo nei collaudi).
// Bersagli del mondo per la pipeline dei colpi: raccolti da Scan entro 15 m
// (distanza dal box), con il box del componente di collisione.
// - FracturedStaticMeshActor (mesh fratturabile qualsiasi, non solo vetro;
//   anche PhysXDestructibleActor, YFracturedSMActorMovable e
//   FracturedSMActorSpawnable, che derivano da lei): kTgtFractured. Esclusi i
//   FracturedStaticMeshPart, i pezzi staccati di una mesh rotta: il livello 1
//   ne tiene 51 in riserva a 9,4 km (24/10).
// - YSandBomb (finestra di sabbia, uno StaticMeshActor).
// - YDestructibleObject (un Actor, non un KActor) e derivate: i vetri dei bus
//   del livello 1 sono YDestructibleObjectDelayed con la mesh
//   m01a_busGlass_flatWindow_single, senza corpo PhysX (BodyInstance 0,
//   BlockNonZeroExtent acceso): li trova solo la trace.
// Gli oggetti di default e gli archetipi (ObjectFlags +0x8: RF_ClassDefaultObject
// 0x200, RF_ArchetypeObject 0x400) hanno un CollisionComponent anche loro
// (il modello, con limiti a zero): esclusi per flag. In un livello in streaming
// caricato ma non visibile il componente di collisione non e' attaccato e ha
// limiti a zero: escluso finche' il livello non diventa visibile.
// glass: la mesh ha "glass" nel nome (vetri dei bus; voce 2).
enum : uint8_t { kTgtNone, kTgtPawn, kTgtFractured, kTgtSandBomb, kTgtDestructible };
struct WorldTarget {
    void* actor;
    uint8_t kind;
    bool glass;
    float mn[3], mx[3];
};
std::vector<WorldTarget> g_targets;
// Muri invisibili (seguito della voce 2, 25/10): BlockingVolume con
// BlockRigidBody entro 30 m da Walker, con i limiti (Scan).
struct BlockVol {
    void* comp;
    void* actor;  // il BlockingVolume (VolumeAlive prima di ogni uso)
    float mn[3], mx[3];
};
std::vector<BlockVol> g_blockVols;
int32_t g_blockVolName = -1;
int32_t g_fsmaName = -1, g_fsmPartName = -1, g_sandBombName = -1, g_destructibleName = -1;
// Classi per la scelta dei nemici della pipeline: cercate in GNames a inizio
// giro di Scan con le altre (una ricerca costa ~1 ms: mai nel tick dei colpi).
int32_t g_yGamePawnName = -1, g_pawnEnemyName = -1, g_teamInfoName = -1;
// Bombole esplosive (voce 5): classi di proprietari che derivano da
// YExplosiveActor (in cache per classe).
int32_t g_explName = -1;
std::unordered_map<uint32_t, bool> g_explClass;
// Armi lasciate a terra dai nemici (voce 6): YPickup_WeaponSpawnable, tipo 8
// di ClassKind. Componente della mesh (m_pickupMesh) e attore, entro 8 m (Scan; anche i guanti).
int32_t g_wpnPickupName = -1;
// YPickupActor / YPickup_Weapon / YPickup_WeaponSpawnable (tools/ue3.py props, 27/10).
constexpr uintptr_t kOffPickupMesh = 0x204;   // m_pickupMesh (SkeletalMeshComponent, anche CollisionComponent)
constexpr uintptr_t kOffPickupFlags = 0x210;  // m_pickupHidden 0x2, m_physicsDisabled 0x4, m_hasPhysAsset 0x200
constexpr uint32_t kPickupHidden = 0x2, kPickupPhysOff = 0x4, kPickupHasPhys = 0x200;
constexpr uintptr_t kOffPickupWeaponClass = 0x2D4;  // m_weaponClass
constexpr uintptr_t kOffPickupAmmoMag = 0x2DC, kOffPickupAmmoRes = 0x2E0;  // m_ammoInMagazine, m_ammoInReserve
constexpr uintptr_t kOffPickupWeaponArch = 0x2F0;  // m_weapon (archetipo dell'arma)
constexpr uintptr_t kOffPickupTemp = 0x2FC;        // m_initTemporarily (bit 0x1)
struct WeaponPick {
    void* actor;
    void* comp;
};
std::vector<WeaponPick> g_weapons;
// Disarmo (voce 10): tutte le YPickup_WeaponSpawnable (al piu' 64), anche
// quelle del pool del gioco (YObjectPool; 10 all'origine nel livello 1).
// ServerDropWeapon ne prende una (SpawnWeaponPickup): cosi' l'arma che cade
// si trova nello stesso tick.
std::vector<void*> g_wpnPool;
bool ExplosiveClass(uint32_t cls) {
    if (!cls || g_explName < 0) return false;
    auto it = g_explClass.find(cls);
    if (it != g_explClass.end()) return it->second;
    const bool e = ClassIsA(cls, g_explName);
    g_explClass[cls] = e;
    return e;
}
constexpr uintptr_t kOffObjectFlags = 0x8;
constexpr uint32_t kRfClassDefaultObject = 0x200, kRfArchetypeObject = 0x400;
std::unordered_map<uint32_t, bool> g_glassMesh;  // StaticMesh -> "glass" nel nome (in cache)

bool NameHasGlass(void* obj) {
    if (!obj) return false;
    std::string n = vr::script::ObjectName(obj);
    for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return n.find("glass") != std::string::npos;
}

bool GlassMesh(uint32_t mesh) {
    if (!mesh) return false;
    auto it = g_glassMesh.find(mesh);
    if (it != g_glassMesh.end()) return it->second;
    bool glass = NameHasGlass(reinterpret_cast<void*>(mesh));
    g_glassMesh[mesh] = glass;
    return glass;
}

// Tipo di una classe per Scan (in cache): 0 niente, 1 KActor, 2 vincolo
// (RB_ConstraintActor), 3 StaticMeshComponent, 4/5/6 mesh fratturabile /
// finestra di sabbia / distruttibile (bersagli dei colpi), 7 BlockingVolume
// (muro invisibile), 8 arma a terra (YPickup_WeaponSpawnable, voce 6).
uint8_t ClassKind(uint32_t cls) {
    auto it = g_grabbableClass.find(cls);
    if (it != g_grabbableClass.end()) return it->second;
    uint8_t kind = 0;
    if (ClassIsA(cls, g_kactorName)) kind = 1;
    else if (g_constraintName >= 0 && ClassIsA(cls, g_constraintName)) kind = 2;
    else if (g_smcName >= 0 && static_cast<int32_t>(U32(reinterpret_cast<void*>(cls), kOffName)) == g_smcName) kind = 3;
    else if (g_fsmPartName >= 0 && ClassIsA(cls, g_fsmPartName)) kind = 0;  // pezzo staccato, non un bersaglio
    else if (g_fsmaName >= 0 && ClassIsA(cls, g_fsmaName)) kind = 4;
    else if (g_sandBombName >= 0 && ClassIsA(cls, g_sandBombName)) kind = 5;
    else if (g_destructibleName >= 0 && ClassIsA(cls, g_destructibleName)) kind = 6;
    else if (g_blockVolName >= 0 && ClassIsA(cls, g_blockVolName)) kind = 7;
    else if (g_wpnPickupName >= 0 && ClassIsA(cls, g_wpnPickupName)) kind = 8;
    g_grabbableClass[cls] = kind;
    return kind;
}

struct ScanBuild {
    uint32_t cursor = 0;
    std::vector<Prop> props;
    std::vector<NoBody> noBody;
    std::vector<WorldTarget> targets;
    std::vector<uint32_t> constrained;  // cerniere: portiere, cartelli sul palo (fase 5)
    std::vector<BlockVol> volumes;      // muri invisibili vicini
    std::vector<WeaponPick> weapons;    // armi a terra (voce 6)
    std::vector<void*> wpnPool;         // tutte le pickup delle armi, pool compreso (voce 10)
    size_t nStatic = 0;
    double ms = 0.0;
} g_scan;

void ScanObject(uint32_t o, const float* pl) {
    PrefetchCheck(o);
    ScanBuild& sb = g_scan;
    const float range = 12.0f * vr::control::g_posScale;
    const float staticRange = 8.5f * vr::control::g_posScale;  // portata dei guanti (8 m) e un po'
    uint32_t cls = U32(reinterpret_cast<void*>(o), kOffClass);
    if (!cls) return;
    uint8_t kind = ClassKind(cls);
    if (kind == 8) {
        // Arma a terra (voce 6): non quelle del pool (nascoste all'origine,
        // m_pickupHidden), non quelle temporanee (m_initTemporarily: senza
        // collisione, il gioco le distrugge dopo 2 s), con la mesh e il
        // PhysicsAsset (m_hasPhysAsset), entro 8 m da Walker (la portata dei
        // guanti gravitazionali, che le attirano dal 27/10; prima 4 m).
        void* actor = reinterpret_cast<void*>(o);
        if (U32(actor, kOffObjectFlags) & (kRfClassDefaultObject | kRfArchetypeObject)) return;
        const uint32_t f9c = U32(actor, kOffActorFlags9C), pf = U32(actor, kOffPickupFlags);
        if (f9c & kDeleteMe) return;
        // Voce 10 (disarmo): tutte, a parte (anche con weaponpickup off), per
        // trovare nello stesso tick quella che ServerDropWeapon prende dal pool.
        // Nel livello 1 le 10 del pool stanno all'origine senza m_pickupHidden
        // e senza m_hasPhysAsset (+0x210 = 0x1145; SetPickupSkelMesh la rende
        // 0x1B45); una tornata nel pool (DisablePickup, dopo una raccolta) tiene
        // 0x1B45 ma perde m_pickupMesh: nessun filtro sullo stato.
        // Solo quelle entro 30 m da Walker, all'origine (pool) o senza mesh: i
        // corpi del livello lasciano decine di armi lontane.
        if (vr::control::g_disarm.load() && sb.wpnPool.size() < 64) {
            const float r30 = 30.0f * vr::control::g_posScale;
            float l[3] = {0, 0, 0};
            const bool locOk = SafeCopy(l, static_cast<char*>(actor) + kOffActorLocation, 12);
            const float d2 = (l[0] - pl[0]) * (l[0] - pl[0]) + (l[1] - pl[1]) * (l[1] - pl[1]) + (l[2] - pl[2]) * (l[2] - pl[2]);
            if (!U32(actor, kOffPickupMesh) || (locOk && (d2 < r30 * r30 || l[0] * l[0] + l[1] * l[1] + l[2] * l[2] < 1.0f)))
                sb.wpnPool.push_back(actor);
        }
        if ((pf & kPickupHidden) || (f9c & kHidden) || !(pf & kPickupHasPhys)) return;
        if (!vr::control::g_weaponPickup.load()) return;
        if (U32(actor, kOffPickupTemp) & 1) return;
        const uint32_t mesh = U32(actor, kOffPickupMesh);
        float loc[3];
        if (!mesh || !SafeCopy(loc, static_cast<char*>(actor) + kOffActorLocation, 12)) return;
        const float dx = loc[0] - pl[0], dy = loc[1] - pl[1], dz = loc[2] - pl[2], lim = 8.0f * vr::control::g_posScale;
        if (dx * dx + dy * dy + dz * dz > lim * lim || sb.weapons.size() >= 32) return;
        sb.weapons.push_back({actor, reinterpret_cast<void*>(mesh)});
        return;
    }
    if (kind == 7) {
        // Muro invisibile che ferma i corpi rigidi, entro 30 m (dal box).
        void* actor = reinterpret_cast<void*>(o);
        if (U32(actor, kOffObjectFlags) & (kRfClassDefaultObject | kRfArchetypeObject)) return;
        if (U32(actor, kOffActorFlags9C) & kDeleteMe) return;
        uint32_t cc = U32(actor, kOffActorCollisionComp);
        Prop b;
        if (!cc || !(U32(reinterpret_cast<void*>(cc), 0x14C) & 0x40) || !U32(reinterpret_cast<void*>(cc), kOffCompBodyInstance) ||
            !ReadBounds(reinterpret_cast<void*>(cc), &b))
            return;
        float d2 = 0.0f;
        for (int i = 0; i < 3; ++i) {
            float v = pl[i] < b.boxMin[i] ? b.boxMin[i] - pl[i] : (pl[i] > b.boxMax[i] ? pl[i] - b.boxMax[i] : 0.0f);
            d2 += v * v;
        }
        const float lim = 30.0f * vr::control::g_posScale;
        if (d2 > lim * lim || sb.volumes.size() >= 256) return;
        BlockVol v;
        v.comp = reinterpret_cast<void*>(cc);
        v.actor = actor;
        std::memcpy(v.mn, b.boxMin, 12);
        std::memcpy(v.mx, b.boxMax, 12);
        sb.volumes.push_back(v);
        return;
    }
    if (kind >= 4) {
        // Bersaglio dei colpi (mesh fratturabile, finestra di sabbia, distruttibile) entro 15 m.
        void* actor = reinterpret_cast<void*>(o);
        if (U32(actor, kOffObjectFlags) & (kRfClassDefaultObject | kRfArchetypeObject)) return;
        if (U32(actor, kOffActorFlags9C) & (kHidden | kDeleteMe)) return;  // la finestra di sabbia si nasconde quando crolla
        uint32_t cc = U32(actor, kOffActorCollisionComp);
        Prop b;
        if (!cc || !(U32(reinterpret_cast<void*>(cc), kOffCompAttached) & 1) || !ReadBounds(reinterpret_cast<void*>(cc), &b)) return;
        // Distanza dal box, non dal centro: niente tetto al raggio (prima oltre
        // 20 m di raggio un bersaglio grande non entrava mai).
        float d2 = 0.0f;
        for (int i = 0; i < 3; ++i) {
            float v = pl[i] < b.boxMin[i] ? b.boxMin[i] - pl[i] : (pl[i] > b.boxMax[i] ? pl[i] - b.boxMax[i] : 0.0f);
            d2 += v * v;
        }
        const float lim = 15.0f * vr::control::g_posScale;
        if (d2 > lim * lim || sb.targets.size() >= 256) return;
        WorldTarget t;
        t.actor = actor;
        t.kind = kind == 4 ? kTgtFractured : (kind == 5 ? kTgtSandBomb : kTgtDestructible);
        uint32_t ccls = U32(reinterpret_cast<void*>(cc), kOffClass);
        t.glass = g_smcName >= 0 && ClassIsA(ccls, g_smcName) && GlassMesh(U32(reinterpret_cast<void*>(cc), kOffSmcStaticMesh));
        std::memcpy(t.mn, b.boxMin, 12);
        std::memcpy(t.mx, b.boxMax, 12);
        sb.targets.push_back(t);
        return;
    }
    if (kind == 3) {
        // Fase 2 (24/10): oggetto di scena statico (bottiglie, borse, radio,
        // detriti in StaticMeshActor / StaticMeshCollectionActor). Resta
        // statico finche' una mano o un guanto non lo prende.
        void* comp = reinterpret_cast<void*>(o);
        uint32_t own = U32(comp, kOffCompOwner);
        if (!own) return;
        uint32_t of = U32(reinterpret_cast<void*>(own), kOffActorFlags9C);
        if (!(of & kWorldGeometry) || (of & (kHidden | kDeleteMe))) return;
        if (U32(comp, kOffCompVisFlags) & kCompHiddenGame) return;
        // Bombola esplosiva (voce 5): solo la mesh intera di una YExplosiveActor
        // non ancora esplosa (non quella distrutta), e solo con propexplosive:
        // spenta, fuori dalla lista come prima del portatore (MakeDynamic le
        // rifiuta: erano evidenziate ma non si prendevano).
        bool explosive = false;
        if (ExplosiveClass(U32(reinterpret_cast<void*>(own), kOffClass))) {
            if (!vr::control::g_propExplosive.load()) return;
            if (U32(reinterpret_cast<void*>(own), kOffExplSmc) != o || (U32(reinterpret_cast<void*>(own), kOffExplFlags) & kExplExploded)) return;
            explosive = true;
        }
        Prop p;
        p.actor = reinterpret_cast<void*>(own);
        p.comp = comp;
        p.isStatic = true;
        p.explosive = explosive;
        if (!ReadBounds(comp, &p)) return;
        float dx = 0.5f * (p.boxMin[0] + p.boxMax[0]) - pl[0], dy = 0.5f * (p.boxMin[1] + p.boxMax[1]) - pl[1],
              dz = 0.5f * (p.boxMin[2] + p.boxMax[2]) - pl[2];
        float d2 = dx * dx + dy * dy + dz * dz;
        if (!explosive && p.radius <= kSupportMaxRadius && d2 <= (staticRange + p.radius) * (staticRange + p.radius) &&
            !U32(comp, kOffCompBodyInstance) && sb.noBody.size() < 4096) {
            uint32_t sm = U32(comp, kOffSmcStaticMesh);
            if (sm && U32(reinterpret_cast<void*>(sm), kOffMeshBodySetup) && SupportMeshAllowed(sm)) {
                NoBody nb;
                nb.comp = comp;
                std::memcpy(nb.mn, p.boxMin, 12);
                std::memcpy(nb.mx, p.boxMax, 12);
                sb.noBody.push_back(nb);
            }
        }
        if (sb.nStatic >= 256 || p.radius < kStaticMinRadius || p.radius > kStaticMaxRadius) return;
        if (d2 > staticRange * staticRange) return;
        if (!StaticMeshAllowed(U32(comp, kOffSmcStaticMesh))) return;
        sb.props.push_back(p);
        ++sb.nStatic;
        return;
    }
    if (kind == 2) {
        for (uintptr_t off : {0x1F8u, 0x1FCu}) {  // RB_ConstraintActor.ConstraintActor1/2
            uint32_t a = U32(reinterpret_cast<void*>(o), off);
            if (a) sb.constrained.push_back(a);
        }
        return;
    }
    if (kind != 1) return;
    void* actor = reinterpret_cast<void*>(o);
    if (actor == g_proxyActor[0] || actor == g_proxyActor[1]) return;
    if (!ActorUsable(actor)) return;
    float loc[3];
    if (!SafeCopy(loc, static_cast<char*>(actor) + kOffActorLocation, 12)) return;
    float dx = loc[0] - pl[0], dy = loc[1] - pl[1], dz = loc[2] - pl[2];
    if (dx * dx + dy * dy + dz * dz > range * range) return;
    if (vr::script::ObjectName(actor).rfind("Default__", 0) == 0) return;
    Prop p;
    p.actor = actor;
    p.comp = PropComponent(actor);
    if (!p.comp || !ReadBounds(p.comp, &p)) return;
    if (p.radius > 2.5f * vr::control::g_posScale) return;  // oltre 5 m di diametro: non e' un oggetto da mano
    if (sb.props.size() - sb.nStatic < 256) sb.props.push_back(p);
}

// Fine del giro: le liste nuove sostituiscono le vecchie.
void ScanFinish(uint32_t num) {
    ScanBuild& sb = g_scan;
    // GObjects non e' in ordine: gli oggetti vincolati si marcano alla fine
    // (fase 5: si prendono tirando il punto afferrato).
    for (Prop& p : sb.props)
        p.constrained = std::find(sb.constrained.begin(), sb.constrained.end(),
                                  static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p.actor))) != sb.constrained.end();
    g_props.swap(sb.props);
    g_noBody.swap(sb.noBody);
    g_targets.swap(sb.targets);
    g_blockVols.swap(sb.volumes);
    g_weapons.swap(sb.weapons);
    g_wpnPool.swap(sb.wpnPool);
    // Costo e contenuto del giro nel log ogni 30 s (diagnosi delle prestazioni).
    static ULONGLONG nextStat = 0;
    if (GetTickCount64() >= nextStat) {
        nextStat = GetTickCount64() + 30000;
        unsigned nGlass = 0;
        for (const WorldTarget& t : g_targets) nGlass += t.glass ? 1u : 0u;
        char b[300];
        std::snprintf(b, sizeof(b),
                      "PROPS: scan %.2f ms in total over %u objects: %u physics, %u static props, %u surfaces without body, "
                      "%u hit targets (fractured meshes, sand windows, destructibles within 15 m; %u glass)",
                      sb.ms, num, static_cast<unsigned>(g_props.size() - sb.nStatic), static_cast<unsigned>(sb.nStatic),
                      static_cast<unsigned>(g_noBody.size()), static_cast<unsigned>(g_targets.size()), nGlass);
        vr::log::Line(b);
    }
    sb.props.clear();
    sb.noBody.clear();
    sb.targets.clear();
    sb.constrained.clear();
    sb.volumes.clear();
    sb.weapons.clear();
    sb.wpnPool.clear();
    sb.nStatic = 0;
    sb.ms = 0.0;
    sb.cursor = 0;
}

void Scan(void* pawn) {
    const double t0 = ProfMs();
    if (g_kactorName < 0) g_kactorName = vr::script::NameIndex("KActor");
    if (g_constraintName < 0) g_constraintName = vr::script::NameIndex("RB_ConstraintActor");
    if (g_smcName < 0) g_smcName = vr::script::NameIndex("StaticMeshComponent");
    if (g_scan.cursor == 0) {
        // Bersagli dei colpi: nomi cercati a inizio giro (un nome che manca
        // costerebbe una ricerca in GNames a ogni tick).
        if (g_fsmaName < 0) g_fsmaName = vr::script::NameIndex("FracturedStaticMeshActor");
        if (g_fsmPartName < 0) g_fsmPartName = vr::script::NameIndex("FracturedStaticMeshPart");
        if (g_yGamePawnName < 0) g_yGamePawnName = vr::script::NameIndex("YGamePawn");
        if (g_pawnEnemyName < 0) g_pawnEnemyName = vr::script::NameIndex("YPawn_Enemy");
        if (g_teamInfoName < 0) g_teamInfoName = vr::script::NameIndex("YTeamInfo");
        if (g_sandBombName < 0) g_sandBombName = vr::script::NameIndex("YSandBomb");
        if (g_destructibleName < 0) g_destructibleName = vr::script::NameIndex("YDestructibleObject");
        if (g_blockVolName < 0) g_blockVolName = vr::script::NameIndex("BlockingVolume");
        if (g_explName < 0) g_explName = vr::script::NameIndex("YExplosiveActor");
        if (g_wpnPickupName < 0) g_wpnPickupName = vr::script::NameIndex("YPickup_WeaponSpawnable");
    }
    if (g_kactorName < 0) return;
    float pl[3];
    if (!SafeCopy(pl, static_cast<char*>(pawn) + kOffActorLocation, 12)) return;
    uint32_t data = U32(reinterpret_cast<void*>(kGObjects), 0), num = U32(reinterpret_cast<void*>(kGObjects), 4);
    if (!data || num > 2000000) return;
    ScanBuild& sb = g_scan;
    uint32_t budget = num / 45 + 1;
    if (g_rescan) {
        // Subito un giro intero (dopo spawnprop: l'oggetto nuovo deve esserci al tick dopo).
        g_rescan = false;
        sb.props.clear();
        sb.noBody.clear();
        sb.targets.clear();
        sb.constrained.clear();
        sb.volumes.clear();
        sb.weapons.clear();
        sb.wpnPool.clear();
        sb.nStatic = 0;
        sb.ms = 0.0;
        sb.cursor = 0;
        budget = num;
    }
    uint32_t end = (std::min)(num, sb.cursor + budget);
    for (uint32_t i = sb.cursor; i < end; ++i) {
        uint32_t o = 0;
        if (TrySafeReadUint32(reinterpret_cast<void*>(data + 4 * i), &o) && o) ScanObject(o, pl);
    }
    sb.cursor = end;
    sb.ms += ProfMs() - t0;
    if (sb.cursor >= num) ScanFinish(num);
}

// Box dell'oggetto fuori dalla capsula di Walker (con 5 unita' di margine, piu'
// il raggio del corpo-spinta se withPush).
bool OutsidePawn(void* pawn, void* actor, bool withPush = true) {
    void* comp = PropComponent(actor);
    Prop p;
    float pl[3], h = 0.0f, r = 0.0f;
    uint32_t cyl = U32(pawn, 0x3AC);  // Pawn.CylinderComponent
    if (!comp || !ReadBounds(comp, &p) || !cyl || !SafeCopy(pl, static_cast<char*>(pawn) + kOffActorLocation, 12) ||
        !SafeCopy(&h, reinterpret_cast<char*>(cyl) + 0x200, 4) || !SafeCopy(&r, reinterpret_cast<char*>(cyl) + 0x204, 4))
        return true;
    // Margine: 5 unita' piu' il raggio del corpo-spinta (Pawn.RBPushRadius).
    float push = 0.0f;
    if (withPush) SafeCopy(&push, static_cast<char*>(pawn) + 0x3B0, 4);
    float m = 5.0f + (std::isfinite(push) ? (std::max)(0.0f, (std::min)(50.0f, push)) : 0.0f);
    if (p.boxMax[2] < pl[2] - h - m || p.boxMin[2] > pl[2] + h + m) return true;
    float cx = (std::max)(p.boxMin[0], (std::min)(pl[0], p.boxMax[0])) - pl[0];
    float cy = (std::max)(p.boxMin[1], (std::min)(pl[1], p.boxMax[1])) - pl[1];
    return cx * cx + cy * cy > (r + m) * (r + m);
}

// Distanza dalla mano al box dell'oggetto (0 dentro).
float BoxDistance(const Prop& p, const float* h) {
    float d2 = 0.0f;
    for (int i = 0; i < 3; ++i) {
        float v = h[i] < p.boxMin[i] ? p.boxMin[i] - h[i] : (h[i] > p.boxMax[i] ? h[i] - p.boxMax[i] : 0.0f);
        d2 += v * v;
    }
    return std::sqrt(d2);
}

const Prop* Nearest(const float* hand, float zone, float* outDist) {
    const Prop* best = nullptr;
    float bd = zone;
    for (const Prop& p : g_props) {
        bool taken = false;
        for (const Held& h : g_held) taken |= h.comp == p.comp;
        if (taken) continue;
        Prop cur = p;
        if (!ReadBounds(p.comp, &cur)) continue;  // i limiti si muovono con l'oggetto
        float d = BoxDistance(cur, hand);
        // A parita' di distanza vince l'oggetto piu' piccolo (bottiglia sul tavolo).
        float score = d + 0.02f * cur.radius;
        if (score < bd) {
            bd = score;
            best = &p;
            *outDist = d;
        }
    }
    return best;
}

float BodyMass(void* comp) {
    struct {
        void* ret;
    } bi = {nullptr};
    if (!vr::script::CallFunction(comp, "GetRootBodyInstance", &bi) || !bi.ret) return -1.0f;
    struct {
        float ret;
    } m = {0.0f};
    if (!vr::script::CallFunction(bi.ret, "GetBodyMass", &m)) return -1.0f;
    return m.ret;
}

void SetLinVel(void* comp, const float* v) {
    struct {
        float v[3];
        uint32_t add;
    } p = {{v[0], v[1], v[2]}, 0};
    vr::script::CallFunction(comp, "SetRBLinearVelocity", &p);
}

void SetAngVel(void* comp, const float* w) {
    struct {
        float v[3];
        uint32_t add;
    } p = {{w[0], w[1], w[2]}, 0};
    vr::script::CallFunction(comp, "SetRBAngularVelocity", &p);
}

void Wake(void* comp) {
    struct {
        int32_t name[2];
    } p = {{0, 0}};
    vr::script::CallFunction(comp, "WakeRigidBody", &p);
}

// PhysX 2.8 limita la rotazione di ogni corpo a 7 rad/s (NX_MAX_ANGULAR_VELOCITY):
// una girata di polso in VR lasciava indietro l'oggetto tenuto e la rotazione
// verso la presa canonica durava fino a 0,45 s. NxActor (BodyInstance.BodyData,
// +0x60) ha setMaxAngularVelocity / getMaxAngularVelocity agli slot +0xF0 / +0xF4
// della vtable (Ghidra: il gioco chiama setAngularVelocity a +0xE4 e
// getAngularVelocity a +0xEC; le asserzioni "Actor::getMaxAngularVelocity" di
// NpActor.cpp confermano l'ordine di NxActor.h).
using NxSetFloat = void(__thiscall*)(void*, float);
using NxGetFloat = float(__thiscall*)(void*);

using NxGetVec = void*(__thiscall*)(void*, float*);  // NxVec3 per puntatore nascosto, come in UE3

bool NxVelocities(void* nx, float lin[3], float ang[3]) {
    __try {
        void** vt = *static_cast<void***>(nx);
        reinterpret_cast<NxGetVec>(vt[58])(nx, lin);
        reinterpret_cast<NxGetVec>(vt[59])(nx, ang);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallNxSetMaxAng(void* nx, float v, float* old) {
    __try {
        void** vt = *static_cast<void***>(nx);
        float o = reinterpret_cast<NxGetFloat>(vt[61])(nx);
        if (!(o > 0.0f && o < 1e6f)) return false;  // non e' un NxActor dinamico come atteso
        *old = o;
        if (o != v) reinterpret_cast<NxSetFloat>(vt[60])(nx, v);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// NxActor::setMass +0xB4 (float), getMass +0xB8, raiseBodyFlag +0x164
// (NxBodyFlag); verificati sulle asserzioni "Actor::setMass",
// "Actor::raiseBodyFlag" di NpActor.cpp (24/10), coerenti con NxActor.h.
using NxSetU32 = void(__thiscall*)(void*, uint32_t);
constexpr uint32_t kNxBfDisableGravity = 1, kNxBfFrozenRot = 0x70;

bool NxSetupProxy(void* nx, float mass, float* oldMass) {
    __try {
        void** vt = *static_cast<void***>(nx);
        *oldMass = reinterpret_cast<NxGetFloat>(vt[46])(nx);
        reinterpret_cast<NxSetFloat>(vt[45])(nx, mass);
        reinterpret_cast<NxSetU32>(vt[89])(nx, kNxBfDisableGravity | kNxBfFrozenRot);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* CompNx(void* comp);  // piu' avanti (anche il corpo dell'arma raccolta, voce 6)
// Tutti i corpi PhysX di una componente (piu' avanti): quello di CompNx, o
// tutti quelli dell'arma raccolta (revisione della voce 6: l'M4 ne ha due) o
// del cadavere trascinato (voce 11: 16 corpi nel ragdoll dei predoni).
constexpr int kMaxCompBodies = 24;
int CompNxAll(void* comp, void* out[kMaxCompBodies]);

void RaiseMaxAngVel(void* comp) {
    // Ogni corpo (revisione della voce 6): il secondo corpo dell'M4 restava a
    // 7 rad/s mentre la guida dell'arma ne chiede fino a 40.
    void* all[kMaxCompBodies];
    const int nb = CompNxAll(comp, all);
    for (int k = 0; k < nb; ++k) {
        uint32_t nx = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(all[k])), vt = 0;
        if (!nx || !TrySafeReadUint32(reinterpret_cast<void*>(nx), &vt) || !vt) continue;
        // Gli slot devono stare nello stesso modulo della vtable (PhysXCore.dll).
        HMODULE mv = nullptr, m60 = nullptr, m61 = nullptr;
        uint32_t s60 = 0, s61 = 0;
        if (!TrySafeReadUint32(reinterpret_cast<char*>(vt) + 60 * 4, &s60) ||
            !TrySafeReadUint32(reinterpret_cast<char*>(vt) + 61 * 4, &s61))
            continue;
        const DWORD fl = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
        if (!GetModuleHandleExA(fl, reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(vt)), &mv) ||
            !GetModuleHandleExA(fl, reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(s60)), &m60) ||
            !GetModuleHandleExA(fl, reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(s61)), &m61) || mv != m60 || mv != m61)
            continue;
        float old = 0.0f;
        bool ok = CallNxSetMaxAng(reinterpret_cast<void*>(nx), 60.0f, &old);
        static bool logged = false;
        if (!logged) {
            logged = true;
            char b[120];
            std::snprintf(b, sizeof(b), "GRAB: max angular velocity %s (was %.1f rad/s, now 60)", ok ? "raised" : "NOT raised", old);
            vr::log::Line(b);
        }
    }
}

// Walker ha corpi PhysX suoi: quello che spinge via gli oggetti vicini
// (Pawn.PhysicsPushBody, bPushesRigidBodies, RBPushStrength 500), i 16 del suo
// PhysicsAsset (braccia, mani, busto, testa), le due capsule a molla di
// YPawnSpringyRBCollider e la capsula cinematica di YPawnKinematicRBCollider
// (Spec Ops: la capsula di Walker, raggio 15 e mezza altezza 45 unita', per
// gli oggetti di gioco; un NxActor senza userData in m_kinActorDataSW). In VR
// testa e petto stanno dentro quella capsula: l'oggetto portato vicino al viso
// la urtava e a ogni passo PhysX ne invertiva la rotazione (comando -38 rad/s,
// risultato +7..+15: fino a 90 gradi e 5 cm di errore, 24/10, trovata per
// bisezione degli attori della scena); uno lasciato li' accanto schizzava via. Mentre e' in mano (o vola verso il guanto) le coppie oggetto /
// corpo di Walker vengono ignorate con NxScene::setActorPairFlags
// (NX_IGNORE_PAIR), slot +0xA4 della vtable della scena: le asserzioni
// "Scene::setActorPairFlags" di NpScene.cpp lo confermano, ret 0xC = tre
// argomenti; la scena da NxActor::getScene, slot +4.
using NxGetScene = void*(__thiscall*)(void*);
using NxSetPairFlags = void(__thiscall*)(void*, void*, void*, uint32_t);
using NxGetPairFlags = uint32_t(__thiscall*)(void*, void*, void*);  // slot +0xA8
constexpr uintptr_t kOffPawnPushBody = 0x404;       // Pawn.PhysicsPushBody (RB_BodyInstance)
constexpr uintptr_t kOffPawnMesh = 0x3A8;           // Pawn.Mesh
constexpr uintptr_t kOffSkelPhysAsset = 0x258;      // SkeletalMeshComponent.PhysicsAsset
constexpr uintptr_t kOffSkelPhysAssetInst = 0x25C;  // SkeletalMeshComponent.PhysicsAssetInstance
constexpr uintptr_t kOffPaiBodies = 0x44;           // PhysicsAssetInstance.Bodies (TArray<RB_BodyInstance*>)
constexpr uint32_t kNxIgnorePair = 1;
constexpr int kMaxWalkerBodies = 48;
// NpScene: NxArray<NxActor*> degli attori (inizio, fine, capacita'), trovato in
// memoria (24/10); ogni uso verifica che contenga un corpo noto di Walker.
constexpr uintptr_t kOffNpSceneActors = 0x308;
constexpr uintptr_t kOffNxActorUserData = 0x4;       // NxActor::userData (UE3: RB_BodyInstance)
constexpr uintptr_t kOffBodyOwnerComponent = 0x3C;  // RB_BodyInstance.OwnerComponent
constexpr uintptr_t kOffComponentOwner = 0x4C;      // ActorComponent.Owner
constexpr uintptr_t kOffActorAllComponents = 0x48;  // Actor.AllComponents (TArray)
constexpr uintptr_t kOffKinActorData = 0x200;       // YPawnKinematicRBCollider.m_kinActorDataSW (NxActor*)

void* NxSceneOf(void* nx) {
    __try {
        void** vt = *static_cast<void***>(nx);
        return reinterpret_cast<NxGetScene>(vt[1])(nx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void* BodyNx(uint32_t bodyInstance) {
    uint32_t nx = 0;
    if (!bodyInstance || !TrySafeReadUint32(reinterpret_cast<char*>(bodyInstance) + 0x60, &nx)) return nullptr;
    return reinterpret_cast<void*>(nx);
}

// Corpo della radice di una SkeletalMeshComponent (arma raccolta, voce 6):
// PhysicsAssetInstance (+0x25C) -> Bodies (+0x44)[0]. Le armi del gioco hanno
// un corpo solo (AK47: osso AR_Root, letto il 27/10). 0 se non c'e' (arma
// addormentata dal gioco: DoSleepActivation toglie il PhysicsAssetInstance).
uint32_t WeaponRootBody(void* comp) {
    uint32_t pai = U32(comp, kOffSkelPhysAssetInst);
    if (!pai) return 0;
    struct {
        uint32_t data;
        int32_t num, max;
    } arr = {};
    if (!SafeCopy(&arr, reinterpret_cast<char*>(pai) + kOffPaiBodies, sizeof(arr)) || !arr.data || arr.num <= 0 || arr.num > 64) return 0;
    return U32(reinterpret_cast<void*>(arr.data), 0);
}

void* CompNx(void* comp) {
    uint32_t bi = 0;
    if (!comp || !TrySafeReadUint32(static_cast<char*>(comp) + kOffCompBodyInstance, &bi)) return nullptr;
    if (!bi && (comp == g_wpnComp[0] || comp == g_wpnComp[1] || CorpsePairMesh(comp)))
        bi = WeaponRootBody(comp);  // voce 11: il corpo radice del ragdoll (0 se congelato)
    return BodyNx(bi);
}

// Tutti i corpi del PhysicsAssetInstance dell'arma raccolta (revisione della
// voce 6): l'M4 ne ha due, e prima coppie ignorate con Walker e con le sfere
// delle mani, rotazione massima e massa valevano solo per Bodies[0]; il
// secondo urtava la capsula cinematica di Walker (l'inversione della
// rotazione del 24/10) e le sfere. Ritorna quanti NxActor.
int WeaponBodies(void* comp, void* out[kMaxCompBodies]) {
    const uint32_t pai = U32(comp, kOffSkelPhysAssetInst);
    if (!pai) return 0;
    struct {
        uint32_t data;
        int32_t num, max;
    } arr = {};
    if (!SafeCopy(&arr, reinterpret_cast<char*>(pai) + kOffPaiBodies, sizeof(arr)) || !arr.data || arr.num <= 0 || arr.num > 64) return 0;
    int n = 0;
    for (int i = 0; i < arr.num && n < kMaxCompBodies; ++i) {
        void* nx = BodyNx(U32(reinterpret_cast<void*>(static_cast<uintptr_t>(arr.data + 4u * static_cast<uint32_t>(i))), 0));
        if (nx) out[n++] = nx;
    }
    return n;
}

int CompNxAll(void* comp, void* out[kMaxCompBodies]) {
    // Cadavere trascinato o appena lasciato (voce 11): tutti i corpi del ragdoll
    // del momento, nessuno se il gioco l'ha congelato (PhysicsAssetInstance tolto).
    if (CorpsePairMesh(comp)) return WeaponBodies(comp, out);
    if (comp && (comp == g_wpnComp[0] || comp == g_wpnComp[1]) && !U32(comp, kOffCompBodyInstance)) {
        const int n = WeaponBodies(comp, out);
        if (n) return n;
    }
    void* nx = CompNx(comp);
    if (!nx) return 0;
    out[0] = nx;
    return 1;
}

// Corpi PhysX di Walker (senza doppioni); ritorna quanti.
int WalkerBodies(void* pawn, void* out[kMaxWalkerBodies]) {
    int n = 0;
    auto add = [&](void* nx) {
        if (!nx || n >= kMaxWalkerBodies) return;
        for (int i = 0; i < n; ++i)
            if (out[i] == nx) return;
        out[n++] = nx;
    };
    if (!pawn) return 0;
    add(BodyNx(U32(pawn, kOffPawnPushBody)));
    uint32_t mesh = U32(pawn, kOffPawnMesh);
    uint32_t pai = mesh ? U32(reinterpret_cast<void*>(mesh), kOffSkelPhysAssetInst) : 0;
    struct {
        uint32_t data;
        int32_t num, max;
    } arr = {};
    if (pai && SafeCopy(&arr, reinterpret_cast<char*>(pai) + kOffPaiBodies, sizeof(arr)) && arr.data && arr.num > 0)
        for (int i = 0; i < arr.num && i < 64; ++i) {
            uint32_t bi = 0;
            if (!TrySafeReadUint32(reinterpret_cast<char*>(arr.data) + 4 * i, &bi)) break;
            add(BodyNx(bi));
        }
    // La capsula cinematica (YPawnKinematicRBCollider tra i componenti di
    // Walker; la classe si cerca per nome una volta sola).
    static uint32_t kinClass = 0;
    struct {
        uint32_t data;
        int32_t num, max;
    } comps = {};
    if (SafeCopy(&comps, static_cast<char*>(pawn) + kOffActorAllComponents, sizeof(comps)) && comps.data && comps.num > 0)
        for (int i = 0; i < comps.num && i < 256; ++i) {
            uint32_t c = 0;
            if (!TrySafeReadUint32(reinterpret_cast<char*>(comps.data) + 4 * i, &c) || !c) continue;
            uint32_t cls = U32(reinterpret_cast<void*>(c), kOffClass);
            if (!kinClass && cls && vr::script::ObjectName(reinterpret_cast<void*>(cls)) == "YPawnKinematicRBCollider") kinClass = cls;
            if (cls && cls == kinClass) add(reinterpret_cast<void*>(U32(reinterpret_cast<void*>(c), kOffKinActorData)));
        }
    // Gli altri corpi della scena che appartengono a Walker (le capsule a molla
    // di YPawnSpringyRBCollider): userData -> RB_BodyInstance -> componente ->
    // Owner == Walker. Solo se l'elenco degli attori contiene un corpo noto.
    uint32_t pushBi = U32(pawn, kOffPawnPushBody);
    uint32_t biClass = pushBi ? U32(reinterpret_cast<void*>(pushBi), kOffClass) : 0;
    void* scene = n && biClass ? NxSceneOf(out[0]) : nullptr;
    struct {
        uint32_t beg, end, cap;
    } list = {};
    if (scene && SafeCopy(&list, static_cast<char*>(scene) + kOffNpSceneActors, sizeof(list)) && list.beg &&
        list.end > list.beg && list.cap >= list.end && (list.end - list.beg) % 4 == 0 && (list.end - list.beg) / 4 < 20000) {
        std::vector<uint32_t> acts((list.end - list.beg) / 4);
        const uint32_t known = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(out[0]));
        if (SafeCopy(acts.data(), reinterpret_cast<void*>(list.beg), acts.size() * 4) &&
            std::find(acts.begin(), acts.end(), known) != acts.end()) {
            const uint32_t me = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pawn));
            for (uint32_t a : acts) {
                uint32_t ud = a ? U32(reinterpret_cast<void*>(a), kOffNxActorUserData) : 0;
                if (!ud || U32(reinterpret_cast<void*>(ud), kOffClass) != biClass) continue;
                uint32_t oc = U32(reinterpret_cast<void*>(ud), kOffBodyOwnerComponent);
                if (oc && U32(reinterpret_cast<void*>(oc), kOffComponentOwner) == me) add(reinterpret_cast<void*>(a));
            }
        }
    }
    return n;
}

uint32_t BodiesSig(void* const b[], int n) {
    uint32_t s = 2166136261u;
    for (int i = 0; i < n; ++i) s = (s ^ static_cast<uint32_t>(reinterpret_cast<uintptr_t>(b[i]))) * 16777619u;
    return s ? s : 1u;
}

// Firma dei corpi di Walker attuali (0 = nessuno): cambia se il gioco li ricrea.
uint32_t WalkerSig(void* pawn) {
    void* b[kMaxWalkerBodies];
    int n = WalkerBodies(pawn, b);
    return n ? BodiesSig(b, n) : 0;
}

bool SameModule(const void* a, const void* b) {
    HMODULE ma = nullptr, mb = nullptr;
    const DWORD fl = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    return GetModuleHandleExA(fl, static_cast<LPCSTR>(a), &ma) && GetModuleHandleExA(fl, static_cast<LPCSTR>(b), &mb) && ma == mb;
}

// true se dopo la chiamata la coppia ha davvero i flag voluti (getActorPairFlags).
bool CallNxPairFlags(void* a, void* b, uint32_t flags) {
    __try {
        void** va = *static_cast<void***>(a);
        void** vb = *static_cast<void***>(b);
        if (va != vb || !SameModule(va, va[1])) return false;  // due NxActor della stessa classe
        void* sa = reinterpret_cast<NxGetScene>(va[1])(a);
        void* sb = reinterpret_cast<NxGetScene>(vb[1])(b);
        if (!sa || sa != sb) return false;  // scene diverse: nessuna collisione possibile
        void** vs = *static_cast<void***>(sa);
        if (!SameModule(va, vs[41]) || !SameModule(va, vs[42])) return false;
        reinterpret_cast<NxSetPairFlags>(vs[41])(sa, a, b, flags);
        return (reinterpret_cast<NxGetPairFlags>(vs[42])(sa, a, b) & kNxIgnorePair) == (flags & kNxIgnorePair);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Ignora (o di nuovo no) le coppie oggetto / corpi di Walker. Ritorna la firma
// dei corpi trattati (0 = nessuno). Con onlySig il ripristino avviene solo se
// i corpi sono ancora quelli (ricreati: le coppie vecchie non esistono piu').
uint32_t IgnoreWalker(void* pawn, void* comp, bool ignore, uint32_t onlySig = 0) {
    void* b[kMaxWalkerBodies];
    int n = WalkerBodies(pawn, b);
    // Tutti i corpi dell'oggetto (l'arma raccolta ne ha piu' d'uno; gli altri uno).
    void* on[kMaxCompBodies];
    const int no = CompNxAll(comp, on);
    if (!n || !no) return 0;
    uint32_t sig = BodiesSig(b, n);
    if (onlySig && sig != onlySig) return 0;
    int ok = 0;
    for (int j = 0; j < no; ++j)
        for (int i = 0; i < n; ++i) ok += CallNxPairFlags(on[j], b[i], ignore ? kNxIgnorePair : 0) ? 1 : 0;
    static int logged = 0;
    if (logged < 3 && (ignore || ok < n * no)) {
        ++logged;
        char t[220];
        std::snprintf(t, sizeof(t), "GRAB: Walker's %d physics bodies %s for %s (%d/%d pairs verified, %d bod%s of the object)", n,
                      ignore ? "ignored" : "back", vr::script::ObjectName(reinterpret_cast<void*>(U32(comp, 0x4C))).c_str(), ok, n * no, no,
                      no == 1 ? "y" : "ies");
        vr::log::Line(t);
    }
    return ok ? sig : 0;
}

void WriteFlags(void* actor, uint32_t a0, uint32_t f9c) {
    uint32_t cur9c = U32(actor, kOffActorFlags9C);
    // Solo i bit che cambiamo: il resto puo' essere cambiato dal gioco nel frattempo.
    cur9c = (cur9c & ~kIgnoreRbPawns) | (f9c & kIgnoreRbPawns);
    uint32_t curA0 = U32(actor, kOffActorFlagsA0);
    curA0 = (curA0 & ~kBlockActors) | (a0 & kBlockActors);
    SafeCopy(static_cast<char*>(actor) + kOffActorFlags9C, &cur9c, 4);
    SafeCopy(static_cast<char*>(actor) + kOffActorFlagsA0, &curA0, 4);
}

// Velocita' della mano negli ultimi ~60 ms (media, non l'ultimo campione:
// il rilascio del grip fa tremare il controller).
// Al piu' gli 8 campioni piu' recenti, come con la storia di 8 di prima (lo
// stesso numero per throwmode classic e per tutti gli altri usi).
bool HistVelocity(const HandHist& hh, double window, float* v) {
    const int n = (std::min)(hh.n, 8);
    if (n < 2) return false;
    int newest = HistIdx(hh, 0);
    int oldest = newest;
    for (int k = 1; k < n; ++k) {
        int i = HistIdx(hh, k);
        oldest = i;
        if (hh.t[newest] - hh.t[i] >= window) break;
    }
    double dt = hh.t[newest] - hh.t[oldest];
    if (dt < 1e-3) return false;
    for (int c = 0; c < 3; ++c) v[c] = static_cast<float>((hh.pos[newest][c] - hh.pos[oldest][c]) / dt);
    return true;
}
bool HandVelocity(int h, double window, float* v) { return HistVelocity(g_hist[h], window, v); }

// Velocita' di Walker (Actor.Velocity, +0x140; al piu' 20 m/s), per i limiti
// di velocita' degli oggetti tenuti rispetto a lui (il "pawn frame" della voce
// 2 del piano). La mano e' nel mondo: correndo, scivolando in copertura o
// scavalcando, il moto di Walker contava nel limite, e un barile (13,5 m/s)
// restava indietro fino a scivolare via; anche il lancio di corsa di un
// oggetto pesante era tagliato di piu'. Zero con "griptune pawnframe 0"
// (limiti nel mondo, come prima: per il confronto). Actor.Velocity e non lo
// spostamento della posizione: correndo nel deserto del livello 1 la
// posizione sale e scende a scatti sui gradini del terreno (fino a 14 m/s in
// verticale per qualche tick, mentre la telecamera, e con lei le mani, li
// smussa), Actor.Velocity resta liscia (24/10).
// Velocita' con cui Walker porta le mani (revisione 25/10, percorso del
// peso): quella del centro del riferimento delle mani negli ultimi 30 ms
// (g_frameHist, storia portata anche dai salti del riferimento), al piu'
// 20 m/s; se manca, Actor.Velocity. Actor.Velocity ha picchi di un tick e
// lascia fuori i mezzi in moto; il centro e' quello che le mani seguono
// davvero (gradini del terreno gia' smussati dalla telecamera). Zero con
// "griptune pawnframe 0", come PawnVelocity.
void PawnVelocity(float v[3]);
void FrameVelocity(float v[3]) {
    v[0] = v[1] = v[2] = 0.0f;
    if (g_tune.pawnFrame < 0.5f) return;
    if (g_tune.frameVel < 0.5f || !HistVelocity(g_frameHist, 0.03, v) || !std::isfinite(v[0] + v[1] + v[2])) {
        PawnVelocity(v);
        return;
    }
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), lim = 20.0f * vr::control::g_posScale;
    if (l > lim)
        for (int c = 0; c < 3; ++c) v[c] *= lim / l;
}

void PawnVelocity(float v[3]) {
    v[0] = v[1] = v[2] = 0.0f;
    if (g_tune.pawnFrame < 0.5f || !g_tickPawn || !SafeCopy(v, static_cast<char*>(g_tickPawn) + 0x140, 12)) return;
    if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) {
        v[0] = v[1] = v[2] = 0.0f;
        return;
    }
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), lim = 20.0f * vr::control::g_posScale;
    if (l > lim)
        for (int c = 0; c < 3; ++c) v[c] *= lim / l;
}

// Fattore 0..1 per la velocita' v data all'oggetto tenuto, con il limite cap
// rispetto a Walker: il k piu' grande con |k v - pv| <= cap (pv =
// PawnVelocity). Solo come allentamento del limite nel mondo di prima (k mai
// sotto cap / |v|) e lungo v: non aggiunge velocita' di lato. Actor.Velocity
// ha picchi di un tick (24/10: oltre 20 m/s all'inizio della corsa, poi
// 3,8): con "v - pv limitata, poi + pv" il picco avrebbe spinto per un tick
// un oggetto pesante verso pv, cioe' uno scatto.
float CapScale(const float* v, const float* pv, float cap) {
    const float a = Dot3(v, v);
    if (!(a > 1e-6f) || a <= cap * cap) return 1.0f;
    const float kOld = cap / std::sqrt(a);
    const float b = Dot3(v, pv), c = Dot3(pv, pv) - cap * cap, disc = b * b - a * c;
    float kNew = 0.0f;
    if (disc >= 0.0f) {
        const float r = std::sqrt(disc), k1 = (b - r) / a, k2 = (b + r) / a;
        if (k1 <= 1.0f) kNew = (std::min)(1.0f, k2);  // [k1, k2] ammessi; se k1 > 1 servirebbe accelerare: no
    }
    return (std::max)(kOld, kNew);
}

// Salto del tracciamento negli ultimi window secondi: uno spostamento oltre
// maxSpeed (unita'/s) su almeno 11 ms (un frame a 90 Hz). Una mano vera non
// arriva a 25 m/s nemmeno nei colpi piu' forti (~10-15 m/s); un controller
// perso e ritrovato salta di decine di cm in un frame. Non tra due tick
// qualsiasi: senza visore il gioco va a ~225 tick/s e la mano finta si
// aggiorna piu' di rado (a 12 m/s: fermo per qualche tick, poi 9,6 unita' in
// 4 ms, cioe' 48 m/s apparenti; 24/10).
// maxN: quanti campioni recenti guardare (8, come prima della storia di 16:
// ~35 ms a 225 tick/s, ~55 ms a 144 Hz; il lancio della voce 1 passa kHistN
// perche' window, 110 ms, sia davvero coperta quando la storia ci arriva).
bool HandJumped(int h, double window, float maxSpeed, int maxN = 8) {
    const HandHist& hh = g_hist[h];
    const int n = (std::min)(hh.n, (std::max)(2, (std::min)(maxN, kHistN)));
    int newest = HistIdx(hh, 0);
    for (int k = 0; k < n; ++k) {
        int i = HistIdx(hh, k);
        if (hh.t[newest] - hh.t[i] > window) break;
        for (int m = k + 1; m < n; ++m) {
            int j = HistIdx(hh, m);  // piu' vecchio di i
            double dt = hh.t[i] - hh.t[j];
            if (dt < 0.011) continue;
            float d2 = 0.0f;
            for (int c = 0; c < 3; ++c) d2 += (hh.pos[i][c] - hh.pos[j][c]) * (hh.pos[i][c] - hh.pos[j][c]);
            if (std::sqrt(d2) / dt > maxSpeed) return true;
            break;  // il primo campione ad almeno 11 ms basta
        }
    }
    return false;
}

// --- Muri invisibili (seguito della voce 2, 25/10) ---------------------------
// I BlockingVolume del livello (YBlockingVolume: confini invisibili per
// Walker; attorno alla cassa di bottiglie del livello 1 due pareti sottili,
// 18-30 cm, alte 1,9 m sopra la cassa, piu' un blocco sulla cassa) hanno
// BlockRigidBody acceso e fermano anche gli oggetti fisici. Una bottiglia presa
// sporgendosi oltre la parete e tirata verso di se' si fermava contro il nulla
// (con il peso: ferma alla parete, errore fino a 43 cm, poi scivolava via dalla
// mano; off la faceva passare col comando da 30 m/s). Era la bottiglia
// "incastrata" del collaudo smash: comando verso Walker, attraverso la
// parete. Con il peso, l'oggetto tenuto ignora i muri invisibili che tocca e
// che non gli fanno da pavimento (coppia ignorata in PhysX, come i corpi di
// Walker; HeldVolumes); lasciato, il muro torna solido appena l'oggetto ne e'
// uscito o ci sta sopra (VolumesTick), cosi' PhysX non lo espelle. griptune
// passvolumes 0: come prima.
struct VolRestore {
    void* actor;
    void* comp;
    void* nx;     // NxActor dell'oggetto al rilascio
    void* vol;    // BrushComponent del muro
    void* volNx;
    void* volActor;  // il BlockingVolume (VolumeAlive)
    double since, nextCheck;
};
std::vector<VolRestore> g_volRestore;

// Dentro lo scafo convesso del muro (BrushAggGeom +0x204, ConvexElems +0x24;
// FKConvexElem 0x64 byte: VertexData +0x00, FacePlaneData +0x3C, piani negli
// assi del componente), allargato di margin unita' (misurate nel mondo, con
// la scala del componente). Senza scafo leggibile: il box dei limiti. Il verso
// dei piani si prende dal baricentro dei vertici.
bool InsideVolume(void* comp, const float* p, float margin) {
    float M[4][4];
    const uint32_t elems = U32(comp, 0x204 + 0x24), nElems = U32(comp, 0x204 + 0x28);
    if (ReadMatrix(comp, M) && elems && nElems > 0 && nElems < 64) {
        const float R[3][3] = {{M[0][0], M[0][1], M[0][2]}, {M[1][0], M[1][1], M[1][2]}, {M[2][0], M[2][1], M[2][2]}};
        const float det = Det3(R);
        if (std::fabs(det) > 1e-6f) {
            // p = T + l R (righe) -> l = (p - T) R^-1
            float inv[3][3];
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) {
                    const int i1 = (i + 1) % 3, i2 = (i + 2) % 3, j1 = (j + 1) % 3, j2 = (j + 2) % 3;
                    inv[j][i] = (R[i1][j1] * R[i2][j2] - R[i1][j2] * R[i2][j1]) / det;
                }
            const float q[3] = {p[0] - M[3][0], p[1] - M[3][1], p[2] - M[3][2]};
            float l[3];
            for (int j = 0; j < 3; ++j) l[j] = q[0] * inv[0][j] + q[1] * inv[1][j] + q[2] * inv[2][j];
            bool any = false;
            for (uint32_t e = 0; e < nElems; ++e) {
                const uintptr_t el = elems + e * 0x64;
                const uint32_t vd = U32(reinterpret_cast<void*>(el), 0x00), vn = U32(reinterpret_cast<void*>(el), 0x04);
                const uint32_t pd = U32(reinterpret_cast<void*>(el), 0x3C), pn = U32(reinterpret_cast<void*>(el), 0x40);
                if (!vd || !pd || vn == 0 || vn > 256 || pn == 0 || pn > 256) continue;
                float c[3] = {0, 0, 0}, v[3];
                for (uint32_t i = 0; i < vn; ++i)
                    if (SafeCopy(v, reinterpret_cast<char*>(vd) + 12 * i, 12))
                        for (int k = 0; k < 3; ++k) c[k] += v[k] / static_cast<float>(vn);
                bool inside = true;
                for (uint32_t i = 0; i < pn && inside; ++i) {
                    float pl4[4];
                    if (!SafeCopy(pl4, reinterpret_cast<char*>(pd) + 16 * i, 16)) continue;
                    // Distanza nel mondo: la normale nel mondo e' R^-1 n (n.l = q.(R^-1 n)).
                    // Revisione 25/10: prima il margine si divideva per la scala media
                    // (cbrt det), sbagliato per un muro sottile scalato diverso sugli
                    // assi: lungo lo spessore il margine era molto piu' piccolo, e la
                    // bottiglia premuta contro la parete non risultava mai "dentro".
                    float m[3];
                    for (int k = 0; k < 3; ++k) m[k] = inv[k][0] * pl4[0] + inv[k][1] * pl4[1] + inv[k][2] * pl4[2];
                    const float ml = std::sqrt(Dot3(m, m));
                    if (!(ml > 1e-9f)) continue;
                    const float sgn = Dot3(pl4, c) - pl4[3] > 0.0f ? -1.0f : 1.0f;  // normale verso l'esterno
                    if (sgn * (Dot3(pl4, l) - pl4[3]) / ml > margin) inside = false;
                }
                any = true;
                if (inside) return true;
            }
            if (any) return false;
        }
    }
    Prop b;
    if (!ReadBounds(comp, &b)) return false;
    for (int i = 0; i < 3; ++i)
        if (p[i] < b.boxMin[i] - margin || p[i] > b.boxMax[i] + margin) return false;
    return true;
}

// Il muro c'e' ancora com'era (revisione 25/10, regola 7 del brief): attore
// non distrutto, ancora un BlockingVolume con quel componente di collisione,
// BlockRigidBody acceso e lo stesso NxActor. Kismet che lo spegne e riaccende,
// o il sottolivello che esce, liberano l'NxActor: una coppia con un NxActor
// morto non va piu' toccata (setActorPairFlags lavorerebbe su memoria libera).
bool VolumeAlive(void* actor, void* comp, void* volNx) {
    if (!actor || !comp) return false;
    uint32_t f9c = 0;
    if (!TrySafeReadUint32(static_cast<char*>(actor) + kOffActorFlags9C, &f9c) || (f9c & kDeleteMe)) return false;
    const uint32_t cls = U32(actor, kOffClass);
    if (!cls || ClassKind(cls) != 7) return false;
    if (U32(actor, kOffActorCollisionComp) != static_cast<uint32_t>(reinterpret_cast<uintptr_t>(comp))) return false;
    if (!(U32(comp, 0x14C) & 0x40)) return false;
    return !volNx || CompNx(comp) == volNx;
}

// Il muro fa da pavimento all'oggetto: il fondo del box dell'oggetto e' sopra
// la cima del box del muro (1 unita' di tolleranza). Lo regge, non lo blocca.
bool VolumeIsFloor(void* volComp, const float* bc) {
    Prop vb;
    return ReadBounds(volComp, &vb) && bc[2] - bc[5] >= vb.boxMax[2] - 1.0f;
}

// Ogni tick con l'oggetto in mano (peso acceso). Revisione 25/10: prima ogni
// muro entro il raggio dell'oggetto + 30 unita' diventava una coppia ignorata,
// anche il blocco su cui stanno le bottiglie della cassa (z 860-942, il piano
// della cassa): rimessa giu' li', una bottiglia ci affondava, e dopo 10 s il
// muro restava attraversabile per sempre. Ora un muro e' ignorato solo se
// l'oggetto lo tocca o ci sta dentro (sfera dei limiti + 4 unita' contro lo
// scafo: la bottiglia presa dal blocco, quella tirata contro la parete) e non
// gli fa da pavimento (VolumeIsFloor). Diventato pavimento in mano (bottiglia
// sollevata sopra il blocco), la coppia torna subito solida e resta tale fino
// al rilascio (volFloor): rimessa sulla cassa ci resta sopra, sbattuta contro
// la cassa si ferma sul piano.
void HeldVolumes(Held& held) {
    if (g_tune.passVolumes < 0.5f) return;
    void* nx = CompNx(held.comp);
    float bc[7];
    if (!nx || !SafeCopy(bc, static_cast<char*>(held.comp) + kOffCompBounds, sizeof(bc))) return;
    std::string meshName;
    auto meshStr = [&]() -> const char* {
        if (meshName.empty()) meshName = vr::script::ObjectName(reinterpret_cast<void*>(U32(held.comp, kOffSmcStaticMesh)));
        return meshName.c_str();
    };
    static int logged = 0;
    // Prima quelli gia' ignorati: muro sparito (si lascia la voce, nessuna
    // chiamata a PhysX) o diventato pavimento (di nuovo solido).
    for (int k = 0; k < held.nVolIgn;) {
        bool drop = false;
        if (!VolumeAlive(held.volActor[k], held.volIgn[k], held.volNx[k])) {
            drop = true;
        } else if (VolumeIsFloor(held.volIgn[k], bc)) {
            CallNxPairFlags(nx, held.volNx[k], 0);
            if (logged < 30 || vr::control::g_grabLog > 0) {
                ++logged;
                char b[220];
                std::snprintf(b, sizeof(b), "GRAB: held %s is above the invisible wall %s: solid again (a floor for it)", meshStr(),
                              vr::script::ObjectName(held.volActor[k]).c_str());
                vr::log::Line(b);
            }
            drop = true;
            if (held.nVolFloor < 8) held.volFloor[held.nVolFloor++] = held.volIgn[k];
        }
        if (drop) {
            --held.nVolIgn;
            held.volIgn[k] = held.volIgn[held.nVolIgn];
            held.volNx[k] = held.volNx[held.nVolIgn];
            held.volActor[k] = held.volActor[held.nVolIgn];
        } else {
            ++k;
        }
    }
    if (g_blockVols.empty()) return;
    const float r = bc[6] + 30.0f;
    for (const BlockVol& v : g_blockVols) {
        bool nearBox = true;
        for (int i = 0; i < 3 && nearBox; ++i) nearBox = bc[i] >= v.mn[i] - r && bc[i] <= v.mx[i] + r;
        if (!nearBox) continue;
        bool have = false;
        for (int k = 0; k < held.nVolIgn; ++k) have |= held.volIgn[k] == v.comp;
        for (int k = 0; k < held.nVolFloor; ++k) have |= held.volFloor[k] == v.comp;  // gia' pavimento: resta solido
        if (have || held.nVolIgn >= 8) continue;
        if (!VolumeAlive(v.actor, v.comp, nullptr) || VolumeIsFloor(v.comp, bc) || !InsideVolume(v.comp, bc, bc[6] + 4.0f)) continue;
        void* vnx = CompNx(v.comp);
        if (!vnx || !CallNxPairFlags(nx, vnx, kNxIgnorePair)) continue;
        held.volIgn[held.nVolIgn] = v.comp;
        held.volNx[held.nVolIgn] = vnx;
        held.volActor[held.nVolIgn] = v.actor;
        ++held.nVolIgn;
        if (logged < 30 || vr::control::g_grabLog > 0) {
            ++logged;
            char b[220];
            std::snprintf(b, sizeof(b), "GRAB: held %s passes the invisible wall %s (%s) while in the hand", meshStr(),
                          vr::script::ObjectName(v.actor).c_str(), vr::script::ClassName(v.actor).c_str());
            vr::log::Line(b);
        }
    }
}

// Dopo il rilascio: il muro torna solido quando l'oggetto (sfera dei limiti
// piu' 2 unita') non lo tocca piu', o quando ci sta sopra (pavimento); dentro,
// PhysX lo espellerebbe di scatto. Ripreso in mano con il peso: la voce passa
// a HeldVolumes; senza il peso resta qui (HeldVolumes non gira) e riprende al
// rilascio. Dopo 10 s ancora dentro: si ricontrolla una volta al secondo,
// non si lascia il muro attraversabile per sempre (revisione 25/10).
void VolumesTick(double now) {
    for (size_t i = 0; i < g_volRestore.size();) {
        VolRestore& r = g_volRestore[i];
        bool drop = !ActorUsable(r.actor) || CompNx(r.comp) != r.nx || !VolumeAlive(r.volActor, r.vol, r.volNx);
        int hand = -1;
        if (!drop) hand = g_held[0].comp == r.comp ? 0 : g_held[1].comp == r.comp ? 1 : -1;
        if (!drop && hand >= 0) {
            Held& hd = g_held[hand];
            if (!hd.weight) {
                ++i;  // tenuto senza il peso: resta in attesa del rilascio
                continue;
            }
            bool have = false;
            for (int k = 0; k < hd.nVolIgn; ++k) have |= hd.volIgn[k] == r.vol;
            if (!have && hd.nVolIgn < 8) {
                hd.volIgn[hd.nVolIgn] = r.vol;
                hd.volNx[hd.nVolIgn] = r.volNx;
                hd.volActor[hd.nVolIgn] = r.volActor;
                ++hd.nVolIgn;
            } else if (!have) {
                CallNxPairFlags(r.nx, r.volNx, 0);  // nessun posto: solido subito
            }
            drop = true;
        } else if (!drop && now >= r.nextCheck) {
            float bc[7];
            if (!SafeCopy(bc, static_cast<char*>(r.comp) + kOffCompBounds, sizeof(bc))) {
                drop = true;
            } else {
                const bool floor = VolumeIsFloor(r.vol, bc);
                if (floor || !InsideVolume(r.vol, bc, bc[6] + 2.0f)) {
                    CallNxPairFlags(r.nx, r.volNx, 0);
                    static int logged = 0;
                    if (logged < 30) {
                        ++logged;
                        char b[220];
                        std::snprintf(b, sizeof(b), "GRAB: %s solid again for the invisible wall %s, %.2f s after release (%s)",
                                      vr::script::ObjectName(reinterpret_cast<void*>(U32(r.comp, kOffSmcStaticMesh))).c_str(),
                                      vr::script::ObjectName(r.volActor).c_str(), now - r.since, floor ? "on top of it" : "out of it");
                        vr::log::Line(b);
                    }
                    drop = true;
                } else if (now > r.since + 10.0) {
                    r.nextCheck = now + 1.0;  // fermo dentro il muro: si ricontrolla piano
                }
            }
        }
        if (drop) g_volRestore.erase(g_volRestore.begin() + i);
        else ++i;
    }
}

float WeightAccel(const Held& held);       // piu' avanti: aMax di WeightOf (unita'/s^2)
float WeightThrowAccel(const Held& held);  // piu' avanti: aMax per il lancio (voce 6: la presa piu' stretta degli ultimi gripThrow ms)

// --- Lancio di picco (voce 4 del piano Boneworks, "throwmode peak") ---------
// Prima (classic) il lancio era la velocita' media della mano negli ultimi
// 60 ms: chi lancia frena prima di aprire la mano (e il grip che si apre fa
// tremare il controller), quindi la media restava sotto il picco (rampa a
// 4 m/s scesa a 2 al rilascio: ~3 m/s), e la rotazione era quella dell'ultimo
// comando della guida (correzione dell'errore compresa: di fatto a caso).
// Ora, come Half-Life: Alyx e GetEstimatedPeakVelocities di SteamVR: il
// campione piu' veloce degli ultimi ~110 ms con i suoi vicini
// (vr::pure::PeakThrow), la velocita' angolare del polso dagli assi della
// stessa storia (anche se la mano prima era vuota), la leva (il baricentro
// lontano dalla mano parte a v + w x r) e un limite di energia cinetica
// rispetto a Walker (85 J: barile da 33 kg al piu' 2,26 m/s, cassa da 52 kg
// 1,8; la bottiglia 10 m/s).
// Revisione 26/10:
// - il picco si cerca sulla mano rispetto a Walker: ogni campione meno il
//   centro del riferimento delle mani dello stesso tick (g_hist fc), poi al
//   rilascio si riaggiunge la velocita' di Walker di adesso (FrameVelocity).
//   Prima i campioni erano nel mondo, confrontati con Actor.Velocity di
//   adesso: fermandosi dopo una camminata a 3-4 m/s (Walker frena in
//   ~100-150 ms) e aprendo la mano subito, il picco era la camminata di prima
//   e l'oggetto volava avanti a mano quasi ferma; partendo, un campione da
//   fermo era il "picco" e l'oggetto restava fermo nel mondo. Actor.Velocity
//   ha anche picchi di un tick e lascia fuori i mezzi in moto. throwboost
//   moltiplica solo la parte della mano (lasciare un oggetto correndo non lo
//   spinge al 130% della corsa);
// - lancio o oggetto posato: sotto peakmin m/s di picco (campioni isolati
//   sbagliati gia' sostituiti dalla mediana dei loro 3, peakout) la media
//   al rilascio, come classic, e la rotazione del polso al rilascio; da
//   peakfull in su il picco; in mezzo un passaggio morbido (ThrowPeakBlend).
//   Prima ogni apertura del grip prendeva il picco: posando una bottiglia con
//   la mano che rallenta (0,6 -> 0,1 m/s) partiva a 0,6 x 1,3.
struct ThrowPeak {
    float v[3] = {0, 0, 0};      // velocita' del baricentro, leva compresa, senza throwboost (unita'/s): rispetto a Walker se rel, se no nel mondo
    float base[3] = {0, 0, 0};   // velocita' di Walker da riaggiungere dopo throwboost (FrameVelocity; 0 se non rel)
    float w[3] = {0, 0, 0};      // velocita' angolare data (rad/s): polso al picco, o al rilascio posando
    float lever[3] = {0, 0, 0};  // parte della leva, w x r (unita'/s)
    bool okW = false;
    bool rel = false;            // campioni rispetto al centro del riferimento delle mani
    float handPeak = 0.0f;       // picco (media 1:2:1 sui campioni ripuliti), rispetto a Walker (unita'/s): quello che conta
    float handRaw = 0.0f;        // il campione grezzo piu' veloce della finestra, rispetto a Walker (unita'/s)
    float handAvg = -1.0f;       // media al rilascio, rispetto a Walker (unita'/s; -1 senza)
    float blend = 1.0f;          // peso del picco: 1 lancio, 0 oggetto posato (media al rilascio)
    double age = 0.0;            // s tra il picco e il rilascio
    int nV = 0, nW = 0, rejected = 0, rejectedW = 0, samples = 0;
    int replaced = 0;            // campioni isolati sbagliati sostituiti dalla mediana dei loro 3
};
bool PeakThrowVel(int h, const Held& held, ThrowPeak* out) {
    const HandHist& hh = g_hist[h];
    const float s = vr::control::g_posScale;
    // Rispetto a Walker se ogni campione ha il centro del suo tick; con
    // "griptune pawnframe 0" (o senza centro: braccia spente) nel mondo,
    // rispetto ad Actor.Velocity, come prima.
    bool rel = g_tune.pawnFrame >= 0.5f;
    for (int k = 0; rel && k < hh.n; ++k) rel = hh.fok[HistIdx(hh, k)];
    vr::pure::ThrowSample smp[kHistN];
    for (int k = 0; k < hh.n; ++k) {
        const int i = HistIdx(hh, k);
        for (int c = 0; c < 3; ++c) smp[k].p[c] = rel ? hh.pos[i][c] - hh.fc[i][c] : hh.pos[i][c];
        std::memcpy(smp[k].ax, hh.ax[i], sizeof(smp[k].ax));
        smp[k].t = hh.pt[i];
    }
    float ref[3] = {0, 0, 0};
    if (!rel) PawnVelocity(ref);
    const vr::pure::PeakThrowOut o = vr::pure::PeakThrow(smp, hh.n, (std::max)(10.0f, g_tune.peakWin) * 0.001,
                                                         (std::max)(0.0f, g_tune.peakDt) * 0.001, (std::max)(1.0f, g_tune.peakJump) * s, ref,
                                                         (std::max)(1.0f, g_tune.spinJump), 0.06, (std::max)(0.0f, g_tune.peakOut) * s, 0.2f);
    out->rejected = o.rejected;
    out->rejectedW = o.rejectedW;
    out->samples = hh.n;
    out->replaced = o.replaced;
    out->rel = rel;
    if (vr::control::g_grabLog > 0) {
        // Diagnostica (grablog): eta' e velocita' di ogni campione rispetto a Walker (m/s; - senza, x scartato).
        char b[640];
        int at = std::snprintf(b, sizeof(b), "GRABTHROW: samples (ms ago: m/s, %s)", rel ? "hand minus frame" : "world minus Actor.Velocity");
        for (int k = 0; k < hh.n && at > 0 && at < static_cast<int>(sizeof(b)) - 24; ++k) {
            const double age = (hh.pt[HistIdx(hh, 0)] - hh.pt[HistIdx(hh, k)]) * 1000.0;
            if (o.speed[k] >= 0.0f) at += std::snprintf(b + at, sizeof(b) - at, " %.0f:%.2f", age, o.speed[k] / s);
            else at += std::snprintf(b + at, sizeof(b) - at, " %.0f:%s", age, o.speed[k] < -1.5f ? "x" : "-");
        }
        vr::log::Line(b);
    }
    if (!o.ok || !std::isfinite(o.v[0] + o.v[1] + o.v[2] + o.w[0] + o.w[1] + o.w[2])) return false;
    if (rel) FrameVelocity(out->base);
    // Lancio o oggetto posato: peso del picco dalla sua velocita' (campioni
    // ripuliti: un campione sbagliato da solo non fa un lancio).
    const float a = vr::pure::ThrowPeakBlend(o.peakSpeed / s, g_tune.peakMin, g_tune.peakFull);
    const bool avgOk = o.okAvg && std::isfinite(o.vAvg[0] + o.vAvg[1] + o.vAvg[2]);
    const float av = avgOk ? a : 1.0f;
    for (int c = 0; c < 3; ++c) out->v[c] = av * o.v[c] + (1.0f - av) * (avgOk ? o.vAvg[c] : 0.0f);
    out->blend = av;
    if (avgOk) {
        const float d[3] = {o.vAvg[0] - ref[0], o.vAvg[1] - ref[1], o.vAvg[2] - ref[2]};
        out->handAvg = std::sqrt(Dot3(d, d));
    }
    // Rotazione: al picco (media di 5) per un lancio, al rilascio posando;
    // se ne manca una, l'altra.
    const bool wAvgOk = o.okWAvg && std::isfinite(o.wAvg[0] + o.wAvg[1] + o.wAvg[2]);
    float aw = a;
    if (!o.okW) aw = 0.0f;
    else if (!wAvgOk) aw = 1.0f;
    out->okW = o.okW || wAvgOk;
    if (out->okW)
        for (int c = 0; c < 3; ++c) out->w[c] = aw * o.w[c] + (1.0f - aw) * o.wAvg[c];
    out->handPeak = o.peakSpeed;
    out->handRaw = o.rawSpeed;
    out->age = o.peakAge;
    out->nV = o.nV;
    out->nW = o.nW;
    if (out->okW && g_tune.lever > 0.5f && held.comp) {  // comp nullo: senza leva (granata della voce 8 senza mesh)
        // Leva: baricentro (centro dei limiti) dalla mano adesso; per la
        // parte del picco riportato negli assi della mano del picco (la presa
        // e' rigida: lo stesso punto della mano, ruotato come lei allora).
        float com[3];
        if (SafeCopy(com, static_cast<char*>(held.comp) + kOffCompBounds, 12) && std::isfinite(com[0] + com[1] + com[2])) {
            const int i0 = HistIdx(hh, 0), ip = HistIdx(hh, o.peak);
            const float r[3] = {com[0] - hh.pos[i0][0], com[1] - hh.pos[i0][1], com[2] - hh.pos[i0][2]};
            if (Dot3(r, r) < 1.5f * s * 1.5f * s) {  // oltre 1,5 m non e' una presa (oggetto rimasto indietro)
                const float rl[3] = {Dot3(r, hh.ax[i0][0]), Dot3(r, hh.ax[i0][1]), Dot3(r, hh.ax[i0][2])};
                float rp[3], lp[3] = {0, 0, 0}, la[3] = {0, 0, 0};
                for (int c = 0; c < 3; ++c) rp[c] = rl[0] * hh.ax[ip][0][c] + rl[1] * hh.ax[ip][1][c] + rl[2] * hh.ax[ip][2][c];
                if (aw > 0.0f) Cross3(o.w, rp, lp);
                if (aw < 1.0f) Cross3(o.wAvg, r, la);
                for (int c = 0; c < 3; ++c) {
                    out->lever[c] = aw * lp[c] + (1.0f - aw) * la[c];
                    out->v[c] += out->lever[c];
                }
            }
        }
    }
    return true;
}
// Rotazione del lancio con il peso (piu' avanti, dopo WeightOf): la variazione
// dw dalla rotazione che l'oggetto ha gia' e' limitata dalla coppia della mano
// in T s, mai sotto alphaMin x T (come la guida).
void ThrowSpinLimit(const Held& held, float dw[3], float T);

// Lancio che ferisce (voce 1 del piano dei combattimenti, 26/10): per ogni
// lancio la velocita' affidabile del rilascio, per il filtro dei salti del
// tracciamento al colpo (ThrowDamage). trusted e' rispetto a Walker
// (unita'/s): con peak la velocita' data all'oggetto (campioni gia'
// ripuliti: salti oltre peakjump scartati, isolati sostituiti dalla mediana),
// con classic (media di 60 ms, salti compresi) al piu' la stima ripulita della
// stessa storia (PeakThrowVel: picco o media al rilascio, x throwboost, leva,
// +10% e 0,5 m/s). suspect: un salto del tracciamento che ha davvero toccato
// il lancio: con classic la media con il salto oltre la stima ripulita (un
// campione scartato e trusted < raw); con peak mai per i campioni scartati,
// che la velocita' data non contiene gia' (revisione della voce 1, 26/10:
// prima bastava un campione oltre peakjump, 12 m/s, e i lanci piu' forti veri,
// 10-15 m/s, non atterravano piu' nessuno). In tutti e due i modi uno
// spostamento oltre 25 m/s su 11 ms tra i 16 campioni della storia (al piu'
// 110 ms): una mano vera non ci arriva. Un lancio suspect non atterra nessuno
// (al piu' il livello leggero).
// tw: la velocita' affidabile nel mondo (Walker al rilascio + trusted lungo il
// lancio): al colpo si confronta con quella del nemico (ThrowDamage), non con
// Walker (revisione: un lancio di corsa era tagliato alla sola velocita'
// rispetto a Walker).
struct ThrowRecord {
    void* comp = nullptr;
    double at = 0.0;
    float trusted = 0.0f;  // unita'/s, rispetto a Walker
    float raw = 0.0f;      // velocita' data all'oggetto, rispetto a Walker (unita'/s)
    float tw[3] = {0, 0, 0};  // velocita' affidabile nel mondo (unita'/s)
    float z = 0.0f;        // quota del centro al rilascio
    bool suspect = false;
    int rejected = 0;      // campioni scartati come salti nella storia della mano
};
std::vector<ThrowRecord> g_throwRec;

void ThrowRecordAdd(int h, const Held& held, const float* v, const float* pv, float boost, const ThrowPeak* tp) {
    const float s = vr::control::g_posScale;
    ThrowRecord r;
    r.comp = held.comp;
    r.at = ProfMs() * 0.001;
    const float d[3] = {v[0] - pv[0], v[1] - pv[1], v[2] - pv[2]};
    r.raw = std::sqrt(Dot3(d, d));
    r.trusted = r.raw;
    float c[3];
    if (SafeCopy(c, static_cast<char*>(held.comp) + kOffCompBounds, 12) && std::isfinite(c[2])) r.z = c[2];
    ThrowPeak q;
    const bool ok = tp ? true : PeakThrowVel(h, held, &q);
    if (tp) q = *tp;
    if (ok) {
        r.rejected = q.rejected;
        if (!tp) {
            // classic: la media di 60 ms prende anche i salti; al piu' la stima ripulita.
            const float est = (std::max)(q.handPeak, q.handAvg) * boost + std::sqrt(Dot3(q.lever, q.lever));
            r.trusted = (std::min)(r.raw, est * 1.1f + 0.5f * s);
            r.suspect = q.rejected > 0 && r.trusted < r.raw;
        }
    }
    if (HandJumped(h, 0.11, 25.0f * s, kHistN)) r.suspect = true;
    const float k = r.raw > 1e-3f ? r.trusted / r.raw : 0.0f;
    for (int i = 0; i < 3; ++i) r.tw[i] = pv[i] + d[i] * k;
    for (ThrowRecord& o : g_throwRec)
        if (o.comp == r.comp) {
            o = r;
            return;
        }
    if (g_throwRec.size() >= 16) g_throwRec.erase(g_throwRec.begin());
    g_throwRec.push_back(r);
}

void Release(int h, const char* why) {
    Held& held = g_held[h];
    if (!held.actor) return;
    float v[3] = {0, 0, 0}, speed = 0.0f;
    bool alive = ActorUsable(held.actor);
    const bool grip = alive && !held.constrained && std::strcmp(why, "grip") == 0;
    // throwmode peak (predefinito): picco, polso, leva, energia; classic: la
    // media della mano di 60 ms di prima, identica. Senza storia utile (meno
    // di 2 campioni validi) anche peak usa la media.
    const bool peakMode = vr::control::g_throwMode.load() != 0;
    ThrowPeak tp;
    const bool peakOk = grip && peakMode && PeakThrowVel(h, held, &tp);
    if (peakOk) std::memcpy(v, tp.v, sizeof(v));
    if (grip && (peakOk || HandVelocity(h, 0.06, v))) {
        // Lancio: la velocita' della mano (classic: media; peak: picco e
        // leva), moltiplicata da throwboost. Il limite segue maxSpeed, che ora
        // dipende davvero dalla massa (Grab), e vale rispetto a Walker:
        // lanciando di corsa la sua velocita' non lo consuma.
        // peak con la storia rispetto a Walker (revisione 26/10): throwboost
        // sulla sola parte della mano, poi la velocita' di Walker di adesso
        // (FrameVelocity, la stessa dei limiti qui sotto); classic e peak nel
        // mondo come prima (Actor.Velocity per i limiti).
        float boost = vr::control::g_throwBoost;
        float pv[3];
        const bool relThrow = peakOk && tp.rel;
        if (relThrow) std::memcpy(pv, tp.base, sizeof(pv));
        else PawnVelocity(pv);
        for (float& c : v) c *= boost;
        if (relThrow)
            for (int c = 0; c < 3; ++c) v[c] += tp.base[c];
        if (held.weight && held.hasCmd) {
            // Peso (revisione 25/10): l'oggetto parte dalla velocita' che aveva
            // (l'ultimo comando, eseguito da PhysX) piu' quanto la mano puo'
            // dargli con la sua forza nei 60 ms della media della mano. Prima
            // prendeva di colpo la velocita' della mano: la cassa da 52 kg che
            // la seguiva piano saltava avanti al rilascio. Bottiglia e radio
            // quasi non cambiano (light: 235 e 50 m/s^2 -> 14 e 3 m/s di
            // margine sulla velocita' che hanno gia'). Il comando del peso, non
            // quello del distacco: lasciato mentre lo si strappa via, un oggetto
            // incastrato non parte con la forza accumulata (seguito 25/10).
            // Forza della presa (voce 6): quella piu' stretta degli ultimi
            // gripThrow ms, non quella della mano che si sta aprendo per lanciare.
            const float lim = WeightThrowAccel(held) * 0.06f;
            const float* c0 = held.weightCmdV;
            float d[3] = {v[0] - c0[0], v[1] - c0[1], v[2] - c0[2]};
            const float dl = std::sqrt(Dot3(d, d));
            if (dl > lim)
                for (float& c : d) c *= lim / dl;
            for (int c = 0; c < 3; ++c) v[c] = c0[c] + d[c];
            if (vr::control::g_analogGrip.load()) {
                const float sc = vr::control::g_posScale, limNow = WeightAccel(held) * 0.06f;
                char b[300];
                std::snprintf(b, sizeof(b),
                              "GRAB: throw grip: f %.2f now, %.2f best of the last %.0f ms; hand budget %.2f m/s (%.2f with the grip of now) "
                              "over the object's %.2f m/s, wanted +%.2f m/s%s",
                              held.gripF, (std::max)(held.gripF, held.gripThrowF), g_tune.gripThrow, lim / sc, limNow / sc,
                              std::sqrt(Dot3(c0, c0)) / sc, dl / sc, dl > lim ? " (limited)" : "");
                vr::log::Line(b);
            }
        }
        // peak: anche il limite di energia (rispetto a Walker, come maxSpeed):
        // gli oggetti pesanti partono piano, in pallonetto.
        const float kg = MassKg(held);
        const float eCap = vr::pure::ThrowEnergyCap(kg, g_tune.throwEnergy) * vr::control::g_posScale;
        const float cap = peakMode ? (std::min)(held.maxSpeed * 1.5f, eCap) : held.maxSpeed * 1.5f;
        const float unc = std::sqrt(Dot3(v, v));
        const float k = CapScale(v, pv, cap);
        for (float& c : v) c *= k;
        speed = std::sqrt(Dot3(v, v));
        SetLinVel(held.comp, v);
        // peak: la rotazione del polso al picco (classic: resta l'ultimo
        // comando della guida, come prima). Con il peso parte da quella che
        // l'oggetto ha (l'ultimo comando del peso) piu' quanto la coppia della
        // mano puo' darle negli stessi 60 ms della parte lineare. Al piu'
        // spinmax (55 rad/s).
        float wThrow[3] = {0, 0, 0};
        const bool spin = peakOk && tp.okW;
        if (spin) {
            std::memcpy(wThrow, tp.w, sizeof(wThrow));
            if (held.weight && held.hasCmd) {
                float dw[3] = {wThrow[0] - held.weightCmdW[0], wThrow[1] - held.weightCmdW[1], wThrow[2] - held.weightCmdW[2]};
                ThrowSpinLimit(held, dw, 0.06f);
                for (int c = 0; c < 3; ++c) wThrow[c] = held.weightCmdW[c] + dw[c];
            }
            const float wl = std::sqrt(Dot3(wThrow, wThrow)), wMax = (std::max)(0.0f, (std::min)(55.0f, g_tune.spinMax));
            if (wl > wMax)
                for (float& c : wThrow) c *= wMax / wl;
            SetAngVel(held.comp, wThrow);
        }
        Wake(held.comp);
        if (peakMode) {
            const float sc = vr::control::g_posScale;
            char b[640];
            if (peakOk)
                std::snprintf(b, sizeof(b),
                              "GRAB: throw peak: hand %.2f m/s %.0f ms before release (fastest raw sample %.2f m/s, %d replaced by their median; %d+%d samples of %d, %d+%d rejected), "
                              "at release %.2f m/s, peak weight %.2f, %s %.2f m/s, lever %.2f m/s, "
                              "object %.2f m/s before the caps, cap %.2f m/s (energy %.2f m/s at %.1f kg), spin %.1f rad/s (wrist %.1f)%s, v (%.0f, %.0f, %.0f) w (%.1f, %.1f, %.1f)",
                              tp.handPeak / sc, tp.age * 1000.0, tp.handRaw / sc, tp.replaced, tp.nV, tp.nW, tp.samples, tp.rejected, tp.rejectedW,
                              tp.handAvg / sc, tp.blend, tp.rel ? "relative to Walker, frame" : "in the world, Actor.Velocity",
                              std::sqrt(Dot3(pv, pv)) / sc, std::sqrt(Dot3(tp.lever, tp.lever)) / sc,
                              unc / sc, cap / sc, eCap / sc, kg, std::sqrt(Dot3(wThrow, wThrow)), std::sqrt(Dot3(tp.w, tp.w)),
                              spin ? "" : " (spin left to the drive)", v[0], v[1], v[2], wThrow[0], wThrow[1], wThrow[2]);
            else
                std::snprintf(b, sizeof(b),
                              "GRAB: throw peak: no usable hand history (%d samples, %d rejected), 60 ms average instead; cap %.2f m/s (energy %.2f m/s at %.1f kg)",
                              tp.samples, tp.rejected, cap / sc, eCap / sc, kg);
            vr::log::Line(b);
        }
        ThrowRecordAdd(h, held, v, pv, boost, peakOk ? &tp : nullptr);
        g_throwCheck = ThrowCheck();
        g_throwCheck.actor = held.actor;
        g_throwCheck.comp = held.comp;
        g_throwCheck.ticks = 3;
        g_throwCheck.wanted = speed;
        std::memcpy(g_throwCheck.wantV, v, sizeof(v));
        std::memcpy(g_throwCheck.wantW, wThrow, sizeof(wThrow));
        g_throwCheck.hasW = spin;
        SafeCopy(g_throwCheck.from, static_cast<char*>(held.comp) + kOffCompBounds, 12);
        g_throwCheck.at = ProfMs() * 0.001;
    }
    if (alive) {
        // Muri invisibili attraversati in mano: di nuovo solidi quando ne e'
        // uscito (VolumesTick). Solo i muri ancora vivi (VolumeAlive); al piu'
        // 64 voci in attesa (oltre: solido subito).
        void* onx = CompNx(held.comp);
        for (int k = 0; k < held.nVolIgn; ++k) {
            if (!onx || !VolumeAlive(held.volActor[k], held.volIgn[k], held.volNx[k])) continue;
            if (g_volRestore.size() >= 64) {
                CallNxPairFlags(onx, held.volNx[k], 0);
                continue;
            }
            g_volRestore.push_back({held.actor, held.comp, onx, held.volIgn[k], held.volNx[k], held.volActor[k], ProfMs() * 0.001, 0.0});
        }
        // Le sfere delle mani lo lasciano uscire prima di tornare solide
        // (ProxyTick). Con relgrace 0 niente, nemmeno il tick del rilascio.
        const double t = ProfMs() * 0.001;
        if (g_tune.relGrace > 0.0f)
            for (int s = 0; s < 2; ++s) g_relGrace[s][h] = {held.comp, t, t + g_tune.relGrace, false};
        g_restore.push_back({held.actor, ProfMs() * 0.001 + 0.5, held.savedA0, held.saved9C, held.walkerSig, false, ProfMs() * 0.001});
        // Lancio o caduta, per la pipeline dei colpi (stessa soglia dei colpi in mano).
        FallingAdd({held.actor, held.comp, ProfMs() * 0.001 + 4.0},
                   speed >= 1.5f * vr::control::g_posScale ? HitSource::kThrown : HitSource::kDropped, h);
    }
    std::memset(g_heldHandW[h], 0, sizeof(g_heldHandW[h]));  // la prossima presa riparte da polso fermo
    char b[240];
    float hvel[3] = {0, 0, 0};
    HandVelocity(h, 0.06, hvel);
    std::snprintf(b, sizeof(b), "GRAB: %s hand released %s (%s), %.1f m/s (hand %.2f m/s)", h ? "left" : "right",
                  vr::script::ObjectName(held.actor).c_str(), why, speed / vr::control::g_posScale,
                  std::sqrt(Dot3(hvel, hvel)) / vr::control::g_posScale);
    vr::log::Line(b);
    held = Held();
    vr::control::g_propReleasedMs[h] = GetTickCount64();
    vr::control::g_propHeld[h] = false;
}

// Limiti locali della StaticMesh (UStaticMesh+0x94: centro, estensione, raggio;
// letti in memoria il 24/10: bottle_round01 3,25 x 3,25 x 12,35 centrata a z 12,35).
bool MeshLocalBounds(void* comp, float center[3], float ext[3], float* sphere = nullptr) {
    uint32_t mesh = U32(comp, 0x214);  // StaticMeshComponent.StaticMesh
    float b[7];
    if (!mesh || !SafeCopy(b, reinterpret_cast<char*>(mesh) + 0x94, sizeof(b))) return false;
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(b[i]) || !std::isfinite(b[3 + i]) || b[3 + i] < 0.0f || b[3 + i] > 5000.0f) return false;
        center[i] = b[i];
        ext[i] = b[3 + i];
    }
    if (sphere) *sphere = b[6];
    return std::isfinite(b[6]) && b[6] > 0.0f && std::fabs(b[6] - std::sqrt(ext[0] * ext[0] + ext[1] * ext[1] + ext[2] * ext[2])) < 0.2f * b[6] + 1.0f;
}

// Presa canonica (utente 24/10: "se fai ruotare durante la presa l'oggetto in
// modo che arrivi alla mano sempre nella stessa posa e' tutto piu' facile").
// Prima (PowerGrip) l'asse lungo ruotava il meno possibile sull'asse delle
// dita e restavano il rollio, il verso e il punto toccato della presa: la
// bottiglia finiva tenuta per il fondo, con uno spigolo nel palmo e il pollice
// che la attraversava. Ora la posa dipende solo dalla mesh (CanonicalPose).
// Ritorna 0 (presa sul palmo), 1 (di forza) o 2 (a pinza).
int CanonicalGrip(int h, void* comp, const float* hp, const float ha[3][3], const float* op, const float oa[3][3], float relPos[3],
                  float relAxes[3][3], float anchor[3], float* halfThick) {
    if (!g_grip[h].valid) return 0;
    float lc[3], le[3], m[4][4];
    if (!MeshLocalBounds(comp, lc, le) || !ReadMatrix(comp, m)) return 0;
    float lcs[3], e[3], R0[3][3], c0[3], d[3] = {op[0] - hp[0], op[1] - hp[1], op[2] - hp[2]};
    for (int i = 0; i < 3; ++i) {
        float s = std::sqrt(m[i][0] * m[i][0] + m[i][1] * m[i][1] + m[i][2] * m[i][2]);
        lcs[i] = lc[i] * s;
        e[i] = le[i] * s;
        for (int r = 0; r < 3; ++r) R0[i][r] = Dot3(oa[i], ha[r]);  // assi alla presa, negli assi della mano
    }
    for (int r = 0; r < 3; ++r) c0[r] = Dot3(d, ha[r]) + lcs[0] * R0[0][r] + lcs[1] * R0[1][r] + lcs[2] * R0[2][r];
    float nc[3];
    int kind = CanonicalPose(g_grip[h], lcs, e, R0, c0, 0.01f * vr::control::g_posScale, g_tune, relAxes, nc, anchor, halfThick);
    if (!kind) return 0;
    for (int r = 0; r < 3; ++r) relPos[r] = nc[r] - (lcs[0] * relAxes[0][r] + lcs[1] * relAxes[1][r] + lcs[2] * relAxes[2][r]);
    float tr = Dot3(relAxes[0], R0[0]) + Dot3(relAxes[1], R0[1]) + Dot3(relAxes[2], R0[2]);
    const GripFrame& g = g_grip[h];
    float rel[3] = {nc[0] - g.o[0], nc[1] - g.o[1], nc[2] - g.o[2]}, k = 100.0f / vr::control::g_posScale;
    char b[260];
    std::snprintf(b, sizeof(b),
                  "GRIP: %s, half extents %.1f %.1f %.1f cm, centre f %.1f p %.1f a %.1f cm from the knuckles, turned %.0f deg",
                  kind == 1 ? "power" : "pinch", e[0] * k, e[1] * k, e[2] * k, Dot3(rel, g.f) * k, Dot3(rel, g.p) * k, Dot3(rel, g.a) * k,
                  std::acos((std::max)(-1.0f, (std::min)(1.0f, (tr - 1.0f) * 0.5f))) * 57.2958f);
    vr::log::Line(b);
    return kind;
}

// --- Peso (voce 2 del piano "Boneworks", 25/10) ------------------------------
// Il peso si sente come ritardo, accelerazione e rotazione piu' lente,
// abbassamento e piega, mai come un rifiuto (regola dell'utente: in Spec Ops la
// destra tiene sempre l'arma, con una sola mano libera tutto si deve poter
// sollevare). La mano ha una forza massima (g_tune.force, gravita' compresa)
// e una coppia massima; light le raddoppia e dimezza abbassamento e piega.
struct WeightLimits {
    float aMax = 0.0f;      // unita'/s^2: variazione massima della velocita' per secondo (forza / massa)
    float tauMax = 0.0f;    // N m
    float sag = 0.0f;       // unita': abbassamento della posa voluta
    float kRot = 1.0f;      // N m/rad
    float droopCap = 0.0f;  // rad
    float g = 750.0f;       // unita'/s^2: gravita' del gioco (modulo)
    float alphaMin = 3.0f;  // rad/s^2: pavimento dell'accelerazione angolare (coppia mai un rifiuto)
    float forceN = 0.0f;    // N: forza della mano in uso (voce 6: per la presa; log)
    float turnGain = 1.0f;  // voce 6: frazione del guadagno della rotazione verso la posa voluta (presa lenta < 1)
    float hang = 1.0f;      // voce 6: piega x tanto (presa lenta > 1)
};
float GravityZ(void* pawn);  // piu' avanti, coi guanti

// Presa lenta (voce 6): 0 = stretta (gripF 1), 1 = la piu' lenta (gripF = gripMin).
float GripLoose(float gripF) {
    const float mn = (std::max)(0.05f, (std::min)(1.0f, g_tune.gripMin));
    if (mn > 0.999f) return 0.0f;
    return (std::max)(0.0f, (std::min)(1.0f, (1.0f - gripF) / (1.0f - mn)));
}

// Limiti del peso con la forza della presa gripF (voce 6; 1 = stretta).
WeightLimits WeightOf(const Held& held, float gripF) {
    WeightLimits w;
    const float s = vr::control::g_posScale;
    const float kg = (std::max)(0.01f, MassKg(held));
    const float mul = held.weight == 1 ? 2.0f : 1.0f;  // light
    w.g = g_tickPawn ? -GravityZ(g_tickPawn) : 750.0f;
    // Pavimento: al netto della gravita' sale sempre di almeno liftMin (3 m/s^2,
    // 6 con light), anche con masskg 500 (il barile a 415 kg): lento, mai
    // fermo. Con 200 N (light 400) ci arrivano gli oggetti oltre ~11 kg
    // (light: ~19 kg): barile e cassa salgono a 3 m/s^2 netti (light 6).
    // Forza della presa (voce 6, analoggrip): la forza della mano x gripF
    // (0,4-1). Il pavimento resta quello della presa stretta (liftMin, mai
    // scalato: l'oggetto piu' pesante sale con la presa lenta come con quella
    // stretta), e la presa lenta non scende sotto gripFloor x g (1,3 g, il
    // valore del piano per il barile da 60 kg) se la presa stretta ci arriva.
    // Con gripF 1 e' esattamente la formula di prima.
    const float gf = (std::max)(0.05f, (std::min)(1.0f, gripF));
    const float loose = GripLoose(gf);
    const float term = mul * (std::max)(1.0f, g_tune.force) / kg * s;
    const float lift = w.g + mul * (std::max)(0.5f, g_tune.liftMin) * s;
    const float firm = (std::max)(term, lift);
    w.aMax = (std::max)((std::max)(gf * term, lift), (std::min)((std::max)(1.0f, g_tune.gripFloor) * w.g, firm));
    w.forceN = gf * mul * (std::max)(1.0f, g_tune.force);
    w.tauMax = gf * mul * (std::max)(0.1f, g_tune.torque);
    const float sagM = (std::min)(kg * 9.81f / ((std::max)(1.0f, g_tune.kLin) * mul), (std::max)(0.0f, g_tune.sagCap) * 0.01f / mul);
    w.sag = sagM * s;
    // Presa lenta: l'oggetto tenuto fuori dal baricentro pende di piu' (la
    // presa cede alla rotazione: rigidezza / hang), mai oltre la piega massima
    // di prima (droopCap). Con la piega massima x hang (prima prova, 26/10) la
    // cassa da 75 kg con full pendeva di 40 gradi, puntava nel terreno, saliva
    // contro il bersaglio a 60-80 cm e scivolava via dalla mano (g_case60_full):
    // un rifiuto.
    w.hang = 1.0f + ((std::max)(1.0f, g_tune.gripDroop) - 1.0f) * loose;
    w.turnGain = 1.0f - (1.0f - (std::max)(0.05f, (std::min)(1.0f, g_tune.gripTurn))) * loose;
    w.kRot = (std::max)(0.1f, g_tune.kRot) * mul / w.hang;
    w.droopCap = (std::max)(0.0f, g_tune.droopCap) / mul * 0.0174533f;
    // Pavimento della rotazione (revisione 25/10): coppia / inerzia cala col
    // quadrato delle misure e con la massa (cassa da 52 kg a full: 1,6 rad/s^2;
    // con masskg 500 frazioni di rad/s^2: girarla era di fatto rifiutato).
    w.alphaMin = mul * (std::max)(0.1f, g_tune.alphaMin);
    return w;
}
// La guida, la posa voluta e le dita: la forza della presa di adesso.
WeightLimits WeightOf(const Held& held) { return WeightOf(held, held.gripF); }
float WeightAccel(const Held& held) { return WeightOf(held).aMax; }
// Il lancio (Release, ThrowSpinLimit): la presa piu' stretta degli ultimi
// gripThrow ms (revisione 26/10). Prima la presa di adesso: l'attesa di
// gripHold proteggeva solo una mano che si apre in meno di 100 ms; aperta in
// 150-250 ms (normale lanciando) la forza era gia' scesa e il lancio della
// radio perdeva fino a un quarto della velocita'.
WeightLimits WeightOfThrow(const Held& held) { return WeightOf(held, (std::max)(held.gripF, held.gripThrowF)); }
float WeightThrowAccel(const Held& held) { return WeightOfThrow(held).aMax; }

// --- Forza della presa (voce 6 del piano "Boneworks", 26/10) ----------------
// Valore del grip della mano h (0 destra, 1 sinistra come g_gripRaw): quello
// finto di "synthgrip" (con il suo rumore), l'analogico del controller col
// grip premuto (GripAxis in vr_input.cpp: un grip solo a tasto da' 1), oppure
// 1 (presa finta di grabsim, grabtest: stretta).
float GripValueNow(int h) {
    const float sv = vr::control::g_synthGrip.load();
    if (sv >= 0.0f) {
        float v = sv;
        const float n = vr::control::g_synthGripNoise.load();
        if (n > 0.0f) {
            static uint32_t x = 2463534242u;  // xorshift: rumore uniforme in -n..n, solo collaudi
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            v += n * (static_cast<float>(x & 0xFFFFu) / 32767.5f - 1.0f);
        }
        return (std::max)(0.0f, (std::min)(1.0f, v));
    }
    if (h < 0 || h > 1) return 1.0f;
    return vr::pure::GripHeldValue(vr::control::g_gripRaw[h].load(), vr::control::g_gripAnalog[h].load());
}

// "synthgrip <v> [rumore] [dopo ms] [rampa ms]" (collaudi): il valore in
// attesa vale da "dopo ms", e con la rampa ci arriva in linea retta dal valore
// finto di prima in tanti ms (la mano che si apre piano lanciando). A ogni
// tick di props, con o senza oggetto in mano (revisione 26/10: prima solo
// dentro GripStep, cioe' solo tenendo qualcosa, e la riga poteva arrivare
// tardi o mai). Un "synthgrip" nuovo ferma la rampa (g_synthGripGen). Stesso
// orologio di control.cpp (QueryPerformanceCounter in ms, come ProfMs).
struct SynthGripRamp {
    bool on = false;
    float from = 0.0f, to = 0.0f;
    double t0 = 0.0, ms = 0.0;
    int gen = 0;
} g_synthGripRamp;

void SynthGripTick() {
    const double now = ProfMs();
    unsigned long long at = vr::control::g_synthGripAt.load();
    if (at && now >= static_cast<double>(at) && vr::control::g_synthGripAt.compare_exchange_strong(at, 0)) {
        const float to = vr::control::g_synthGripNext.load(), from = vr::control::g_synthGrip.load();
        const float ramp = vr::control::g_synthGripNextRamp.load();
        vr::control::g_synthGripNoise = vr::control::g_synthGripNextNoise.load();
        g_synthGripRamp = SynthGripRamp();
        if (ramp > 0.0f && from >= 0.0f && to >= 0.0f) {
            g_synthGripRamp.on = true;
            g_synthGripRamp.from = from;
            g_synthGripRamp.to = to;
            g_synthGripRamp.t0 = now;
            g_synthGripRamp.ms = ramp;
            g_synthGripRamp.gen = vr::control::g_synthGripGen.load();
        } else {
            vr::control::g_synthGrip = to;
        }
        char b[120];
        std::snprintf(b, sizeof(b), "GRAB: synthgrip %.2f now%s", to, g_synthGripRamp.on ? " (ramp start)" : "");
        vr::log::Line(b);
    }
    if (!g_synthGripRamp.on) return;
    if (g_synthGripRamp.gen != vr::control::g_synthGripGen.load()) {
        g_synthGripRamp.on = false;  // comando nuovo: vale quello
        return;
    }
    const double k = (now - g_synthGripRamp.t0) / g_synthGripRamp.ms;
    if (k >= 1.0) {
        vr::control::g_synthGrip = g_synthGripRamp.to;
        g_synthGripRamp.on = false;
        char b[96];
        std::snprintf(b, sizeof(b), "GRAB: synthgrip ramp done at %.2f", g_synthGripRamp.to);
        vr::log::Line(b);
    } else {
        vr::control::g_synthGrip = g_synthGripRamp.from + (g_synthGripRamp.to - g_synthGripRamp.from) * static_cast<float>(k);
    }
}

// Frazione di forza e coppia per un valore del grip: gripMin a gripLo (0,6,
// la soglia della presa), 1 a gripHi (0,9), in linea retta (il piano: 40% a
// 0,6 e 100% a 1,0; il 100% gia' a 0,9 dalla revisione 26/10).
float GripFactor(float v) { return vr::pure::GripFactorOf(v, g_tune.gripMin, g_tune.gripLo, g_tune.gripHi); }

// Un tick della forza della presa (Drive, prima della posa voluta: la piega
// dipende da gripF). Solo con "analoggrip on" e il peso (light/full); se no 1
// esatto, e la guida e' quella di prima. Filtro (vr::pure::GripFilterStep):
// stringere ci arriva con la costante gripRise (25 ms), allentare dopo
// gripHold ms dall'ultimo massimo (100) e poi con la costante gripEase (80
// ms): forza, piega e guadagno cambiano senza scalini nei due versi, e il
// tremolio del grip non entra. Il lancio usa la presa piu' stretta degli
// ultimi gripThrow ms (gripThrowF, WeightOfThrow): aprire la mano lanciando,
// anche in 150-250 ms, non lo indebolisce. Si parte dalla presa stretta.
void GripStep(Held& held, int h, double now, float dt) {
    const bool on = vr::control::g_analogGrip.load() && held.weight > 0;
    held.gripV = on ? GripValueNow(h) : 1.0f;
    const float f = on ? GripFactor(held.gripV) : 1.0f;
    const float before = held.gripF;
    vr::pure::GripFilterStep(held.gripFilt, f, now, dt, (std::max)(0.0f, g_tune.gripHold) * 0.001f, (std::max)(0.0f, g_tune.gripEase) * 0.001f,
                             (std::max)(0.0f, g_tune.gripRise) * 0.001f, (std::max)(0.0f, g_tune.gripThrow) * 0.001f);
    held.gripF = held.gripFilt.f;
    held.gripThrowF = vr::pure::GripPeakOf(held.gripFilt);
    // Nel log quando la forza si e' assestata su un valore nuovo: ferma da
    // 0,2 s (una riga per cambio, mai per tick; un grip che trema non scrive).
    if (std::fabs(held.gripF - before) > 0.002f) held.gripStableAt = now;
    if (on && now - held.gripStableAt >= 0.2 && std::fabs(held.gripF - held.gripLogF) >= 0.03f) {
        held.gripLogF = held.gripF;
        const WeightLimits wl = WeightOf(held);
        const float s = vr::control::g_posScale, mul = held.weight == 1 ? 2.0f : 1.0f;
        char b[300];
        std::snprintf(b, sizeof(b),
                      "GRAB: grip %.2f (%s hand, %s): pull %.0f N of %.0f (%.0f%%), twist %.1f of %.1f N m, turn gain x%.2f, droop x%.2f (cap %.0f deg);"
                      " %.1f kg: accel %.1f m/s2 = %.2f g (net up %.1f), turn floor %.0f rad/s2",
                      held.gripV, h ? "left" : "right", held.weight == 1 ? "light" : "full", wl.forceN,
                      mul * (std::max)(1.0f, g_tune.force), held.gripF * 100.0f, wl.tauMax, mul * (std::max)(0.1f, g_tune.torque),
                      wl.turnGain, wl.hang, wl.droopCap * 57.2958f, MassKg(held), wl.aMax / s, wl.aMax / (std::max)(1.0f, wl.g), (wl.aMax - wl.g) / s, wl.alphaMin);
        vr::log::Line(b);
    }
}

void ThrowSpinLimit(const Held& held, float dw[3], float T) {
    float op[3], oa[3][3];
    if (!(T > 0.0f) || !CompPose(held.comp, op, oa)) return;
    const WeightLimits wl = WeightOfThrow(held);  // voce 6: la presa piu' stretta degli ultimi gripThrow ms
    const float kg = (std::max)(0.01f, MassKg(held));
    float tq[3];
    for (int i = 0; i < 3; ++i) tq[i] = kg * held.inertia[i] * Dot3(dw, oa[i]) / T;
    const float tl = std::sqrt(Dot3(tq, tq));
    if (!(tl > wl.tauMax)) return;
    const float dl = std::sqrt(Dot3(dw, dw));
    const float f = (std::min)(1.0f, (std::max)(wl.tauMax / tl, dl > 1e-6f ? wl.alphaMin * T / dl : 1.0f));
    for (int c = 0; c < 3; ++c) dw[c] *= f;
}

// Arresto sicuro (Drive): la velocita' piu' alta da cui l'oggetto si ferma,
// frenando con aB, entro l'errore e che resta piu' allow, o entro quanto fa in
// T (oggetti leggeri e veloci): v^2 <= 2 aB (e + allow) oppure
// v^2 <= 2 aB (e + v T) -> v <= aB T + sqrt((aB T)^2 + 2 aB e). (Provato
// anche col tick di ritardo, - v dt: a 90 Hz l'oltrepasso del barile a full
// non cambiava, 6-7 cm; viene dalla forza spartita con la correzione
// verticale, vedi docs/TECHNICAL.md.)
float SafeStopCap(float aB, float e, float allow, float T) {
    if (!(aB > 0.0f)) return 0.0f;
    const float at = aB * T;
    const float a = std::sqrt(2.0f * aB * (std::max)(0.0f, e + allow));
    const float b = at + std::sqrt(at * at + 2.0f * aB * (std::max)(0.0f, e));
    return (std::max)(a, b);
}

// Decelerazione disponibile (unita'/s^2) per fermare un moto nella direzione
// u (uz = componente verticale): la forza a meno della gravita', che aiuta a
// fermare una salita e ostacola una discesa: |-a u + g z| <= aMax.
float BrakeAccel(float aMax, float g, float uz) {
    const float r = aMax * aMax - g * g * (1.0f - uz * uz);
    return r > 0.0f ? (std::max)(0.0f, g * uz + std::sqrt(r)) : 0.0f;
}

// Baricentro stimato (centro dei limiti della mesh, con la scala del
// componente) e raggi d'inerzia al quadrato della scatola dei limiti
// (m^2, per kg): I_x = kg (e_y^2 + e_z^2) / 3 con e le mezze estensioni.
void WeightSetup(Held& held) {
    float lc[3], le[3], m[4][4];
    if (MeshLocalBounds(held.comp, lc, le) && ReadMatrix(held.comp, m)) {
        float e[3];
        for (int i = 0; i < 3; ++i) {
            const float sc = std::sqrt(m[i][0] * m[i][0] + m[i][1] * m[i][1] + m[i][2] * m[i][2]);
            held.comLocal[i] = lc[i] * sc;
            held.boxExt[i] = le[i] * sc;  // scatola per la distanza del palmo (Regrip in WedgeStep)
            e[i] = le[i] * sc / vr::control::g_posScale;
        }
        held.hasBox = true;
        for (int i = 0; i < 3; ++i) held.inertia[i] = (e[(i + 1) % 3] * e[(i + 1) % 3] + e[(i + 2) % 3] * e[(i + 2) % 3]) / 3.0f;
    } else {
        const float r = held.radiusCm * 0.01f;  // sfera piena dal raggio dei limiti
        for (float& v : held.inertia) v = 0.4f * r * r;
    }
    for (float& v : held.inertia) v = (std::max)(v, 1e-5f);
}

// --- Vibrazione degli urti (voce 3 del piano Boneworks, 25/10) ---------------
// Un tonfo quando la radio tenuta urta un muro, un tic quando la bottiglia
// tocca il tavolo, uno schiaffo sentito con la mano vuota. Il rilevatore non
// guarda la mano ma la fisica: dal tick prima PhysX ha avuto un comando di
// velocita' (lastCmdV, o quello della sfera); quello che manca dopo il passo,
// tolta la gravita' di un passo, l'ha tolto un contatto. Dv = comando - g dt
// (giu') - velocita' misurata, J = kg x |Dv| (N s). In aria libera Dv ~ 0
// qualunque cosa faccia la mano (anche il tremolio del tracciamento): niente
// impulsi tenendo fermo. Un impulso per contatto (ImpactEdge: la velocita'
// tolta sommata per hapwindow dal primo tick oltre meta' soglia, al piu' quella
// che l'oggetto aveva prima, oltre hapdv; poi il contatto deve finire per
// haprearm), al piu' uno ogni haprefr per mano, mai nelle finestre mute (salto
// del riferimento o giro a scatti, arrivo nella presa e assestamento dopo la
// ripresa, distacco, sfera rimessa sul palmo). Ampiezza, durata e frequenza:
// vr::pure::ImpactPulse. Mani: props h = 1 (sinistra) e' PulseHand(0).
double g_hapLast[2] = {-10.0, -10.0};  // ultimo impulso d'urto per mano (refrattario condiviso)
float g_hapLastAmp[2] = {0.0f, 0.0f};    // e la sua ampiezza (la mischia lo sostituisce, HeldMelee)
double g_hapQuietUntil = 0.0;           // salto del riferimento, giro a scatti, tick lungo: niente impulsi fino a qui (tutte e due le mani)
int g_hapLogged = 0;                    // righe HAPTIC della sessione (le prime 30 sempre, poi con grablog / hapticlog)

// 1 full, 0,5 low, 0 off.
float HapticGain() {
    const int m = vr::control::g_impactHaptics.load();
    return m >= 2 ? 1.0f : (m == 1 ? 0.5f : 0.0f);
}

// Soglie del rilevatore dai griptune (ms -> s).
vr::pure::ImpactEdgeTune HapEdgeTune() {
    vr::pure::ImpactEdgeTune t;
    t.dvMin = g_tune.hapDv;
    t.rearm = (std::max)(0.0f, g_tune.hapRearm) * 0.001;
    t.window = (std::max)(0.0f, g_tune.hapWindow) * 0.001;
    t.approach = (std::max)(0.0f, g_tune.hapApproach) * 0.001;
    return t;
}

// Mano dell'arma (regola dell'utente: la mano dell'arma non cambia, revisione
// 26/10): la destra, salvo un oggetto in mano con grabright, e la sinistra
// mentre tiene l'arma (sull'astina, a due mani) o fa la ricarica fisica. Li'
// gli impulsi restano quelli fissi di prima (oggetto urtato 20 ms, presa
// 50 ms, a 3500 us) e niente schiaffi: prima la sinistra sull'astina che
// sfiorava il riparo a 0,5 m/s dava un impulso pieno di 44 ms.
bool GunHandHaptics(int h) {
    if (h == 0) return !g_held[0].actor;
    return vr::control::g_leftOnGun || vr::control::g_twoHandEngaged || vr::input::LeftHandOnReload();
}

bool HapLogOn() { return g_hapLogged < 30 || vr::control::g_grabLog > 0 || vr::control::g_hapticLog > 0; }
bool HapLogVerbose() { return vr::control::g_grabLog > 0 || vr::control::g_hapticLog > 0; }

std::string HapName(void* comp) {
    return comp ? vr::script::ObjectName(reinterpret_cast<void*>(U32(comp, kOffSmcStaticMesh))) : std::string("-");
}

// Impulso alla mano h (indice di props) e riga HAPTIC; false se il
// refrattario della mano lo tace (refr = false: la presa, che non aspetta).
// at: dove (oggetto tenuto o sfera della mano, unita' del mondo), solo per il log.
// J = impulso sentito (N s: massa sentita x dv), quello della formula.
bool HapticSend(int h, const char* src, void* comp, float J, float dvMs, float kg, const vr::pure::HapticPulseSpec& sp, double now,
                bool refr = true, const float* at = nullptr) {
    if (refr && now - g_hapLast[h] < (std::max)(0.0f, g_tune.hapRefr) * 0.001) {
        if (HapLogVerbose()) {
            char b[220];
            std::snprintf(b, sizeof(b), "HAPTIC: %s hand %s %s: J %.2f N s (dv %.2f m/s) not pulsed (%.0f ms after the last pulse)",
                          h ? "left" : "right", src, HapName(comp).c_str(), J, dvMs, (now - g_hapLast[h]) * 1000.0);
            vr::log::Line(b);
        }
        return false;
    }
    g_hapLast[h] = now;
    g_hapLastAmp[h] = sp.amp;
    vr::input::PulseHand(h ? 0 : 1, sp.ms, sp.amp, sp.freq);
    if (HapLogOn()) {
        ++g_hapLogged;
        char w[64] = "";
        if (at) std::snprintf(w, sizeof(w), " at (%.0f, %.0f, %.0f)", at[0], at[1], at[2]);
        char b[320];
        std::snprintf(b, sizeof(b), "HAPTIC: %s hand %s %s: J %.2f N s (dv %.2f m/s, %.1f kg, felt %.2f kg) -> amp %.2f, %u ms, %.0f Hz (%s)%s",
                      h ? "left" : "right", src, HapName(comp).c_str(), J, dvMs, kg, dvMs > 1e-3f ? J / dvMs : 0.0f, sp.amp, sp.ms, sp.freq,
                      vr::control::g_impactHaptics.load() == 1 ? "low" : "full", w);
        vr::log::Line(b);
    }
    return true;
}

// Urto taciuto da una finestra muta: nel log solo con grablog / hapticlog.
void HapticMuted(int h, const char* src, void* comp, float J, float dvMs, const char* why) {
    if (!HapLogVerbose()) return;
    char b[220];
    std::snprintf(b, sizeof(b), "HAPTIC: %s hand %s %s: J %.2f N s (dv %.2f m/s) not pulsed (%s)", h ? "left" : "right", src,
                  HapName(comp).c_str(), J, dvMs, why);
    vr::log::Line(b);
}

// Finestre mute dell'oggetto tenuto (HeldImpact e colpo della pipeline,
// OnPropHit): nullptr = si puo' vibrare, altrimenti il perche' (log).
const char* HeldHapticQuiet(int h, const Held& held, double now) {
    if (now < g_hapQuietUntil || now < held.jumpUntil) return "frame jump or game hitch";
    if (now < held.grabAt + held.blendT + 0.05 || now < held.weightAt + held.weightBlendT + 0.05) return "settling into the hand";
    if (held.settleUntil > 0.0 && now < held.settleUntil + 0.05) return "settling after a regrip";
    if (held.brkStart > 0.0 || now - held.hapBrkAt < 0.3) return "breakaway";
    if (GunHandHaptics(h)) return "hand on the gun";
    return nullptr;
}

// Rilevatore dell'oggetto tenuto (Drive, prima del comando di questo tick):
// measV = velocita' di PhysX dopo il passo (unita'/s).
void HeldImpact(int h, Held& held, const float* measV, float dt, double now, const float* op) {
    const float s = vr::control::g_posScale;
    const float g = -GravityZ(g_tickPawn);
    const float d[3] = {(held.lastCmdV[0] - measV[0]) / s, (held.lastCmdV[1] - measV[1]) / s, (held.lastCmdV[2] - g * dt - measV[2]) / s};
    const float kg = MassKg(held);
    // Finestre mute: urti che non sono urti, o non della mano. Distacco e
    // 0,3 s dopo: la radio premuta 20 cm nel terreno, il distacco la fa
    // saltare su di 5 cm ("thrashing") e il peso la riporta giu' a 2 m/s, un
    // secondo urto fatto dalla guida, non dalla mano (F3_radio, 2 impulsi per contatto).
    if (held.brkStart > 0.0) held.hapBrkAt = now;
    const char* why = HeldHapticQuiet(h, held, now);
    bool muted = false;
    float lost = 0.0f;
    const float mv[3] = {measV[0] / s, measV[1] / s, measV[2] / s};
    const bool fire = held.hapEdge.Step(d, mv, now, dt, HapEdgeTune(), why != nullptr, &muted, &lost);
    // Impulso sentito: massa ridotta oggetto-mano (hapspherekg) per la
    // velocita' tolta. Prima kg x dv: la radio da 8 kg dava 8,5-11,9 N s,
    // sempre piena; ora 3,1 kg x dv (0,69 alla soglia, piena da 1,5 m/s),
    // una bottiglia da 0,8 kg 0,69 kg x dv.
    const float J = vr::pure::ReducedMass(kg, (std::max)(0.1f, g_tune.hapSphereKg)) * lost;
    if (muted) HapticMuted(h, "held", held.comp, J, lost, why);
    if (fire) HapticSend(h, "held", held.comp, J, lost, kg, vr::pure::ImpactPulse(J, kg, HapticGain(), g_tune.hapJ), now, true, op);
}

void Grab(int h, const Prop& target, const float* hp, const float ha[3][3]) {
    Prop p = target;
    if (p.isStatic) {
        // Oggetto di scena statico: diventa fisico alla presa (fase 2).
        // Bombola esplosiva (voce 5): in mano va il portatore, col suo componente.
        p.actor = MakePropDynamic(target, HitSource::kOther, h, &p.comp);  // in mano subito; kOther se la presa non riesce
        if (!p.actor) return;
        p.isStatic = false;
    }
    float op[3], oa[3][3];
    if (!CompPose(p.comp, op, oa)) return;
    Held& held = g_held[h];
    held = Held();
    held.actor = p.actor;
    held.comp = p.comp;
    float d[3] = {op[0] - hp[0], op[1] - hp[1], op[2] - hp[2]};
    for (int r = 0; r < 3; ++r) held.relPos[r] = d[0] * ha[r][0] + d[1] * ha[r][1] + d[2] * ha[r][2];
    std::memcpy(held.relPosFrom, held.relPos, sizeof(held.relPos));
    held.grabAt = ProfMs() * 0.001;
    if (p.constrained) {
        // Portiera, cartello sul palo: si afferra il punto dove sta il palmo e
        // lo si tira (ConstrainedDrive); l'oggetto resta dov'e'.
        held.constrained = true;
        float palm[3];
        for (int i = 0; i < 3; ++i) {
            palm[i] = hp[i];
            if (g_palmValid[h])
                palm[i] += g_palmLocal[h][0] * ha[0][i] + g_palmLocal[h][1] * ha[1][i] + g_palmLocal[h][2] * ha[2][i];
        }
        float dp[3] = {palm[0] - op[0], palm[1] - op[1], palm[2] - op[2]};
        for (int i = 0; i < 3; ++i) held.grabLocal[i] = Dot3(dp, oa[i]);
    }
    if (!p.constrained) {
    // Appoggio sul palmo: l'oggetto si sposta perche' la sua superficie tocchi
    // il palmo (1 cm di guanto) e il palmo stia sopra l'oggetto, non oltre il
    // bordo; il punto toccato lungo l'oggetto resta (collo o fondo della
    // bottiglia). Prima l'oggetto restava dov'era e attraversava la mano.
    float halfN = 0.0f;
    Prop cur = p;
    if (g_palmValid[h] && ReadBounds(p.comp, &cur)) {
        float P[3], n[3], c[3], ext[3];
        for (int i = 0; i < 3; ++i) {
            P[i] = hp[i] + g_palmLocal[h][0] * ha[0][i] + g_palmLocal[h][1] * ha[1][i] + g_palmLocal[h][2] * ha[2][i];
            n[i] = g_palmNormalLocal[h][0] * ha[0][i] + g_palmNormalLocal[h][1] * ha[1][i] + g_palmNormalLocal[h][2] * ha[2][i];
            c[i] = 0.5f * (cur.boxMin[i] + cur.boxMax[i]);
            ext[i] = 0.5f * (cur.boxMax[i] - cur.boxMin[i]);
        }
        // Mezzo spessore lungo la normale del palmo (box allineato agli assi del
        // mondo: per eccesso se l'oggetto e' ruotato) e limite laterale.
        halfN = std::fabs(n[0]) * ext[0] + std::fabs(n[1]) * ext[1] + std::fabs(n[2]) * ext[2];
        float q[3] = {P[0] - c[0], P[1] - c[1], P[2] - c[2]};
        float qn = q[0] * n[0] + q[1] * n[1] + q[2] * n[2];
        float gap = 0.01f * vr::control::g_posScale;
        float newC[3];
        for (int i = 0; i < 3; ++i) {
            float lat = q[i] - n[i] * qn;
            float lim = 0.7f * ext[i];
            float latC = (std::max)(-lim, (std::min)(lim, lat));
            newC[i] = P[i] + n[i] * (halfN + gap) - latC;
        }
        float shift[3] = {newC[0] - c[0], newC[1] - c[1], newC[2] - c[2]};
        float sl = std::sqrt(shift[0] * shift[0] + shift[1] * shift[1] + shift[2] * shift[2]);
        // Oltre 25 cm (palmo misurato male, oggetto enorme): niente appoggio.
        if (sl < 0.25f * vr::control::g_posScale) {
            float d2[3] = {d[0] + shift[0], d[1] + shift[1], d[2] + shift[2]};
            for (int r = 0; r < 3; ++r) held.relPos[r] = d2[0] * ha[r][0] + d2[1] * ha[r][1] + d2[2] * ha[r][2];
        } else {
            halfN = 0.0f;
        }
    }
    held.halfThickCm = halfN / vr::control::g_posScale * 100.0f;
    for (int i = 0; i < 3; ++i)
        for (int r = 0; r < 3; ++r) held.relAxes[i][r] = oa[i][0] * ha[r][0] + oa[i][1] * ha[r][1] + oa[i][2] * ha[r][2];
    std::memcpy(held.relAxesFrom, held.relAxes, sizeof(held.relAxes));
    {
        float rpos[3], rax[3][3], anc[3], ht = 0.0f;
        int kind = CanonicalGrip(h, p.comp, hp, ha, op, oa, rpos, rax, anc, &ht);
        if (kind) {
            std::memcpy(held.relPos, rpos, sizeof(rpos));
            std::memcpy(held.relAxes, rax, sizeof(rax));
            std::memcpy(held.anchor, anc, sizeof(anc));
            held.halfThickCm = ht / vr::control::g_posScale * 100.0f;
            held.powerGrip = true;
            held.gripKind = kind;
            // Arrivo nella presa: 0,10 s piu' 0,10 s ogni 180 gradi di rotazione.
            float tr = Dot3(rax[0], held.relAxesFrom[0]) + Dot3(rax[1], held.relAxesFrom[1]) + Dot3(rax[2], held.relAxesFrom[2]);
            held.blendT = 0.10f + 0.10f * std::acos((std::max)(-1.0f, (std::min)(1.0f, (tr - 1.0f) * 0.5f))) / 3.14159265f;
        } else if (!g_grip[h].valid) {
            // Mano appena comparsa (le nocche si misurano in PoseFingers): la
            // presa canonica appena c'e' la misura, non quella sul palmo.
            held.wantCanonical = true;
        }
    }
    }  // !p.constrained
    held.weightAt = held.grabAt;  // abbassamento e piega entrano con l'arrivo nella presa
    held.weightBlendT = held.blendT;
    float m = BodyMass(p.comp);
    // Senza massa letta: 0,2 unita' (8 kg, la radio del livello). Prima 5,0 (200 kg),
    // anche nell'impulso di ConstrainedDrive su una portiera.
    held.mass = m > 0.0f ? m : 0.2f;
    held.radiusCm = p.radius / vr::control::g_posScale * 100.0f;
    // Oggetti pesanti piu' lenti a seguire la mano: fino a 15 kg come la mano,
    // poi la velocita' massima cala (una portiera non si sventola): il barile
    // di carburante (33 kg) 13,5 m/s; mai sotto 5 m/s (HeldMaxSpeed).
    held.maxSpeed = HeldMaxSpeed(held);
    held.hand = h;
    // Peso: modo fissato alla presa (cambiarlo dal menu con l'oggetto in mano
    // non lo fa saltare); i vincolati (portiere, cartelli) restano come prima.
    held.weight = p.constrained ? 0 : (std::max)(0, (std::min)(2, vr::control::g_objectWeight.load()));
    if (held.weight) WeightSetup(held);
    // Voce 5: solo la mano libera, mai quella dell'arma. Oggetti liberi solo
    // con il peso (revisione 26/10): con objectweight off la guida di prima
    // spinge la radio premuta nella cassa a 7-11 m/s e la fa rimbalzare di
    // 6-12 cm per tick; la mano disegnata sull'oggetto vero tremava a dente di
    // sega (~2 cm, ~20 Hz, p5_pr3). Senza peso l'oggetto segue la mano 1:1 e
    // la mano resta sul controller, come off. Portiere e cartelli (peso
    // sempre 0) restano: il loro punto afferrato non rimbalza.
    held.solid = h == 1 && vr::control::g_solidHands.load() > 0 && (held.weight != 0 || p.constrained);
    held.saved9C = U32(p.actor, kOffActorFlags9C);
    held.savedA0 = U32(p.actor, kOffActorFlagsA0);
    // Tenuto in mano non spinge Walker e non lo blocca.
    WriteFlags(p.actor, held.savedA0 & ~kBlockActors, held.saved9C | kIgnoreRbPawns);
    RaiseMaxAngVel(p.comp);  // segue le girate di polso (resta alzato: anche i lanci ruotano)
    held.walkerSig = IgnoreWalker(g_tickPawn, p.comp, true);
    for (size_t i = 0; i < g_restore.size(); ++i)
        if (g_restore[i].actor == p.actor) {
            held.savedA0 = g_restore[i].savedA0;
            held.saved9C = g_restore[i].saved9C;
            g_restore.erase(g_restore.begin() + i);
            break;
        }
    Wake(p.comp);
    vr::control::g_propHeld[h] = true;
    if (HapticGain() > 0.0f && !GunHandHaptics(h)) {
        // Voce 3: la conferma cresce con la massa e, presa al volo (guanti,
        // oggetto che cade), con l'impulso della velocita' relativa. Con la
        // sinistra sull'arma o nella ricarica: l'impulso fisso di prima.
        float hv[3] = {0, 0, 0}, ov[3] = {0, 0, 0}, ow[3];
        HandVelocity(h, 0.03, hv);
        void* nx = CompNx(p.comp);
        if (nx && NxVelocities(nx, ov, ow) && std::isfinite(ov[0] + ov[1] + ov[2]))
            for (float& c : ov) c *= 50.0f;
        else
            ov[0] = ov[1] = ov[2] = 0.0f;
        const float rel[3] = {hv[0] - ov[0], hv[1] - ov[1], hv[2] - ov[2]};
        const float rs = std::sqrt(Dot3(rel, rel)) / vr::control::g_posScale, kg = MassKg(held);
        const float handKg = (std::max)(0.1f, g_tune.hapSphereKg);
        const vr::pure::HapticPulseSpec sp = vr::pure::GrabPulse(kg, rs, HapticGain(), g_tune.hapJ, handKg);
        HapticSend(h, "grab", p.comp, rs > 0.5f ? vr::pure::ReducedMass(kg, handKg) * rs : 0.0f, rs, kg, sp, ProfMs() * 0.001, false);
    } else {
        vr::input::PulseHand(h ? 0 : 1, 50);
    }
    char b[320];
    std::snprintf(b, sizeof(b), "GRAB: %s hand took %s %s (%s, mass %.1f kg = %.3f units%s, max speed %.1f m/s, radius %.0f cm)",
                  h ? "left" : "right", vr::script::ClassName(p.actor).c_str(), vr::script::ObjectName(p.actor).c_str(),
                  vr::script::ObjectName(reinterpret_cast<void*>(U32(p.comp, kOffSmcStaticMesh))).c_str(), MassKg(held), held.mass,
                  m > 0.0f ? "" : " (not read)", held.maxSpeed / vr::control::g_posScale, p.radius / vr::control::g_posScale * 100.0f);
    vr::log::Line(b);
    if (MassKg(held) > 60.0f) {
        // Il piu' pesante visto nei livelli e' sui 45 kg (barili da 1,0-1,1
        // unita'): un oggetto oltre 60 kg e' una sorpresa da guardare.
        std::snprintf(b, sizeof(b), "GRAB: WARNING heavy object: %.0f kg (over 60; masskg %.0f), max speed %.1f m/s (floor %.1f)",
                      MassKg(held), vr::control::g_massKgPerUnit.load(), held.maxSpeed / vr::control::g_posScale,
                      (std::max)(0.5f, g_tune.minSpeed));
        vr::log::Line(b);
    }
    if (held.weight) {
        const WeightLimits wl = WeightOf(held);
        const float kg = MassKg(held), s = vr::control::g_posScale;
        const float iMin = (std::min)(held.inertia[0], (std::min)(held.inertia[1], held.inertia[2]));
        const float iMax = (std::max)(held.inertia[0], (std::max)(held.inertia[1], held.inertia[2]));
        std::snprintf(b, sizeof(b),
                      "GRAB: weight %s: %.1f kg, accel %.0f m/s2 (net up %.1f), inertia %.3f %.3f %.3f kg m2 -> %.1f-%.1f rad/s2"
                      " (floor %.0f), sag %.1f cm, droop at most %.0f deg",
                      held.weight == 1 ? "light" : "full", kg, wl.aMax / s, (wl.aMax - wl.g) / s, kg * held.inertia[0], kg * held.inertia[1],
                      kg * held.inertia[2], (std::max)(wl.alphaMin, wl.tauMax / (kg * iMax)), (std::max)(wl.alphaMin, wl.tauMax / (kg * iMin)),
                      wl.alphaMin, wl.sag / s * 100.0f, wl.droopCap * 57.2958f);
        vr::log::Line(b);
    }
}

// Massa riletta 5 e 30 tick dopo la presa (nota dell'orchestratore, 24/10):
// la radio e il sacco di sabbia convertiti da statici davano esattamente 0,200
// unita' nel tick della conversione (MakePropDynamic), quanto il valore di
// riserva. Se il corpo di PhysX non fosse ancora quello finale, qui si
// vedrebbe (e la massa e la velocita' massima si aggiornano); la scala (righe
// di LocalToWorld) dice quanto pesa l'istanza rispetto alla mesh a scala 1.
// Misurato: 0,200 anche dopo 5 e 30 tick, ed e' la massa della scala 1,25
// (MassKg).
void MassReread(int h) {
    Held& held = g_held[h];
    const float m = BodyMass(held.comp);
    float M[4][4], sc[3] = {0, 0, 0};
    if (ReadMatrix(held.comp, M))
        for (int i = 0; i < 3; ++i) sc[i] = std::sqrt(M[i][0] * M[i][0] + M[i][1] * M[i][1] + M[i][2] * M[i][2]);
    const bool changed = m > 0.0f && std::fabs(m - held.mass) > 0.05f * held.mass;
    static int logged = 0;
    if (changed || logged < 40 || vr::control::g_grabLog > 0) {
        ++logged;
        char b[260];
        std::snprintf(b, sizeof(b), "GRAB: mass re-read %d ticks after the grab: %.3f units (%.3f at the grab%s), %s, scale %.3f %.3f %.3f",
                      held.ticks, m, held.mass, changed ? ", updated" : "",
                      vr::script::ObjectName(reinterpret_cast<void*>(U32(held.comp, kOffSmcStaticMesh))).c_str(), sc[0], sc[1], sc[2]);
        vr::log::Line(b);
    }
    if (changed) {
        held.mass = m;
        held.maxSpeed = HeldMaxSpeed(held);
    }
}

// Posa voluta dell'oggetto negli assi della mano: nei primi blendT secondi
// dalla posa della presa a quella finale, per la via piu' corta (slerp: anche
// 180 gradi, che l'interpolazione delle righe con Gram-Schmidt non reggeva) e
// ruotando attorno al punto che finisce in mano (la bottiglia ha l'origine sul
// fondo: ruotando attorno a quella spazzava la mano).
// Peso (voce 2): la presa cede come una molla. La posa voluta (assi della
// mano) scende di kg x 9,81 / kLin (al piu' sagCap) e, se l'oggetto e' tenuto
// fuori dal baricentro, ruota attorno al punto della presa di (coppia del
// peso) / kRot (al piu' droopCap), con coppia = (baricentro - presa) x m g.
// Entrano dalla presa nel tempo dell'arrivo (WeightBlend): niente scatto alla presa. Il punto
// della presa: quello dell'oggetto che finisce in mano (presa canonica) o il
// palmo. ha = assi della mano nel mondo (il basso negli assi della mano).
void WeightPose(const Held& held, const float ha[3][3], float blend, float rp[3], float ra[3][3], float* sagOut = nullptr,
                float* droopOut = nullptr) {
    const WeightLimits wl = WeightOf(held);
    const float s = vr::control::g_posScale;
    float down[3];
    for (int r = 0; r < 3; ++r) down[r] = -ha[r][2];
    float gp[3];
    const float* k = held.anchor;
    for (int c = 0; c < 3; ++c) gp[c] = rp[c] + k[0] * ra[0][c] + k[1] * ra[1][c] + k[2] * ra[2][c];
    if (!held.gripKind && held.hand >= 0 && held.hand < 2 && g_palmValid[held.hand]) std::memcpy(gp, g_palmLocal[held.hand], sizeof(gp));
    float arm[3];
    for (int c = 0; c < 3; ++c)
        arm[c] = (rp[c] + held.comLocal[0] * ra[0][c] + held.comLocal[1] * ra[1][c] + held.comLocal[2] * ra[2][c] - gp[c]) / s;
    float fg[3], tau[3];
    const float w = MassKg(held) * 9.81f;
    for (int c = 0; c < 3; ++c) fg[c] = down[c] * w;
    Cross3(arm, fg, tau);  // N m
    const float tl = std::sqrt(Dot3(tau, tau));
    float droop = 0.0f;
    if (tl > 1e-4f && wl.droopCap > 0.0f) {
        droop = (std::min)(tl / wl.kRot, wl.droopCap) * blend;
        const float ax[3] = {tau[0] / tl, tau[1] / tl, tau[2] / tl};
        const Quat q = AxisAngle(ax, droop);
        for (int i = 0; i < 3; ++i) {
            float r2[3];
            Rotate(q, ra[i], r2);
            std::memcpy(ra[i], r2, sizeof(r2));
        }
        float d[3] = {rp[0] - gp[0], rp[1] - gp[1], rp[2] - gp[2]}, d2[3];
        Rotate(q, d, d2);
        for (int c = 0; c < 3; ++c) rp[c] = gp[c] + d2[c];
    }
    for (int c = 0; c < 3; ++c) rp[c] += down[c] * wl.sag * blend;
    if (sagOut) *sagOut = wl.sag * blend;
    if (droopOut) *droopOut = droop;
}

// Quanto abbassamento e piega sono gia' entrati (0..1, smoothstep): dalla
// presa in weightBlendT (weightAt), non dall'arrivo nella presa di grabAt.
float WeightBlend(const Held& held) {
    float b = static_cast<float>((ProfMs() * 0.001 - held.weightAt) / (std::max)(1e-3f, held.weightBlendT));
    b = (std::max)(0.0f, (std::min)(1.0f, b));
    return b * b * (3.0f - 2.0f * b);
}

void HeldTarget(const Held& held, const float ha[3][3], float rp[3], float ra[3][3], float* sagOut = nullptr, float* droopOut = nullptr) {
    float blend = (std::min)(1.0f, static_cast<float>((ProfMs() * 0.001 - held.grabAt) / held.blendT));
    blend = blend * blend * (3.0f - 2.0f * blend);
    float fr[3][3], to[3][3];
    std::memcpy(fr, held.relAxesFrom, sizeof(fr));
    std::memcpy(to, held.relAxes, sizeof(to));
    bool mir = Det3(to) < 0.0f;  // mesh specchiata: si interpola la parte propria
    if (mir)
        for (int c = 0; c < 3; ++c) {
            fr[2][c] = -fr[2][c];
            to[2][c] = -to[2][c];
        }
    QuatToRows(Slerp(RowsToQuat(fr), RowsToQuat(to), blend), ra);
    if (mir)
        for (float& v : ra[2]) v = -v;
    const float* k = held.anchor;
    for (int c = 0; c < 3; ++c) {
        float from = held.relPosFrom[c] + k[0] * held.relAxesFrom[0][c] + k[1] * held.relAxesFrom[1][c] + k[2] * held.relAxesFrom[2][c];
        float dest = held.relPos[c] + k[0] * held.relAxes[0][c] + k[1] * held.relAxes[1][c] + k[2] * held.relAxes[2][c];
        rp[c] = from + (dest - from) * blend - (k[0] * ra[0][c] + k[1] * ra[1][c] + k[2] * ra[2][c]);
    }
    if (held.weight && ha) WeightPose(held, ha, WeightBlend(held), rp, ra, sagOut, droopOut);
}

// Asse-angolo della rotazione R (righe/colonne come in Drive): asse unitario e
// angolo in radianti; false se nulla.
bool AxisAngleOf(const float R[3][3], float axis[3], float* angle) {
    float tr = R[0][0] + R[1][1] + R[2][2];
    float ang = std::acos((std::max)(-1.0f, (std::min)(1.0f, (tr - 1.0f) * 0.5f)));
    if (!(ang > 1e-4f)) return false;
    float ax[3] = {R[2][1] - R[1][2], R[0][2] - R[2][0], R[1][0] - R[0][1]};
    float l = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
    if (l < 1e-5f) {
        // 180 gradi: asse dalla colonna dominante di R + I.
        int k = R[0][0] > R[1][1] ? (R[0][0] > R[2][2] ? 0 : 2) : (R[1][1] > R[2][2] ? 1 : 2);
        for (int c = 0; c < 3; ++c) ax[c] = R[c][k] + (c == k ? 1.0f : 0.0f);
        l = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
    }
    if (!(l > 1e-6f)) return false;
    for (int c = 0; c < 3; ++c) axis[c] = ax[c] / l;
    *angle = ang;
    return true;
}

// Guida l'oggetto tenuto verso la posa della mano con le velocita' del corpo
// rigido: il moto della mano in anticipo (velocita' del punto di presa e
// velocita' angolare del polso) piu' una frazione dell'errore per tick
// (g_tune.gain, 0,5). Prima si correggeva tutto l'errore in un tick: con la
// posa letta un tick in ritardo il controllo oscillava (la bottiglia girata
// verso il petto restava a 15-25 gradi dalla mano, a scatti ogni 4 tick, 24/10).
// Oggetto vincolato (fase 5): portiera su cerniera, cartello sul palo. Guidarlo
// tutto verso la mano non funzionava (la cerniera annullava tutto: il cartello
// restava fermo con la mano a 38 cm). Si tira invece il punto afferrato: a
// ogni tick un impulso nel punto, massa x (velocita' voluta del punto -
// velocita' attuale del punto), la velocita' voluta e' quella della mano piu'
// l'errore recuperato in 50 ms; il vincolo di PhysX la trasforma nel moto che
// permette (la portiera gira sulla cerniera). Oltre 25 cm dal palmo per
// 0,25 s (fine corsa, mano tirata via) la mano lascia la presa.
void ConstrainedDrive(int h, const float* hp, const float ha[3][3], float dt, const float* op, const float oa[3][3]) {
    Held& held = g_held[h];
    const float s = vr::control::g_posScale;
    float palm[3], pt[3];
    for (int c = 0; c < 3; ++c) {
        palm[c] = hp[c];
        if (g_palmValid[h])
            palm[c] += g_palmLocal[h][0] * ha[0][c] + g_palmLocal[h][1] * ha[1][c] + g_palmLocal[h][2] * ha[2][c];
        pt[c] = op[c] + held.grabLocal[0] * oa[0][c] + held.grabLocal[1] * oa[1][c] + held.grabLocal[2] * oa[2][c];
    }
    float err[3] = {palm[0] - pt[0], palm[1] - pt[1], palm[2] - pt[2]};
    float el = std::sqrt(err[0] * err[0] + err[1] * err[1] + err[2] * err[2]);
    double now = ProfMs() * 0.001;
    if (el > 0.25f * s) {
        if (held.stuckSince < 0.0) held.stuckSince = now;
        if (now - held.stuckSince > 0.25) {
            Release(h, "slipped off");
            return;
        }
    } else {
        held.stuckSince = -1.0;
    }
    float hv[3] = {0, 0, 0};
    HandVelocity(h, 0.03, hv);
    // Velocita' del punto misurata dal suo spostamento (qualunque sia l'asse
    // della cerniera; con il baricentro stimato al centro dei limiti la stima
    // era sbagliata per il cartello, che gira attorno al palo, e il controllo
    // oscillava). Velocita' voluta: quella della mano piu' l'errore in 50 ms.
    float vpt[3] = {0, 0, 0};
    if (held.hasPrevPt && dt > 0.0f)
        for (int c = 0; c < 3; ++c) vpt[c] = (pt[c] - held.prevPt[c]) / dt;
    std::memcpy(held.prevPt, pt, sizeof(pt));
    held.hasPrevPt = true;
    float vwant[3], dv[3];
    for (int c = 0; c < 3; ++c) {
        vwant[c] = err[c] / 0.05f + hv[c];
        dv[c] = vwant[c] - vpt[c];
    }
    float dl = std::sqrt(dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2]), lim = 10.0f * s;
    if (dl > lim)
        for (float& c : dv) c *= lim / dl;
    struct {
        float impulse[3];
        float pos[3];
        int32_t bone[2];
        uint32_t velChange;
    } ai = {{dv[0] * held.mass * 0.5f, dv[1] * held.mass * 0.5f, dv[2] * held.mass * 0.5f}, {pt[0], pt[1], pt[2]}, {0, 0}, 0};
    vr::script::CallFunction(held.comp, "AddImpulse", &ai);
    Wake(held.comp);
    if (vr::control::g_grabLog > 0) {
        vr::control::g_grabLog.fetch_sub(1);
        char b[220];
        std::snprintf(b, sizeof(b), "GRABLOG: %s constrained err %.1f cm, point speed %.2f m/s, wanted %.2f m/s, mass %.1f kg", h ? "L" : "R",
                      el / s * 100.0f, std::sqrt(Dot3(vpt, vpt)) / s, std::sqrt(Dot3(vwant, vwant)) / s, MassKg(held));
        vr::log::Line(b);
    }
}

// Polso della mano tenente (Drive): assi del tick prima e velocita' angolare
// smorzata; posa voluta del tick prima (GRABLOG). Qui e non statici in Drive:
// un salto del riferimento delle mani (FrameJump) li ruota con le mani.
float g_wristPrevHa[2][3][3] = {}, g_wristW[2][3] = {}, g_prevTa[2][3][3] = {};

bool WorldBlocked(void* source, const float* a, const float* b, bool pawns = false, float minWall = 40.0f);  // piu' avanti, coi salti del riferimento
bool TouchingPawn(void* walker, const float* c, float r);  // piu' avanti, con la pipeline dei colpi

// --- Distacco (seguito della voce 2, 25/10) ----------------------------------
// Regola dell'utente: nulla di afferrabile deve diventare impossibile da
// sollevare o spostare. Con il peso un oggetto leggero incastrato (bottiglia
// tra le vicine nella cassa del livello 1) partiva ogni tick dalla velocita'
// misurata (0) con un tick di forza: restava li' e scivolava via dalla mano,
// mentre off lo strappava con il comando da 30 m/s (e tremava: fino a 55 cm
// in un tick e 240 inversioni in una presa). Ora, come in Boneworks:
// - bloccato: la velocita' misurata da PhysX (dopo il passo, rispetto a
//   Walker) lungo il comando, in media su almeno 3 tick e 30 ms, e' sotto
//   breakRatio (35%) di quella comandata, con il punto della presa oltre
//   breakErr (8 cm) dal palmo e la mano che lo tiene lontano (l'errore non
//   cala). Un oggetto pesante in aria che resta indietro segue il suo
//   comando: non e' bloccato, il peso non cambia. Sotto 8 cm e' appoggiato a
//   qualcosa vicino alla mano (un parapetto): resta li', senza strappi;
// - niente distacco se tra il punto della presa e il palmo c'e' un muro o un
//   nemico (WorldBlocked con i Pawn): spingerlo dentro farebbe solo tremare
//   l'oggetto (o lo farebbe passare attraverso il tavolo);
// - il budget di forza raddoppia ogni breakTime ms, verso il comando della
//   guida senza peso per il punto della presa (meta' errore per tick, al piu'
//   maxSpeed); la rotazione resta col peso (breakturn): il moltiplicatore
//   torna in Drive (mul);
// - libero (segue almeno meta' del comando per 3 tick avvicinandosi di 3 cm,
//   o l'errore e' sotto 3/4 di breakErr): subito il budget del peso (kFreed), con la
//   velocita' di partenza tagliata a quella voluta dal peso con l'arresto
//   sicuro: niente resto dello strappo, niente oggetto che vola oltre la
//   mano. Di nuovo bloccato entro 0,3 s: si riprende da meta' della forza,
//   ma se e' di nuovo dov'era all'inizio (entro 3 cm) lo strappo non e'
//   servito: come una rinuncia;
// - rinuncia: alla guida piena e ancora fermo dopo breakGive ms (muro,
//   oggetto inamovibile), o se si dimena (2 tick oltre 1 m/s di lato o
//   all'indietro: la posa voluta e' dentro qualcosa che lo respinge). Un nuovo
//   tentativo solo se l'errore cambia di molto (la mano torna di 5 cm o tira
//   altri 10 cm) e non prima di 0,25 s. La regola dello scivolamento resta
//   quella del peso, senza contare le spinte dello strappo.
// Il segmento a-b attraversa un muro invisibile solido per l'oggetto tenuto
// che gli fa da pavimento? Quelli diventati pavimento in questa presa
// (volFloor) e quelli vicini, non ignorati, con la cima sotto il fondo
// dell'oggetto (bc: limiti dell'oggetto; box in cache di Scan, nessun
// puntatore letto). Box dei limiti del muro.
bool SegmentHitsBox(const float* a, const float* b, const float* mn, const float* mx) {
    float t0 = 0.0f, t1 = 1.0f;
    for (int i = 0; i < 3; ++i) {
        const float d = b[i] - a[i];
        if (std::fabs(d) < 1e-6f) {
            if (a[i] < mn[i] || a[i] > mx[i]) return false;
            continue;
        }
        float u0 = (mn[i] - a[i]) / d, u1 = (mx[i] - a[i]) / d;
        if (u0 > u1) std::swap(u0, u1);
        t0 = (std::max)(t0, u0);
        t1 = (std::min)(t1, u1);
        if (t0 > t1) return false;
    }
    return true;
}

bool SolidVolumeBetween(const Held& held, const float* bc, const float* a, const float* b) {
    for (int k = 0; k < held.nVolFloor; ++k) {
        Prop vb;
        if (ReadBounds(held.volFloor[k], &vb) && SegmentHitsBox(a, b, vb.boxMin, vb.boxMax)) return true;
    }
    for (const BlockVol& v : g_blockVols) {
        bool ign = false;
        for (int k = 0; k < held.nVolIgn; ++k) ign |= held.volIgn[k] == v.comp;
        if (ign || bc[2] - bc[5] < v.mx[2] - 1.0f) continue;  // ignorato, o non e' un pavimento per l'oggetto
        if (SegmentHitsBox(a, b, v.mn, v.mx)) return true;
    }
    return false;
}

enum class Brk { kNone, kActive, kFreed };
int g_brkLogged = 0;

Brk BreakawayStep(Held& held, float g, float dt, double now, bool usable, const float* measV, const float* pv, float eEff, const float* objC,
                  const float* tgtC, bool still, float* mul) {
    *mul = 1.0f;
    const float s = vr::control::g_posScale;
    const float bt = g_tune.breakTime * 0.001f;
    const bool log = g_brkLogged < 60 || vr::control::g_grabLog > 0;
    char b[300];
    if (!(bt > 0.0f) || !usable || MassKg(held) > g_tune.breakKg) {
        // Salto del riferimento, misura mancante, distacco spento, oggetto
        // pesante (oltre breakKg: un barile trascinato e frenato dall'attrito
        // non e' incastrato, e WorldBlocked non vede gli altri KActor; il
        // peso resta; incastrato davvero lo lascia WedgeStep): si ricomincia
        // da capo. Un distacco interrotto lascia il comando del peso come
        // comando di prima (revisione 25/10): la base del tick dopo (la
        // velocita' misurata) e' limitata a quel comando + 0,5 m/s, non a
        // quello gonfiato dello strappo.
        if (held.brkStart > 0.0) {
            std::memcpy(held.lastCmdV, held.weightCmdV, sizeof(held.lastCmdV));
            std::memcpy(held.lastCmdW, held.weightCmdW, sizeof(held.lastCmdW));
        }
        held.brkStart = 0.0;
        held.brkSince = -1.0;
        return Brk::kNone;
    }
    // Velocita' attesa dopo il passo (l'ultimo comando piu' la gravita' di un
    // passo) e quella misurata, rispetto a Walker.
    float vx[3], mr[3];
    for (int c = 0; c < 3; ++c) {
        vx[c] = held.lastCmdV[c] - pv[c];
        mr[c] = measV[c] - pv[c];
    }
    vx[2] -= g * dt;
    const float ce = std::sqrt(Dot3(vx, vx)), ml = std::sqrt(Dot3(mr, mr));
    const float prog = ce > 1e-3f ? Dot3(mr, vx) / ce : 0.0f;
    // Velocita' misurata di lato o all'indietro rispetto al comando (tutta, se va all'indietro).
    const float offV = prog > 0.0f ? std::sqrt((std::max)(0.0f, ml * ml - prog * prog)) : ml;
    const float ratio = (std::max)(0.05f, (std::min)(0.9f, g_tune.breakRatio));
    if (held.brkStart > 0.0) {
        held.brkErrMax = (std::max)(held.brkErrMax, eEff);
        // Libero: segue almeno meta' del comando per almeno 2 tick e 8 ms di
        // fila (3 tick a 250 Hz, 2 a 90; prima 3 tick, cioe' 33 ms a 90 Hz:
        // revisione 25/10) e il punto della presa si e' avvicinato al palmo
        // di 3 cm; o l'errore e' sotto breakErr. Un tick solo era un
        // sussulto (bottiglia di nuovo ferma subito dopo, 25/10; il comando
        // della guida senza peso copre meta' errore per tick, quindi in 2-3
        // tick non va oltre il bersaglio). Non solo "l'errore cala": cala
        // anche quando il bersaglio si avvicina all'oggetto fermo; non solo
        // "si muove": 3 tick a 2-3 m/s e di nuovo fermo li' accanto
        // (ribloccato dov'era, 25/10).
        if (ce > 0.5f && prog >= 0.5f * ce) {
            if (held.brkMoving == 0) held.brkMovingSince = now;
            ++held.brkMoving;
        } else {
            held.brkMoving = 0;
        }
        const bool moving = held.brkMoving >= 2 && now - held.brkMovingSince >= 0.008 - 1e-4 &&
                            eEff <= held.brkErrMax - 0.03f * s;  // e si e' avvicinato di 3 cm
        // Isteresi: finisce sotto 3/4 della soglia d'inizio (6 cm); a cavallo
        // degli 8 cm una bottiglia appoggiata a un parapetto entrava e usciva
        // dal distacco ogni 10-20 ms (25/10).
        const bool atHand = eEff < 0.75f * (std::max)(0.5f, g_tune.breakErr) * 0.01f * s;
        // Si dimena: va di lato o all'indietro piu' forte di quanto vibrava gia'
        // col peso prima del distacco (la posa voluta e' dentro qualcosa che lo
        // respinge: a guida piena la bottiglia vicino al parapetto saltava a
        // 3-10 m/s in ogni direzione per 250 ms, 25/10). Revisione 25/10: prima
        // bastavano 2 tick oltre 1 m/s, e la vibrazione che il peso aveva gia'
        // (0,4-1,3 m/s contro la cassa) faceva rinunciare dopo 8 ms a forza x1,
        // senza aver mai spinto di piu'. Ora: oltre 1 m/s e oltre il doppio della
        // vibrazione della finestra + 0,5 m/s, per almeno 2 tick e 30 ms di
        // fila, e solo da forza x4 in su (dopo due raddoppi).
        const double since = now - held.brkStart;
        if (offV > (std::max)(1.0f * s, 2.0f * held.brkVib + 0.5f * s)) {
            if (held.brkThrash == 0) held.brkThrashSince = now;
            ++held.brkThrash;
        } else {
            held.brkThrash = 0;
        }
        if (moving || atHand) {
            const float m = static_cast<float>(std::exp2((std::min)(14.0, since / bt)));
            if (log) {
                ++g_brkLogged;
                std::snprintf(b, sizeof(b),
                              "GRAB: breakaway: %s %s after %.0f ms (force x%.0f), measured %.2f of %.2f m/s, error %.1f cm (at most %.1f)",
                              vr::script::ObjectName(reinterpret_cast<void*>(U32(held.comp, kOffSmcStaticMesh))).c_str(),
                              moving ? "came free" : "is at the hand", since * 1000.0, m, prog / s, ce / s, eEff / s * 100.0f,
                              held.brkErrMax / s * 100.0f);
                vr::log::Line(b);
            }
            held.brkStart = 0.0;
            held.brkSince = -1.0;
            held.brkEnd = now;
            held.brkEndMul = m;
            // Collaudo (breakdrop): presa finta aperta poco dopo, per misurare
            // il lancio di un oggetto appena liberato.
            const int drop = vr::control::g_breakDropMs.load();
            if (drop >= 0 && moving && held.hand >= 0 && held.hand < 2 && vr::control::g_gripSim[held.hand].load())
                vr::control::g_gripSimOffAt[held.hand] = GetTickCount64() + static_cast<unsigned long long>((std::max)(1, drop));
            return Brk::kFreed;
        }
        const bool thrash = held.brkThrash >= 2 && now - held.brkThrashSince >= 0.03 - 1e-4 && since >= 2.0 * bt;
        const bool giveUp = thrash || (held.brkSat > 0.0 && now - held.brkSat > (std::max)(0.01f, g_tune.breakGive * 0.001f)) || since > 1.0;
        if (giveUp) {
            if (log) {
                ++g_brkLogged;
                std::snprintf(b, sizeof(b), "GRAB: breakaway: %s %s after %.0f ms (force x%.0f, measured %.2f m/s, %.2f along), error %.1f cm: back to the weight",
                              vr::script::ObjectName(reinterpret_cast<void*>(U32(held.comp, kOffSmcStaticMesh))).c_str(),
                              thrash ? "thrashing (pushed back by something)" : "did not come free at the full drive", since * 1000.0,
                              std::exp2((std::min)(14.0, since / bt)), ml / s, prog / s, eEff / s * 100.0f);
                vr::log::Line(b);
            }
            held.brkStart = 0.0;
            held.brkSince = -1.0;
            held.brkCoolErr = (std::max)(eEff, 1e-3f);
            held.brkCoolUntil = now + 0.25;
            held.brkGiveAt = now;
            held.brkAgainN = 0;
            return Brk::kFreed;  // via il resto dello strappo (base tagliata in Drive)
        }
        *mul = static_cast<float>(std::exp2((std::min)(14.0, since / bt)));
        return Brk::kActive;
    }
    // Dopo una rinuncia: di nuovo solo se la mano torna di 5 cm o tira altri 10
    // cm, e non prima di 0,25 s (tirando via in fretta da un oggetto bloccato,
    // un tentativo ogni 10 cm erano strattoni ogni 70 ms, 25/10); o comunque
    // dopo 0,5 s (revisione 25/10: con la mano ferma non riprovava mai).
    if (held.brkCoolErr > 0.0f && now >= held.brkCoolUntil &&
        (eEff < held.brkCoolErr - 0.05f * s || eEff > held.brkCoolErr + 0.10f * s || (held.brkGiveAt > 0.0 && now - held.brkGiveAt >= 0.5)))
        held.brkCoolErr = 0.0f;
    const float eMin = (std::max)(0.5f, g_tune.breakErr) * 0.01f * s;
    // Mano ferma (seguito 2, 25/10 sera: HandStillStep, il palmo non si e'
    // spostato di regripMove da 0,25 s e va sotto regripSpeed): nessuno strappo nuovo. Lo strappo e'
    // per chi tira davvero; con la mano ferma la bottiglia della cassa,
    // incastrata dalla presa canonica, veniva strattonata fino a forza x32 per
    // 150 ms ogni 0,5 s senza uscire, poi lasciata (WedgeStep). Ora la presa si
    // riprende dove si trova (Regrip in WedgeStep); uno strappo gia' partito finisce.
    if (still || !(ce > 0.5f) || eEff < eMin) {  // 0,5 unita'/s = 1 cm/s
        held.brkSince = -1.0;
        return Brk::kNone;
    }
    if (held.brkSince < 0.0) {
        held.brkSince = now;
        held.brkTicks = 0;
        held.brkErr0 = eEff;
        held.brkSumCe = held.brkSumProg = held.brkSumOff = 0.0f;
    }
    ++held.brkTicks;
    held.brkSumCe += ce;
    held.brkSumProg += prog;
    held.brkSumOff += offV;
    // Di nuovo bloccato entro 0,3 s da un distacco riuscito (oggetto liberato e
    // subito fermato dal prossimo ostacolo: bottiglie della cassa, 25/10): si
    // riprende dopo 2 tick e 8 ms, da meta' della forza di allora, senza chiedere di
    // nuovo che la mano tiri (ferma, l'oggetto restava a 30 cm).
    bool again = held.brkEnd > 0.0 && now - held.brkEnd < 0.3;
    // Bloccato in media sulla finestra (30 ms, almeno 2 tick; ripresa: 8 ms; a
    // tempo, non a tick: revisione 25/10, a 90 Hz 3 tick erano 33 ms): la
    // velocita' misurata lungo il comando, sommata, sotto breakratio di quella
    // comandata. Non tick per tick: una bottiglia che vibrava contro la cassa
    // misurava 0,4-0,8 m/s su 1,2-1,5 a ogni tick (oltre il 35%) senza
    // avanzare di un millimetro, e non contava mai come bloccata (25/10).
    if (held.brkTicks < 2 || now - held.brkSince < (again ? 0.008 : 0.03) - 1e-4) return Brk::kNone;
    if (!(held.brkSumProg < ratio * held.brkSumCe)) {
        held.brkSince = -1.0;  // si muove: finestra nuova
        return Brk::kNone;
    }
    if (held.brkCoolErr > 0.0f || now < held.brkTraceAt) return Brk::kNone;
    if (again && held.brkAgainN >= 3) {
        // Tre riprese di fila (revisione 25/10): l'oggetto arriva al palmo
        // (sotto 6 cm) e viene respinto oltre 8 cm, ancora e ancora (bottiglia
        // a terra con la posa voluta nel terreno: un ciclo ogni 100-200 ms,
        // 134 inversioni oltre 1 cm in una presa). Come una rinuncia.
        if (log) {
            ++g_brkLogged;
            std::snprintf(b, sizeof(b), "GRAB: breakaway: %s blocked again %d times in a row (error %.1f cm): back to the weight",
                          vr::script::ObjectName(reinterpret_cast<void*>(U32(held.comp, kOffSmcStaticMesh))).c_str(), held.brkAgainN, eEff / s * 100.0f);
            vr::log::Line(b);
        }
        held.brkAgainN = 0;
        held.brkEnd = -1.0;
        held.brkSince = -1.0;
        held.brkCoolErr = (std::max)(eEff, 1e-3f);
        held.brkCoolUntil = now + 0.25;
        held.brkGiveAt = now;
        return Brk::kNone;
    }
    if (again) {
        // Ribloccato dov'era all'inizio (entro 3 cm): lo strappo non l'ha
        // spostato, l'ha solo spinto dentro qualcosa che lo ha respinto (la
        // posa voluta tocca un parapetto: senza questo, un ciclo di strappi
        // ogni 130 ms, 25/10). Come una rinuncia.
        float d[3] = {objC[0] - held.brkPos0[0], objC[1] - held.brkPos0[1], objC[2] - held.brkPos0[2]};
        if (Dot3(d, d) < 0.03f * s * 0.03f * s) {
            if (log) {
                ++g_brkLogged;
                std::snprintf(b, sizeof(b), "GRAB: breakaway: %s blocked again where it started (error %.1f cm): something solid, back to the weight",
                              vr::script::ObjectName(reinterpret_cast<void*>(U32(held.comp, kOffSmcStaticMesh))).c_str(), eEff / s * 100.0f);
                vr::log::Line(b);
            }
            held.brkEnd = -1.0;
            held.brkSince = -1.0;
            held.brkCoolErr = (std::max)(eEff, 1e-3f);
            held.brkCoolUntil = now + 0.25;
            held.brkGiveAt = now;
            held.brkAgainN = 0;
            return Brk::kNone;
        }
    } else {
        // La mano tiene l'oggetto lontano: l'errore non cala da quando e'
        // bloccato (la mano non sta tornando verso l'oggetto). Non solo "cresce":
        // con la mano ferma una bottiglia rimasta a 22-35 cm non si liberava
        // mai, e alzando la mano l'errore poteva anche calare (25/10).
        if (eEff < held.brkErr0 - 0.01f * s) {
            held.brkSince = now;  // la mano torna: si ricomincia a contare da qui
            held.brkTicks = 0;
            held.brkErr0 = eEff;
            // Anche le somme (revisione 25/10): prima restavano quelle della
            // finestra vecchia con il conto dei tick azzerato, e le medie
            // (comando "22,6 m/s", vibrazione di lato 2,7 m/s) erano gonfiate.
            held.brkSumCe = held.brkSumProg = held.brkSumOff = 0.0f;
            return Brk::kNone;
        }
    }
    // Contro un Pawn (nemico, compagno) anche se non e' sulla linea: l'oggetto
    // premuto di lato contro il manichino, con la linea punto-palmo libera,
    // veniva spinto per 120 ms contro la capsula e si dimenava (pipeline dei
    // colpi, 25/10). Sfera dei limiti dell'oggetto + 8 unita' contro i cilindri.
    float bs[7] = {};
    const bool touching = SafeCopy(bs, static_cast<char*>(held.comp) + kOffCompBounds, sizeof(bs)) && TouchingPawn(g_tickPawn, bs, bs[6] + 8.0f);
    // Un muro invisibile che fa da pavimento all'oggetto, sulla linea
    // punto-palmo, ferma come un muro vero (revisione 25/10): le tracce a
    // estensione zero di WorldBlocked non vedono i BlockingVolume, e la
    // bottiglia rimessa sulla cassa e premuta 10 cm sotto il piano veniva
    // spinta attraverso il blocco invisibile a forza x19. Un tavolo vero lo
    // vede gia' WorldBlocked. (Un primo divieto di ogni distacco "verso il
    // basso" bloccava anche le bottiglie della cassa incastrate dalla presa
    // canonica, il cui bersaglio sta piu' in basso: tremavano ferme per 1,3 s.)
    if (touching || SolidVolumeBetween(held, bs, objC, tgtC) || WorldBlocked(g_tickPawn, objC, tgtC, true)) {
        held.brkTraceAt = now + 0.05;  // ricontrollo tra 50 ms (la mano si muove)
        if (log && !held.brkWallLogged) {
            ++g_brkLogged;
            held.brkWallLogged = true;
            std::snprintf(b, sizeof(b), "GRAB: breakaway: %s %s (error %.1f cm): no breakaway",
                          vr::script::ObjectName(reinterpret_cast<void*>(U32(held.comp, kOffSmcStaticMesh))).c_str(),
                          touching ? "pressed against a pawn" : "blocked by a wall or an enemy between it and the hand", eEff / s * 100.0f);
            vr::log::Line(b);
        }
        return Brk::kNone;
    }
    // Ripresa: il budget riparte da meta' di quello della fine (brkStart nel passato).
    const float m0 = again ? (std::max)(1.0f, held.brkEndMul * 0.5f) : 1.0f;
    held.brkAgainN = again ? held.brkAgainN + 1 : 0;
    held.brkStart = now - bt * std::log2(m0);
    if (!again) std::memcpy(held.brkPos0, objC, sizeof(held.brkPos0));
    held.brkSat = -1.0;
    held.brkErrMax = eEff;
    held.brkMoving = 0;
    held.brkThrash = 0;
    held.brkVib = held.brkSumOff / static_cast<float>((std::max)(1, held.brkTicks));  // vibrazione di prima (soglia del "si dimena")
    held.brkEnd = -1.0;
    if (log) {
        ++g_brkLogged;
        std::snprintf(b, sizeof(b),
                      "GRAB: breakaway: %s blocked for %.0f ms (measured %.2f of %.2f m/s on average, sideways %.2f, error %.1f cm, was %.1f)%s: force x%.0f, doubling every %.0f ms",
                      vr::script::ObjectName(reinterpret_cast<void*>(U32(held.comp, kOffSmcStaticMesh))).c_str(), (now - held.brkSince) * 1000.0,
                      held.brkSumProg / (std::max)(1, held.brkTicks) / s, held.brkSumCe / (std::max)(1, held.brkTicks) / s, held.brkVib / s, eEff / s * 100.0f,
                      held.brkErr0 / s * 100.0f, again ? ", again" : "", m0, bt * 1000.0f);
        vr::log::Line(b);
    }
    *mul = m0;
    return Brk::kActive;
}

// --- Ripresa dove si trova (seguito 2 della voce 2, 25/10 sera) -------------
// Regola dell'utente: nulla di afferrabile deve diventare impossibile da
// tenere, e la mano deve sembrare vera (niente tremolii). Con il peso una
// bottiglia della cassa presa con la mano ferma veniva lasciata dopo ~2 s
// (suite sulla dll finale del seguito: tre distacchi fino a forza x32, poi
// WedgeStep "wedged for 0.86 s, grip error 18.7 cm"): la presa canonica la
// vuole girare di 118 gradi dentro le vicine, e li' non ci arriva. Ora, come
// in Boneworks, quando la mano non tira:
// - mano ferma (HandStillStep): il palmo, nel mondo, non si e' spostato di
//   regripMove (2 cm) da almeno 0,25 s e non va piu' veloce di regripSpeed
//   (3 cm/s, media di 0,1 s). Alla presa conta la velocita' della mano. Nel
//   mondo e non rispetto all'oggetto: la presa canonica che gira l'oggetto
//   non e' la mano che tira, e un oggetto che segue la mano non e' bloccato.
//   Con la mano ferma il distacco non parte (BreakawayStep). La velocita'
//   (revisione del seguito 2): con il solo spostamento una mano che tira via
//   piano (5 cm/s) contava come ferma tra un passo di 2 cm e l'altro, la
//   ripresa scattava a ogni 4-5 cm di errore e lo strappo non partiva mai;
// - l'oggetto bloccato: finito l'arrivo nella presa, il punto della presa
//   resta oltre regripErr (4 cm) dal suo bersaglio senza avvicinarsi
//   (l'errore medio non cala di 2 cm), fuori da un distacco e non contro un
//   Pawn;
// - dopo regripTime (0,2 s) di fila cosi', con la mano ferma, se il palmo
//   e' ancora vicino all'oggetto (entro regripReach, 16 cm, il doppio della
//   zona di presa, dalla scatola dei limiti negli assi dell'oggetto) la presa si riprende
//   dove l'oggetto si trova (Regrip), come se la mano si fosse chiusa li'; alla
//   prima mossa della mano l'oggetto torna ad appoggiarsi al palmo, come
//   all'appoggio della presa (RestOnPalm);
// - se il palmo e' piu' lontano (mano tirata via e poi ferma) niente ripresa,
//   perche' un oggetto che galleggia lontano dalla mano non e' una presa
//   vera: la guida non lo spinge piu' contro l'ostacolo (held.stuck: al piu'
//   stuckSpeed verso il bersaglio; spinto con tutta la forza la bottiglia
//   rimbalzava di 1-2 cm per tick, tremando finche' la mano restava li') e,
//   fermo cosi' per T (weightwedge, 1 s a 16 cm), la mano lo lascia.
// Se la mano si muove il tempo riparte da zero e la guida torna piena, e lo
// strappo resta per chi tira davvero. La mano lo lascia anche allo
// scivolamento (60/80 cm, Drive).

// Il palmo nel mondo: mano ferma (true) o in moto. Quando si muove (si sposta
// di regripMove o va oltre regripSpeed) azzera il tempo della ripresa e
// chiude l'assestamento. False con la ripresa spenta (regripTime 0): il
// distacco torna quello del seguito.
bool HandStillStep(Held& held, double now, float dt, const float* palm, const float* hv) {
    const float s = vr::control::g_posScale;
    const float vm = (std::max)(0.5f, g_tune.regripSpeed) * 0.01f * s;
    if (!held.palmHasPrev) {
        std::memcpy(held.palmVelF, hv, sizeof(held.palmVelF));  // primo tick: la velocita' della mano (30 ms)
    } else if (dt > 1e-4f) {
        const float a = 1.0f - std::exp(-dt / 0.1f);
        for (int c = 0; c < 3; ++c) held.palmVelF[c] += ((palm[c] - held.palmPrevW[c]) / dt - held.palmVelF[c]) * a;
    }
    std::memcpy(held.palmPrevW, palm, sizeof(held.palmPrevW));
    held.palmHasPrev = true;
    const bool fast = Dot3(held.palmVelF, held.palmVelF) > vm * vm;
    if (held.handMovedAt < 0.0) {
        std::memcpy(held.palmRefW, palm, sizeof(held.palmRefW));
        held.handMovedAt = fast ? now : now - 1.0;
    }
    const float d[3] = {palm[0] - held.palmRefW[0], palm[1] - held.palmRefW[1], palm[2] - held.palmRefW[2]};
    const float mv = (std::max)(0.2f, g_tune.regripMove) * 0.01f * s;
    if (fast || Dot3(d, d) > mv * mv) {
        std::memcpy(held.palmRefW, palm, sizeof(held.palmRefW));
        held.handMovedAt = now;
        held.regripT = 0.0;
        held.settleUntil = 0.0;
    }
    return g_tune.regripTime > 0.0f && now - held.handMovedAt >= 0.25 - 1e-4;
}

// Distanza (unita') del palmo dalla scatola dei limiti dell'oggetto, negli
// assi dell'oggetto (WeightSetup); senza scatola dalla sfera dei limiti.
float PalmToObject(const Held& held, const float* palm, const float* op, const float oa[3][3]) {
    const float q[3] = {palm[0] - op[0], palm[1] - op[1], palm[2] - op[2]};
    if (held.hasBox) {
        float d2 = 0.0f;
        for (int i = 0; i < 3; ++i) {
            const float e = (std::max)(0.0f, std::fabs(Dot3(q, oa[i]) - held.comLocal[i]) - held.boxExt[i]);
            d2 += e * e;
        }
        return std::sqrt(d2);
    }
    float bs[7];
    if (!SafeCopy(bs, static_cast<char*>(held.comp) + kOffCompBounds, sizeof(bs))) return 1e9f;
    const float c[3] = {palm[0] - bs[0], palm[1] - bs[1], palm[2] - bs[2]};
    return (std::max)(0.0f, std::sqrt(Dot3(c, c)) - bs[6]);
}

// Posa voluta (assi della mano) = la posa vera dell'oggetto, e niente arrivo
// (from = to). Con il peso la posa voluta T e' quella da cui WeightPose (con
// WeightBlend, come HeldTarget) rida' esattamente la posa vera R. La piega
// ruota il braccio (baricentro - perno) verso il basso nel suo piano
// verticale, di theta(phi) = min(c sin phi, droopCap) con phi l'angolo del
// braccio dal basso: il braccio di T sta nello stesso piano, e basta
// risolvere phi_T - theta(phi_T) = phi_R (una variabile; tra le radici la piu'
// vicina a R), poi T = R ruotata indietro di theta attorno al perno, senza
// l'abbassamento. Il perno: il punto della presa (si muove con l'oggetto: in
// R e' piu' in basso dell'abbassamento) o il palmo per le prese sul palmo.
// Revisione del seguito 2: prima un passo in avanti e l'inversa (errore
// dell'ordine di abbassamento x piega), poi un punto fisso, che con il barile
// da 33 kg (full) preso da terra divergeva (residuo 16-18 cm e 40 gradi, il
// doppio del tetto: c ~ 1,6 > 1, la piega del modello oltrepassa la verticale
// e il passo non e' una contrazione), e la ripresa lo faceva saltare di
// 10-12 cm. resPos/resAng: il residuo (unita', radianti) di WeightPose(T)
// rispetto a R (controllo).
void AnchorHere(Held& held, const float* hp, const float ha[3][3], const float* op, const float oa[3][3], float* resPos = nullptr,
                float* resAng = nullptr) {
    const float d[3] = {op[0] - hp[0], op[1] - hp[1], op[2] - hp[2]};
    float rp1[3], ra1[3][3];  // la posa vera negli assi della mano
    for (int r = 0; r < 3; ++r) rp1[r] = Dot3(d, ha[r]);
    for (int i = 0; i < 3; ++i)
        for (int r = 0; r < 3; ++r) ra1[i][r] = Dot3(oa[i], ha[r]);
    float rp[3], ra[3][3];
    std::memcpy(rp, rp1, sizeof(rp));
    std::memcpy(ra, ra1, sizeof(ra));
    float ep = 0.0f, ea = 0.0f;
    if (held.weight) {
        const float bw = WeightBlend(held);
        const WeightLimits wl = WeightOf(held);
        const float s = vr::control::g_posScale;
        const float* k = held.anchor;
        float down[3], sg[3], P[3], arm[3];
        for (int r = 0; r < 3; ++r) down[r] = -ha[r][2];
        for (int c = 0; c < 3; ++c) {
            sg[c] = down[c] * wl.sag * bw;
            P[c] = rp1[c] + k[0] * ra1[0][c] + k[1] * ra1[1][c] + k[2] * ra1[2][c] - sg[c];
        }
        if (!held.gripKind && held.hand >= 0 && held.hand < 2 && g_palmValid[held.hand]) std::memcpy(P, g_palmLocal[held.hand], sizeof(P));
        for (int c = 0; c < 3; ++c)
            arm[c] = rp1[c] + held.comLocal[0] * ra1[0][c] + held.comLocal[1] * ra1[1][c] + held.comLocal[2] * ra1[2][c] - sg[c] - P[c];
        const float al = std::sqrt(Dot3(arm, arm));
        const float c0 = al / s * MassKg(held) * 9.81f / (std::max)(1e-3f, wl.kRot);
        float th = 0.0f, u[3] = {0.0f, 0.0f, 1.0f};
        if (al > 1e-4f && wl.droopCap > 0.0f && c0 > 0.0f && bw > 0.0f) {
            // Piano del braccio: il basso e l'orizzontale e verso il braccio (o una qualsiasi, braccio verticale).
            const float ad = Dot3(arm, down);
            float e[3] = {arm[0] - down[0] * ad, arm[1] - down[1] * ad, arm[2] - down[2] * ad};
            float el = std::sqrt(Dot3(e, e));
            if (!(el > 1e-5f * al)) {
                const float ref[3] = {std::fabs(down[0]) < 0.9f ? 1.0f : 0.0f, std::fabs(down[0]) < 0.9f ? 0.0f : 1.0f, 0.0f};
                const float rd = Dot3(ref, down);
                for (int c = 0; c < 3; ++c) e[c] = ref[c] - down[c] * rd;
                el = std::sqrt(Dot3(e, e));
            }
            for (float& v : e) v /= el;
            Cross3(e, down, u);  // ruotando attorno a u di +theta il braccio scende verso il basso
            const float phiR = std::atan2(Dot3(arm, e), ad);
            auto thS = [&](float ph) {
                const float v = (std::min)(c0 * std::fabs(std::sin(ph)), wl.droopCap) * bw;
                return ph >= 0.0f ? v : -v;
            };
            auto hf = [&](float ph) { return ph - thS(ph) - phiR; };
            const float pi = 3.14159265f, step = 2.0f * pi / 360.0f;
            float best = 0.0f, bestD = 1e9f;
            float x0 = -pi, h0 = hf(x0);
            for (int i = 1; i <= 360; ++i) {
                const float x1 = -pi + step * static_cast<float>(i), h1 = hf(x1);
                if (h0 == 0.0f || (h0 < 0.0f) != (h1 < 0.0f)) {
                    float lo = x0, hi = x1, hlo = h0;
                    for (int it = 0; it < 30 && hlo != 0.0f; ++it) {
                        const float mid = 0.5f * (lo + hi), hm = hf(mid);
                        if ((hm < 0.0f) == (hlo < 0.0f)) {
                            lo = mid;
                            hlo = hm;
                        } else {
                            hi = mid;
                        }
                    }
                    const float root = hlo == 0.0f ? lo : 0.5f * (lo + hi);
                    if (std::fabs(root - phiR) < bestD) {
                        bestD = std::fabs(root - phiR);
                        best = root;
                    }
                }
                x0 = x1;
                h0 = h1;
            }
            if (bestD < 1e8f) th = thS(best);
        }
        const Quat qi = AxisAngle(u, -th);  // l'inversa della piega
        float v[3], w[3];
        for (int c = 0; c < 3; ++c) v[c] = rp1[c] - sg[c] - P[c];
        Rotate(qi, v, w);
        for (int c = 0; c < 3; ++c) rp[c] = P[c] + w[c];
        for (int i = 0; i < 3; ++i) Rotate(qi, ra1[i], ra[i]);
        // Controllo: WeightPose(T) contro R.
        float rpf[3], raf[3][3];
        std::memcpy(rpf, rp, sizeof(rpf));
        std::memcpy(raf, ra, sizeof(raf));
        WeightPose(held, ha, bw, rpf, raf);
        const float dd[3] = {rpf[0] - rp1[0], rpf[1] - rp1[1], rpf[2] - rp1[2]};
        ep = std::sqrt(Dot3(dd, dd));
        const float tr = Dot3(raf[0], ra1[0]) + Dot3(raf[1], ra1[1]) + Dot3(raf[2], ra1[2]);
        ea = std::acos((std::max)(-1.0f, (std::min)(1.0f, (tr - 1.0f) * 0.5f)));
    }
    std::memcpy(held.relPosFrom, rp, sizeof(rp));
    std::memcpy(held.relAxesFrom, ra, sizeof(ra));
    std::memcpy(held.relPos, rp, sizeof(rp));
    std::memcpy(held.relAxes, ra, sizeof(ra));
    if (resPos) *resPos = ep;
    if (resAng) *resAng = ea;
}

// Punto della presa per la ripresa e appoggio sul palmo, con la scatola
// dell'oggetto nei suoi assi. Palmo fuori dalla scatola: l'oggetto va verso
// il palmo finche' il punto della scatola piu' vicino e' a 1 cm (guanto).
// Palmo dentro (oggetto premuto su un tavolo con la mano ferma): l'oggetto va
// lungo la normale del palmo finche' il palmo ne esce dal lato del dorso, piu'
// 1 cm. La prima versione rifaceva l'appoggio di Grab (centro a mezzo spessore
// lungo la normale, il palmo entro il 70% di lato): con la normale del palmo di
// sbieco rispetto alle facce lasciava il palmo ancora a 5-7 cm dalla scatola
// (R_slow_0: da 5,3 a 6,7 cm dopo l'appoggio). Il punto della presa (perno di
// rotazione, abbassamento e piega, e punto misurato da distacco, incastro e
// scivolamento) e' quello che tocchera' il palmo; la posa voluta resta quella
// vera (AnchorHere). Lo spostamento (negli assi della mano) si applica alla
// prima mossa della mano (RestOnPalm), non qui: l'oggetto e' bloccato, e spinto
// subito verso il palmo tornerebbe contro l'ostacolo. Revisione del seguito 2:
// prima il punto della presa era il palmo stesso, fino a 8 cm fuori dalla
// scatola o dentro l'oggetto, e la presa canonica non tornava (la bottiglia
// sollevata a 5,4-5,7 cm dal palmo per tutta la presa, F_lift_4). Palmo
// dentro piu' di 10 cm (in fondo a un oggetto grande) o regripRest 0: niente
// appoggio, il punto della presa e' il palmo.
void AnchorAtPalm(int h, Held& held, const float* hp, const float ha[3][3], const float* op, const float oa[3][3], const float* palm,
                  float* resPos = nullptr, float* resAng = nullptr) {
    const float s = vr::control::g_posScale;
    float shift[3] = {0, 0, 0};
    bool rest = false;
    if (held.hasBox && h >= 0 && h < 2 && g_palmValid[h] && g_tune.regripRest > 0.0f) {
        const float gap = 0.01f * s;
        float n[3], q[3], nO[3], dO[3], shO[3] = {0, 0, 0};
        const float pq[3] = {palm[0] - op[0], palm[1] - op[1], palm[2] - op[2]};
        bool inside = true;
        for (int c = 0; c < 3; ++c)
            n[c] = g_palmNormalLocal[h][0] * ha[0][c] + g_palmNormalLocal[h][1] * ha[1][c] + g_palmNormalLocal[h][2] * ha[2][c];
        for (int i = 0; i < 3; ++i) {
            q[i] = Dot3(pq, oa[i]) - held.comLocal[i];  // il palmo dal centro della scatola, assi dell'oggetto
            nO[i] = Dot3(n, oa[i]);
            const float cp = (std::max)(-held.boxExt[i], (std::min)(held.boxExt[i], q[i]));
            dO[i] = q[i] - cp;
            if (std::fabs(q[i]) > held.boxExt[i]) inside = false;
        }
        if (!inside) {
            const float dl = std::sqrt(Dot3(dO, dO));
            if (dl > gap)
                for (int i = 0; i < 3; ++i) shO[i] = dO[i] * (1.0f - gap / dl);
        } else {
            // Uscita del palmo lungo -n: la faccia piu' vicina in quella direzione (lastre della scatola).
            float t = 1e9f;
            for (int i = 0; i < 3; ++i) {
                if (std::fabs(nO[i]) < 1e-4f) continue;
                const float ti = -nO[i] > 0.0f ? (held.boxExt[i] - q[i]) / -nO[i] : (-held.boxExt[i] - q[i]) / -nO[i];
                t = (std::min)(t, ti);
            }
            if (t < 1e8f)
                for (int i = 0; i < 3; ++i) shO[i] = nO[i] * (t + gap);
        }
        for (int c = 0; c < 3; ++c) shift[c] = shO[0] * oa[0][c] + shO[1] * oa[1][c] + shO[2] * oa[2][c];
        held.restRaw = std::sqrt(Dot3(shift, shift));
        held.restAlongN = Dot3(shift, n);
        // Fuori dalla scatola lo spostamento e' la distanza del palmo meno 1 cm
        // (al piu' regripReach); dentro al piu' 10 cm (palmo in fondo a un oggetto grande: niente appoggio).
        const float lim = (inside ? 10.0f : (std::max)(0.0f, g_tune.regripReach) + 2.0f) * 0.01f * s;
        rest = Dot3(shift, shift) < lim * lim;
        if (!rest) std::memset(shift, 0, sizeof(shift));
    }
    const float pq[3] = {palm[0] - shift[0] - op[0], palm[1] - shift[1] - op[1], palm[2] - shift[2] - op[2]};
    for (int i = 0; i < 3; ++i) held.anchor[i] = Dot3(pq, oa[i]);
    AnchorHere(held, hp, ha, op, oa, resPos, resAng);
    for (int r = 0; r < 3; ++r) held.restShift[r] = Dot3(shift, ha[r]);
    held.restPending = rest && Dot3(shift, shift) > 0.005f * s * 0.005f * s;  // sotto 5 mm resta dov'e'
}

// La presa si riprende dove l'oggetto si trova: punto della presa da
// AnchorAtPalm, posa voluta = quella vera. Poi, per regripSettle s (0,15),
// la posa voluta segue a ogni tick quella vera (di nuovo AnchorAtPalm): la
// guida tiene l'oggetto fermo rispetto alla mano senza spingerlo, i contatti
// lo assestano dove sta (una posa presa a meta' di un rimbalzo restava dentro
// le vicine, e la bottiglia continuava a vibrare a 6-8 cm dal bersaglio,
// 25/10 sera, dn_1); poi resta li'. Prima di questo un arrivo di 0,25 s dalla
// posa di prima: la guida spingeva ancora contro le vicine per tutto
// l'arrivo. L'oggetto non salta: il bersaglio va a lui, non lui al
// bersaglio, con abbassamento e piega interi (orologio loro, WeightBlend:
// revisione del seguito 2, prima a ogni tick dell'assestamento ne tornava
// solo una frazione e la posa voluta saliva con l'oggetto), e il comando di
// questo tick parte gia' dalla posa nuova (Drive). Le dita passano alla posa
// nuova col loro filtro (80 ms, PoseFingers). La rotazione della presa
// canonica non torna (la bottiglia della cassa ci si incastrava); la
// posizione si', con l'appoggio sul palmo alla prima mossa della mano.
int g_regripLogged = 0;

void Regrip(int h, Held& held, double now, const float* hp, const float ha[3][3], const float* op, const float oa[3][3], const float* palm,
            float gripErr, float palmDist) {
    float rp0[3], ra0[3][3];
    HeldTarget(held, nullptr, rp0, ra0);  // posa voluta di prima (diagnostica: quanto voleva girarlo)
    float resP = 0.0f, resA = 0.0f;
    AnchorAtPalm(h, held, hp, ha, op, oa, palm, &resP, &resA);
    const float tr = Dot3(ra0[0], held.relAxes[0]) + Dot3(ra0[1], held.relAxes[1]) + Dot3(ra0[2], held.relAxes[2]);
    const float turned = std::acos((std::max)(-1.0f, (std::min)(1.0f, (tr - 1.0f) * 0.5f))) * 57.2958f;
    held.grabAt = now;
    held.blendT = 0.02f;  // niente arrivo: from = to
    held.wantCanonical = false;
    held.settleUntil = now + (std::max)(0.0f, g_tune.regripSettle);
    held.regripT = 0.0;
    held.regripErrF = -1.0f;
    held.wedgeT = 0.0;
    held.stuckSince = -1.0;
    held.brkSince = -1.0;
    held.brkCoolErr = 0.0f;
    held.brkEnd = -1.0;
    held.brkAgainN = 0;
    held.wedgeHasPrev = false;  // punto della presa nuovo: la sua velocita' riparte dal tick dopo
    ++held.regrips;
    if (g_regripLogged < 60 || vr::control::g_grabLog > 0) {
        ++g_regripLogged;
        const float s = vr::control::g_posScale;
        char b[360];
        std::snprintf(b, sizeof(b),
                      "GRAB: %s hand re-grips %s where it is (%d): blocked %.2f s with the hand still, grip error %.1f cm, %.0f deg from the wanted pose,"
                      " palm %.1f cm from it, rest on the palm %.1f cm (computed %.1f, %.1f along the palm normal), residual %.2f cm %.2f deg",
                      h ? "left" : "right", vr::script::ObjectName(reinterpret_cast<void*>(U32(held.comp, kOffSmcStaticMesh))).c_str(), held.regrips,
                      g_tune.regripTime, gripErr / s * 100.0f, turned, palmDist / s * 100.0f,
                      held.restPending ? std::sqrt(Dot3(held.restShift, held.restShift)) / s * 100.0f : 0.0f, held.restRaw / s * 100.0f,
                      held.restAlongN / s * 100.0f, resP / s * 100.0f, resA * 57.2958f);
        vr::log::Line(b);
    }
}

// Appoggio sul palmo dopo la ripresa, alla prima mossa della mano: la posa
// voluta scivola di restShift (assi della mano) in regripRest s (0,25),
// smoothstep, con l'arrivo nella presa (grabAt/blendT; abbassamento e piega
// restano interi). Libero, l'oggetto torna sul palmo mentre la mano lo porta;
// ancora bloccato, se la mano si ferma la ripresa lo riprende li'.
void RestOnPalm(int h, Held& held, double now) {
    held.restPending = false;
    std::memcpy(held.relPosFrom, held.relPos, sizeof(held.relPos));
    std::memcpy(held.relAxesFrom, held.relAxes, sizeof(held.relAxes));
    for (int r = 0; r < 3; ++r) held.relPos[r] += held.restShift[r];
    held.grabAt = now;
    held.blendT = (std::max)(0.02f, g_tune.regripRest);
    if (g_regripLogged < 60 || vr::control::g_grabLog > 0) {
        ++g_regripLogged;
        const float s = vr::control::g_posScale;
        char b[220];
        std::snprintf(b, sizeof(b), "GRAB: %s hand moves: %s back on the palm, %.1f cm in %.2f s", h ? "left" : "right",
                      vr::script::ObjectName(reinterpret_cast<void*>(U32(held.comp, kOffSmcStaticMesh))).c_str(),
                      std::sqrt(Dot3(held.restShift, held.restShift)) / s * 100.0f, held.blendT);
        vr::log::Line(b);
    }
}

// --- Incastrato senza via d'uscita (revisione del seguito, 25/10) -----------
// Il distacco non basta quando la posa voluta e' dentro qualcosa (la bottiglia
// incastrata tra le vicine nel fondo della cassa, con la mano ferma: smash
// loop, corsa 7, 5 s a 43 cm dal palmo, girata di 114 gradi, mai liberata ne'
// lasciata, perche' lo scivolamento parte solo oltre 60 cm). Seguito 2 (25/10
// sera): con la mano ferma la presa si riprende li' (Regrip) o, il palmo
// fuori portata, la guida smette di spingere (held.stuck); vedi sopra. Il
// rilascio ("wedged", weightwedge, 1 s) vale solo con il palmo fuori
// portata: se il punto della presa e' oltre breakErr dal suo bersaglio e non
// si muove (velocita' dalle posizioni, nel mondo, media su 0,15 s: la
// vibrazione sul posto si annulla; sotto 0,15 m/s) fuori da un distacco in
// corso, il tempo si accumula; oltre T la mano lo lascia, come una presa che
// scivola. T = wedgeTime a 2 x breakErr (16 cm), piu' breve piu' e' lontano
// (al piu' 0,3 s), piu' lungo vicino alla soglia (3 s a 8 cm). Si azzera
// quando si muove (oltre 0,3 m/s) o l'errore scende sotto 3/4 di breakErr.
// Contro un Pawn non conta: il nemico si sposta, e la regola dello
// scivolamento resta; il rilascio nemmeno nel primo secondo dopo la presa.
// 0 = niente, 1 = presa ripresa o in assestamento (Drive ricalcola la posa
// voluta), 2 = lasciato.
int WedgeStep(int h, Held& held, double now, float dt, const float* ancO, float gripErr, Brk brk, bool still, const float* hp,
              const float ha[3][3], const float* op, const float oa[3][3], const float* palm, float palmDist) {
    const float s = vr::control::g_posScale;
    if (held.wedgeHasPrev && dt > 1e-4f) {
        const float a = 1.0f - std::exp(-dt / 0.15f);
        for (int c = 0; c < 3; ++c) held.wedgeVel[c] += ((ancO[c] - held.wedgePrev[c]) / dt - held.wedgeVel[c]) * a;
    }
    std::memcpy(held.wedgePrev, ancO, sizeof(held.wedgePrev));
    held.wedgeHasPrev = true;
    held.stuck = false;
    const float eMin = (std::max)(0.5f, g_tune.breakErr) * 0.01f * s;
    const bool regripOn = g_tune.regripTime > 0.0f;
    const bool palmOn = palmDist <= (std::max)(0.0f, g_tune.regripReach) * 0.01f * s;
    // Contro un Pawn (TouchingPawn): letto al piu' ogni 0,1 s e solo con
    // l'oggetto bloccato. Prima a ogni tick con l'errore medio oltre 4 cm (anche
    // portando un oggetto pesante in ritardo con la mano in moto).
    auto touchingPawn = [&]() {
        if (now >= held.pawnChkAt) {
            float bs[7];
            held.pawnTouch = SafeCopy(bs, static_cast<char*>(held.comp) + kOffCompBounds, sizeof(bs)) && TouchingPawn(g_tickPawn, bs, bs[6] + 8.0f);
            held.pawnChkAt = now + 0.1;
        }
        return held.pawnTouch;
    };
    if (held.settleUntil > 0.0) {
        // Assestamento dopo la ripresa: la posa voluta segue quella vera
        // (HandStillStep lo chiude quando la mano si muove).
        if (now < held.settleUntil && regripOn) {
            AnchorAtPalm(h, held, hp, ha, op, oa, palm);
            held.grabAt = now;
            held.regripT = 0.0;
            held.regripErrF = -1.0f;
            held.wedgeT = 0.0;
            return 1;
        }
        held.settleUntil = 0.0;
    }
    if (held.restPending && !still) RestOnPalm(h, held, now);
    // Errore della presa in media esponenziale (0,05 s): una bottiglia che
    // vibra a 1-2 cm per tick contro le vicine non "si avvicina" a ogni minimo.
    if (held.regripErrF < 0.0f) held.regripErrF = gripErr;
    else held.regripErrF += (gripErr - held.regripErrF) * (1.0f - std::exp(-(std::max)(0.0f, dt) / 0.05f));
    if (regripOn) {
        // Bloccato = nessun progresso verso il bersaglio. Finito l'arrivo
        // nella presa (blendT: il bersaglio e' fermo rispetto alla mano), il
        // punto della presa resta oltre regripErr (4 cm, in media su 0,05 s;
        // prima breakErr, 8 cm: una bottiglia della cassa vibrava a 6-8 cm dal
        // bersaglio con la mano ferma per 12 s, 444 inversioni,
        // p2r2/old_F_still_1) e il suo errore non cala di 2 cm in regripTime,
        // fuori da un distacco e non contro un Pawn. Non "fermo" come per il
        // rilascio: incastrata tra le vicine la bottiglia vibrava a 1-2 cm per
        // tick (120 Hz) senza avvicinarsi, la media delle velocita' restava
        // sopra 0,15 m/s e la ripresa arrivava solo dopo ~1 s (25/10 sera,
        // st_0). Un oggetto che arriva, anche pesante e lento (3 m/s^2 netti
        // con full: 6 cm in 0,2 s), cala di 2 cm prima.
        const float eReg = (std::max)(0.5f, g_tune.regripErr) * 0.01f * s;
        // Contro un Pawn il tempo non si accumula (come prima, ma letto al piu'
        // ogni 0,1 s). Contato a ogni tick e controllato solo alla decisione, la
        // guida passava di colpo da "bloccato" (al piu' 0,1 m/s) a piena ogni
        // volta che la cache cambiava: la radio premuta contro il manichino con
        // la mano ferma gli dava un colpo a 1,5-2 m/s (heldswing, prima 0).
        if (brk != Brk::kActive && held.regripErrF >= eReg && now - held.grabAt >= held.blendT && !touchingPawn()) {
            if (held.regripT <= 0.0 || held.regripErrF < held.regripErr0 - 0.02f * s) {  // finestra nuova (prima volta, o si avvicina)
                held.regripErr0 = held.regripErrF;
                held.regripT = 0.0;
            }
            held.regripT += dt;  // HandStillStep lo azzera quando la mano si muove
            const bool due = still && held.regripT >= g_tune.regripTime - 1e-4;
            if (palmOn) {
                held.wedgeT = 0.0;  // il palmo lo tocca: non lo si lascia
                if (due && SolidHeld(held)) {
                    // Voce 5 ("solidhands"): la mano disegnata sta sull'oggetto
                    // (premuto su un tavolo resta li' sopra). La ripresa dove si
                    // trova sposterebbe la presa nel vuoto sotto il tavolo e la
                    // mano disegnata ci sprofonderebbe (radio premuta 12 cm nella
                    // cassa del fucile: dopo 0,45 s la mano tornava sul
                    // controller, 12 cm dentro la cassa, e la radio restava 12 cm
                    // sopra il palmo per tutta la presa). Invece: niente spinte
                    // contro l'ostacolo a mano ferma (come col palmo fuori
                    // portata), e la presa resta quella; mossa la mano, la guida
                    // riparte com'era.
                    held.stuck = g_tune.stuckSpeed > 0.0f;
                    return 0;
                }
                if (due) {
                    Regrip(h, held, now, hp, ha, op, oa, palm, gripErr, palmDist);
                    return 1;
                }
                return 0;
            }
            // Il palmo fuori portata, la mano ferma e bloccato da regripTime:
            // niente spinte contro l'ostacolo. Non prima: un oggetto pesante
            // che parte da fermo con 3 m/s^2 netti fa 1,5 cm in 0,1 s.
            held.stuck = due && g_tune.stuckSpeed > 0.0f;
        } else {
            held.regripT = 0.0;
        }
    }
    if (!(g_tune.wedgeTime > 0.0f) || (regripOn && palmOn)) {
        held.wedgeT = 0.0;  // rilascio spento, o il palmo lo tocca: non lo si lascia
        return 0;
    }
    const float vl = std::sqrt(Dot3(held.wedgeVel, held.wedgeVel));
    if (gripErr < 0.75f * eMin || vl > 0.3f * s) {
        held.wedgeT = 0.0;
        return 0;
    }
    if (brk == Brk::kActive || gripErr < eMin || vl > 0.15f * s) return 0;  // in bilico: non conta e non azzera
    if (touchingPawn()) return 0;
    // Il primo secondo dopo la presa (weightAt: la ripresa non lo sposta) non
    // conta: la presa canonica vuole girare la bottiglia della cassa di
    // 110-130 gradi, le vicine la bloccano e l'errore della presa arriva a
    // 20-30 cm con la mano ancora ferma. Senza questa attesa la mano la
    // lasciava dopo 0,6-0,9 s, prima ancora di sollevarla (smash loop, 4 corse
    // su 12); sollevata, di solito si libera.
    if (now - held.weightAt < 1.0) return 0;
    held.wedgeT += dt;
    const float e2 = 2.0f * eMin;
    const float T = gripErr >= e2 ? (std::max)(0.3f, g_tune.wedgeTime * e2 / gripErr) : g_tune.wedgeTime * (1.0f + 2.0f * (e2 - gripErr) / (e2 - eMin));
    if (held.wedgeT < T) return 0;
    char b[300];
    std::snprintf(b, sizeof(b), "GRAB: %s hand lets go of %s: wedged for %.2f s, grip error %.1f cm, palm %.1f cm from it, moving %.2f m/s (limit %.2f s)",
                  h ? "left" : "right", vr::script::ObjectName(reinterpret_cast<void*>(U32(held.comp, kOffSmcStaticMesh))).c_str(), held.wedgeT,
                  gripErr / s * 100.0f, palmDist / s * 100.0f, vl / s, T);
    vr::log::Line(b);
    // Lasciato fermo (revisione 25/10): l'oggetto non si muove (velocita' dalle
    // posizioni sotto 0,15 m/s), ma PhysX gli lascia la velocita' dei contatti
    // che vibrano; seguito come oggetto che cade, la bottiglia "urtava" a
    // 5,6 m/s 4 ms dopo il rilascio e si rompeva da sola.
    const float zero[3] = {0.0f, 0.0f, 0.0f};
    SetLinVel(held.comp, zero);
    SetAngVel(held.comp, zero);
    Release(h, "wedged");
    return 2;
}

// Scudo (voce 3 dei combattimenti): spinta del proiettile sulla posa voluta
// dell'oggetto tenuto. Sale in 30 ms e torna in ~0,2 s (ShieldKickShape):
// la guida lo porta li' e indietro, e con solidhands la mano disegnata segue
// l'oggetto. Un colpo fuori centro lo gira anche (al piu' 0,15 rad). Solo un
// impulso a PhysX (AddImpulse) durava un tick: la guida riprende la velocita'
// a ogni tick.
void ShieldKick(Held& held, double now, float tp[3], float ta[3][3]) {
    if (held.kickAt < 0.0) return;
    const float t = static_cast<float>(now - held.kickAt);
    const float f = vr::pure::ShieldKickShape(t, 0.03f);
    if (!(f > 0.0f)) {
        if (t > 0.03f) held.kickAt = -1.0;
        return;
    }
    for (int c = 0; c < 3; ++c) tp[c] += held.kickDir[c] * held.kickAmp * f;
    if (held.kickAng > 0.0f) {
        const Quat q = AxisAngle(held.kickAxis, held.kickAng * f);
        for (int i = 0; i < 3; ++i) {
            float r[3];
            Rotate(q, ta[i], r);
            std::memcpy(ta[i], r, sizeof(r));
        }
    }
}

// Voce 9 (rumore per distrarre): definita piu' sotto, accanto alla finta granata.
void PropNoise(void* walker, void* actor, void* comp, const char* how, float dv, bool breaks, double now);

void Drive(int h, const float* hp, const float ha[3][3], float dt) {
    Held& held = g_held[h];
    float op[3], oa[3][3];
    if (!ActorUsable(held.actor) || !CompPose(held.comp, op, oa)) {
        Release(h, "object gone");
        return;
    }
    if (++held.ticks == 5 || held.ticks == 30) MassReread(h);
    if (held.constrained) {
        ConstrainedDrive(h, hp, ha, dt, op, oa);
        return;
    }
    GripStep(held, h, ProfMs() * 0.001, dt);  // voce 6: forza della presa (prima della posa voluta: la piega ne dipende)
    if (held.wantCanonical && g_grip[h].valid) {
        held.wantCanonical = false;
        float rpos[3], rax[3][3], anc[3], ht = 0.0f;
        int kind = CanonicalGrip(h, held.comp, hp, ha, op, oa, rpos, rax, anc, &ht);
        if (kind) {
            // Si parte dalla posa attuale rispetto alla mano.
            float d[3] = {op[0] - hp[0], op[1] - hp[1], op[2] - hp[2]};
            for (int r = 0; r < 3; ++r) held.relPosFrom[r] = d[0] * ha[r][0] + d[1] * ha[r][1] + d[2] * ha[r][2];
            for (int i = 0; i < 3; ++i)
                for (int r = 0; r < 3; ++r) held.relAxesFrom[i][r] = oa[i][0] * ha[r][0] + oa[i][1] * ha[r][1] + oa[i][2] * ha[r][2];
            std::memcpy(held.relPos, rpos, sizeof(rpos));
            std::memcpy(held.relAxes, rax, sizeof(rax));
            std::memcpy(held.anchor, anc, sizeof(anc));
            held.halfThickCm = ht / vr::control::g_posScale * 100.0f;
            held.powerGrip = true;
            held.gripKind = kind;
            held.grabAt = ProfMs() * 0.001;
            float tr = Dot3(rax[0], held.relAxesFrom[0]) + Dot3(rax[1], held.relAxesFrom[1]) + Dot3(rax[2], held.relAxesFrom[2]);
            held.blendT = 0.10f + 0.10f * std::acos((std::max)(-1.0f, (std::min)(1.0f, (tr - 1.0f) * 0.5f))) / 3.14159265f;
            held.weightAt = held.grabAt;  // come alla presa
            held.weightBlendT = held.blendT;
        }
    }
    float tp[3], ta[3][3], rp[3], ra[3][3], sagU = 0.0f, droopR = 0.0f;
    HeldTarget(held, ha, rp, ra, &sagU, &droopR);
    for (int c = 0; c < 3; ++c) tp[c] = hp[c] + rp[0] * ha[0][c] + rp[1] * ha[1][c] + rp[2] * ha[2][c];
    for (int i = 0; i < 3; ++i)
        for (int c = 0; c < 3; ++c) ta[i][c] = ra[i][0] * ha[0][c] + ra[i][1] * ha[1][c] + ra[i][2] * ha[2][c];
    ShieldKick(held, ProfMs() * 0.001, tp, ta);  // voce 3 dei combattimenti: colpo di proiettile sullo scudo
    float hv[3] = {0, 0, 0};
    HandVelocity(h, 0.03, hv);
    {
        // Sbattuto (fase 4): la mano va veloce ma l'oggetto si e' fermato contro
        // qualcosa: dopo il passo di PhysX la sua velocita' lungo quella data al
        // tick prima e' sotto il 40%, per due tick di fila. Velocita' lette da
        // PhysX (NxActor +0xE8): quella dell'attore UE3 arriva un tick dopo e
        // ogni accelerazione brusca sembrava un urto.
        const float s = vr::control::g_posScale;
        // Con il peso, dal secondo tick il confronto e' con il comando di prima
        // dell'urto (smashRef): dopo l'urto il comando riparte dalla velocita'
        // misurata piu' un tick di forza (~2 m/s per la bottiglia) e la
        // conferma del secondo tick non arrivava mai (bottiglia non rotta, 25/10).
        // Il comando del peso (weightCmdV, uguale a lastCmdV fuori dal
        // distacco): strappato via con la forza del distacco, un oggetto
        // incastrato non sembra fermato contro qualcosa.
        const bool second = held.weight && held.stoppedTicks > 0;
        const float* ref = second ? held.smashRef : held.weightCmdV;
        float hs = std::sqrt(hv[0] * hv[0] + hv[1] * hv[1] + hv[2] * hv[2]);
        float cs = std::sqrt(ref[0] * ref[0] + ref[1] * ref[1] + ref[2] * ref[2]);
        bool stopped = false;
        // Oltre 8 m/s e' un salto del tracciamento, non un colpo. Con il peso
        // (revisione 25/10) a decidere e' il comando di prima dell'urto (cs oltre
        // 3 m/s: con il peso parte dalla velocita' misurata, quindi l'oggetto
        // andava davvero cosi' veloce; uno bloccato non ci arriva), e alla mano
        // si chiede solo un colpo in corso: 1,5 m/s al primo tick, niente al
        // secondo. La mano finta dei collaudi salta tra 4,9 e 2,4 m/s ogni 4
        // tick (GetTickCount64) e i tick cadevano su quella bassa: bottiglia
        // fermata contro la cassa a 3,2 m/s, non rotta.
        const float handMin = held.weight ? 1.5f : 3.0f;
        const bool handFast = second ? hs < 8.0f * s : (hs > handMin * s && hs < 8.0f * s);
        if (handFast && cs > 3.0f * s && Breakable(held.actor)) {
            void* nx = CompNx(held.comp);
            float lin[3], ang[3];
            if (nx && NxVelocities(nx, lin, ang)) {
                float along = (lin[0] * ref[0] + lin[1] * ref[1] + lin[2] * ref[2]) * 50.0f / cs;
                // Conferma (secondo tick, con il peso): rispetto al piu' veloce
                // tra il comando di prima dell'urto e la mano. Il comando con
                // il peso e' limitato (arresto sicuro, forza per tick): la soglia
                // del 40% restava piu' vicina alla velocita' dell'oggetto
                // deviato dall'urto che senza peso (revisione 25/10). Il primo
                // tick resta sul comando vero: un oggetto che resta indietro
                // per il peso non e' un urto.
                stopped = along < 0.4f * (second ? (std::max)(cs, hs) : cs);
                if (vr::control::g_grabLog > 0) {
                    char b[200];
                    std::snprintf(b, sizeof(b), "GRABSMASH: tick %d: hand %.2f m/s, reference %.2f m/s, along %.2f m/s -> %s",
                                  held.stoppedTicks + 1, hs / s, cs / s, along / s, stopped ? "stopped" : "moving");
                    vr::log::Line(b);
                }
            }
        }
        if (stopped && held.stoppedTicks == 0) std::memcpy(held.smashRef, held.weightCmdV, sizeof(held.smashRef));
        held.stoppedTicks = stopped ? held.stoppedTicks + 1 : 0;
        if (held.stoppedTicks >= 2) {
            void* a = held.actor;
            void* c = held.comp;
            Release(h, "smashed");
            vr::input::PulseHand(h ? 0 : 1, 70);  // si rompe in mano
            // Voce 9: la bottiglia rotta in mano si sente (cambio = la velocita' di prima dell'urto).
            PropNoise(g_tickPawn, a, c, "smashed in the hand", cs, true, ProfMs() * 0.001);
            BreakProp(a, c, "smashed against something in the hand");
            return;
        }
    }
    // Velocita' angolare della mano dal tick prima (rad/s), un po' smorzata.
    // Riparte a ogni presa (Held nuovo), anche se la mano riprende lo stesso
    // oggetto: prima ripartiva solo cambiando oggetto, e ripreso lo stesso il
    // primo tick misurava la rotazione dalla posa della presa precedente in un
    // solo dt (fino a ~60 rad/s dopo lo smorzamento: un colpo finto della
    // pipeline con un nemico vicino; revisione 24/10). Fuori da Drive
    // (g_wristPrevHa, g_wristW): un salto del riferimento (FrameJump) li ruota.
    float (&prevHa)[2][3][3] = g_wristPrevHa;
    float (&handW)[2][3] = g_wristW;
    if (!held.wristInit) {
        std::memcpy(prevHa[h], ha, sizeof(prevHa[h]));
        std::memset(handW[h], 0, sizeof(handW[h]));
        held.wristInit = true;
    }
    {
        float Rh[3][3] = {}, axh[3], angh = 0.0f;
        for (int i = 0; i < 3; ++i)
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) Rh[r][c] += ha[i][r] * prevHa[h][i][c];
        float wNow[3] = {0, 0, 0};
        if (AxisAngleOf(Rh, axh, &angh) && angh < 1.5f)
            for (int c = 0; c < 3; ++c) wNow[c] = axh[c] * angh / dt;
        for (int c = 0; c < 3; ++c) handW[h][c] += (wNow[c] - handW[h][c]) * 0.5f;
        std::memcpy(prevHa[h], ha, sizeof(prevHa[h]));
        std::memcpy(g_heldHandW[h], handW[h], sizeof(handW[h]));
    }
    const float* wh = handW[h];
    const float k = (std::max)(0.1f, (std::min)(1.0f, g_tune.gain));
    float err[3] = {tp[0] - op[0], tp[1] - op[1], tp[2] - op[2]};
    float errLen = std::sqrt(err[0] * err[0] + err[1] * err[1] + err[2] * err[2]);
    double now = ProfMs() * 0.001;
    // Peso: velocita' dopo il passo di PhysX, prima del comando di questo tick
    // (SetRBLinearVelocity vale subito: letta dopo, da' il comando stesso).
    float measV[3] = {0, 0, 0}, measW[3] = {0, 0, 0};
    bool haveMeas = false;
    const bool hap = HapticGain() > 0.0f;  // vibrazione degli urti (voce 3): serve anche senza peso
    if (held.weight || hap) {
        void* nx = CompNx(held.comp);
        float lin[3], angv[3];
        if (nx && NxVelocities(nx, lin, angv) && std::isfinite(lin[0] + lin[1] + lin[2] + angv[0] + angv[1] + angv[2])) {
            for (int c = 0; c < 3; ++c) {
                measV[c] = lin[c] * 50.0f;
                measW[c] = angv[c];
            }
            haveMeas = true;
        }
    }
    if (hap && haveMeas && held.hasCmd) HeldImpact(h, held, measV, dt, now, op);
    if (!held.weight) {
        // Bloccato (contro un muro, sotto un peso) a oltre 60 cm dalla mano per
        // 0,3 s: la mano lo lascia, come una presa che scivola.
        if (errLen > 0.6f * vr::control::g_posScale) {
            if (held.stuckSince < 0.0) held.stuckSince = now;
            if (now - held.stuckSince > 0.3) {
                Release(h, "slipped");
                return;
            }
        } else {
            held.stuckSince = -1.0;
        }
    } else {
        // Con il peso un oggetto pesante resta piu' indietro: 80 cm oltre i
        // 30 kg (60 sotto), e il tempo conta solo se non si sta avvicinando
        // (fermo contro qualcosa): uno che arriva piano non scivola via.
        // Avvicinarsi (revisione 25/10) = l'errore cala in media di almeno
        // 0,25 m/s dall'inizio della finestra, OPPURE la velocita' misurata
        // verso il bersaglio, in media sulla finestra, e' di almeno 0,25 m/s
        // (la regola di prima, mediata). Solo la prima (seguito della voce 2)
        // lasciava cadere un oggetto pesante che segue ma resta indietro di
        // oltre 80 cm con l'errore fermo (sollevamento lungo e veloce con
        // full): un rifiuto. La bottiglia che vibrava contro la cassa (0,6 m/s
        // misurati verso il bersaglio, ferma a 131 cm) la lascia ora WedgeStep,
        // che guarda le posizioni, non le velocita' di PhysX.
        // Seguito 2 (25/10 sera): la distanza e' quella del punto della presa
        // (held.anchor: quello che finisce nel palmo), non dell'origine. La
        // bottiglia della cassa (47 cm, origine sul fondo) che la presa
        // canonica vuole girare di 110 gradi ha l'origine a 60 cm dal suo
        // bersaglio con il collo gia' in mano: con la mano ferma scivolava
        // via 0,42 s dopo la presa, prima ancora della ripresa (lat_0). Lo
        // scivolamento e' la mano tirata via lontano dall'oggetto.
        float slipE[3];
        for (int c = 0; c < 3; ++c)
            slipE[c] = tp[c] - op[c] + held.anchor[0] * (ta[0][c] - oa[0][c]) + held.anchor[1] * (ta[1][c] - oa[1][c]) +
                       held.anchor[2] * (ta[2][c] - oa[2][c]);
        const float slipLen = std::sqrt(Dot3(slipE, slipE));
        const float slipDist = (MassKg(held) > 30.0f ? 0.8f : 0.6f) * vr::control::g_posScale;
        if (slipLen > slipDist) {
            const float s = vr::control::g_posScale;
            const bool fresh = held.stuckSince < 0.0;
            const float el = fresh ? 0.0f : static_cast<float>(now - held.stuckSince);
            if (!fresh && haveMeas && slipLen > 1e-3f) held.slipCloseSum += Dot3(measV, slipE) / slipLen * dt;
            const bool nearer = !fresh && slipLen <= held.slipErr0 - 0.25f * s * el;
            const bool closing = !fresh && el > 0.0f && held.slipCloseSum >= 0.25f * s * el;
            if (fresh || nearer || closing) {
                held.stuckSince = now;  // finestra nuova (prima volta, o si sta davvero avvicinando)
                held.slipErr0 = slipLen;
                held.slipCloseSum = 0.0f;
            }
            if (now - held.stuckSince > 0.3) {
                Release(h, "slipped");
                return;
            }
        } else {
            held.stuckSince = -1.0;
        }
    }
    // Il punto voluto si muove con la mano: traslazione + rotazione del polso.
    float arm[3] = {tp[0] - hp[0], tp[1] - hp[1], tp[2] - hp[2]}, spin[3];
    Cross3(wh, arm, spin);
    float v[3], pv[3];
    float R[3][3] = {};
    for (int i = 0; i < 3; ++i)
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) R[r][c] += ta[i][r] * oa[i][c];
    float ax[3], ang = 0.0f;
    float w[3] = {wh[0], wh[1], wh[2]};
    bool capped = false, budget = false, budgetW = false;
    float vWeight[3] = {0, 0, 0}, wWeight[3] = {0, 0, 0};  // comando senza il distacco (weightCmdV/W)
    Brk brk = Brk::kNone;
    float brkMul = 1.0f;
    bool still = false, regripped = false;  // ripresa (seguito 2): mano ferma, presa ripresa in questo tick
    float palmDist = 0.0f;                  // palmo dalla scatola dell'oggetto (unita')
    float ancO[3] = {}, ancT[3] = {}, comO[3] = {}, errA[3] = {};  // distacco: punto della presa (oggetto, bersaglio), baricentro
    if (!held.weight) {
        for (int c = 0; c < 3; ++c) v[c] = k * err[c] / dt + hv[c] + spin[c];
        // Limite di velocita' rispetto a Walker (CapScale): la sua corsa non
        // consuma il margine dell'oggetto pesante.
        PawnVelocity(pv);
        const float kc = CapScale(v, pv, held.maxSpeed);
        capped = kc < 1.0f;
        for (float& c : v) c *= kc;
        // Rotazione: R = somma t_i o_i^T porta gli assi attuali su quelli voluti;
        // asse-angolo -> velocita' angolare (rad/s, come PhysX), piu' quella del polso.
        if (AxisAngleOf(R, ax, &ang)) {
            float rate = (std::min)(k * ang / dt, 40.0f);
            for (int c = 0; c < 3; ++c) w[c] += ax[c] * rate;
        }
        std::memcpy(vWeight, v, sizeof(v));
        std::memcpy(wWeight, w, sizeof(w));
    } else {
        // Peso (voce 2 del piano): la mano ha una forza e una coppia massime.
        // 1) Arresto sicuro: rispetto a Walker l'oggetto non va piu' veloce di
        //    quanto possa frenare nell'errore che resta (piu' g_tune.overshoot),
        //    con la decelerazione disponibile nella direzione del moto
        //    (g_tune.brake di quella vera: margine per il ritardo di un tick):
        //    se la mano si ferma di colpo si ferma anche lui. Vale per tutta la
        //    velocita' voluta (anticipo della mano compreso) e in ogni
        //    direzione: limitando solo la correzione, o solo la componente verso
        //    il bersaglio, la cassa da 52 kg oltrepassava di 20-57 cm e tornava
        //    indietro (25/10). In cambio un oggetto pesante resta indietro anche
        //    a velocita' costante, di v^2 / 2a meno overshoot (radio da 8 kg a
        //    1,5 m/s con full: ~1,4 cm; la bottiglia niente).
        // 2) Velocita' di base = quella misurata da PhysX, senza l'energia data
        //    dai contatti: al piu' quella attesa (comando del tick prima piu' la
        //    gravita' di un passo) + 0,5 m/s; nei 0,2 s dopo un salto del
        //    riferimento quella attesa e basta. Un urto o una compenetrazione non
        //    lo fanno schizzare via; un muro che lo ferma si', resta fermo.
        // 3) La variazione per tick e' al piu' aMax x dt (forza / massa), prima
        //    per la gravita' (tenerlo su costa g) e il resto per il moto: tirato
        //    di lato con tutta la forza non cade (la cassa scendeva a 0,8 m/s).
        //    L'accelerazione di Walker (corsa, copertura) e' data gratis, con un
        //    tetto (Actor.Velocity ha picchi di un tick).
        HeldVolumes(held);  // i muri invisibili non fermano l'oggetto tenuto
        const WeightLimits wl = WeightOf(held);
        const float s = vr::control::g_posScale, kg = (std::max)(0.01f, MassKg(held));
        const float over = (std::max)(0.0f, g_tune.overshoot), brake = (std::max)(0.05f, g_tune.brake);
        const float overT = (std::max)(0.0f, g_tune.overTime) * 0.001f;
        float vdes[3], base[3];
        // Velocita' con cui Walker porta le mani: FrameVelocity (il centro del
        // riferimento delle mani), non Actor.Velocity. Con Actor.Velocity, che
        // ha picchi di un tick (20 m/s all'avvio della corsa), l'arresto
        // sicuro, relativo a Walker, voleva l'oggetto a ~19 m/s e la forza lo
        // spingeva con tutto il budget; con 20 m/s forzati per 0,6 s la radio
        // partiva in avanti e scivolava via (revisione 25/10, velspike.py).
        FrameVelocity(pv);
        // Velocita' voluta dalla posa voluta (di nuovo dopo una ripresa in questo tick).
        auto desiredV = [&]() {
            for (int c = 0; c < 3; ++c) vdes[c] = hv[c] + spin[c] + k * err[c] / dt;
            const float rel[3] = {vdes[0] - pv[0], vdes[1] - pv[1], vdes[2] - pv[2]};
            const float rl = std::sqrt(Dot3(rel, rel));
            if (rl > 1e-3f) {
                // Margine: overshoot (2 cm) o quanto l'oggetto fa in overTime
                // (15 ms, quanto restava indietro la media di 30 ms della mano):
                // SafeStopCap. Gli oggetti leggeri restano liberi fin oltre
                // 4 m/s (bottiglia).
                const float aB = brake * BrakeAccel(wl.aMax, wl.g, rel[2] / rl);
                const float cap = SafeStopCap(aB, errLen, over * 0.01f * s, overT);
                if (rl > cap)
                    for (int c = 0; c < 3; ++c) vdes[c] = pv[c] + rel[c] * cap / rl;
            }
        };
        desiredV();
        // Nei 0,2 s dopo un salto del riferimento (FrameJump) la velocita'
        // misurata conta solo se e' piu' lenta di quella attesa (l'oggetto
        // portato si e' fermato contro qualcosa): l'energia in piu' delle
        // compenetrazioni nuove no. Prima la si ignorava sempre, e un oggetto
        // bloccato vedeva crescere il comando fino al limite dell'arresto
        // sicuro, per partire di scatto una volta libero (revisione 25/10).
        const bool jumping = now < held.jumpUntil;
        if (held.hasCmd) {
            const float vexp[3] = {held.lastCmdV[0], held.lastCmdV[1], held.lastCmdV[2] - wl.g * dt};
            const bool useExp = !haveMeas || (jumping && Dot3(measV, measV) > Dot3(vexp, vexp));
            std::memcpy(base, useExp ? vexp : measV, sizeof(base));
            const float lm = std::sqrt(Dot3(base, base)), le = std::sqrt(Dot3(vexp, vexp)) + 0.5f * s;
            if (lm > le)
                for (float& c : base) c *= le / lm;
        } else {
            // Primo tick: com'era (fermo o quasi), al piu' 1 m/s oltre la mano.
            std::memcpy(base, haveMeas ? measV : hv, sizeof(base));
            const float lm = std::sqrt(Dot3(base, base)), le = std::sqrt(Dot3(hv, hv)) + 1.0f * s;
            if (lm > le)
                for (float& c : base) c *= le / lm;
        }
        if (!held.hasCmd) std::memcpy(held.prevPawnV, pv, sizeof(pv));
        float dpv[3];
        for (int c = 0; c < 3; ++c) dpv[c] = pv[c] - held.prevPawnV[c];
        std::memcpy(held.prevPawnV, pv, sizeof(pv));
        {
            const float l = std::sqrt(Dot3(dpv, dpv)), lim = (std::max)(0.0f, g_tune.pawnAcc) * s * dt;
            if (l > lim)
                for (float& c : dpv) c *= lim / l;
        }
        // Distacco (BreakawayStep): oggetto incastrato mentre la mano tira via.
        // Conta il punto della presa (held.anchor: quello che finisce nel
        // palmo; l'origine per le prese sul palmo), non l'origine: una
        // bottiglia che la presa canonica vuole girare di 100-160 gradi tra le
        // vicine ha l'origine lontana dal bersaglio anche quando il collo e'
        // gia' in mano.
        for (int c = 0; c < 3; ++c) {
            ancO[c] = op[c] + held.anchor[0] * oa[0][c] + held.anchor[1] * oa[1][c] + held.anchor[2] * oa[2][c];
            ancT[c] = tp[c] + held.anchor[0] * ta[0][c] + held.anchor[1] * ta[1][c] + held.anchor[2] * ta[2][c];
            comO[c] = op[c] + held.comLocal[0] * oa[0][c] + held.comLocal[1] * oa[1][c] + held.comLocal[2] * oa[2][c];
            errA[c] = ancT[c] - ancO[c];
        }
        // Ripresa dove si trova (seguito 2): il palmo nel mondo, la mano ferma
        // o in moto (con la mano ferma nessuno strappo nuovo), la distanza del
        // palmo dall'oggetto (lo tocca ancora: la presa si puo' riprendere li').
        float palmW[3];
        for (int c = 0; c < 3; ++c) {
            palmW[c] = hp[c];
            if (g_palmValid[h]) palmW[c] += g_palmLocal[h][0] * ha[0][c] + g_palmLocal[h][1] * ha[1][c] + g_palmLocal[h][2] * ha[2][c];
        }
        still = HandStillStep(held, now, dt, palmW, hv);
        const float gripErr = std::sqrt(Dot3(errA, errA));
        brk = BreakawayStep(held, wl.g, dt, now, held.hasCmd && haveMeas && !jumping, measV, pv, gripErr, ancO, ancT, still, &brkMul);
        palmDist = PalmToObject(held, palmW, op, oa);
        const int wedge = WedgeStep(h, held, now, dt, ancO, gripErr, brk, still, hp, ha, op, oa, palmW, palmDist);
        if (wedge == 2) return;  // incastrato, il palmo fuori portata e weightwedge > 0: lasciato
        regripped = wedge == 1;  // presa ripresa dove si trova (o in assestamento): la posa voluta e' quella vera
        if (regripped) {
            // Il comando di questo tick parte dalla posa voluta nuova (= quella
            // vera), non da quella bloccata calcolata prima di WedgeStep:
            // revisione del seguito 2, dopo la ripresa ancora 2-4 tick spinti
            // contro l'ostacolo, passi di 1,2-3,6 cm (smR4, F_still_2, F_lift_4).
            HeldTarget(held, ha, rp, ra, &sagU, &droopR);
            for (int c = 0; c < 3; ++c) tp[c] = hp[c] + rp[0] * ha[0][c] + rp[1] * ha[1][c] + rp[2] * ha[2][c];
            for (int i = 0; i < 3; ++i)
                for (int c = 0; c < 3; ++c) ta[i][c] = ra[i][0] * ha[0][c] + ra[i][1] * ha[1][c] + ra[i][2] * ha[2][c];
            for (int c = 0; c < 3; ++c) {
                err[c] = tp[c] - op[c];
                arm[c] = tp[c] - hp[c];
                ancT[c] = tp[c] + held.anchor[0] * ta[0][c] + held.anchor[1] * ta[1][c] + held.anchor[2] * ta[2][c];
                ancO[c] = op[c] + held.anchor[0] * oa[0][c] + held.anchor[1] * oa[1][c] + held.anchor[2] * oa[2][c];
                errA[c] = ancT[c] - ancO[c];
            }
            errLen = std::sqrt(Dot3(err, err));
            Cross3(wh, arm, spin);
            std::memset(R, 0, sizeof(R));
            for (int i = 0; i < 3; ++i)
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 3; ++c) R[r][c] += ta[i][r] * oa[i][c];
            desiredV();
        }
        if (held.stuck) {
            // Bloccato con la mano ferma (WedgeStep): verso il bersaglio al piu'
            // stuckSpeed, rispetto a Walker. Spinto con tutta la forza dentro
            // l'ostacolo rimbalzava di 1-2 cm per tick finche' la mano restava
            // ferma (25/10 sera, dn_1: 406 inversioni in 3 s).
            const float rel[3] = {vdes[0] - pv[0], vdes[1] - pv[1], vdes[2] - pv[2]};
            const float rl = std::sqrt(Dot3(rel, rel)), cap = g_tune.stuckSpeed * s;
            if (rl > cap)
                for (int c = 0; c < 3; ++c) vdes[c] = pv[c] + rel[c] * cap / rl;
        }
        if (brk == Brk::kFreed) {
            // Libero (o rinuncia): via l'eccesso dello strappo. La base (la
            // velocita' misurata) non supera, rispetto a Walker, quella voluta
            // dal peso con l'arresto sicuro: da li' l'oggetto si ferma alla
            // mano. Prima si ripartiva dalla velocita' della mano: una
            // bottiglia appena uscita di 3 cm si fermava, ribloccava e
            // ripartiva, a scatti ogni 30 ms (25/10).
            float br[3], vrel[3];
            for (int c = 0; c < 3; ++c) {
                br[c] = base[c] - pv[c];
                vrel[c] = vdes[c] - pv[c];
            }
            const float lb = std::sqrt(Dot3(br, br)), lv = std::sqrt(Dot3(vrel, vrel));
            if (lb > lv)
                for (int c = 0; c < 3; ++c) base[c] = pv[c] + br[c] * lv / lb;
        }
        // Prima la gravita' (gc: la velocita' persa in un passo), poi il moto (r).
        const float gc[3] = {0.0f, 0.0f, wl.g * dt};
        float r[3], dv[3];
        for (int c = 0; c < 3; ++c) {
            r[c] = vdes[c] - base[c] - dpv[c] - gc[c];
            dv[c] = gc[c] + r[c];
        }
        {
            const float lim = wl.aMax * dt;
            budget = Dot3(dv, dv) > lim * lim;
            if (budget) {
                // Il t piu' grande in 0..1 con |gc + t r| <= lim (gc da solo ci
                // sta: aMax supera sempre g, WeightOf).
                const float A = Dot3(r, r), B = 2.0f * Dot3(gc, r), C = Dot3(gc, gc) - lim * lim;
                float t = 0.0f;
                if (A > 1e-9f && C < 0.0f) t = (std::min)(1.0f, (std::max)(0.0f, (-B + std::sqrt(B * B - 4.0f * A * C)) / (2.0f * A)));
                for (int c = 0; c < 3; ++c) dv[c] = gc[c] + t * r[c];
            }
        }
        for (int c = 0; c < 3; ++c) v[c] = base[c] + dpv[c] + dv[c];
        const float kc = CapScale(v, pv, held.maxSpeed);
        capped = kc < 1.0f;
        for (float& c : v) c *= kc;
        std::memcpy(vWeight, v, sizeof(v));
        // Rotazione: lo stesso arresto sicuro (polso compreso, in ogni
        // direzione) con l'accelerazione angolare attorno all'asse del moto
        // (coppia / inerzia dei limiti, mai sotto alphaMin) e
        // g_tune.overshoot x 2,5 gradi; poi la
        // variazione per tick limitata dalla coppia, nel riferimento
        // dell'oggetto (I diagonale negli assi della mesh).
        if (AxisAngleOf(R, ax, &ang)) {
            // Bloccato con la mano ferma: anche la rotazione piano (0,5 rad/s per 0,1 m/s di stuckSpeed).
            // Presa lenta (voce 6): il guadagno verso la posa voluta x turnGain
            // (0,5 alla piu' lenta): l'oggetto torna piano all'angolo della mano.
            // Solo per l'oggetto che si muove libero: bloccato (distacco in
            // corso, o fermo contro qualcosa con la mano ferma) gira come con la
            // presa stretta. Revisione 26/10: la bottiglia della cassa, girata
            // piano nella presa canonica mentre lo strappo la tirava fuori, si
            // incastrava tra le vicine e la mano la lasciava (smash in mano con
            // grip 0,6: rotta 1 volta su 6, 3 su 3 senza questo fattore; con la
            // presa stretta 3 su 3).
            const float turnGain = (brk == Brk::kActive || held.stuck) ? 1.0f : wl.turnGain;
            const float rate = (std::min)(k * turnGain * ang / dt, held.stuck ? 5.0f * g_tune.stuckSpeed : 40.0f);
            for (int c = 0; c < 3; ++c) w[c] += ax[c] * rate;
        }
        {
            const float wn = std::sqrt(Dot3(w, w));
            if (wn > 1e-3f) {
                const float ao[3] = {Dot3(w, oa[0]) / wn, Dot3(w, oa[1]) / wn, Dot3(w, oa[2]) / wn};
                const float In = kg * (held.inertia[0] * ao[0] * ao[0] + held.inertia[1] * ao[1] * ao[1] + held.inertia[2] * ao[2] * ao[2]);
                const float aB = brake * (std::max)(wl.tauMax / (std::max)(In, 1e-5f), wl.alphaMin);
                const float cap = SafeStopCap(aB, ang, over * 2.5f * 0.0174533f, overT);
                if (wn > cap)
                    for (float& c : w) c *= cap / wn;
            }
        }
        float wbase[3];
        if (held.hasCmd) {
            const bool useExp = !haveMeas || (jumping && Dot3(measW, measW) > Dot3(held.lastCmdW, held.lastCmdW));
            std::memcpy(wbase, useExp ? held.lastCmdW : measW, sizeof(wbase));
            const float lm = std::sqrt(Dot3(wbase, wbase)), le = std::sqrt(Dot3(held.lastCmdW, held.lastCmdW)) + 1.0f;
            if (lm > le)
                for (float& c : wbase) c *= le / lm;
        } else {
            std::memcpy(wbase, measW, sizeof(wbase));
            const float lm = std::sqrt(Dot3(wbase, wbase));
            if (lm > 10.0f)
                for (float& c : wbase) c *= 10.0f / lm;
        }
        if (brk == Brk::kFreed) {
            // Libero: la rotazione di base al piu' quella voluta dal peso (arresto sicuro).
            const float lb = std::sqrt(Dot3(wbase, wbase)), lv = std::sqrt(Dot3(w, w));
            if (lb > lv)
                for (float& c : wbase) c *= lv / lb;
        }
        // Variazione limitata dalla coppia (moltiplicata nel distacco), mai sotto alphaMin x dt.
        auto torqueLimit = [&](float* x, float m) {
            float tq[3];
            for (int i = 0; i < 3; ++i) tq[i] = kg * held.inertia[i] * Dot3(x, oa[i]) / dt;
            const float tl = std::sqrt(Dot3(tq, tq));
            if (!(tl > wl.tauMax * m)) return false;
            const float dl = std::sqrt(Dot3(x, x));
            const float f = (std::min)(1.0f, (std::max)(wl.tauMax * m / tl, dl > 1e-6f ? wl.alphaMin * m * dt / dl : 1.0f));
            for (int c = 0; c < 3; ++c) x[c] *= f;
            return true;
        };
        float dw[3] = {w[0] - wbase[0], w[1] - wbase[1], w[2] - wbase[2]};
        // Al piu' la coppia, ma la variazione non scende mai sotto
        // alphaMin x dt (pavimento: anche la cassa piu' grande gira).
        budgetW = torqueLimit(dw, 1.0f);
        for (int c = 0; c < 3; ++c) w[c] = wbase[c] + dw[c];
        std::memcpy(wWeight, w, sizeof(w));
        if (brk == Brk::kActive) {
            // Distacco, rotazione: resta quella del peso (breakturn 0, il
            // predefinito). Girare a forza nella posa canonica una bottiglia
            // ancora tra le vicine la faceva sbattere contro di loro a 3-10 m/s
            // (25/10); tirata fuori per il punto della presa, poi gira da sola.
            // breakturn 1: verso la rotazione della guida senza peso (polso piu'
            // meta' dell'angolo per tick, al piu' 40 rad/s), coppia moltiplicata.
            if (g_tune.breakTurn > 0.5f) {
                float woff[3] = {wh[0], wh[1], wh[2]};
                if (ang > 0.0f)
                    for (int c = 0; c < 3; ++c) woff[c] += ax[c] * (std::min)(k * ang / dt, 40.0f);
                float db[3] = {woff[0] - wbase[0], woff[1] - wbase[1], woff[2] - wbase[2]};
                torqueLimit(db, brkMul);
                for (int c = 0; c < 3; ++c) w[c] = wbase[c] + db[c];
            }
            // Distacco, moto: il punto della presa verso il palmo come la guida
            // senza peso (meta' errore per tick piu' la mano e il polso), con il
            // budget moltiplicato; quando basta, e' quel comando (saturo). Al
            // baricentro (la velocita' di PhysX): quella del punto meno
            // w x (punto - baricentro). Rispetto a Walker al piu' la velocita'
            // della mano + breakCap (4 m/s; revisione 25/10: prima maxSpeed,
            // 30 m/s per la bottiglia, la legge che con off faceva saltare
            // l'oggetto incastrato di 55 cm in un tick). Lo strappo e' la forza
            // (il budget), non la velocita': liberato, non vola oltre la mano.
            float armT[3] = {ancT[0] - hp[0], ancT[1] - hp[1], ancT[2] - hp[2]}, spinA[3], arm2[3], wr[3], voff[3], d[3];
            Cross3(wh, armT, spinA);
            for (int c = 0; c < 3; ++c) arm2[c] = ancO[c] - comO[c];
            Cross3(w, arm2, wr);
            for (int c = 0; c < 3; ++c) voff[c] = k * errA[c] / dt + hv[c] + spinA[c] - wr[c];
            const float hrel[3] = {hv[0] - pv[0], hv[1] - pv[1], hv[2] - pv[2]};
            const float strap = (std::min)(held.maxSpeed, std::sqrt(Dot3(hrel, hrel)) + (std::max)(0.5f, g_tune.breakCap) * s);
            const float ko = CapScale(voff, pv, strap);
            for (float& c : voff) c *= ko;
            for (int c = 0; c < 3; ++c) d[c] = voff[c] - base[c] - dpv[c];
            const float dl = std::sqrt(Dot3(d, d)), lim = wl.aMax * dt * brkMul;
            if (dl > lim) {
                for (float& c : d) c *= lim / dl;
            } else if (held.brkSat < 0.0) {
                held.brkSat = now;
            }
            for (int c = 0; c < 3; ++c) v[c] = base[c] + dpv[c] + d[c];
            const float kb = CapScale(v, pv, held.maxSpeed);
            for (float& c : v) c *= kb;
        }
    }
    if (held.meleeBrakeTicks > 0 || now < held.meleeBrakeUntil) {
        // Freno della mischia (voce 4, HeldMelee): per 2 tick (almeno 25 ms)
        // l'oggetto che ha colpito un nemico non va avanti lungo il colpo
        // (rispetto a Walker: resta il 10%) e gira al 30%: si vede fermarsi
        // sul corpo invece di passarci attraverso, poi la guida lo riporta
        // alla mano. Il comando del peso (weightCmdV/W, colpi e rilascio)
        // resta quello senza freno.
        if (held.meleeBrakeTicks > 0) --held.meleeBrakeTicks;
        const float rel[3] = {v[0] - pv[0], v[1] - pv[1], v[2] - pv[2]};
        const float along = Dot3(rel, held.meleeBrakeDir);
        if (along > 0.0f)
            for (int c = 0; c < 3; ++c) v[c] -= 0.9f * along * held.meleeBrakeDir[c];
        for (float& c : w) c *= 0.3f;
    }
    float wl = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
    if (wl > 55.0f) for (float& c : w) c *= 55.0f / wl;  // sotto i 60 rad/s di RaiseMaxAngVel
    wl = std::sqrt(Dot3(wWeight, wWeight));
    if (wl > 55.0f) for (float& c : wWeight) c *= 55.0f / wl;
    SetLinVel(held.comp, v);
    std::memcpy(held.lastCmdV, v, sizeof(v));  // il comando dopo i limiti (anche per l'oggetto sbattuto)
    std::memcpy(held.lastCmdW, w, sizeof(w));
    std::memcpy(held.weightCmdV, vWeight, sizeof(vWeight));  // senza il distacco: rilascio, urto, colpi
    std::memcpy(held.weightCmdW, wWeight, sizeof(wWeight));
    held.hasCmd = true;
    float realV[3] = {0, 0, 0}, realW[3] = {0, 0, 0};  // cio' che PhysX ha fatto dell'ultimo comando
    if (vr::control::g_grabLog > 0) {
        uint32_t bi = 0, nx = 0;
        if (TrySafeReadUint32(static_cast<char*>(held.comp) + kOffCompBodyInstance, &bi) && bi &&
            TrySafeReadUint32(reinterpret_cast<char*>(bi) + 0x60, &nx) && nx)
            NxVelocities(reinterpret_cast<void*>(nx), realV, realW);
    }
    SetAngVel(held.comp, w);
    Wake(held.comp);
    float (&prevTa)[2][3][3] = g_prevTa;
    if (vr::control::g_grabLog > 0) {
        vr::control::g_grabLog.fetch_sub(1);
        // Quanto e' girata la posa voluta dal tick prima (mano che trema = bersaglio che trema).
        float tt = Dot3(ta[0], prevTa[h][0]) + Dot3(ta[1], prevTa[h][1]) + Dot3(ta[2], prevTa[h][2]);
        float step = std::acos((std::max)(-1.0f, (std::min)(1.0f, (tt - 1.0f) * 0.5f))) * 57.2958f;
        // Walker: la velocita' usata per il limite (Actor.Velocity) e quella
        // misurata dal suo spostamento negli ultimi 30 ms (per confronto).
        float pm[3] = {0, 0, 0};
        HistVelocity(g_pawnHist, 0.03, pm);
        // Peso: velocita' misurata prima del comando, limiti raggiunti,
        // abbassamento e piega della posa voluta.
        char wtxt[440] = "", btxt[48] = "";
        if (brk == Brk::kActive) std::snprintf(btxt, sizeof(btxt), " | breakaway x%.0f", brkMul);
        else if (brk == Brk::kFreed) std::snprintf(btxt, sizeof(btxt), " | breakaway freed");
        char gtxt[96] = "";  // errore del punto della presa (distacco), palmo dall'oggetto, mano ferma, ripresa
        if (held.weight)
            std::snprintf(gtxt, sizeof(gtxt), " | grip err %.1f cm | palm %.1f cm%s%s", std::sqrt(Dot3(errA, errA)) / vr::control::g_posScale * 100.0f,
                          palmDist / vr::control::g_posScale * 100.0f, still ? " | still" : "", regripped ? " | regrip" : held.stuck ? " | stuck" : "");
        if (held.weight)
            std::snprintf(wtxt, sizeof(wtxt), " | weight %s meas v (%.0f, %.0f, %.0f) w (%.2f, %.2f, %.2f)%s%s sag %.2f cm droop %.2f deg%s",
                          held.weight == 1 ? "light" : "full", measV[0], measV[1], measV[2], measW[0], measW[1], measW[2],
                          budget ? " | force limit" : "", budgetW ? " | torque limit" : "", sagU / vr::control::g_posScale * 100.0f,
                          droopR * 57.2958f, btxt);
        std::strncat(wtxt, gtxt, sizeof(wtxt) - std::strlen(wtxt) - 1);
        if (held.weight && vr::control::g_analogGrip.load()) {
            // Forza della presa (voce 6): valore del grip, frazione in uso, forza e coppia.
            const WeightLimits wg = WeightOf(held);
            char ptxt[100];
            std::snprintf(ptxt, sizeof(ptxt), " | grip %.2f f %.2f F %.0f N tau %.1f N m throw f %.2f", held.gripV, held.gripF, wg.forceN, wg.tauMax,
                          (std::max)(held.gripF, held.gripThrowF));
            std::strncat(wtxt, ptxt, sizeof(wtxt) - std::strlen(wtxt) - 1);
        }
        char b[960];
        std::snprintf(b, sizeof(b),
                      "GRABLOG: %s err %.1f cm rot %.1f deg vel (%.0f, %.0f, %.0f) hand (%.0f, %.0f, %.0f) dt %.3f target step %.1f deg"
                      " | det hand %.2f target %.2f object %.2f | w (%.1f, %.1f, %.1f) | obj (%.1f, %.1f, %.1f) tgt (%.1f, %.1f, %.1f)"
                      " | real v (%.0f, %.0f, %.0f) w (%.1f, %.1f, %.1f) | wrist %.1f rad/s | pawn v (%.0f, %.0f, %.0f) moved (%.0f, %.0f, %.0f)%s%s",
                      h ? "L" : "R", errLen / vr::control::g_posScale * 100.0f, ang * 57.2958f, v[0], v[1], v[2], hv[0], hv[1],
                      hv[2], dt, step, Det3(ha), Det3(ta), Det3(oa), w[0], w[1], w[2], op[0], op[1], op[2], tp[0], tp[1], tp[2], realV[0] * 50.0f, realV[1] * 50.0f,
                      realV[2] * 50.0f, realW[0], realW[1], realW[2], std::sqrt(Dot3(wh, wh)), pv[0], pv[1], pv[2], pm[0], pm[1], pm[2],
                      capped ? " | capped" : "", wtxt);
        vr::log::Line(b);
    }
    std::memcpy(prevTa[h], ta, sizeof(ta));
}

void DoSpawn(void* pawn, const float* hp, const float ha[3][3]) {
    std::string mesh;
    float distCm, scale3[3];
    {
        std::lock_guard<std::mutex> lock(g_spawnMutex);
        if (g_spawnMesh.empty()) return;
        mesh.swap(g_spawnMesh);
        distCm = g_spawnDistCm;
        std::memcpy(scale3, g_spawnScale, sizeof(scale3));
    }
    void* meshObj = nullptr;
    uint32_t data = U32(reinterpret_cast<void*>(kGObjects), 0), num = U32(reinterpret_cast<void*>(kGObjects), 4);
    for (uint32_t i = 0; i < num && !meshObj; ++i) {
        uint32_t o = 0;
        if (!TrySafeReadUint32(reinterpret_cast<void*>(data + 4 * i), &o) || !o) continue;
        void* obj = reinterpret_cast<void*>(o);
        if (_stricmp(vr::script::ObjectName(obj).c_str(), mesh.c_str()) == 0 && vr::script::ClassName(obj) == "StaticMesh")
            meshObj = obj;
    }
    if (!meshObj) {
        vr::log::Line("SPAWNPROP: StaticMesh " + mesh + " not loaded in this level.");
        return;
    }
    void* cls = nullptr;
    for (uint32_t i = 0; i < num && !cls; ++i) {
        uint32_t o = 0;
        if (!TrySafeReadUint32(reinterpret_cast<void*>(data + 4 * i), &o) || !o) continue;
        void* obj = reinterpret_cast<void*>(o);
        if (vr::script::ObjectName(obj) == "KActorSpawnable" && vr::script::ClassName(obj) == "Class") cls = obj;
    }
    if (!cls) return;
    struct {
        void* cls;
        void* owner;
        int32_t tag[2];
        float loc[3];
        int32_t rot[3];
        void* tmpl;
        uint32_t noFail;
        void* ret;
    } sp = {cls, nullptr, {0, 0}, {0, 0, 0}, {0, 0, 0}, nullptr, 1, nullptr};
    float d = distCm * 0.01f * vr::control::g_posScale;
    for (int c = 0; c < 3; ++c) sp.loc[c] = hp[c] + ha[0][c] * d;
    if (!vr::script::CallFunction(pawn, "Spawn", &sp) || !sp.ret) {
        vr::log::Line("SPAWNPROP: Spawn failed.");
        return;
    }
    uint32_t smc = U32(sp.ret, kOffKActorSmc);
    if (smc && (scale3[0] != 1.0f || scale3[1] != 1.0f || scale3[2] != 1.0f)) {
        // Prima della mesh, come la sfera della mano (ProxyCreate): SetScale su
        // un corpo gia' creato cambia i limiti ma non la massa (radio x2: 0,128
        // unita' come a scala 1, 24/10).
        struct {
            float s[3];
        } s3 = {{scale3[0], scale3[1], scale3[2]}};
        vr::script::CallFunction(reinterpret_cast<void*>(smc), "SetScale3D", &s3);
    }
    struct {
        void* mesh;
        uint32_t force;
        uint32_t ret;
    } sm = {meshObj, 1, 0};
    if (!smc || !vr::script::CallFunction(reinterpret_cast<void*>(smc), "SetStaticMesh", &sm)) {
        vr::log::Line("SPAWNPROP: SetStaticMesh failed.");
        return;
    }
    Wake(reinterpret_cast<void*>(smc));
    g_rescan = true;
    char b[240];
    std::snprintf(b, sizeof(b), "SPAWNPROP: %s at (%.0f, %.0f, %.0f), scale %.3f %.3f %.3f, mass %.3f", mesh.c_str(), sp.loc[0], sp.loc[1],
                  sp.loc[2], scale3[0], scale3[1], scale3[2], BodyMass(reinterpret_cast<void*>(smc)));
    vr::log::Line(b);
}

// --- Mani solide (fase 3, 24/10) --------------------------------------------
// Ogni mano ha un corpo PhysX invisibile: una sfera di 10 cm (la sfera di
// CH_PHYS_ASSETS.PawnCollisionTriggerMesh, raggio 50 unita', in scala 0,05)
// su un KActorSpawnable, guidata a velocita' sul palmo come un oggetto tenuto
// (anticipo del moto della mano + meta' dell'errore per tick), massa fissa,
// senza gravita' e senza rotazione. Spinge e fa cadere gli oggetti fisici;
// i muri la fermano (oltre 15 cm dalla mano per 0,15 s torna sul palmo).
// Ignora i corpi di Walker, l'oggetto tenuto, quello in volo verso il guanto
// e, mentre la mano si avvicina lentamente (sotto 1 m/s), l'oggetto che sta
// per prendere: senza, la mano lo spingeva via prima del grip. Un oggetto di
// scena statico piccolo toccato a piu' di 0,3 m/s diventa fisico
// (MakePropDynamic) e la sfera lo spinge: una bottiglia urtata cade.
constexpr float kProxyScale = 0.05f;  // 50 unita' -> 2,5 unita' = 5 cm di raggio
// Unita' di GetBodyMass (MassKg): bottiglia 0,04, radio 0,13 (0,2 quella del
// livello, in scala 1,25), barile di carburante 0,83; 1 unita' ~ 40 kg. Mano con l'avambraccio ~4 kg: una bottiglia urtata
// vola, una radio si sposta, un barile si muove appena spingendo a lungo
// (con 1,5 la mano spingeva il barile come niente).
constexpr float kProxyMass = 0.1f;
struct HandProxy {
    void* actor = nullptr;
    void* comp = nullptr;
    double stuckSince = -1.0;
    double nextTry = 0.0;
    // Vibrazione degli urti (voce 3): velocita' data al tick prima, fronte di
    // salita dello schiaffo, sfera appena rimessa sul palmo (muta fino a qui).
    float lastCmd[3] = {0, 0, 0};
    bool hasCmd = false;
    vr::pure::ImpactEdge hapEdge;
    double hapSkipUntil = 0.0;
    // La mano e' passata oltre una superficie (terreno, muro) su cui la sfera
    // si era fermata: punto dove era ferma e verso del palmo. Finche' il palmo
    // resta oltre, nessuno schiaffo: la sfera ci passa (rimessa sul palmo, o
    // attraverso il terreno col comando di 10 m/s) e tornando indietro urta la
    // superficie dall'altra parte (1 impulso in piu' per discesa nel collaudo
    // F_slap, dalla sabbia da sotto).
    bool thru = false;
    float thruP[3] = {0, 0, 0}, thruN[3] = {0, 0, 0};
    double thruSince = 0.0;
    double otherNearUntil = 0.0;  // l'altra sfera vicina fino a qui: il contatto in corso e' tra le mani (SphereImpact)
    float radius = 3.5f;          // unita': raggio dei limiti della sfera (ProxyCreate; 7 cm, non i 5 cm di kProxyScale)
    // Voce 5 ("solidhands full"): muro tra la sfera ferma e il palmo (trace
    // del mondo, rifatta al piu' ogni 0,1 s): la sfera non torna sul palmo
    // attraverso il tavolo finche' il palmo non e' oltre shtele cm.
    double wallAt = 0.0;
    bool wall = false;
    std::vector<std::pair<void*, void*>> ignored;  // (componente, NxActor) con la coppia ignorata
} g_proxy[2];

void ProxyForget(void* comp) {
    for (HandProxy& pr : g_proxy)
        pr.ignored.erase(std::remove_if(pr.ignored.begin(), pr.ignored.end(), [&](const std::pair<void*, void*>& e) { return e.first == comp; }),
                         pr.ignored.end());
}

void* ProxyMesh() {
    if (void* pf = Prefetched(kPfSphereMesh)) return pf;
    static void* cached = nullptr;
    if (cached && vr::script::ObjectName(cached) == "Sphere") return cached;
    cached = nullptr;
    uint32_t data = U32(reinterpret_cast<void*>(kGObjects), 0), num = U32(reinterpret_cast<void*>(kGObjects), 4);
    static int32_t sphereName = -1;
    if (sphereName < 0) sphereName = vr::script::NameIndex("Sphere");
    if (sphereName < 0) return nullptr;
    for (uint32_t i = 0; data && i < num && i < 2000000; ++i) {
        uint32_t o = 0;
        if (!TrySafeReadUint32(reinterpret_cast<void*>(data + 4 * i), &o) || !o) continue;
        void* obj = reinterpret_cast<void*>(o);
        if (static_cast<int32_t>(U32(obj, kOffName)) != sphereName || vr::script::ClassName(obj) != "StaticMesh") continue;
        if (vr::script::ObjectName(reinterpret_cast<void*>(U32(obj, 0x28))) != "PawnCollisionTriggerMesh") continue;  // Outer
        cached = obj;
        break;
    }
    return cached;
}

void* KActorSpawnableClass() {
    if (void* pf = Prefetched(kPfSpawnableClass)) return pf;
    static void* cached = nullptr;
    if (cached && vr::script::ObjectName(cached) == "KActorSpawnable") return cached;
    cached = nullptr;
    uint32_t data = U32(reinterpret_cast<void*>(kGObjects), 0), num = U32(reinterpret_cast<void*>(kGObjects), 4);
    for (uint32_t i = 0; data && i < num && i < 2000000; ++i) {
        uint32_t o = 0;
        if (!TrySafeReadUint32(reinterpret_cast<void*>(data + 4 * i), &o) || !o) continue;
        void* obj = reinterpret_cast<void*>(o);
        if (vr::script::ObjectName(obj) == "KActorSpawnable" && vr::script::ClassName(obj) == "Class") {
            cached = obj;
            break;
        }
    }
    return cached;
}

// Sfere fuori da tutte le trace (25/10, trovato dal collaudo della mira della
// copia B): con la sinistra sull'astina la sfera sta sulla canna, e la trace
// spalla destra -> volata di MuzzleStart e quella del colpo del gioco la
// colpivano. Il colpo partiva incastrato sulla nostra mano (impatto
// sull'inizio del colpo, "on KActorSpawnable"), il mirino ci si fermava sopra.
// PrimitiveComponent +0x14C (tools/ue3.py props PrimitiveComponent):
// CollideActors 0x1, BlockActors 0x4, BlockZeroExtent 0x8 (trace a
// estensione zero: colpi, mirino, vista), BlockNonZeroExtent 0x10 (trace col
// box), CanBlockCamera 0x20, BlockRigidBody 0x40 (PhysX). La nativa
// SetTraceBlocking (UPrimitiveComponent_execSetTraceBlocking 0x007B8B50,
// Ghidra) scrive solo i bit 0x8 e 0x10, senza riattaccare il componente: le
// trace li leggono a ogni chiamata. BlockRigidBody resta: la sfera spinge e
// fa cadere gli oggetti e i muri la fermano come prima. Sfera appena creata:
// 0xC3007D -> 0xC30065.
constexpr uintptr_t kOffCompCollision = 0x14C;
constexpr uint32_t kCompBlockTraces = 0x8 | 0x10;

// true se ha dovuto spegnere i bit (sfera nuova, o riaccesi da qualcosa).
bool ProxyNoTraces(void* comp, uint32_t* before = nullptr) {
    uint32_t f = 0;
    if (!comp || !TrySafeReadUint32(static_cast<char*>(comp) + kOffCompCollision, &f)) return false;
    if (before) *before = f;
    if (!(f & kCompBlockTraces)) return false;
    f &= ~kCompBlockTraces;
    return SafeCopy(static_cast<char*>(comp) + kOffCompCollision, &f, 4);
}

// Coppia oggetto / sfera della mano ignorata (true) o di nuovo attiva.
// Una voce per corpo dell'oggetto (revisione della voce 6: l'arma raccolta ne
// ha piu' d'uno, l'M4 due; gli altri oggetti uno, come prima).
void ProxyPair(HandProxy& pr, void* comp, bool ignore) {
    void* pn = CompNx(pr.comp);
    void* on[kMaxCompBodies];
    const int no = CompNxAll(comp, on);
    auto live = [&](void* nx) {
        for (int k = 0; k < no; ++k)
            if (on[k] == nx) return true;
        return false;
    };
    if (!ignore) {
        // Tutte le voci dell'oggetto, sempre: anche se non ha piu' un corpo.
        for (auto it = pr.ignored.begin(); it != pr.ignored.end();) {
            if (it->first != comp) {
                ++it;
                continue;
            }
            if (pn && live(it->second)) CallNxPairFlags(pn, it->second, 0);
            it = pr.ignored.erase(it);
        }
        return;
    }
    if (!pn || !no) return;
    // Corpi vecchi (dopo MakeDynamic o con il PhysicsAssetInstance ricreato cambiano): via.
    for (auto it = pr.ignored.begin(); it != pr.ignored.end();) {
        if (it->first == comp && !live(it->second)) it = pr.ignored.erase(it);
        else ++it;
    }
    for (int k = 0; k < no; ++k) {
        const bool already = std::any_of(pr.ignored.begin(), pr.ignored.end(),
                                         [&](const std::pair<void*, void*>& e) { return e.first == comp && e.second == on[k]; });
        if (!already && CallNxPairFlags(pn, on[k], kNxIgnorePair)) pr.ignored.push_back({comp, on[k]});
    }
}

bool ProxyCreate(int h, void* pawn, const float* at) {
    HandProxy& pr = g_proxy[h];
    void* mesh = ProxyMesh();
    void* cls = KActorSpawnableClass();
    if (!mesh || !cls) return false;
    struct {
        void* cls;
        void* owner;
        int32_t tag[2];
        float loc[3];
        int32_t rot[3];
        void* tmpl;
        uint32_t noFail;
        void* ret;
    } sp = {cls, nullptr, {0, 0}, {at[0], at[1], at[2]}, {0, 0, 0}, nullptr, 1, nullptr};
    if (!vr::script::CallFunction(pawn, "Spawn", &sp) || !sp.ret) return false;
    void* comp = reinterpret_cast<void*>(U32(sp.ret, kOffKActorSmc));
    if (!comp) return false;
    struct {
        float s;
    } sc = {kProxyScale};
    vr::script::CallFunction(comp, "SetScale", &sc);  // prima della mesh: il corpo nasce gia' piccolo
    struct {
        void* mesh;
        uint32_t force;
        uint32_t ret;
    } sm = {mesh, 1, 0};
    if (!vr::script::CallFunction(comp, "SetStaticMesh", &sm)) return false;
    struct {
        uint32_t b;
    } hid = {1};
    vr::script::CallFunction(comp, "SetHidden", &hid);
    WriteFlags(sp.ret, U32(sp.ret, kOffActorFlagsA0) & ~kBlockActors, U32(sp.ret, kOffActorFlags9C) | kIgnoreRbPawns);
    uint32_t collBefore = 0;
    ProxyNoTraces(comp, &collBefore);  // niente trace: colpi, mirino e vista la attraversano
    pr = HandProxy();
    pr.actor = sp.ret;
    pr.comp = comp;
    g_proxyActor[h] = sp.ret;
    IgnoreWalker(pawn, comp, true);
    float oldMass = 0.0f;
    void* nx = CompNx(comp);
    bool ok = nx && NxSetupProxy(nx, kProxyMass, &oldMass);
    Prop b;
    ReadBounds(comp, &b);
    if (b.radius > 0.5f && b.radius < 20.0f) pr.radius = b.radius;
    char t[260];
    std::snprintf(t, sizeof(t), "PROPS: %s hand body created (radius %.1f cm, mass %.3f -> %.2f%s, collision flags 0x%X -> 0x%X: no traces)",
                  h ? "left" : "right", b.radius / vr::control::g_posScale * 100.0f, oldMass, kProxyMass, ok ? "" : ", PhysX setup FAILED",
                  collBefore, U32(comp, kOffCompCollision));
    vr::log::Line(t);
    return true;
}

// Fine dell'esenzione dopo il rilascio per la sfera h (oggetto lasciato dalla
// mano k). Nel log solo se ha davvero tenuto ignorata una coppia che la regola
// dello schiaffo avrebbe reso solida: prima ogni fine finiva nel log, anche
// secondi dopo e lontano ("3,039 s after release", "8698,8 cm from the
// palm") e consumava le 20 righe.
void GraceEnd(int h, int k, ReleaseGrace& gr, double now, float distUnits, const char* why) {
    if (gr.used) {
        static int logged = 0;
        if (logged < 20 || vr::control::g_grabLog > 0) {
            ++logged;
            char b[240];
            std::snprintf(b, sizeof(b),
                          "GRAB: %s hand sphere ends the release grace %.3f s after release, object %.1f cm from the palm (%s, released by the %s hand)",
                          h ? "left" : "right", now - gr.since, distUnits >= 0.0f ? distUnits / vr::control::g_posScale * 100.0f : -1.0f, why,
                          k ? "left" : "right");
            vr::log::Line(b);
        }
    }
    gr = ReleaseGrace();
}

// Schiaffo della mano vuota (voce 3): lo stesso rilevatore dell'oggetto
// tenuto con la massa della mano (hapspherekg, 5 kg); la sfera non ha
// gravita' (NxSetupProxy). Conta solo la mano libera e vuota (slapOk), piu'
// veloce di hapspeed e al piu' 8 m/s (oltre e' un salto del tracciamento, come
// per gli oggetti urtati), con la sfera sul palmo (lag = distanza dal palmo):
// la sfera rimasta indietro che insegue la mano a 10 m/s non e' uno schiaffo
// (collaudo hitthrow: la mano finta rimessa al suo posto, schiaffo a 6,5 m/s).
void SphereImpact(int h, HandProxy& pr, double now, float dt, float speed, bool slapOk, float lag, const float* palm, const float* at) {
    void* nx = CompNx(pr.comp);
    float lin[3], ang[3];
    if (!pr.hasCmd || !nx || !NxVelocities(nx, lin, ang) || !std::isfinite(lin[0] + lin[1] + lin[2])) return;
    const float s = vr::control::g_posScale;
    const float d[3] = {(pr.lastCmd[0] - lin[0] * 50.0f) / s, (pr.lastCmd[1] - lin[1] * 50.0f) / s, (pr.lastCmd[2] - lin[2] * 50.0f) / s};
    const float kg = (std::max)(0.1f, g_tune.hapSphereKg);
    if (pr.thru) {
        // Palmo di nuovo dalla parte di prima (o 5 s): la sfera e' libera.
        const float d2[3] = {palm[0] - pr.thruP[0], palm[1] - pr.thruP[1], palm[2] - pr.thruP[2]};
        if (Dot3(d2, pr.thruN) < 0.0f || now - pr.thruSince > 2.0) pr.thru = false;
    }
    // Revisione (26/10): le due sfere non sono una coppia ignorata, e sui palmi
    // si toccano nella presa a due mani della pistola, inserendo il caricatore
    // o portando la sinistra sulla destra: quell'urto non e' uno schiaffo.
    // L'altra sfera entro i due raggi + 2 cm (e un tick di moto): il
    // contatto consuma lo scatto senza impulso. Raggi dai limiti (7 cm: le
    // sfere si toccano a 5,8-6,7 unita' tra i centri in p3h/twohand.py, non
    // a 5 come direbbe kProxyScale). Vale per hapwindow + 10 ms dall'ultimo
    // tick vicino: lo scatto arriva qualche tick dopo l'inizio del contatto,
    // quando le sfere (stessa massa) si sono gia' separate.
    {
        const HandProxy& o = g_proxy[1 - h];
        float oc[3], oa[3][3];
        if (o.actor && o.comp && CompPose(o.comp, oc, oa)) {
            const float dd[3] = {oc[0] - at[0], oc[1] - at[1], oc[2] - at[2]};
            const float r2 = pr.radius + o.radius + 0.02f * s + speed * (std::max)(dt, 0.0f);
            if (Dot3(dd, dd) < r2 * r2) pr.otherNearUntil = now + (std::max)(0.0f, g_tune.hapWindow) * 0.001 + 0.01;
        }
    }
    const bool otherHand = now < pr.otherNearUntil;
    const char* why = nullptr;
    const bool gunHand = GunHandHaptics(h);
    if (!slapOk) why = gunHand ? "hand on the gun" : "hand not empty";
    else if (pr.thru) why = "hand beyond the surface the sphere was stuck on";
    else if (now < g_hapQuietUntil) why = "frame jump or game hitch";
    else if (now < pr.hapSkipUntil) why = "sphere moved back to the palm";
    else if (otherHand) why = "other hand";
    else if (speed < (std::max)(0.0f, g_tune.hapSpeed) * s) why = "slow hand";
    else if (speed > 8.0f * s) why = "tracking jump";
    else if (lag > 0.15f * s) why = "sphere catching up with the hand";
    bool muted = false;
    float lost = 0.0f;
    const float sv[3] = {lin[0] * 50.0f / s, lin[1] * 50.0f / s, lin[2] * 50.0f / s};
    const bool fire = pr.hapEdge.Step(d, sv, now, dt, HapEdgeTune(), why != nullptr, &muted, &lost);
    // Impulso sentito: la mano che colpisce pesa hapslapkg (2 kg) contro un
    // ostacolo fermo; con i 5 kg della sfera ogni schiaffo era pieno.
    const float J = (std::max)(0.1f, g_tune.hapSlapKg) * lost;
    // Nel log anche la sinistra sull'arma (per i collaudi: il contatto c'era, l'impulso no).
    if (muted && (slapOk || (h == 1 && gunHand))) HapticMuted(h, "slap", nullptr, J, lost, why);
    if (fire) HapticSend(h, "slap", nullptr, J, lost, kg, vr::pure::ImpactPulse(J, kg, HapticGain(), g_tune.hapJ), now, true, at);
}

// Un passo per la sfera della mano h. palm = punto del palmo nel mondo;
// keep = oggetti da non spingere (tenuto, in volo, quello che si sta per prendere);
// slapOk = mano libera e vuota (lo schiaffo vibra: SphereImpact).
void ProxyTick(int h, void* pawn, const float* palm, float dt, void* const* keep, int nKeep, double now, float speed, bool slapOk = false) {
    HandProxy& pr = g_proxy[h];
    if (pr.actor && !ActorUsable(pr.actor)) {
        pr = HandProxy();
        g_proxyActor[h] = nullptr;
    }
    if (!pr.actor) {
        if (now < pr.nextTry) return;
        pr.nextTry = now + 2.0;
        if (!ProxyCreate(h, pawn, palm)) return;
    }
    {
        // Bit delle trace di nuovo accesi (non dovrebbe succedere: nessuno li
        // tocca dopo ProxyCreate): spenti ancora, e lo si dice. Una lettura per tick.
        uint32_t was = 0;
        if (ProxyNoTraces(pr.comp, &was)) {
            static int logged = 0;
            if (logged < 10) {
                ++logged;
                char b[160];
                std::snprintf(b, sizeof(b), "PROPS: %s hand sphere was blocking traces again (collision flags 0x%X): turned off",
                              h ? "left" : "right", was);
                vr::log::Line(b);
            }
        }
    }
    // Oggetti da ignorare: quelli richiesti; gli altri tornano attivi solo
    // quando sono lontani dal palmo (dentro la sfera PhysX li espellerebbe).
    for (int i = 0; i < nKeep; ++i)
        if (keep[i]) ProxyPair(pr, keep[i], true);
    // Esenzioni dopo il rilascio scadute: via subito, anche se l'oggetto non e'
    // tra le coppie ignorate o resta il candidato alla presa (prima il record
    // restava finche' l'oggetto usciva da pr.ignored, anche secondi dopo).
    for (int k = 0; k < 2; ++k) {
        ReleaseGrace& gr = g_relGrace[h][k];
        if (!gr.comp || now < gr.until) continue;
        Prop cur;
        GraceEnd(h, k, gr, now, gr.used && ReadBounds(gr.comp, &cur) ? BoxDistance(cur, palm) : -1.0f, "timeout");
    }
    for (size_t i = 0; i < pr.ignored.size();) {
        void* c = pr.ignored[i].first;
        bool wanted = false;
        for (int k = 0; k < nKeep; ++k) wanted |= keep[k] == c;
        Prop cur;
        int gk = -1;
        for (int k = 0; k < 2; ++k)
            if (g_relGrace[h][k].comp == c) gk = k;
        if (!wanted && gk >= 0) {
            // Appena lasciato: ignorato anche sopra 1 m/s finche' non e' uscito
            // da questa sfera (raggio + 6 unita') o per relGrace secondi.
            ReleaseGrace& gr = g_relGrace[h][gk];
            const bool box = ReadBounds(c, &cur);
            const float dist = box ? BoxDistance(cur, palm) : 1e9f;
            if (box && dist <= kProxyScale * 50.0f + 6.0f) {
                if (speed > 1.0f * vr::control::g_posScale || dist > 6.0f) gr.used = true;  // la regola qui sotto l'avrebbe reso solido
                ++i;
                continue;
            }
            GraceEnd(h, gk, gr, now, box ? dist : -1.0f, box ? "out of reach" : "no bounds");  // rientrando vicino non ricomincia
        }
        // Sopra 1 m/s e' uno schiaffo: l'oggetto torna solido anche se e' vicino.
        if (!wanted && (speed > 1.0f * vr::control::g_posScale || !ReadBounds(c, &cur) || BoxDistance(cur, palm) > 6.0f)) {
            ProxyPair(pr, c, false);
            continue;  // ProxyPair ha tolto l'elemento i
        }
        ++i;
    }
    float op[3], oa[3][3];
    if (!CompPose(pr.comp, op, oa)) return;
    float err[3] = {palm[0] - op[0], palm[1] - op[1], palm[2] - op[2]};
    float el = std::sqrt(err[0] * err[0] + err[1] * err[1] + err[2] * err[2]);
    if (HapticGain() > 0.0f) SphereImpact(h, pr, now, dt, speed, slapOk, el, palm, op);  // prima del comando di questo tick
    // Voce 5 ("solidhands full", mano libera): la sfera e' la mano disegnata.
    // Muro (tavolo) tra la sfera e il palmo, trace del mondo rifatta al piu'
    // ogni 0,1 s da 3 cm di distacco in su: la sfera non torna sul palmo
    // attraverso il tavolo finche' il palmo non e' oltre shtele cm (Valve
    // rimette la mano a 0,6 m; SLZ la fa passare solo vuota e tirata), e non ci
    // viene spinta contro a tutta velocita' (piu' sotto).
    bool wallHold = false;
    const float sTele = (std::max)(5.0f, g_tune.shTele) * 0.01f * vr::control::g_posScale;
    if (h == 1 && vr::control::g_solidHands.load() >= 2 && el > 0.03f * vr::control::g_posScale && el <= sTele) {
        if (now >= pr.wallAt) {
            pr.wall = WorldBlocked(pawn, op, palm, false, (std::max)(1.0f, g_tune.shWall));
            pr.wallAt = now + 0.1;
        }
        wallHold = pr.wall;
    } else {
        pr.wallAt = 0.0;  // la prossima volta la trace subito
    }
    if (el > 0.15f * vr::control::g_posScale) {
        if (pr.stuckSince < 0.0) {
            pr.stuckSince = now;
            // Rimasta indietro perche' ferma contro qualcosa (al tick prima
            // perdeva oltre meta' soglia della velocita' data), non per un
            // salto della mano: la mano passa oltre la superficie (SphereImpact).
            if (pr.hapEdge.prevDvN > 0.5f * g_tune.hapDv && speed <= 8.0f * vr::control::g_posScale) {
                pr.thru = true;
                std::memcpy(pr.thruP, op, sizeof(pr.thruP));
                for (int c = 0; c < 3; ++c) pr.thruN[c] = err[c] / el;
                pr.thruSince = now;
            }
        }
        // Voce 5: con il muro in mezzo resta dov'e' (rimessa sul palmo passava
        // attraverso il tavolo, e con lei la mano disegnata).
        if (!wallHold && now - pr.stuckSince > 0.15) {
            // Rimasta dietro un muro o lontana: di nuovo sul palmo.
            struct {
                float pos[3];
                int32_t bone[2];
            } rp = {{palm[0], palm[1], palm[2]}, {0, 0}};
            vr::script::CallFunction(pr.comp, "SetRBPosition", &rp);
            pr.stuckSince = -1.0;
            pr.hapSkipUntil = now + 0.05;  // il salto non e' uno schiaffo
        }
    } else {
        pr.stuckSince = -1.0;
    }
    float hv[3] = {0, 0, 0};
    HandVelocity(h, 0.03, hv);
    float v[3];
    for (int c = 0; c < 3; ++c) v[c] = 0.5f * err[c] / dt + hv[c];
    if (wallHold && el > 1e-3f) {
        // Voce 5: verso il palmo (dentro il tavolo) al piu' 0,3 m/s, di lato
        // libera (la mano scorre sul piano). A 10 m/s contro il piano la sfera
        // rimbalzava di 7 cm per tick (cassa del fucile, 16 cm sopra il palmo:
        // 10-17,5 cm misurati) e la mano disegnata tremava con lei.
        const float n[3] = {err[0] / el, err[1] / el, err[2] / el};
        const float vn = Dot3(v, n), cap = 0.3f * vr::control::g_posScale;
        if (vn > cap)
            for (int c = 0; c < 3; ++c) v[c] -= n[c] * (vn - cap);
    }
    // Al piu' 10 m/s (una mano vera): un salto del tracciamento la lanciava a
    // 30 m/s contro gli oggetti (una bottiglia volata a 3,6 m nei collaudi); i
    // salti grandi li riprende il riposizionamento qui sopra.
    float sp = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), lim = 10.0f * vr::control::g_posScale;
    if (sp > lim)
        for (float& c : v) c *= lim / sp;
    float w0[3] = {0, 0, 0};
    SetLinVel(pr.comp, v);
    SetAngVel(pr.comp, w0);
    Wake(pr.comp);
    std::memcpy(pr.lastCmd, v, sizeof(v));
    pr.hasCmd = true;
}

// Mano lontana dal gioco (menu, cinema, prese disattivate): sfere parcheggiate
// sotto Walker, ferme, e coppie ignorate rimesse a posto.
void ProxyPark(int h, void* pawn) {
    HandProxy& pr = g_proxy[h];
    if (!pr.actor || !ActorUsable(pr.actor)) return;
    float pl[3];
    if (!SafeCopy(pl, static_cast<char*>(pawn) + kOffActorLocation, 12)) return;
    struct {
        float pos[3];
        int32_t bone[2];
    } rp = {{pl[0], pl[1], pl[2] - 5000.0f}, {0, 0}};
    vr::script::CallFunction(pr.comp, "SetRBPosition", &rp);
    float z[3] = {0, 0, 0};
    SetLinVel(pr.comp, z);
    pr.hasCmd = false;  // parcheggiata: niente schiaffo al ritorno
    while (!pr.ignored.empty()) ProxyPair(pr, pr.ignored.back().first, false);
}

// Oggetto di scena statico urtato dalla mano: diventa fisico (la sfera lo spinge).
void ProxyKnockStatics(int h, const float* palm, const Prop* candidate, float speed) {
    const float s = vr::control::g_posScale;
    // Oltre 8 m/s non e' una mano ma un salto del tracciamento (o della mano finta dei collaudi).
    if (speed < 0.3f * s || speed > 8.0f * s) return;
    const float r = kProxyScale * 50.0f;
    for (const Prop& p : g_props) {
        if (!p.isStatic) continue;
        // Solo oggetti piccoli (sfera fino a 30 unita', 60 cm di diagonale):
        // un barile urtato rotolava giu' dalla duna per 10 m (24/10). I grandi
        // diventano fisici solo prendendoli o col guanto.
        if (p.radius > 30.0f) continue;
        if (p.explosive) continue;  // bombola (voce 5): diventa un portatore solo presa o col guanto
        if (&p == candidate && speed < 1.0f * s) continue;  // la mano lo sta per prendere
        if (U32(p.comp, kOffCompOwner) != static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p.actor))) continue;  // gia' fisico
        Prop cur;
        if (!ReadBounds(p.comp, &cur) || BoxDistance(cur, palm) > r) continue;
        if (MakePropDynamic(p, HitSource::kKnocked, h)) {
            if (HapticGain() > 0.0f && !GunHandHaptics(h)) {
                // Voce 3: l'impulso dell'urto anelastico tra la mano (5 kg) e
                // l'oggetto, alla velocita' della mano (prima 20 ms pieni).
                // Mano dell'arma (la destra, la sinistra sull'arma o nella
                // ricarica): resta l'impulso fisso di prima, qui sotto.
                const float m = BodyMass(p.comp), kg = m > 0.0f ? m * vr::control::g_massKgPerUnit.load() : 2.0f;
                const float J = vr::pure::ReducedMass(kg, g_tune.hapSphereKg) * speed / s;
                HapticSend(h, "knock", p.comp, J, speed / s, kg, vr::pure::ImpactPulse(J, kg, HapticGain(), g_tune.hapJ), ProfMs() * 0.001, true,
                           palm);
            } else {
                vr::input::PulseHand(h ? 0 : 1, 20);  // si sente l'urto
            }
            // Colpito: puo' partire veloce, ma non piu' di 2,5 volte la mano.
            for (Falling& f : g_falling)
                if (f.comp == p.comp) f.maxSpeed = (std::max)(f.maxSpeed, 2.5f * speed);
            char t[160];
            std::snprintf(t, sizeof(t), "PROPS: %s hand knocked %s at %.1f m/s", h ? "left" : "right",
                          vr::script::ObjectName(reinterpret_cast<void*>(U32(p.comp, kOffSmcStaticMesh))).c_str(), speed / s);
            vr::log::Line(t);
        }
        return;  // al piu' una conversione per tick
    }
}

// --- Guanti gravitazionali (come Half-Life: Alyx, 24/10) ---------------------
// Utente: "la presa e' difficile, gli oggetti a terra sono troppo difficili da
// prendere" (il pavimento del gioco puo' stare sotto quello vero). Si punta la
// mano verso un oggetto (cono di 16 gradi, fino a 8 m: tick leggero quando
// cambia), si stringe il grip (aggancio, impulso piu' forte), si scatta la
// mano verso di se' o in alto: l'oggetto vola alla mano su un arco ricalcolato
// a ogni tick verso il palmo (la mano si puo' muovere). Arrivato: con il grip
// ancora stretto lo si prende; altrimenti resta sospeso davanti alla mano per
// 0,6 s e lo si prende stringendo di nuovo.
enum GloveState { kGloveIdle, kGloveAim, kGloveLocked, kGloveFlying, kGloveHover };
struct Glove {
    GloveState state = kGloveIdle;
    void* actor = nullptr;
    void* comp = nullptr;
    double until = 0.0;     // arrivo previsto (volo) o fine della sospensione
    double started = 0.0;   // inizio del volo
    uint32_t savedA0 = 0, saved9C = 0;
    bool flagsChanged = false;
    uint32_t walkerSig = 0;  // corpi di Walker ignorati durante il volo
    bool weapon = false;     // arma a terra (voce 6, YPickup_WeaponSpawnable), non un oggetto di g_props
    double remakeAt = 0.0;   // prossima riattivazione della fisica dell'arma permessa (s)
} g_glove[2];

// Armi a terra per i guanti (27/10, utente: "la raccolta delle armi non avviene
// coi guanti gravitazionali"): definite con la presa della voce 6, piu' avanti.
bool WeaponPickUsable(const WeaponPick& wp);
bool WeaponMakePhysical(void* a, void* comp, uint8_t* physOut);
bool WeaponTryGrabPick(void* pawn, const WeaponPick& wp, const float* hp, const float ha[3][3], const char* how);
bool WeaponPose(void* comp, float pos[3], float axes[3][3]);
void WeaponSetVel(void* comp, const float* v, const float* w);

float GravityZ(void* pawn) {
    uint32_t wi = U32(pawn, kOffActorWorldInfo);
    float g[4] = {0, -750.0f, 0, 1.0f};  // WorldGravityZ, DefaultGravityZ, GlobalGravityZ, RBPhysicsGravityScaling
    if (wi) SafeCopy(g, reinterpret_cast<char*>(wi) + 0x4DC, 16);
    float gz = g[0] != 0.0f ? g[0] : g[1];
    float s = g[3] > 0.0f ? g[3] : 1.0f;
    return std::isfinite(gz * s) && gz * s < 0.0f ? gz * s : -750.0f;
}

// Bersaglio ancora utilizzabile: un oggetto deve essere un corpo rigido; un'arma
// (voce 6) basta che esista ancora, perche' il gioco la rimette a riposo
// (Physics none) pochi decimi dopo che si e' fermata e la si riattiva
// (WeaponMakePhysical). Collaudo nel visore del 27/10: tra l'aggancio e lo
// scatto 0,4 s, il primo tick del volo la trovava a riposo ("target gone"), e
// le collisioni cambiate per il volo non tornavano com'erano.
bool GloveTargetUsable(const Glove& gl) {
    if (!gl.actor) return false;
    return gl.weapon ? WeaponPickUsable({gl.actor, gl.comp}) : ActorUsable(gl.actor);
}

// Perche' un'arma agganciata non e' piu' usabile (riga GLOVE, visore 27/10: tre
// "target gone" in una sparatoria, uno senza volo).
const char* WeaponGoneWhy(const Glove& gl) {
    if (!vr::script::LiveObject(gl.actor)) return "weapon gone: the pickup was destroyed";
    if (U32(gl.actor, kOffActorFlags9C) & (kHidden | kDeleteMe)) return "weapon gone: the pickup was hidden or deleted";
    if (U32(gl.actor, kOffPickupFlags) & kPickupHidden)
        return "weapon gone: taken by the game (same gun as Walker's: ammo on contact) or back in its pool";
    if (U32(gl.actor, kOffPickupMesh) != static_cast<uint32_t>(reinterpret_cast<uintptr_t>(gl.comp)))
        return "weapon gone: the game reused the pickup for another drop (new mesh)";
    return "target gone";
}

void GloveRestoreFlags(Glove& gl) {
    if (gl.flagsChanged && GloveTargetUsable(gl)) WriteFlags(gl.actor, gl.savedA0, gl.saved9C);
    gl.flagsChanged = false;
}

void GloveReset(int h, const char* why) {
    Glove& gl = g_glove[h];
    if (gl.state >= kGloveLocked && why) {
        vr::log::Line(std::string("GLOVE: ") + (h ? "left" : "right") + " " + why);
    }
    if (gl.flagsChanged && GloveTargetUsable(gl))
        g_restore.push_back({gl.actor, ProfMs() * 0.001 + 0.5, gl.savedA0, gl.saved9C, gl.walkerSig, false,
                             ProfMs() * 0.001});  // fuori da Walker, poi di nuovo solido
    // Un'arma (voce 6) non entra tra gli oggetti in caduta: cade e basta.
    if (gl.state >= kGloveLocked && gl.actor && ActorUsable(gl.actor) && !gl.weapon)
        FallingAdd({gl.actor, gl.comp, ProfMs() * 0.001 + 4.0}, HitSource::kGlove, h);
    gl = Glove();
}

// Punto in cui arriva l'oggetto: davanti al palmo.
void GloveCatchPoint(int h, const float* hp, const float ha[3][3], float* out) {
    for (int i = 0; i < 3; ++i) {
        out[i] = hp[i];
        if (g_palmValid[h]) {
            out[i] += g_palmLocal[h][0] * ha[0][i] + g_palmLocal[h][1] * ha[1][i] + g_palmLocal[h][2] * ha[2][i];
            float n = g_palmNormalLocal[h][0] * ha[0][i] + g_palmNormalLocal[h][1] * ha[1][i] + g_palmNormalLocal[h][2] * ha[2][i];
            out[i] += n * 0.06f * vr::control::g_posScale;
        }
    }
}

// Oggetto puntato: angolo dal raggio della mano meno il suo raggio apparente.
// Angolo (radianti) tra la mira e l'oggetto, meno il suo raggio apparente;
// la mira e' la direzione del controller o la normale del palmo (si punta in
// tutti e due i modi), la migliore delle due. -1 = fuori portata.
float AimScore(const Prop& p, const float* hp, const float* dir, const float* dir2, float minDist) {
    Prop cur = p;
    if (!ReadBounds(p.comp, &cur)) return -1.0f;
    float c[3], d[3];
    for (int i = 0; i < 3; ++i) {
        c[i] = 0.5f * (cur.boxMin[i] + cur.boxMax[i]);
        d[i] = c[i] - hp[i];
    }
    float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (dist < minDist || dist > 8.0f * vr::control::g_posScale) return -1.0f;
    float best = 10.0f;
    for (const float* v : {dir, dir2}) {
        if (!v) continue;
        float cosA = (d[0] * v[0] + d[1] * v[1] + d[2] * v[2]) / dist;
        best = (std::min)(best, std::acos((std::max)(-1.0f, (std::min)(1.0f, cosA))));
    }
    return (std::max)(0.0f, best - std::atan(0.6f * cur.radius / dist)) + 0.004f * dist / vr::control::g_posScale;
}

const Prop* GloveTarget(const float* hp, const float* dir, float minDist) {
    const Prop* best = nullptr;
    float bestScore = 10.0f * 0.0174533f;
    const float maxDist = 8.0f * vr::control::g_posScale;
    for (const Prop& p : g_props) {
        bool taken = false;
        for (const Held& hd : g_held) taken |= hd.comp == p.comp;
        if (taken) continue;
        Prop cur = p;
        if (!ReadBounds(p.comp, &cur)) continue;
        float c[3], d[3];
        for (int i = 0; i < 3; ++i) {
            c[i] = 0.5f * (cur.boxMin[i] + cur.boxMax[i]);
            d[i] = c[i] - hp[i];
        }
        float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (dist < minDist || dist > maxDist) continue;
        float cosA = (d[0] * dir[0] + d[1] * dir[1] + d[2] * dir[2]) / dist;
        float ang = std::acos((std::max)(-1.0f, (std::min)(1.0f, cosA)));
        float score = ang - std::atan(0.5f * cur.radius / dist) + 0.01f * dist / vr::control::g_posScale * 0.0174533f;
        if (score < bestScore) {
            bestScore = score;
            best = &p;
        }
    }
    return best;
}

// Ritorna true se in questo tick la mano ha preso l'oggetto in arrivo.
bool GloveTick(int h, void* pawn, const float* hp, const float ha[3][3], bool grip, bool gripRose, float zone, double now,
               float dt) {
    Glove& gl = g_glove[h];
    if (!vr::control::g_gravityGloves || h != 1 || !vr::control::g_supportFwdValid) {
        if (gl.state != kGloveIdle) GloveReset(h, "off");
        return false;
    }
    if (gl.state >= kGloveLocked && !GloveTargetUsable(gl)) {
        GloveReset(h, gl.weapon ? WeaponGoneWhy(gl) : "target gone");
        return false;
    }
    if (gl.state >= kGloveLocked && gl.weapon) {
        // Rimessa a riposo dal gioco dopo l'aggancio: di nuovo fisica, prima di
        // muoverla. Solo se ha davvero perso il corpo rigido e al piu' ogni 0,5 s:
        // il bit m_physicsDisabled resta acceso anche dopo la riattivazione, e
        // guardandolo si chiamava ConditionalReactivatePhysics a ogni tick (120
        // al secondo), che ricrea i corpi PhysX dell'arma: spreco certo, sospettato
        // (mai confermato) di riempire la scena fisica. I cadaveri spariti nella
        // stessa sessione del 27/10 erano invece il limite del livello
        // (YGameInfo +0x544 = 1, docs/TECHNICAL.md).
        uint8_t ph = 0;
        SafeCopy(&ph, static_cast<char*>(gl.actor) + kOffActorPhysics, 1);
        if (ph != kPhysRigidBody && now >= gl.remakeAt) {
            gl.remakeAt = now + 0.5;
            if (!WeaponMakePhysical(gl.actor, gl.comp, nullptr)) {
                GloveReset(h, "weapon could not be made physical again");
                return false;
            }
            vr::log::Line(std::string("GLOVE: ") + (h ? "left" : "right") + " weapon put to rest by the game, made physical again");
        }
    }
    float dir[3] = {vr::control::g_supportFwd[0].load(), vr::control::g_supportFwd[1].load(), vr::control::g_supportFwd[2].load()};
    float catchP[3];
    GloveCatchPoint(h, hp, ha, catchP);
    if (gl.state == kGloveIdle || gl.state == kGloveAim) {
        // Normale del palmo nel mondo (si punta anche col palmo aperto, come in Alyx).
        float pn[3], *pnp = nullptr;
        if (g_palmValid[h]) {
            for (int i = 0; i < 3; ++i)
                pn[i] = g_palmNormalLocal[h][0] * ha[0][i] + g_palmNormalLocal[h][1] * ha[1][i] + g_palmNormalLocal[h][2] * ha[2][i];
            pnp = pn;
        }
        // Cono di 16 gradi; l'oggetto gia' puntato resta finche' sta entro 26
        // gradi e un altro non e' meglio di 5 gradi (niente sfarfallio).
        const Prop* t = nullptr;
        float tScore = 16.0f * 0.0174533f, curScore = -1.0f;
        for (const Prop& p : g_props) {
            bool taken = false;
            for (const Held& hd : g_held) taken |= hd.comp == p.comp;
            if (taken) continue;
            if (p.constrained) continue;  // una portiera non vola in mano
            float sc = AimScore(p, hp, dir, pnp, zone * 1.5f);
            if (sc < 0.0f) continue;
            if (p.comp == gl.comp) curScore = sc;
            if (sc < tScore) {
                tScore = sc;
                t = &p;
            }
        }
        // Armi a terra (voce 6): puntate come gli oggetti, nello stesso cono.
        // Mai con la sinistra sull'arma, a due mani o nella ricarica (come la
        // zona della voce 6: la mano dell'arma non cambia).
        const WeaponPick* tw = nullptr;
        if (vr::control::g_weaponPickup.load() && !GunHandHaptics(1)) {
            for (const WeaponPick& wp : g_weapons) {
                if (wp.comp == g_wpnComp[0] || !WeaponPickUsable(wp)) continue;  // [0] = quella gia' in mano
                Prop q;
                q.actor = wp.actor;
                q.comp = wp.comp;
                float sc = AimScore(q, hp, dir, pnp, zone * 1.5f);
                if (sc < 0.0f) continue;
                if (wp.comp == gl.comp) curScore = sc;
                if (sc < tScore) {
                    tScore = sc;
                    tw = &wp;
                    t = nullptr;
                }
            }
        }
        if (gl.comp && curScore >= 0.0f && curScore < 26.0f * 0.0174533f && (!(t || tw) || tScore > curScore - 5.0f * 0.0174533f)) {
            if (gl.weapon) {
                for (const WeaponPick& wp : g_weapons)
                    if (wp.comp == gl.comp) {
                        tw = &wp;
                        t = nullptr;
                    }
            } else {
                for (const Prop& p : g_props)
                    if (p.comp == gl.comp) {
                        t = &p;
                        tw = nullptr;
                    }
            }
        }
        void* tc = t ? t->comp : (tw ? tw->comp : nullptr);
        if (tc != gl.comp && tc) vr::input::PulseHand(h ? 0 : 1, 8);  // puntato
        gl.actor = t ? t->actor : (tw ? tw->actor : nullptr);
        gl.comp = tc;
        gl.weapon = tw != nullptr;
        gl.state = tc ? kGloveAim : kGloveIdle;
        if (tw && gripRose) {
            // Arma a terra: fisica e dinamica adesso (deve volare), come alla
            // presa della voce 6; arrivata la prende WeaponTryGrabPick.
            if (!WeaponMakePhysical(gl.actor, gl.comp, nullptr)) {
                vr::log::Line(std::string("GLOVE: ") + (h ? "left" : "right") + " could not make " +
                              vr::script::ObjectName(gl.actor) + " physical");
                gl = Glove();
                return false;
            }
            gl.state = kGloveLocked;
            RaiseMaxAngVel(gl.comp);
            vr::input::PulseHand(h ? 0 : 1, 35);
            const uint32_t wcls = U32(gl.actor, kOffPickupWeaponClass);
            vr::log::Line(std::string("GLOVE: ") + (h ? "left" : "right") + " locked " + vr::script::ObjectName(gl.actor) + " (weapon " +
                          (wcls ? vr::script::ObjectName(reinterpret_cast<void*>(static_cast<uintptr_t>(wcls))) : std::string("-")) + ")");
            return false;
        }
        if (tc && gripRose) {
            // Oggetto di scena statico: diventa fisico adesso (deve volare).
            // Bombola esplosiva (voce 5): vola il portatore, col suo componente.
            void* tcNow = tc;
            void* ta = MakePropDynamic(*t, HitSource::kGlove, h, &tcNow);
            if (!ta) {
                gl = Glove();
                return false;
            }
            gl.actor = ta;
            gl.comp = tcNow;
            gl.state = kGloveLocked;
            RaiseMaxAngVel(gl.comp);
            vr::input::PulseHand(h ? 0 : 1, 35);
            vr::log::Line(std::string("GLOVE: ") + (h ? "left" : "right") + " locked " + vr::script::ObjectName(ta));
        }
        return false;
    }
    float op[3], oa[3][3];
    if (!(gl.weapon ? WeaponPose(gl.comp, op, oa) : CompPose(gl.comp, op, oa))) {
        GloveReset(h, "no pose");
        return false;
    }
    // Velocita' del volo: un'arma (SkeletalMeshComponent col PhysicsAsset) la
    // prende per tutti i corpi con WeaponSetVel, senza rotazione.
    auto setVel = [&](const float* v) {
        if (gl.weapon) {
            const float w0[3] = {0.0f, 0.0f, 0.0f};
            WeaponSetVel(gl.comp, v, w0);
        } else {
            SetLinVel(gl.comp, v);
        }
        Wake(gl.comp);
    };
    // Presa all'arrivo: l'oggetto (Grab) o l'arma con la presa della voce 6
    // (in mano, o le munizioni se Walker ha gia' la stessa arma).
    auto catchIt = [&](const char* how) {
        GloveRestoreFlags(gl);
        bool ok = true;
        if (gl.weapon) {
            const WeaponPick wp = {gl.actor, gl.comp};
            ok = WeaponTryGrabPick(pawn, wp, hp, ha, "gravity glove");
        } else {
            Prop p;
            p.actor = gl.actor;
            p.comp = gl.comp;
            ReadBounds(gl.comp, &p);
            Grab(h, p, hp, ha);
        }
        vr::log::Line(std::string("GLOVE: ") + (h ? "left" : "right") + (ok ? " caught it " : " could not take it ") + how);
        gl = Glove();
        return ok;
    };
    Prop cur;
    float oc[3];
    if (!ReadBounds(gl.comp, &cur)) return false;
    for (int i = 0; i < 3; ++i) oc[i] = 0.5f * (cur.boxMin[i] + cur.boxMax[i]);
    float d[3] = {catchP[0] - oc[0], catchP[1] - oc[1], catchP[2] - oc[2]};
    float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (gl.state == kGloveLocked) {
        if (!grip) {
            GloveReset(h, "released before the flick");
            return false;
        }
        // Agganciato e fermo in attesa dello scatto: sveglio. Un KActorFromStatic
        // che si addormenta torna statico (il gioco lo rifa' StaticMeshActor) e
        // il volo trovava l'attore sparito ("target gone", visore 27/10: il
        // monitor riagganciato dopo averlo lasciato).
        Wake(gl.comp);
        // Scatto: mano verso di se' (verso il punto di presa dall'oggetto) o in alto.
        float v[3];
        if (!HandVelocity(h, 0.06, v)) return false;
        float toward = dist > 1e-3f ? (v[0] * d[0] + v[1] * d[1] + v[2] * d[2]) / dist : 0.0f;
        float s = vr::control::g_posScale;
        if (toward > 1.0f * s || v[2] > 1.2f * s) {
            float T = (std::max)(0.35f, (std::min)(0.8f, 0.3f + 0.06f * dist / s));
            gl.state = kGloveFlying;
            gl.started = now;
            gl.until = now + T;
            gl.savedA0 = U32(gl.actor, kOffActorFlagsA0);
            gl.saved9C = U32(gl.actor, kOffActorFlags9C);
            gl.flagsChanged = true;
            WriteFlags(gl.actor, gl.savedA0 & ~kBlockActors, gl.saved9C | kIgnoreRbPawns);
            gl.walkerSig = IgnoreWalker(pawn, gl.comp, true);  // arriva al petto senza essere respinto
            vr::input::PulseHand(h ? 0 : 1, 25);
            char b[160];
            std::snprintf(b, sizeof(b), "GLOVE: %s flick %.1f m/s, %s flies %.1f m in %.2f s", h ? "left" : "right",
                          (std::max)(toward, v[2]) / s, vr::script::ObjectName(gl.actor).c_str(), dist / s, T);
            vr::log::Line(b);
        }
        return false;
    }
    bool arrived = dist < zone * 1.5f;
    if (gl.state == kGloveFlying && arrived) {
        if (grip) return catchIt("in flight");
        gl.state = kGloveHover;
        gl.until = now + 0.6;
    }
    if (gl.state == kGloveHover) {
        if (grip && gripRose) return catchIt("while floating");
        if (now > gl.until) {
            GloveReset(h, "not caught, dropped");
            return false;
        }
        // Sospeso davanti al palmo: insegue il punto in ~0,1 s, gravita' compensata.
        float v[3];
        for (int i = 0; i < 3; ++i) v[i] = d[i] / 0.1f;
        v[2] -= GravityZ(pawn) * dt;
        setVel(v);
        return false;
    }
    // In volo: arco balistico ricalcolato verso il palmo con il tempo che resta.
    if (now - gl.started > 2.0) {
        GloveReset(h, "flight timed out");
        return false;
    }
    float Trem = static_cast<float>((std::max)(0.12, gl.until - now));
    float gz = GravityZ(pawn);
    float v[3] = {d[0] / Trem, d[1] / Trem, d[2] / Trem - 0.5f * gz * Trem};
    float sp = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), lim = 25.0f * vr::control::g_posScale;
    if (sp > lim)
        for (float& c : v) c *= lim / sp;
    setVel(v);
    return false;
}

// --- Colpi degli oggetti: pipeline condivisa (piano "oggetti nel gameplay", voce 0, 24/10) ---
// Gli oggetti lanciati, lasciati o urtati (g_falling) e quelli tenuti in mano
// diventano colpi su nemici vivi e su bersagli del mondo (vetri, finestre di
// sabbia, distruttibili). Ogni tick, per gli oggetti che si muovono:
//  - segmento del moto dal tick prima. In volo e' quello previsto dalla
//    velocita' PhysX del tick prima: l'oggetto rimbalza sulla capsula
//    cinematica del nemico (YPawnKinematicRBCollider) e il suo centro non
//    entra mai nel corpo. In mano parte dalla posizione vera e va avanti con
//    la velocita' data dalla mano (anche l'oggetto tenuto e' fermato dalla
//    capsula, la mano no), per il centro e per la punta degli oggetti lunghi;
//  - prova geometrica economica contro i cilindri dei nemici entro 15 m
//    (WorldInfo.PawnList +0x4F4, NextPawn +0x218, vivi con Health +0x304 > 0;
//    squadra, Walker e civili esclusi per nome di classe come nel selettore
//    degli ordini) e contro i box dei bersagli raccolti da Scan;
//  - solo se c'e' un candidato, la trace del motore (SingleLineCheck) con
//    l'oggetto come sorgente conferma e dice cosa e dove; per i Pawn le
//    trace a estensione zero prendono la mesh e l'osso (PhysicsAsset).
// Un colpo per lancio (o per colpo in mano) e per bersaglio, e comunque non
// piu' di uno ogni 0,5 s per coppia (oggetto, bersaglio). Gli eventi (PropHit) vanno a OnPropHit
// dopo la rilevazione di tutti gli oggetti, prima delle rotture per urto del
// tick (come Source: il danno fuori dal passo di simulazione); le voci 1, 2 e
// 4 del piano ci attaccano gli effetti.

// UWorld::SingleLineCheck @ 0x009BDE30 (thiscall su GWorld [0x014DF734], la
// stessa di TraceLine in hooks.cpp; ret 0x1C): chiama MultiLineCheck
// (0x0071B130) con TraceFlags | TRACE_SingleResult (0x400), copia il colpo
// piu' vicino (0x4C byte) e ritorna 1 se non ha colpito niente (Ghidra, 24/10).
// FCheckResult: Actor +4, Location +8, Normal +0x14, Time +0x20, Item +0x24
// (indice del corpo nel PhysicsAsset), Material +0x28, PhysMaterial +0x2C,
// Component +0x30, BoneName +0x38 (non +0x34 come in UE3: +0x34 resta 0;
// verificato 24/10 su una trace nel manichino: Item 2, BodySetup[2].BoneName =
// Spine3 = il nome a +0x38), Level +0x40, LevelIndex +0x44 (-1). Flag:
// TRACE_Pawns 0x1, Movers 0x2, Level 0x4,
// Others 0x10, LevelGeometry 0x80, Terrain 0x2000 (valori UE3: FastTrace usa
// 0x2286 = World | StopAtAnyHit); senza StopAtAnyHit, serve il piu' vicino.
// Con estensione la trace prende il cilindro dei Pawn (BlockNonZeroExtent), a
// estensione zero la mesh fisica con l'osso (BlockZeroExtent, come "hittest").
const uintptr_t& kSingleLineCheck = vr::addr::g[vr::addr::SingleLineCheck];
const uintptr_t& kGWorld = vr::addr::g[vr::addr::GWorld];
constexpr uint32_t kHitTraceFlags = 0x1 | 0x2 | 0x4 | 0x10 | 0x80 | 0x2000;
// TRACE_ComplexCollision: le mesh statiche con la collisione poligono per
// poligono anche se hanno quella semplice (lo usano i proiettili: Actor.Trace
// con TRACEFLAG_Bullet aggiunge 0x20000, AActor_execTrace 0x009180A0). Solo
// nella trace a estensione zero verso i bersagli del mondo (voce 2, 26/10): i
// vetri del pullman di sabbia del livello 1 stanno dentro la collisione
// semplice del pullman (InterpActor dubai_bus_wrecked), che la trace semplice
// prendeva a 0 unita' dall'inizio, mentre PhysX lascia passare l'oggetto.
constexpr uint32_t kTraceComplex = 0x20000;
using SingleLineCheckFn = int(__thiscall*)(void* world, void* hit, void* source, const float* end, const float* start,
                                           uint32_t flags, const float* extent, void* light);
constexpr uintptr_t kOffWorldPawnList = 0x4F4, kOffPawnNextPawn = 0x218, kOffPawnHealth = 0x304;
constexpr uintptr_t kOffPawnCylinder = 0x3AC, kOffCylHeight = 0x200, kOffCylRadius = 0x204;

struct TraceResult {
    void* actor = nullptr;
    void* comp = nullptr;  // componente colpito (FCheckResult.Component)
    float loc[3] = {};     // centro del box della trace all'impatto
    float normal[3] = {};
    int32_t bone = 0;  // FName (0 = None)
    int32_t item = 0;  // FCheckResult.Item
    void* material = nullptr;      // FCheckResult.Material +0x28 (triangolo colpito, trace a estensione zero)
    void* physMaterial = nullptr;  // FCheckResult.PhysMaterial +0x2C
};

bool EngineTrace(void* source, const float* start, const float* end, float extent, TraceResult* out, uint32_t flags = kHitTraceFlags) {
    __try {
        if (!kGWorld || !kSingleLineCheck) return false;
        void* world = *reinterpret_cast<void**>(kGWorld);
        if (!world) return false;
        alignas(16) unsigned char hit[0x100] = {};
        const float ext[3] = {extent, extent, extent};
        reinterpret_cast<SingleLineCheckFn>(kSingleLineCheck)(world, hit, source, end, start, flags, ext, nullptr);
        out->actor = *reinterpret_cast<void**>(hit + 4);
        std::memcpy(out->loc, hit + 8, 12);
        std::memcpy(out->normal, hit + 0x14, 12);
        out->comp = *reinterpret_cast<void**>(hit + 0x30);
        std::memcpy(&out->bone, hit + 0x38, 4);
        std::memcpy(&out->item, hit + 0x24, 4);
        out->material = *reinterpret_cast<void**>(hit + 0x28);
        out->physMaterial = *reinterpret_cast<void**>(hit + 0x2C);
        return out->actor != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Evento di colpo: lo producono la pipeline, lo consumano le voci 1, 2, 4.
enum class HitKind : uint8_t { kFlying, kHeld };
struct PropHit {
    void* prop = nullptr;    // attore dell'oggetto (KActor)
    void* comp = nullptr;    // il suo StaticMeshComponent
    float mass = 0.0f;       // GetBodyMass in unita' del motore (bottiglia 0,04, radio 0,13-0,2, barile 0,83; kg: x masskg)
    float vel[3] = {};       // velocita' del punto che colpisce, prima dell'urto (PhysX, unita'/s)
    float speed = 0.0f;      // |vel| (unita'/s; / g_posScale = m/s)
    // In mano (revisione della voce 4, 26/10): |vel - FrameVelocity|, il colpo
    // rispetto a Walker senza la sua camminata (unita'/s); in volo = speed.
    float swing = -1.0f;
    void* target = nullptr;  // nemico, mesh fratturabile, finestra di sabbia, distruttibile
    void* targetComp = nullptr;  // componente colpito (mesh del nemico se l'osso viene dalla trace)
    uint8_t targetKind = kTgtNone;
    bool glass = false;      // bersaglio del mondo con "glass" nel nome della mesh
    float loc[3] = {};       // punto d'impatto (sulla mesh se l'osso viene dalla trace)
    float normal[3] = {};    // normale della superficie colpita
    float dir[3] = {};       // direzione del moto
    int32_t bone = 0;        // FName dell'osso colpito (0 = None / non noto)
    uint8_t boneFrom = 0;    // 1 trace lungo il moto, 2 trace verso l'asse del nemico, 3 corpo piu' vicino
    // L'osso del corpo toccato e' entro la portata del contatto (mezza misura
    // dell'oggetto + 12 unita'): sempre per le trace (1, 2), per il corpo piu'
    // vicino (3) solo se la sua posizione (GetBoneLocation) ci sta. Un oggetto
    // che sfiora la cima del cilindro non e' un colpo in testa (revisione della
    // voce 1, 26/10: "head (closest body)" a z 729-732 con la bottiglia).
    bool boneNear = false;
    HitKind kind = HitKind::kFlying;
    // Origine: kHeld in mano; in volo kThrown, kDropped, kGlove, kShot,
    // kKnocked o kOther (un colpo di un oggetto spinto da OnShot non e' un
    // lancio: la voce 1 deve filtrare su questo).
    HitSource source = HitSource::kOther;
    // Mano: quella che lo tiene (kHeld) o che l'ha lanciato, lasciato,
    // agganciato o urtato (0 destra, 1 sinistra); -1 per kShot e kOther.
    int hand = -1;
    // Colpo di un'estremita' di un oggetto lungo: in mano la punta lontana
    // dalla mano, in volo uno dei due capi dell'asse maggiore.
    bool tip = false;
    // FCheckResult.Item della trace (voce 2): per una mesh fratturabile il
    // frammento colpito, come per i proiettili; -1 se non noto.
    int32_t item = -1;
    // Il colpo viene dalla trace col box (estensione > 0), non da quella a
    // estensione zero sui poligoni: item allora non e' un frammento sicuro
    // (revisione della voce 2, 26/10; WorldDamage rifa' la trace).
    bool boxTrace = false;
    // Velocita' PhysX del centro dell'oggetto prima dell'urto (unita'/s): per
    // un capo o una punta vel e' quella del punto, rotazione compresa.
    float cvel[3] = {};
};

// Oggetto seguito dalla pipeline (stato al tick prima).
struct HitTrack {
    void* actor = nullptr;
    void* comp = nullptr;
    float c[3] = {};         // centro dei limiti
    float v[3] = {};         // velocita' PhysX (unita'/s)
    float w[3] = {};         // velocita' angolare PhysX (rad/s)
    float tip[3] = {};       // punta (oggetti lunghi tenuti in mano)
    float tipLocal[3] = {};  // punta nello spazio della mesh
    bool hasTip = false;
    bool tipValid = false;   // tip contiene la punta del tick prima
    float endLocal[2][3] = {};  // capi dell'asse maggiore (oggetti lunghi, nello spazio della mesh)
    float ends[2][3] = {};      // e nel mondo al tick prima
    bool hasEnds = false;
    bool endsValid = false;
    int hand = -1;           // mano che lo tiene, -1 in volo
    HitSource source = HitSource::kOther;
    int srcHand = -1;        // mano dell'origine (vedi PropHit.hand)
    uint32_t gen = 0;        // Falling.gen dell'origine
    uint32_t epoch = 0;      // un lancio o un colpo in mano: cambia a ogni rilascio e a ogni colpo nuovo
    bool swingOver = true;   // in mano: mano sotto 1 m/s dopo l'ultimo colpo (il prossimo e' un colpo nuovo)
    float mass = -1.0f;      // GetBodyMass, letta al primo colpo
    bool alive = false;      // seguito in questo tick
};
std::vector<HitTrack> g_tracks;
struct HitMemo {
    void* comp;
    void* target;
    uint32_t epoch;
    double at;
    float speed;  // velocita' del colpo (unita'/s): un colpo in mano lento puo' essere seguito da uno da mischia (SweepHits)
};
std::vector<HitMemo> g_hitMemo;
uint32_t g_hitEpoch = 0;
struct PawnClassInfo {
    bool yGame = false;     // deriva da YGamePawn (ha m_squadMemberType)
    bool enemy = false;     // deriva da YPawn_Enemy e non ha il nome di un amico
};
std::unordered_map<uint32_t, PawnClassInfo> g_pawnClass;  // classe di Pawn -> info (decise una volta per classe)
struct PawnSide {
    void* pawn;
    uint32_t cls;
    bool hostile;
    double until;  // si decide di nuovo dopo 2 s (la squadra puo' cambiare)
};
std::vector<PawnSide> g_pawnSide;
struct {
    void* walker = nullptr;
    void* team = nullptr;  // YTeamInfo di Walker (GetYTeam), nullptr se non c'e'
    double until = 0.0;
} g_walkerTeam;
struct HitPawn {
    void* pawn;
    float loc[3];
    float r, h;  // cilindro di collisione (Pawn.CylinderComponent: raggio, mezza altezza)
};
// Costo nel log ogni 30 s insieme a "PROPS: tick".
struct HitStats {
    double sum = 0.0, worst = 0.0, movingSum = 0.0;
    uint32_t ticks = 0, movingTicks = 0, traces = 0, hits = 0, maxTracks = 0, maxPawns = 0;
} g_hitStats;

// Nemico o no (revisione 24/10): lo decide il gioco, non il nome della classe.
// Prima bastava che il nome non contenesse Adams, Lugo, Walker, Civil o
// Refugee: contavano come nemici YPawn_Riggs (l'alleato della CIA),
// YPawn_Guide, YPawn_FakeGould, MatineePawn (i pawn delle scene), YPawn_Coop,
// i veicoli e YGamePawn_Menu. Ora, in ordine:
// 1. YGamePawn.m_squadMemberType (+0x4BC, YE_SquadMember: Adams 0, Lugo 1,
//    Walker 2, Daniels 3, Riggs 4, Gould 5, Fari 6, None 7): un personaggio
//    con nome non e' mai un bersaglio (letto in memoria: manichino 7, Lugo 1,
//    Adams 0, Walker 2).
// 2. Con una squadra (Pawn.GetTeamNum, nativa senza opcode, via
//    ProcessEvent; 255 = nessuna): YTeamInfo.m_standing[7] (+0x23C,
//    YE_Standing: Neutral 0, Friendly 1, Hostile 2) della squadra di Walker
//    (YGamePawn.GetYTeam), indicizzata con la squadra del pawn. E' la tabella
//    di YGamePawn::IsHostile (vtable +0x698 -> FUN_0086ff90 via +0x818),
//    che per conto suo, in giocatore singolo, conta ostile chiunque non sia
//    se stesso. Nemico solo se Hostile. Livello 1: Walker, Lugo e Adams nella
//    stessa YTeamInfo (TeamIndex 0, m_standing 1 2 1 0 1 2 2).
// 3. Senza squadra (nessun controller: il manichino evocato, i pawn delle
//    scene): nemico solo una classe derivata da YPawn_Enemy, senza il nome di
//    un amico (YPawn_LugoCanDie deriva da YPawn_Enemy).
// Deciso per pawn, rifatto ogni 2 s; le due chiamate al gioco solo allora.
PawnClassInfo PawnClass(uint32_t cls) {
    auto it = g_pawnClass.find(cls);
    if (it != g_pawnClass.end()) return it->second;
    if (g_yGamePawnName < 0 || g_pawnEnemyName < 0) return PawnClassInfo();  // nomi non ancora cercati da Scan: non in cache
    PawnClassInfo info;
    info.yGame = ClassIsA(cls, g_yGamePawnName);
    info.enemy = ClassIsA(cls, g_pawnEnemyName);
    std::string n = vr::script::ObjectName(reinterpret_cast<void*>(cls));
    for (const char* k : {"Adams", "Lugo", "Walker", "Riggs", "Gould", "Guide", "Civil", "Refugee"})
        if (n.find(k) != std::string::npos) info.enemy = false;
    g_pawnClass[cls] = info;
    return info;
}

void* WalkerTeam(void* walker, double now) {
    if (g_walkerTeam.walker == walker && now < g_walkerTeam.until) return g_walkerTeam.team;
    void* ret[4] = {};
    void* team = vr::script::CallFunction(walker, "GetYTeam", ret) ? ret[0] : nullptr;
    if (team && !(g_teamInfoName >= 0 && ClassIsA(U32(team, kOffClass), g_teamInfoName))) team = nullptr;
    g_walkerTeam.walker = walker;
    g_walkerTeam.team = team;
    g_walkerTeam.until = now + 2.0;
    return team;
}

bool PawnHostile(void* walker, void* pw, uint32_t cls, double now) {
    for (const PawnSide& s : g_pawnSide)
        if (s.pawn == pw && s.cls == cls && now < s.until) return s.hostile;
    PawnClassInfo info = PawnClass(cls);
    bool hostile = false;
    int team = -1, standing = -1, squad = -1;
    const char* why = "";
    if (info.yGame && (squad = static_cast<int>(U32(pw, 0x4BC) & 0xFF)) != 7) {
        why = "squad member";
    } else {
        uint8_t parm[16] = {};
        team = vr::script::CallFunction(pw, "GetTeamNum", parm) ? parm[0] : 255;
        void* wt = team < 7 ? WalkerTeam(walker, now) : nullptr;
        uint8_t st = 0;
        if (wt && SafeCopy(&st, static_cast<char*>(wt) + 0x23C + team, 1) && st <= 2) {
            standing = st;
            hostile = st == 2;
            why = "team standing";
        } else {
            hostile = info.enemy;
            why = info.enemy ? "no team, enemy class" : "no team, not an enemy class";
        }
    }
    bool found = false;
    for (PawnSide& s : g_pawnSide)
        if (s.pawn == pw) {
            s = {pw, cls, hostile, now + 2.0};
            found = true;
        }
    if (!found) {
        if (g_pawnSide.size() >= 128) g_pawnSide.erase(g_pawnSide.begin());
        g_pawnSide.push_back({pw, cls, hostile, now + 2.0});
    }
    // Una riga per combinazione (classe, squadra, esito), al piu' 32 per sessione.
    static std::vector<uint64_t> logged;
    uint64_t key = (static_cast<uint64_t>(cls) << 32) | (static_cast<uint64_t>(team & 0xFF) << 24) |
                   (static_cast<uint64_t>(standing & 0xFF) << 16) | (static_cast<uint64_t>(squad & 0xFF) << 8) | (hostile ? 1u : 0u);
    if (logged.size() < 32 && std::find(logged.begin(), logged.end(), key) == logged.end()) {
        logged.push_back(key);
        char b[220];
        std::snprintf(b, sizeof(b), "PROPS: pawn %s: squad %d, team %d, standing %d (%s) -> %s",
                      vr::script::ObjectName(reinterpret_cast<void*>(cls)).c_str(), squad, team, standing, why,
                      hostile ? "enemy" : "not a target");
        vr::log::Line(b);
    }
    return hostile;
}

// Nemici vivi entro 15 m da Walker (al piu' maxN).
// Distacco (BreakawayStep): la sfera (c, r) tocca il cilindro di un Pawn vivo
// qualsiasi tranne Walker (nemici, compagni, civili)?
bool TouchingPawn(void* walker, const float* c, float r) {
    if (!walker) return false;
    uint32_t wi = U32(walker, kOffActorWorldInfo);
    uint32_t q = wi ? U32(reinterpret_cast<void*>(wi), kOffWorldPawnList) : 0;
    for (int guard = 0; q && guard < 512; ++guard) {
        void* pw = reinterpret_cast<void*>(q);
        q = U32(pw, kOffPawnNextPawn);
        if (pw == walker || static_cast<int32_t>(U32(pw, kOffPawnHealth)) <= 0 || (U32(pw, kOffActorFlags9C) & kDeleteMe)) continue;
        float loc[3], h = 0.0f, rr = 0.0f;
        uint32_t cyl = U32(pw, kOffPawnCylinder);
        if (!cyl || !SafeCopy(loc, static_cast<char*>(pw) + kOffActorLocation, 12) ||
            !SafeCopy(&h, reinterpret_cast<char*>(cyl) + kOffCylHeight, 4) || !SafeCopy(&rr, reinterpret_cast<char*>(cyl) + kOffCylRadius, 4))
            continue;
        if (!(rr > 1.0f && rr < 200.0f && h > 1.0f && h < 300.0f)) continue;
        const float dx = c[0] - loc[0], dy = c[1] - loc[1], dz = std::fabs(c[2] - loc[2]);
        if (std::sqrt(dx * dx + dy * dy) < rr + r && dz < h + r) return true;
    }
    return false;
}

int GatherPawns(void* walker, HitPawn* out, int maxN, double now) {
    uint32_t wi = U32(walker, kOffActorWorldInfo);
    float wl[3];
    if (!wi || !SafeCopy(wl, static_cast<char*>(walker) + kOffActorLocation, 12)) return 0;
    const float range = 15.0f * vr::control::g_posScale;
    uint32_t q = U32(reinterpret_cast<void*>(wi), kOffWorldPawnList);
    int n = 0;
    for (int guard = 0; q && guard < 512 && n < maxN; ++guard) {
        void* pw = reinterpret_cast<void*>(q);
        q = U32(pw, kOffPawnNextPawn);
        if (pw == walker || static_cast<int32_t>(U32(pw, kOffPawnHealth)) <= 0 || (U32(pw, kOffActorFlags9C) & (kHidden | kDeleteMe)))
            continue;
        HitPawn hp;
        if (!SafeCopy(hp.loc, static_cast<char*>(pw) + kOffActorLocation, 12)) continue;
        float dx = hp.loc[0] - wl[0], dy = hp.loc[1] - wl[1], dz = hp.loc[2] - wl[2];
        if (dx * dx + dy * dy + dz * dz > range * range) continue;
        uint32_t cls = U32(pw, kOffClass), cyl = U32(pw, kOffPawnCylinder);
        if (!cls || !cyl || !PawnHostile(walker, pw, cls, now)) continue;
        if (!SafeCopy(&hp.h, reinterpret_cast<char*>(cyl) + kOffCylHeight, 4) ||
            !SafeCopy(&hp.r, reinterpret_cast<char*>(cyl) + kOffCylRadius, 4) || !(hp.r > 1.0f && hp.r < 200.0f) ||
            !(hp.h > 1.0f && hp.h < 300.0f))
            continue;
        hp.pawn = pw;
        out[n++] = hp;
    }
    return n;
}

float Clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

// Distanza tra i segmenti p1-q1 e p2-q2 (Ericson, Real-Time Collision Detection, 5.1.9).
float SegSegDist(const float* p1, const float* q1, const float* p2, const float* q2) {
    float d1[3], d2[3], r[3];
    for (int i = 0; i < 3; ++i) {
        d1[i] = q1[i] - p1[i];
        d2[i] = q2[i] - p2[i];
        r[i] = p1[i] - p2[i];
    }
    const float a = Dot3(d1, d1), e = Dot3(d2, d2), f = Dot3(d2, r), eps = 1e-6f;
    float s = 0.0f, t = 0.0f;
    if (a <= eps && e <= eps) {
        // due punti
    } else if (a <= eps) {
        t = Clamp01(f / e);
    } else {
        const float c = Dot3(d1, r);
        if (e <= eps) {
            s = Clamp01(-c / a);
        } else {
            const float b = Dot3(d1, d2), den = a * e - b * b;
            s = den > eps ? Clamp01((b * f - c * e) / den) : 0.0f;
            t = (b * s + f) / e;
            if (t < 0.0f) {
                t = 0.0f;
                s = Clamp01(-c / a);
            } else if (t > 1.0f) {
                t = 1.0f;
                s = Clamp01((b - c) / a);
            }
        }
    }
    float d2s = 0.0f;
    for (int i = 0; i < 3; ++i) {
        float k = p1[i] + d1[i] * s - (p2[i] + d2[i] * t);
        d2s += k * k;
    }
    return std::sqrt(d2s);
}

bool InCylinder(const float* p, const float* c, float r, float h) {
    float dx = p[0] - c[0], dy = p[1] - c[1];
    return dx * dx + dy * dy < r * r && std::fabs(p[2] - c[2]) < h;
}

// Parametro (0..1) in cui il segmento a-b esce dal cilindro verticale (centro
// c, raggio r, mezza altezza h): 0 se a e' gia' fuori, oltre 1 se non ne esce.
float ExitCylinder(const float* a, const float* b, const float* c, float r, float h) {
    if (!InCylinder(a, c, r, h)) return 0.0f;
    float ax = a[0] - c[0], ay = a[1] - c[1], az = a[2] - c[2];
    float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
    float tc = 2.0f, tz = 2.0f, A = dx * dx + dy * dy;
    if (A > 1e-6f) {
        float B = 2.0f * (ax * dx + ay * dy), C = ax * ax + ay * ay - r * r, disc = B * B - 4.0f * A * C;
        if (disc >= 0.0f) tc = (-B + std::sqrt(disc)) / (2.0f * A);
    }
    if (dz > 1e-6f) tz = (h - az) / dz;
    else if (dz < -1e-6f) tz = (-h - az) / dz;
    return (std::min)(tc, tz);
}

// Capi dell'asse piu' lungo della mesh (nello spazio della mesh). false se
// l'oggetto non e' lungo (asse maggiore sotto 16 cm o meno di 1,5 volte il secondo).
bool LongAxisEnds(void* comp, float out[2][3]) {
    float lc[3], le[3], m[4][4];
    if (!MeshLocalBounds(comp, lc, le) || !ReadMatrix(comp, m)) return false;
    float e[3];
    for (int i = 0; i < 3; ++i) e[i] = le[i] * std::sqrt(m[i][0] * m[i][0] + m[i][1] * m[i][1] + m[i][2] * m[i][2]);
    int L = e[0] >= e[1] ? (e[0] >= e[2] ? 0 : 2) : (e[1] >= e[2] ? 1 : 2);
    float second = (std::max)(e[(L + 1) % 3], e[(L + 2) % 3]);
    if (e[L] < 0.08f * vr::control::g_posScale || e[L] < 1.5f * second) return false;
    for (int k = 0; k < 2; ++k) {
        std::memcpy(out[k], lc, sizeof(lc));
        out[k][L] += (k ? 1.0f : -1.0f) * le[L];
    }
    return true;
}

bool LocalToWorld(void* comp, const float* local, float out[3]);

// Punta di un oggetto lungo tenuto in mano: il capo dell'asse maggiore
// lontano dalla mano (nello spazio della mesh).
bool HeldTipLocal(void* comp, const float* handPos, float out[3]) {
    float ends[2][3];
    if (!LongAxisEnds(comp, ends)) return false;
    float best = -1.0f;
    for (int k = 0; k < 2; ++k) {
        float w[3];
        if (!LocalToWorld(comp, ends[k], w)) return false;
        float d = std::sqrt((w[0] - handPos[0]) * (w[0] - handPos[0]) + (w[1] - handPos[1]) * (w[1] - handPos[1]) +
                            (w[2] - handPos[2]) * (w[2] - handPos[2]));
        if (d > best) {
            best = d;
            std::memcpy(out, ends[k], sizeof(ends[k]));
        }
    }
    return true;
}

bool LocalToWorld(void* comp, const float* local, float out[3]) {
    float m[4][4];
    if (!ReadMatrix(comp, m)) return false;
    for (int c = 0; c < 3; ++c) out[c] = local[0] * m[0][c] + local[1] * m[1][c] + local[2] * m[2][c] + m[3][c];
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

// Osso del corpo del PhysicsAsset del nemico piu' vicino al punto at (FName,
// 0 se non c'e'): SkeletalMeshComponent.PhysicsAsset +0x258 -> BodySetup
// (TArray +0x40) -> RB_BodySetup.BoneName +0x84; posizione delle ossa con
// GetBoneLocation (nativa senza opcode: ProcessEvent; parametri BoneName @0,
// Space @8, ritorno @0xC). Solo a colpo avvenuto: al piu' 32 chiamate.
// dist: distanza (unita') dell'osso scelto dal punto, se richiesta.
int32_t ClosestBodyBone(void* mesh, const float* at, float* dist = nullptr) {
    uint32_t pa = U32(mesh, kOffSkelPhysAsset);
    struct {
        uint32_t data;
        int32_t num, max;
    } arr = {};
    if (!pa || !SafeCopy(&arr, reinterpret_cast<char*>(pa) + 0x40, sizeof(arr)) || !arr.data || arr.num <= 0) return 0;
    int32_t best = 0;
    float bestD = 1e30f;
    for (int i = 0; i < arr.num && i < 32; ++i) {
        uint32_t bs = U32(reinterpret_cast<void*>(arr.data), 4 * i);
        struct {
            int32_t name[2];
            int32_t space;
            float ret[3];
        } q = {{0, 0}, 0, {0, 0, 0}};
        if (!bs || !SafeCopy(q.name, reinterpret_cast<char*>(bs) + 0x84, 8) || q.name[0] <= 0) continue;
        if (!vr::script::CallFunction(mesh, "GetBoneLocation", &q)) continue;
        float d = (q.ret[0] - at[0]) * (q.ret[0] - at[0]) + (q.ret[1] - at[1]) * (q.ret[1] - at[1]) + (q.ret[2] - at[2]) * (q.ret[2] - at[2]);
        if (std::isfinite(d) && d < bestD) {
            bestD = d;
            best = q.name[0];
        }
    }
    if (dist) *dist = best ? std::sqrt(bestD) : 1e30f;
    return best;
}

// --- Lancio che ferisce, stordisce o atterra (voce 1 del piano, 26/10) ------
// Un oggetto lanciato (origine kThrown: lasciato dalla mano col grip a 1,5 m/s
// o piu') che la pipeline vede colpire un nemico. I bersagli della pipeline
// sono gia' solo i nemici (GatherPawns -> PawnHostile): squadra, alleati e
// civili non ricevono mai niente (nemmeno uno stumble). Tre livelli
// (vr::pure::ThrowHitTier) dall'energia cinetica rispetto al nemico e
// dall'osso:
// - leggero: Pawn.TakeDamage(3-15, PC di Walker, punto, spinta, YDamageType_Melee,
//   HitInfo con l'osso e la mesh, oggetto): la reazione al colpo del gioco.
//   YDamageType_Melee ha m_neverKills (+0x88 bit 0x1, letto in memoria): un
//   oggetto lanciato non uccide;
// - pesante (radio, sacco, valigia; o qualunque oggetto da 1 kg in testa):
//   YGamePawn.HandleMeleeAttack(Walker, spinta, danno, YDamageType_Melee), la
//   stessa funzione della mischia di Walker sulla vittima (ParmsSize 0x28:
//   attacker @0, Momentum @4, Damage @0x10, DamageType @0x14, doResetPawn
//   @0x18, ResetLocation @0x1C; iNative 0, letti in memoria 26/10). Prima
//   IsMeleeAttackable(Walker, checkStanding 1, YMAT_None) (ParmsSize 0x10,
//   ritorno @0xC; nativa 0x636310 -> 0x00EC0BF0: falso per chi e' gia' a
//   terra, +0x5FC bit 0x40000, per i morti, chi cade, chi e' in una mossa
//   speciale che la mischia non interrompe): se no, livello leggero. Misurato
//   sul manichino con un AIController: la mossa speciale 9, barcolla 3,5 m
//   lungo Momentum (il modulo non conta: 20 e 5000 uguali), poi a terra
//   (m_isKnockedDown, +0x5FC bit 0x10000) ~1 s dopo;
//   Solo con un YPlayerReplicationInfo (Pawn.PlayerReplicationInfo +0x36C;
//   revisione della voce 1, 26/10): il gioco si e' chiuso (0xC0000005 a
//   0x008323C4) sullo sparo dell'esecuzione di un manichino atterrato senza
//   PRI. FUN_00832350 (virtuale di YGamePawn in 13 vtable) per un pawn con
//   controller, vivo e a terra (+0x5FC bit 0x40000, bit 8 spento) legge
//   Cast<YPlayerReplicationInfo>(PRI)+0x2D4 (m_canBeDamagedInDBNO, bit 0x40)
//   senza controllare il null. I nemici veri hanno un PRI (la squadra e
//   m_canBeExecutedInDBNO stanno li'); un pawn senza resta al livello leggero,
//   che non lo atterra;
// - stordimento: un colpo leggero in testa con YDamageType_Stun al posto della
//   mischia, solo se il controller e' un YAIController con m_mayReactToStun
//   (+0x3E8 bit 0x8000000: le classi non lo accendono, lo accende il Kismet di
//   certe scene). Danno 1, e solo con piu' di 1 di vita: YDamageType_Stun non
//   ha m_neverKills e un atterramento lascia il nemico a 1 (revisione).
//   Nessuno stordimento visto nel gioco finora (non verificato).
// Solo pawn con un controller: senza (il manichino evocato, i pawn di scena)
// il gioco li uccide con qualunque danno (HandleMeleeAttack con 1: mossa 53 e
// ragdoll; TakeDamage(8, Melee): 75 -> 0), e un oggetto lanciato non uccide.
// Massa: GetBodyMass x 40 kg per unita' (kThrowKgPerUnit), non masskg: i
// livelli non cambiano con la sensazione del peso scelta dal giocatore.
// Testa: solo se il corpo toccato e' entro la portata del contatto
// (PropHit.boneNear), non il corpo piu' vicino a qualunque distanza.
// Filtro dei salti del tracciamento: ThrowRecord (rilascio) e ThrowDamageSpeed;
// il limite e' la velocita' affidabile nel mondo meno quella del nemico.
// Al piu' un atterramento ogni 1,5 s per nemico (due oggetti nello stesso
// colpo: il secondo e' leggero). Il log e le statistiche dicono l'esito vero:
// vita, mossa speciale (+0x491) e bit di atterramento (+0x5FC) prima e dopo la
// chiamata; se non cambia niente, "no effect".
constexpr uintptr_t kOffPawnController = 0x214, kOffActorVelocity = 0x140;
constexpr uintptr_t kOffPawnPRI = 0x36C;              // Pawn.PlayerReplicationInfo
constexpr uintptr_t kOffPawnSpecialMove = 0x491;      // YGamePawn: mossa speciale in corso (byte; 9 YSPM_TakeHit)
constexpr uintptr_t kOffPawnKnockFlags = 0x5FC;       // YGamePawn: m_isKnockedDown 0x10000, a terra 0x40000
constexpr uintptr_t kOffWalkerMeleeDamage = 0x1C48;  // YGamePawn.m_meleeDamagePawns (Walker: 40)
constexpr uintptr_t kOffAiMayReactToStun = 0x3E8;    // YAIController.m_mayReactToStun, bit 0x8000000
constexpr float kThrowKgPerUnit = 40.0f;              // il masskg predefinito (radio 0,128 -> 5,1 kg)
struct ThrowDmgStats {
    uint32_t hits = 0, light = 0, heavy = 0, stun = 0, none = 0, refused = 0, noEffect = 0, suspect = 0;
} g_throwDmgStats;
std::vector<std::pair<void*, double>> g_knockCooldown;  // nemico -> fino a quando niente atterramento

bool HeadBone(int32_t bone) {
    static std::unordered_map<int32_t, bool> cache;
    if (bone <= 0) return false;
    auto it = cache.find(bone);
    if (it != cache.end()) return it->second;
    std::string n = vr::script::NameFromIndex(bone);
    for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const bool head = n.find("head") != std::string::npos || n.find("neck") != std::string::npos;
    if (cache.size() < 256) cache[bone] = head;
    return head;
}

// Attore ancora vivo (non bDeleteMe) e leggibile; ActorUsable vale solo per i corpi rigidi.
bool ActorAlive(void* a) {
    uint32_t f9c = 0;
    return a && TrySafeReadUint32(static_cast<char*>(a) + kOffActorFlags9C, &f9c) && !(f9c & kDeleteMe);
}

// Il nemico ha un YPlayerReplicationInfo (vedi sopra: senza, atterrarlo fa
// cadere il gioco nella FUN_00832350).
bool PawnHasYPri(void* pw) {
    static int32_t yPriName = -2;
    if (yPriName == -2) yPriName = vr::script::NameIndex("YPlayerReplicationInfo");
    void* pri = reinterpret_cast<void*>(U32(pw, kOffPawnPRI));
    if (!pri || !ActorAlive(pri) || yPriName < 0) return false;
    const uint32_t cls = U32(pri, kOffClass);
    return cls && ClassIsA(cls, yPriName);
}

struct PawnDmgState {
    int32_t hp = 0;
    uint8_t sm = 0;
    uint32_t knock = 0;
};
PawnDmgState ReadPawnDmgState(void* pw) {
    PawnDmgState st;
    st.hp = static_cast<int32_t>(U32(pw, kOffPawnHealth));
    SafeCopy(&st.sm, static_cast<char*>(pw) + kOffPawnSpecialMove, 1);
    st.knock = U32(pw, kOffPawnKnockFlags) & 0x50000u;
    return st;
}

// Chiamate al gioco condivise dalle voci 1 (lancio) e 4 (mischia).
// IsMeleeAttackable(Walker, checkStanding 1, YMAT_None): 1 si', 0 no, -1 chiamata fallita.
int MeleeAttackable(void* pw, void* walker) {
    struct {
        void* attacker;
        int32_t checkStanding;
        uint8_t type;
        uint8_t pad[3];
        int32_t ret;
        uint8_t spare[0x30];
    } q = {};
    q.attacker = walker;
    q.checkStanding = 1;
    q.type = 0;  // YMAT_None
    if (!vr::script::CallFunction(pw, "IsMeleeAttackable", &q)) return -1;
    return (q.ret & 1) ? 1 : 0;
}

// YGamePawn.HandleMeleeAttack(Walker, Momentum, Damage, YDamageType_Melee): la reazione della mischia di Walker.
bool CallHandleMelee(void* pw, void* walker, const float* momentum, int32_t damage) {
    static void* meleeClass = nullptr;
    struct {
        void* attacker;
        float momentum[3];
        int32_t damage;
        void* damageType;
        int32_t doResetPawn;
        float resetLocation[3];
        uint8_t spare[0x40];  // ParmsSize 0x28 (ProcessEvent ne copia solo tanti; locali fino a +0x44): solo margine
    } m = {};
    m.attacker = walker;
    std::memcpy(m.momentum, momentum, sizeof(m.momentum));
    m.damage = damage;
    m.damageType = ClassByName("YDamageType_Melee", &meleeClass);
    return m.damageType && vr::script::CallFunction(pw, "HandleMeleeAttack", &m);
}

// Pawn.TakeDamage(damage, PC di Walker, punto, spinta, tipo, HitInfo con l'osso e la mesh, oggetto).
bool CallPawnTakeDamage(void* pw, void* pc, const PropHit& e, const float* momentum, int32_t damage, void* damageType) {
    struct {
        int32_t damage;
        void* instigator;
        float loc[3];
        float momentum[3];
        void* damageType;
        void* hiMaterial;
        void* hiPhysMaterial;
        int32_t hiItem;
        int32_t hiLevelIndex;
        int32_t hiBoneName[2];
        void* hiComponent;
        void* causer;
        uint8_t spare[0x80];  // ParmsSize 0x44 (YGamePawn.TakeDamage ha locali fino a +0xAC)
    } td = {};
    td.damage = damage;
    td.instigator = pc;
    std::memcpy(td.loc, e.loc, sizeof(td.loc));
    std::memcpy(td.momentum, momentum, sizeof(td.momentum));
    td.damageType = damageType;
    td.hiItem = -1;
    td.hiLevelIndex = -1;
    td.hiBoneName[0] = e.bone;
    uint32_t mesh = U32(pw, kOffPawnMesh);
    td.hiComponent = mesh ? reinterpret_cast<void*>(mesh) : e.targetComp;
    td.causer = ActorAlive(e.prop) ? e.prop : nullptr;
    return damageType && vr::script::CallFunction(pw, "TakeDamage", &td);
}

void ThrowDamage(const PropHit& e, double now) {
    if (!vr::control::g_propThrowDamage.load() || e.targetKind != kTgtPawn || e.kind != HitKind::kFlying ||
        e.source != HitSource::kThrown)
        return;
    void* walker = g_tickPawn;
    void* pw = e.target;
    if (!walker || !pw || pw == walker || !ActorAlive(pw)) return;
    // L'attaccante passato al gioco e' Walker: deve essere un YGamePawn (non un
    // veicolo o una torretta, su cui IsHostile e m_meleeDamagePawns non valgono).
    const uint32_t wcls = U32(walker, kOffClass);
    if (!(g_yGamePawnName >= 0 && wcls && ClassIsA(wcls, g_yGamePawnName))) return;
    const PawnDmgState st0 = ReadPawnDmgState(pw);
    const int32_t hp0 = st0.hp;
    if (hp0 <= 0) return;
    // Ancora nemico (la pipeline lo ha appena deciso: in cache per 2 s).
    const uint32_t cls = U32(pw, kOffClass);
    if (!cls || !PawnHostile(walker, pw, cls, now)) return;
    const float s = vr::control::g_posScale;
    ++g_throwDmgStats.hits;
    // Velocita' rispetto al nemico (Actor.Velocity: chi corre incontro all'oggetto lo prende piu' forte).
    float pv[3] = {0, 0, 0};
    if (!SafeCopy(pv, static_cast<char*>(pw) + kOffActorVelocity, 12) || !std::isfinite(pv[0] + pv[1] + pv[2])) pv[0] = pv[1] = pv[2] = 0.0f;
    const float rv[3] = {e.vel[0] - pv[0], e.vel[1] - pv[1], e.vel[2] - pv[2]};
    const float impact = std::sqrt(Dot3(rv, rv)) / s;
    // Filtro dei salti del tracciamento: al piu' il rilascio affidabile (nel
    // mondo, rispetto al nemico) piu' la caduta.
    const ThrowRecord* rec = nullptr;
    for (const ThrowRecord& r : g_throwRec)
        if (r.comp == e.comp && now - r.at < 6.0) rec = &r;
    float relRelease = -1.0f;
    if (rec) {
        const float d[3] = {rec->tw[0] - pv[0], rec->tw[1] - pv[1], rec->tw[2] - pv[2]};
        relRelease = std::sqrt(Dot3(d, d)) / s;
    }
    const float g = -GravityZ(walker) / s;
    const float vDmg = vr::pure::ThrowDamageSpeed(impact, relRelease, rec ? (rec->z - e.loc[2]) / s : 0.0f, g > 1.0f ? g : 15.0f, 1.0f, 15.0f);
    const bool suspect = rec && rec->suspect;
    if (suspect) ++g_throwDmgStats.suspect;
    const float kg = e.mass > 0.0f ? e.mass * kThrowKgPerUnit : 0.0f;
    const bool head = HeadBone(e.bone) && e.boneNear;
    vr::pure::ThrowHitParams prm;
    const int32_t wm = static_cast<int32_t>(U32(walker, kOffWalkerMeleeDamage));
    if (wm >= 10 && wm <= 200) prm.heavyMax = static_cast<float>(wm);  // mai piu' della mischia di Walker
    vr::pure::ThrowHitOut t = vr::pure::ThrowHitTier(kg, vDmg, head, suspect, prm);
    // Atterramento: YGamePawn con un controller e un YPlayerReplicationInfo,
    // nessun atterramento recente, IsMeleeAttackable.
    const char* why = suspect && t.demoted ? "tracking jump at release" : "";
    g_knockCooldown.erase(std::remove_if(g_knockCooldown.begin(), g_knockCooldown.end(),
                                         [&](const std::pair<void*, double>& c) { return now >= c.second; }),
                          g_knockCooldown.end());
    void* ctrl = reinterpret_cast<void*>(U32(pw, kOffPawnController));
    bool refused = false;
    if (!ctrl || !ActorAlive(ctrl)) {
        // Senza controller (manichino evocato, pawn di scena) il gioco lo
        // uccide con qualunque danno: TakeDamage(8, Melee) 75 -> 0 e ragdoll,
        // HandleMeleeAttack anche con 1 (26/10). Un oggetto lanciato non uccide:
        // niente.
        refused = t.tier != 0;
        t.tier = 0;
        t.damage = 0;
        why = "no controller: the game would kill it with any damage";
    }
    if (t.tier == 2) {
        bool ok = true;
        if (!(g_yGamePawnName >= 0 && ClassIsA(cls, g_yGamePawnName))) {
            ok = false;
            why = "not a YGamePawn";
        } else if (!PawnHasYPri(pw)) {
            ok = false;
            why = "no YPlayerReplicationInfo: the game crashes on a knocked-down pawn without one";
        } else {
            for (const auto& c : g_knockCooldown)
                if (c.first == pw) {
                    ok = false;
                    why = "knocked down less than 1.5 s ago";
                }
        }
        if (ok) {
            const int ma = MeleeAttackable(pw, walker);
            if (ma < 0) {
                ok = false;
                why = "IsMeleeAttackable call failed";
            } else if (ma == 0) {
                ok = false;
                why = "IsMeleeAttackable false (down, dead or in a move the melee does not interrupt)";
            }
        }
        if (!ok) {
            const vr::pure::ThrowHitOut l = vr::pure::ThrowHitTier(kg, vDmg, head, true, prm);
            t.tier = 1;
            t.damage = l.damage;
        }
    }
    // Direzione: il moto dell'oggetto in orizzontale (la mossa del gioco segue Momentum; il modulo non conta).
    float dir[3] = {e.dir[0], e.dir[1], 0.0f};
    float dl = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1]);
    if (dl < 1e-3f) {
        float wl[3] = {0, 0, 0}, tl[3] = {0, 0, 0};
        SafeCopy(wl, static_cast<char*>(walker) + kOffActorLocation, 12);
        SafeCopy(tl, static_cast<char*>(pw) + kOffActorLocation, 12);
        dir[0] = tl[0] - wl[0];
        dir[1] = tl[1] - wl[1];
        dl = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1]);
    }
    if (dl > 1e-3f) {
        dir[0] /= dl;
        dir[1] /= dl;
    }
    const float mom = kg * vDmg * s;  // kg x unita'/s (quantita' di moto rispetto al nemico)
    static void* meleeClass = nullptr;
    static void* stunClass = nullptr;
    void* pc = reinterpret_cast<void*>(U32(walker, kOffPawnController));
    const char* did = "nothing";
    bool stun = false, called = false;
    const float momentum[3] = {dir[0] * mom, dir[1] * mom, dir[2] * mom};
    if (t.tier == 2) {
        if (CallHandleMelee(pw, walker, momentum, t.damage)) {
            called = true;
            did = "HandleMeleeAttack (stagger / knock-down)";
            if (g_knockCooldown.size() < 64) g_knockCooldown.push_back({pw, now + 1.5});
        } else {
            did = "HandleMeleeAttack failed";
        }
    } else if (t.tier == 1) {
        // Stordimento: colpo leggero in testa, solo se il controller lo permette
        // e se il danno 1 dello stordimento (senza m_neverKills) non lo uccide.
        uint32_t ccls = ctrl ? U32(ctrl, kOffClass) : 0;
        static int32_t yaiName = -2;
        if (yaiName == -2) yaiName = vr::script::NameIndex("YAIController");
        const bool mayStun = head && hp0 > 1 && ccls && ActorAlive(ctrl) && yaiName >= 0 && ClassIsA(ccls, yaiName) &&
                             (U32(ctrl, kOffAiMayReactToStun) & 0x8000000);
        void* stunType = mayStun ? ClassByName("YDamageType_Stun", &stunClass) : nullptr;
        stun = stunType != nullptr;
        if (CallPawnTakeDamage(pw, pc, e, momentum, stun ? 1 : t.damage, stun ? stunType : ClassByName("YDamageType_Melee", &meleeClass))) {
            called = true;
            did = stun ? "TakeDamage (stun)" : "TakeDamage (light)";
        } else {
            did = "TakeDamage failed";
        }
    } else if (refused) {
        ++g_throwDmgStats.refused;
    } else {
        ++g_throwDmgStats.none;
    }
    // Esito vero (revisione della voce 1): la chiamata riuscita non basta, conta
    // se vita, mossa speciale o bit di atterramento sono cambiati.
    const PawnDmgState st1 = ReadPawnDmgState(pw);
    const bool effect = st1.hp != st0.hp || st1.sm != st0.sm || st1.knock != st0.knock;
    if (called) {
        if (!effect) {
            ++g_throwDmgStats.noEffect;
            did = t.tier == 2 ? "HandleMeleeAttack: no effect (health, special move and knock-down flags unchanged)"
                              : "TakeDamage: no effect (health, special move and knock-down flags unchanged)";
        } else if (t.tier == 2) {
            ++g_throwDmgStats.heavy;
        } else {
            ++(stun ? g_throwDmgStats.stun : g_throwDmgStats.light);
        }
    }
    std::string tail = t.tier ? std::string(", ") + did : std::string();
    if (*why) tail += std::string(t.tier ? " (not knocked down: " : " (no effect: ") + why + ")";
    char b[640];
    std::snprintf(b, sizeof(b),
                  "PROPS: throw hit %s%s: %.1f kg at %.1f m/s (impact %.1f, release %.1f trusted / %.1f given, %.1f against the enemy%s) = %.0f J -> %s, "
                  "%d damage, health %d -> %d, special move %u -> %u, knock flags %05X -> %05X%s",
                  vr::script::ClassName(pw).c_str(), head ? " (head)" : (HeadBone(e.bone) ? " (head bone out of reach: body)" : ""), kg, vDmg,
                  impact, rec ? rec->trusted / s : -1.0f, rec ? rec->raw / s : -1.0f, relRelease,
                  suspect ? ", tracking jump at release" : (rec ? "" : ", no release record"), t.joules,
                  t.tier == 2 ? "heavy" : (t.tier == 1 ? (stun ? "stun" : "light") : "none"), t.tier ? (stun ? 1 : t.damage) : 0, hp0, st1.hp,
                  st0.sm, st1.sm, st0.knock, st1.knock, tail.c_str());
    vr::log::Line(b);
}

// --- Mischia con oggetti tenuti, bottiglia in testa (voce 4 del piano, 26/10) ---
// Un oggetto tenuto in mano che la pipeline vede colpire un nemico (evento
// kHeld: sweep di centro e punta dalla mano, con la velocita' della mano piu'
// polso x leva come in Drive; gli oggetti tenuti non hanno NX_IGNORE_PAIR con
// i nemici, ma la loro capsula li ferma o li devia senza dire niente al gioco).
// - Conta solo oltre 2,5 m/s del punto che colpisce, nel mondo e anche
//   rispetto a Walker (PropHit.swing: camminare con l'oggetto fermo contro un
//   nemico non e' un colpo), non rispetto al nemico (vr::pure::MeleeHit); al
//   piu' un colpo ogni 0,6 s per nemico.
// - Effetto: la reazione della mischia di Walker, HandleMeleeAttack (da
//   3 m/s con almeno 0,5 kg, o in testa: barcolla o cade, come il tasto B), o
//   TakeDamage leggero; YDamageType_Melee ha m_neverKills: non uccide mai, come
//   la mischia di Walker (poi B per l'esecuzione). Danno dall'energia col
//   braccio, al piu' 20 al corpo, testa x2 al piu' la mischia di Walker (40).
//   Stesse regole della voce 1: niente senza controller (il gioco ucciderebbe
//   il pawn con qualunque danno), la reazione solo con un
//   YPlayerReplicationInfo e IsMeleeAttackable, al piu' un atterramento ogni
//   1,5 s per nemico (g_knockCooldown, condiviso col lancio).
// - Niente doppio colpo con la mischia di Walker (B, o il gesto col fucile):
//   durante la sua mossa (PlayMeleeAttack 8, AIMeleeAttack 7) e per 0,6 s
//   dopo, o con m_currentMeleeTarget (+0x1C44) su questo nemico, l'oggetto non
//   conta (g_walkerMeleeUntil). Il pugno col gesto della sinistra non preme B
//   con un oggetto in mano (vr_input.cpp, voce 2), e ora nemmeno il colpo col fucile
//   con un oggetto nella destra. Lo stato di mischia di Walker
//   (m_currentMeleeTarget, mossa speciale, m_hasPerformedMeleeCheck +0x5F8 bit
//   0x200000) e' letto prima e dopo la chiamata e scritto nel log.
// - Un oggetto che si rompe (KActorFromStatic con template: bottiglie,
//   bicchieri, vasi) si rompe sul colpo con BreakProp (effetto e suono del
//   gioco) e la mano lo lascia, anche quando il danno non conta (attesa,
//   mischia di Walker, nessun controller): ha colpito un corpo abbastanza forte.
// - Vibrazione di 70 ms (niente sulla mano dell'arma); la guida frena
//   l'oggetto lungo il colpo per 2 tick (Drive, meleeBrakeTicks): si ferma
//   sul corpo.
struct MeleeStats {
    uint32_t hits = 0, stagger = 0, light = 0, soft = 0, cooldown = 0, walkerMelee = 0, refused = 0, noEffect = 0, broken = 0;
} g_meleeStats;
std::vector<std::pair<void*, double>> g_meleeCooldown;  // nemico -> fino a quando nessun colpo in mano conta
double g_walkerMeleeUntil = 0.0;                        // mischia di Walker in corso o appena finita
constexpr uintptr_t kOffPawnMeleeTarget = 0x1C44;       // YGamePawn.m_currentMeleeTarget
constexpr uintptr_t kOffPawnFlags5F8 = 0x5F8;           // YGamePawn: m_hasPerformedMeleeCheck bit 0x200000

// Ogni tick (HitPipeline): la mischia di Walker (mossa 7 o 8) apre la finestra di 0,6 s.
void WalkerMeleeTrack(void* walker, double now) {
    uint8_t sm = 0;
    if (walker && SafeCopy(&sm, static_cast<char*>(walker) + kOffPawnSpecialMove, 1) && (sm == 7 || sm == 8)) g_walkerMeleeUntil = now + 0.6;
}

struct WalkerMeleeState {
    uint32_t target = 0;
    uint8_t sm = 0;
    bool check = false;
};
WalkerMeleeState ReadWalkerMelee(void* walker) {
    WalkerMeleeState w;
    w.target = U32(walker, kOffPawnMeleeTarget);
    SafeCopy(&w.sm, static_cast<char*>(walker) + kOffPawnSpecialMove, 1);
    w.check = (U32(walker, kOffPawnFlags5F8) & 0x200000u) != 0;
    return w;
}

// true se ha dato la vibrazione del colpo (OnPropHit non da' quella generica).
bool HeldMelee(const PropHit& e, double now) {
    if (!vr::control::g_propMelee.load() || e.kind != HitKind::kHeld || e.targetKind != kTgtPawn || e.hand < 0 || e.hand > 1) return false;
    const int h = e.hand;
    Held& held = g_held[h];
    void* walker = g_tickPawn;
    void* pw = e.target;
    if (held.comp != e.comp || !held.actor || !walker || !pw || pw == walker || !ActorAlive(pw)) return false;
    const uint32_t wcls = U32(walker, kOffClass), cls = U32(pw, kOffClass);
    if (!(g_yGamePawnName >= 0 && wcls && ClassIsA(wcls, g_yGamePawnName))) return false;  // Walker a piedi, non un veicolo
    const PawnDmgState st0 = ReadPawnDmgState(pw);
    if (st0.hp <= 0 || !cls || !PawnHostile(walker, pw, cls, now)) return false;
    const float s = vr::control::g_posScale;
    // Velocita' del punto che colpisce come la da' la pipeline: mano (nel
    // mondo, la corsa di Walker compresa) piu' polso x leva, o il comando del
    // peso. Non rispetto al nemico (al contrario della voce 1): un nemico che
    // barcolla ha Actor.Velocity di parecchi m/s, e un oggetto tenuto fermo che
    // lui urtava dopo la mischia di Walker leggeva 7,6 m/s con la mano a 2,3
    // (prima prova, 26/10). Conta il colpo del giocatore: soglia e livello
    // anche rispetto a Walker (swing, senza la sua camminata: revisione, 26/10;
    // RF1 20:53:13 la radio ferma in mano sfiorava a 3,2 m/s un nemico a terra
    // mentre Walker si muoveva: 15 di danno), il danno dalla velocita' nel mondo.
    const float v = e.speed / s;
    const float vSwing = (e.swing >= 0.0f ? e.swing : e.speed) / s;
    const float kg = e.mass > 0.0f ? e.mass * kThrowKgPerUnit : 0.0f;  // come la voce 1: non cambia con masskg
    const bool head = HeadBone(e.bone) && e.boneNear;
    vr::pure::MeleeHitParams prm;
    const int32_t wm = static_cast<int32_t>(U32(walker, kOffWalkerMeleeDamage));
    if (wm >= 10 && wm <= 200) prm.headMax = static_cast<float>(wm);  // mai piu' della mischia di Walker
    vr::pure::MeleeHitOut t = vr::pure::MeleeHit(kg, v, vSwing, head, prm);
    const std::string mesh = vr::script::ObjectName(reinterpret_cast<void*>(U32(e.comp, kOffSmcStaticMesh)));
    char b[800];
    if (t.tier == 0) {
        // Troppo piano: resta il colpo della pipeline (riga "hit", vibrazione d'urto).
        ++g_meleeStats.soft;
        std::snprintf(b, sizeof(b), "PROPS: melee hit %s%s with %s (%s hand%s): %.1f m/s (swing %.1f m/s relative to Walker), under %.1f m/s: no effect%s",
                      vr::script::ClassName(pw).c_str(), head ? " (head)" : "", mesh.c_str(), h ? "left" : "right", e.tip ? ", tip" : "", v, vSwing,
                      prm.minSpeed, v >= prm.minSpeed && vSwing < prm.minSpeed ? " (Walker's own movement, not a swing)" : "");
        vr::log::Line(b);
        return false;
    }
    ++g_meleeStats.hits;
    auto prune = [&](std::vector<std::pair<void*, double>>& list) {
        list.erase(std::remove_if(list.begin(), list.end(), [&](const std::pair<void*, double>& c) { return now >= c.second; }), list.end());
    };
    prune(g_meleeCooldown);
    prune(g_knockCooldown);
    const WalkerMeleeState w0 = ReadWalkerMelee(walker);
    void* ctrl = reinterpret_cast<void*>(U32(pw, kOffPawnController));
    const char* refuse = nullptr;
    for (const auto& c : g_meleeCooldown)
        if (c.first == pw) refuse = "hit less than 0.6 s ago";
    // m_currentMeleeTarget solo se e' questo nemico: lo imposta la mischia di
    // Walker e lo azzera alla fine, ma uno rimasto sporco (mischia interrotta?
    // mai visto) non deve spegnere i colpi sugli altri.
    if (!refuse && (now < g_walkerMeleeUntil || w0.sm == 7 || w0.sm == 8 || w0.target == static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pw))))
        refuse = "Walker's own melee in progress or just done";
    if (!refuse && (!ctrl || !ActorAlive(ctrl))) refuse = "no controller: the game would kill it with any damage";
    if (!refuse && (st0.knock & 0x40000u) && !PawnHasYPri(pw)) refuse = "down without a YPlayerReplicationInfo (the game's DBNO code crashes)";
    const char* demote = "";
    if (!refuse && t.tier == 2) {
        // La reazione della mischia: come il pesante della voce 1.
        if (!(g_yGamePawnName >= 0 && ClassIsA(cls, g_yGamePawnName))) {
            demote = "not a YGamePawn";
        } else if (!PawnHasYPri(pw)) {
            demote = "no YPlayerReplicationInfo: the game crashes on a knocked-down pawn without one";
        } else {
            for (const auto& c : g_knockCooldown)
                if (c.first == pw) demote = "knocked down less than 1.5 s ago";
            if (!*demote) {
                const int ma = MeleeAttackable(pw, walker);
                if (ma < 0) demote = "IsMeleeAttackable call failed";
                else if (ma == 0) demote = "IsMeleeAttackable false (down, dead or in a move the melee does not interrupt)";
            }
        }
        if (*demote) {
            t.tier = 1;
            if (t.damage > static_cast<int>(prm.lightMax)) t.damage = static_cast<int>(prm.lightMax);
        }
    }
    // Direzione: il moto del punto che colpisce in orizzontale (la mossa del gioco segue Momentum).
    float dir[3] = {e.dir[0], e.dir[1], 0.0f};
    float dl = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1]);
    if (dl < 1e-3f) {
        float wl[3] = {0, 0, 0}, tl[3] = {0, 0, 0};
        SafeCopy(wl, static_cast<char*>(walker) + kOffActorLocation, 12);
        SafeCopy(tl, static_cast<char*>(pw) + kOffActorLocation, 12);
        dir[0] = tl[0] - wl[0];
        dir[1] = tl[1] - wl[1];
        dl = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1]);
    }
    if (dl > 1e-3f) {
        dir[0] /= dl;
        dir[1] /= dl;
    }
    const float mom = (kg + prm.armKg) * v * s;
    const float momentum[3] = {dir[0] * mom, dir[1] * mom, 0.0f};
    const char* did = "nothing";
    bool called = false;
    if (!refuse) {
        g_meleeCooldown.push_back({pw, now + 0.6});
        if (t.tier == 2) {
            called = CallHandleMelee(pw, walker, momentum, t.damage);
            did = called ? "HandleMeleeAttack (stagger / knock-down)" : "HandleMeleeAttack failed";
            if (called && g_knockCooldown.size() < 64) g_knockCooldown.push_back({pw, now + 1.5});
        } else {
            static void* meleeClass = nullptr;
            called = CallPawnTakeDamage(pw, reinterpret_cast<void*>(U32(walker, kOffPawnController)), e, momentum, t.damage,
                                        ClassByName("YDamageType_Melee", &meleeClass));
            did = called ? "TakeDamage (light)" : "TakeDamage failed";
        }
    }
    const PawnDmgState st1 = ReadPawnDmgState(pw);
    const WalkerMeleeState w1 = ReadWalkerMelee(walker);
    const bool effect = st1.hp != st0.hp || st1.sm != st0.sm || st1.knock != st0.knock;
    if (refuse) {
        ++(std::strncmp(refuse, "hit less", 8) == 0 ? g_meleeStats.cooldown
                                                     : (std::strncmp(refuse, "Walker", 6) == 0 ? g_meleeStats.walkerMelee : g_meleeStats.refused));
    } else if (called && !effect) {
        ++g_meleeStats.noEffect;
        did = t.tier == 2 ? "HandleMeleeAttack: no effect (health, special move and knock-down flags unchanged)"
                          : "TakeDamage: no effect (health, special move and knock-down flags unchanged)";
    } else if (called) {
        ++(t.tier == 2 ? g_meleeStats.stagger : g_meleeStats.light);
    }
    // Bottiglia, bicchiere, vaso: si rompe sul colpo, la mano lo lascia.
    void* a = held.actor;
    void* c = held.comp;
    const bool breaks = Breakable(a);
    const bool gunHand = GunHandHaptics(h);  // prima del rilascio (la destra senza oggetto e' la mano dell'arma)
    if (breaks) {
        ++g_meleeStats.broken;
        Release(h, "smashed on an enemy");
    } else {
        // Freno della guida: l'oggetto si ferma sul corpo (Drive).
        float d3[3] = {e.dir[0], e.dir[1], e.dir[2]};
        if (Norm3(d3)) {
            std::memcpy(held.meleeBrakeDir, d3, sizeof(d3));
            held.meleeBrakeTicks = 2;
            held.meleeBrakeUntil = now + 0.025;
        }
    }
    // Vibrazione di 70 ms (il piano): ampiezza dalla formula degli urti, almeno
    // 0,8 x il guadagno. impacthaptics off: l'impulso fisso di sempre (70 ms,
    // come l'oggetto rotto in mano).
    // - Mano dell'arma (GunHandHaptics: la sinistra sull'arma, a due mani o
    //   nella ricarica; revisione, 26/10): niente, la mano dell'arma non cambia
    //   (il colpo generico li' era gia' muto, "hand on the gun").
    // - Un impulso partito meno di haprefr fa (il primo contatto lento dello
    //   stesso gesto, 9 ms prima: HitMemo.speed) non ne fa partire un secondo:
    //   questo lo sostituisce (PulseHand: lo slot d'urto della mano), con
    //   almeno la sua ampiezza e 70 ms contati dal suo inizio.
    const float J = vr::pure::ReducedMass(kg, (std::max)(0.1f, g_tune.hapSphereKg)) * v;
    if (gunHand) {
        HapticMuted(h, "melee", e.comp, J, v, "hand on the gun");
    } else if (HapticGain() > 0.0f) {
        vr::pure::HapticPulseSpec sp = vr::pure::ImpactPulse(J, kg, HapticGain(), g_tune.hapJ);
        sp.ms = 70;
        sp.amp = (std::max)(sp.amp, 0.8f * HapticGain());
        const double since = now - g_hapLast[h];
        const char* src = "melee";
        char srcBuf[64];
        if (since >= 0.0 && since < (std::max)(0.0f, g_tune.hapRefr) * 0.001) {
            const unsigned el = static_cast<unsigned>(since * 1000.0);
            sp.ms = el + 10u < 70u ? 70u - el : 10u;
            sp.amp = (std::max)(sp.amp, g_hapLastAmp[h]);
            std::snprintf(srcBuf, sizeof(srcBuf), "melee (replaces the pulse of %u ms ago)", el);
            src = srcBuf;
        }
        HapticSend(h, src, e.comp, J, v, kg, sp, now, false, e.loc);
    } else {
        vr::input::PulseHand(h ? 0 : 1, 70);
    }
    std::string tail = refuse ? std::string("no effect: ") + refuse : std::string(did);
    if (*demote) tail += std::string(" (no stagger: ") + demote + ")";
    std::snprintf(b, sizeof(b),
                  "PROPS: melee hit %s%s with %s (%s hand%s): %.1f kg at %.1f m/s (swing %.1f) = %.0f J (with the arm) -> %s, %d damage%s, health %d -> %d, "
                  "special move %u -> %u, knock flags %05X -> %05X, %s; Walker's melee state: target %08X -> %08X, special move %u -> %u, "
                  "check %d -> %d%s",
                  vr::script::ClassName(pw).c_str(), head ? " (head)" : (HeadBone(e.bone) ? " (head bone out of reach: body)" : ""), mesh.c_str(),
                  h ? "left" : "right", e.tip ? ", tip" : "", kg, v, vSwing, t.joules, refuse ? "none" : (t.tier == 2 ? "stagger" : "light"),
                  refuse ? 0 : t.damage, t.capped && !refuse ? " (capped)" : "", st0.hp, st1.hp, st0.sm, st1.sm, st0.knock, st1.knock, tail.c_str(),
                  w0.target, w1.target, w0.sm, w1.sm, w0.check ? 1 : 0, w1.check ? 1 : 0, breaks ? "; it breaks" : "");
    vr::log::Line(b);
    if (breaks) BreakProp(a, c, head ? "smashed on an enemy's head" : "smashed on an enemy");
    return true;
}

// Vetri, finestre di sabbia e casse (voce 2 del piano, 26/10): un oggetto
// lanciato o tenuto che colpisce un bersaglio del mondo gli fa danno con la
// TakeDamage del gioco (YDamageType_Melee, che ha bCausesFracture: DamageType
// +0x60 bit 0x80; Walker's PC come istigatore, l'oggetto come causa), in base
// all'energia (vr::pure::WorldHitDamage). Il resto e' del gioco:
// - YDestructibleObject (e Delayed): i vetri e le casse dei livelli, e anche le
//   finestre che trattengono la sabbia. Nei pacchetti dei livelli (tutti gli
//   SP_*.upk, tabella dei nomi decompressa, 26/10) la classe YSandBomb non c'e'
//   mai: le finestre di sabbia sono YDestructibleObject con i materiali
//   breakingGlass_sand; la valanga la fa partire il Kismet del livello con
//   YSeqEvent_YDODamageModApplied sulla modifica "Destroyed" (il pullman del
//   livello 1, SP_Desert01_03_K). TakeDamage (script) cerca il pezzo
//   colpito confrontando HitInfo.HitComponent con m_subObjects[i].m_mesh,
//   taglia a m_maxDamagePerHit e chiama DamageSubObject (nativa, vtable +0x3BC:
//   0x008C7930, Delayed 0x00AB0F90): se m_vulnerableToDamageTypes (+0x378) non
//   e' vuota il tipo deve esserci (o una sua base), toglie la salute
//   (m_subObjectHealths +0x21C) e applica le modifiche con soglia >= la salute
//   nuova ("Cracked" a 30 e "Destroyed" a 0 sui vetri del pullman, salute
//   60); la Delayed le applica con un ritardo. Se HitComponent non e' un pezzo,
//   ucall TakeDamage solleva un'eccezione in ProcessEvent (26/10): qui il pezzo
//   si trova prima (componente colpito, o il piu' vicino entro 60 unita'),
//   altrimenti niente.
// - FracturedStaticMeshActor: TakeDamage (script) prende il frammento da
//   HitInfo.Item se HitInfo.HitComponent c'e' (come i proiettili), altrimenti lo
//   cerca da se' con TraceComponent lungo Momentum (sulle ciotole di Gorge01 non
//   ne trovava nessuno): qui HitComponent = la mesh fratturata e Item =
//   FCheckResult.Item della trace (PropHit.item se viene dalla trace a
//   estensione zero ed e' un frammento intero, altrimenti una trace a
//   estensione zero lungo il moto). Se il frammento e' visibile e distruttibile gli
//   toglie WorldInfo.FracturedMeshWeaponDamage x DamageType.FracturedMeshDamage
//   (1 x 1, non Damage: un colpo, un frammento, come un proiettile; ChunkHealth
//   +0x204, 1 sulle ciotole); a zero il frammento si stacca.
// - YSandBomb (StaticMeshActor; m_health +0x200, m_currentDamage +0x208,
//   m_enabled +0x20C bit 1): somma il danno e a m_health avvia
//   YSeqEvent_SandBombTriggered. Nessun livello la usa (sopra): resta per
//   completezza, mai provata.
// Dopo il colpo, solo in volo: se il bersaglio regge e il pezzo non ha corpo
// PhysX (i vetri del pullman: BodyInstance 0, l'oggetto ci passerebbe
// attraverso) l'oggetto rimbalza (normale della trace, 30%), anche quando il
// colpo e' rifiutato (danno spento dal livello, troppo debole); se si e' rotto
// e PhysX l'aveva fermato, riprende il 70% della velocita' del centro di prima
// (come Source: il vetro rotto non ferma l'oggetto), tranne sulle mesh
// fratturabili (un frammento per colpo: il resto e' ancora davanti).
constexpr uintptr_t kOffDoFlags = 0x1FC, kOffDoSubObjects = 0x204, kOffDoSubHealths = 0x21C, kOffDoVulnerable = 0x378;
constexpr uint32_t kDoDamagingEnabled = 0x400;
constexpr uintptr_t kDoSubStride = 0x7C, kOffDoSubName = 0x0, kOffDoSubMesh = 0x8, kOffDoSubMods = 0xC;
constexpr uintptr_t kDoModStride = 0x54, kOffDoModThreshold = 0x8, kOffDoModApplied = 0x50;  // m_applied bit 1
constexpr uintptr_t kOffSbHealth = 0x200, kOffSbCurrent = 0x208, kOffSbEnabled = 0x20C;
constexpr uintptr_t kOffFsmaComponent = 0x1FC, kOffFsmaChunkHealth = 0x204;
struct WorldDmgStats {
    uint32_t hits = 0, broken = 0, damaged = 0, soft = 0, refused = 0;
} g_worldDmgStats;

// Pezzo di un YDestructibleObject: indice in m_subObjects del componente colpito
// (o di quello col centro piu' vicino al contatto entro 60 unita'), -1 se nessuno.
int DestructibleSub(void* actor, void* comp, const float* at, uint32_t* subMesh) {
    const uint32_t data = U32(actor, kOffDoSubObjects), num = U32(actor, kOffDoSubObjects + 4);
    if (!data || num == 0 || num > 64) return -1;
    int best = -1;
    float bestD = 60.0f * 60.0f;
    for (uint32_t i = 0; i < num; ++i) {
        const uint32_t m = U32(reinterpret_cast<void*>(data + kDoSubStride * i), kOffDoSubMesh);
        if (!m) continue;
        if (comp && reinterpret_cast<void*>(m) == comp) {
            *subMesh = m;
            return static_cast<int>(i);
        }
        Prop b;
        if (!ReadBounds(reinterpret_cast<void*>(m), &b)) continue;
        float d2 = 0.0f;
        for (int k = 0; k < 3; ++k) {
            const float c = 0.5f * (b.boxMin[k] + b.boxMax[k]) - at[k];
            d2 += c * c;
        }
        if (d2 < bestD) {
            bestD = d2;
            best = static_cast<int>(i);
            *subMesh = m;
        }
    }
    return best;
}

float DestructibleHealth(void* actor, int sub) {
    const uint32_t data = U32(actor, kOffDoSubHealths), num = U32(actor, kOffDoSubHealths + 4);
    float h = 0.0f;
    if (sub < 0 || !data || static_cast<uint32_t>(sub) >= num ||
        !SafeCopy(&h, reinterpret_cast<void*>(data + 4 * static_cast<uint32_t>(sub)), 4) || !std::isfinite(h))
        return -1e30f;
    return h;
}

// Frammenti ancora interi di una mesh fratturabile (ChunkHealth > 0).
int FsmaChunksAlive(void* actor) {
    const uint32_t data = U32(actor, kOffFsmaChunkHealth), num = U32(actor, kOffFsmaChunkHealth + 4);
    if (!data || num > 4096) return -1;
    int alive = 0;
    for (uint32_t i = 0; i < num; ++i) alive += static_cast<int32_t>(U32(reinterpret_cast<void*>(data + 4 * i), 0)) > 0 ? 1 : 0;
    return alive;
}

void WorldDamage(const PropHit& e, double now) {
    if (!vr::control::g_propGlass.load() || e.targetKind < kTgtFractured || e.targetKind > kTgtDestructible) return;
    void* tgt = e.target;
    void* walker = g_tickPawn;
    if (!tgt || !walker || !ActorAlive(tgt) || (U32(tgt, kOffActorFlags9C) & kHidden)) return;
    const float s = vr::control::g_posScale;
    ++g_worldDmgStats.hits;
    // Velocita' del danno: il bersaglio e' fermo; per un lancio al piu' quella
    // del rilascio pulito (il filtro dei salti del tracciamento della voce 1).
    const float impact = e.speed / s;
    float v = impact;
    bool suspect = false;
    const ThrowRecord* rec = nullptr;
    if (e.source == HitSource::kThrown) {
        for (const ThrowRecord& r : g_throwRec)
            if (r.comp == e.comp && now - r.at < 6.0) rec = &r;
        const float g = -GravityZ(walker) / s;
        v = vr::pure::ThrowDamageSpeed(impact, rec ? rec->trusted / s : -1.0f, rec ? (rec->z - e.loc[2]) / s : 0.0f, g > 1.0f ? g : 15.0f, 1.0f,
                                       15.0f);
        suspect = rec && rec->suspect;
    }
    const float kg = e.mass > 0.0f ? e.mass * vr::control::g_massKgPerUnit.load() : 0.0f;
    const bool held = e.kind == HitKind::kHeld;
    const vr::pure::WorldHitOut w = vr::pure::WorldHitDamage(kg, v, held, suspect, vr::pure::WorldHitParams());
    // Stato prima del colpo, per tipo.
    static void* meleeClass = nullptr;
    void* dmgType = ClassByName("YDamageType_Melee", &meleeClass);
    void* hitComp = e.targetComp;
    const char* refuse = nullptr;
    std::string part;  // pezzo (distruttibile) per il log
    int sub = -1;
    uint32_t subMesh = 0;
    float before = 0.0f, limit = 0.0f;
    int32_t frag = -1;  // frammento della mesh fratturabile (FCheckResult.Item)
    uint32_t modsApplied = 0;  // modifiche del pezzo gia' applicate prima del colpo (m_applied, bit per indice)
    if (e.targetKind == kTgtDestructible) {
        sub = DestructibleSub(tgt, hitComp, e.loc, &subMesh);
        if (sub < 0) {
            refuse = "no sub-object at the hit";
        } else {
            hitComp = reinterpret_cast<void*>(subMesh);
            before = DestructibleHealth(tgt, sub);
            part = vr::script::NameFromIndex(static_cast<int32_t>(U32(reinterpret_cast<void*>(U32(tgt, kOffDoSubObjects) + kDoSubStride * sub), kOffDoSubName)));
            const uint32_t so = U32(tgt, kOffDoSubObjects) + kDoSubStride * sub;
            const uint32_t md = U32(reinterpret_cast<void*>(so), kOffDoSubMods), mn = U32(reinterpret_cast<void*>(so), kOffDoSubMods + 4);
            for (uint32_t k = 0; md && k < mn && k < 16; ++k)
                if (U32(reinterpret_cast<void*>(md + kDoModStride * k), kOffDoModApplied) & 1) modsApplied |= 1u << k;
            if (before <= 0.0f) refuse = "already destroyed";
            else if (!(U32(tgt, kOffDoFlags) & kDoDamagingEnabled)) refuse = "damage disabled by the level (m_damagingEnabled off)";
            const uint32_t vd = U32(tgt, kOffDoVulnerable), vn = U32(tgt, kOffDoVulnerable + 4);
            if (!refuse && vd && vn > 0 && vn < 64 && dmgType) {
                // Solo certi tipi (es. esplosivi): il melee o una sua base deve esserci.
                bool ok = false;
                for (uint32_t i = 0; i < vn && !ok; ++i) {
                    const uint32_t want = U32(reinterpret_cast<void*>(vd + 4 * i), 0);
                    for (uint32_t c = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(dmgType)), d = 0; c && d < 16 && !ok;
                         c = U32(reinterpret_cast<void*>(c), kOffSuper), ++d)
                        ok = c == want;
                }
                if (!ok) refuse = "not vulnerable to melee damage";
            }
        }
    } else if (e.targetKind == kTgtFractured) {
        before = static_cast<float>(FsmaChunksAlive(tgt));
        if (before <= 0.0f) refuse = "no intact fragment";
    } else {
        before = 0.0f;
        SafeCopy(&before, static_cast<char*>(tgt) + kOffSbCurrent, 4);
        SafeCopy(&limit, static_cast<char*>(tgt) + kOffSbHealth, 4);
        if (!(U32(tgt, kOffSbEnabled) & 1)) refuse = "sand bomb disabled (m_enabled off)";
    }
    static const char* const kTooSoft = "too soft";
    if (!refuse && !dmgType) refuse = "YDamageType_Melee not found";
    if (!refuse && w.damage <= 0) refuse = kTooSoft;
    bool called = false;
    const char* fragFrom = "";  // da dove viene il frammento (log)
    if (!refuse) {
        struct {
            int32_t damage;
            void* instigator;
            float loc[3];
            float momentum[3];
            void* damageType;
            void* hiMaterial;
            void* hiPhysMaterial;
            int32_t hiItem;
            int32_t hiLevelIndex;
            int32_t hiBoneName[2];
            void* hiComponent;
            void* causer;
            uint8_t spare[0x100];  // ParmsSize 0x44 (locali fino a +0xF0 in FracturedStaticMeshActor.TakeDamage): solo margine
        } td = {};
        td.damage = w.damage;
        td.instigator = reinterpret_cast<void*>(U32(walker, kOffPawnController));
        std::memcpy(td.loc, e.loc, sizeof(td.loc));
        // Momentum lungo il moto (FracturedStaticMeshActor ci cerca il frammento colpito).
        for (int i = 0; i < 3; ++i) td.momentum[i] = e.dir[i] * w.kg * v * s;
        td.damageType = dmgType;
        td.hiLevelIndex = -1;
        // FracturedStaticMeshActor: il frammento come lo danno i proiettili
        // (HitComponent = la mesh fratturata, Item = FCheckResult.Item della trace
        // della pipeline). Senza, HitComponent None e Item -1: lo script lo cerca
        // da se' con TraceComponent lungo Momentum, ma con le ciotole di Gorge01
        // non ne trovava nessuno (glasshit e ucall, 26/10: 8 frammenti -> 8).
        // L'Item vale come frammento solo se viene dalla trace a estensione
        // zero (poligoni) sulla mesh fratturata, e' un indice di ChunkHealth e
        // quel frammento e' ancora intero. Se il colpo viene dalla trace col
        // box (il suo Item non e' un frammento sicuro: la trace complessa su una
        // mesh normale da' 0) o l'Item non vale, una trace a estensione zero
        // lungo il moto attraverso il contatto, come quella della pipeline e di
        // glasshit (revisione della voce 2, 26/10).
        void* fsmComp = e.targetKind == kTgtFractured ? reinterpret_cast<void*>(U32(tgt, kOffFsmaComponent)) : nullptr;
        const uint32_t chunkData = fsmComp ? U32(tgt, kOffFsmaChunkHealth) : 0, chunkNum = fsmComp ? U32(tgt, kOffFsmaChunkHealth + 4) : 0;
        auto fragAlive = [&](int32_t it) {
            return chunkData && chunkNum <= 4096 && it >= 0 && static_cast<uint32_t>(it) < chunkNum &&
                   static_cast<int32_t>(U32(reinterpret_cast<void*>(chunkData + 4 * static_cast<uint32_t>(it)), 0)) > 0;
        };
        frag = fsmComp && e.targetComp == fsmComp && !e.boxTrace && fragAlive(e.item) ? e.item : -1;
        if (frag >= 0) fragFrom = "from the hit";
        if (fsmComp && frag < 0) {
            float a[3], b[3];
            for (int i = 0; i < 3; ++i) {
                a[i] = e.loc[i] - e.dir[i] * 30.0f;
                b[i] = e.loc[i] + e.dir[i] * 60.0f;
            }
            TraceResult fr;
            ++g_hitStats.traces;
            if (EngineTrace(ActorAlive(e.prop) ? e.prop : walker, a, b, 0.0f, &fr, kHitTraceFlags | kTraceComplex) && fr.actor == tgt &&
                fr.comp == fsmComp && fragAlive(fr.item)) {
                frag = fr.item;
                fragFrom = "from the fragment trace";
            } else {
                fragFrom = "none found";
            }
        }
        td.hiItem = frag >= 0 ? frag : -1;
        td.hiComponent = e.targetKind == kTgtDestructible ? hitComp : (frag >= 0 ? fsmComp : nullptr);
        td.causer = ActorAlive(e.prop) ? e.prop : nullptr;
        called = vr::script::CallFunction(tgt, "TakeDamage", &td);
        if (!called) refuse = "TakeDamage call failed";
    }
    // Stato dopo.
    float after = before;
    bool broke = false;
    std::string result;
    if (called) {
        char r[200];
        if (e.targetKind == kTgtDestructible) {
            after = DestructibleHealth(tgt, sub);
            broke = after <= 0.0f;
            // Modifiche che il colpo applica ("Cracked", "Destroyed"): soglia raggiunta
            // dalla salute nuova e non ancora applicate prima (una soglia sopra la
            // salute iniziale, come "Destroyed" a 40 sul vetro m01a_166_166 da 25,
            // scatta al primo danno). La Delayed le applica poco dopo.
            std::string mods;
            const uint32_t so = U32(tgt, kOffDoSubObjects) + kDoSubStride * sub;
            const uint32_t md = U32(reinterpret_cast<void*>(so), kOffDoSubMods), mn = U32(reinterpret_cast<void*>(so), kOffDoSubMods + 4);
            for (uint32_t k = 0; md && k < mn && k < 16; ++k) {
                float th = 0.0f;
                if (!SafeCopy(&th, reinterpret_cast<void*>(md + kDoModStride * k + kOffDoModThreshold), 4)) continue;
                if (th >= after && !(modsApplied & (1u << k)))
                    mods += (mods.empty() ? "" : ", ") + vr::script::NameFromIndex(static_cast<int32_t>(U32(reinterpret_cast<void*>(md + kDoModStride * k), 0)));
            }
            std::snprintf(r, sizeof(r), "health %.0f -> %.0f%s%s%s", before, after, mods.empty() ? "" : " (", mods.c_str(), mods.empty() ? "" : ")");
        } else if (e.targetKind == kTgtFractured) {
            after = static_cast<float>(FsmaChunksAlive(tgt));
            broke = after < before;
            std::snprintf(r, sizeof(r), "%sintact fragments %.0f -> %.0f (fragment %d, %s)", broke ? "" : "no fragment broken, ", before, after, frag,
                          fragFrom);
        } else {
            SafeCopy(&after, static_cast<char*>(tgt) + kOffSbCurrent, 4);
            broke = (limit > 0.0f && after >= limit) || (U32(tgt, kOffActorFlags9C) & kHidden);
            std::snprintf(r, sizeof(r), "damage %.0f -> %.0f of %.0f%s", before, after, limit, broke ? " (triggered)" : "");
        }
        result = r;
    }
    // Esito (log e conteggi): una mesh fratturabile chiamata senza frammento
    // rotto e' "nessun effetto", non "danneggiata"; "troppo debole" solo per il
    // rifiuto dell'energia, non per gli altri con danno 0 (revisione della voce 2).
    const bool noFragment = called && e.targetKind == kTgtFractured && !broke;
    if (called && !noFragment) ++(broke ? g_worldDmgStats.broken : g_worldDmgStats.damaged);
    else ++(refuse == kTooSoft ? g_worldDmgStats.soft : g_worldDmgStats.refused);
    // Il moto dell'oggetto dopo il colpo (solo in volo).
    // - Il pezzo regge e non ha corpo PhysX (i vetri del pullman: l'oggetto ci
    //   passerebbe attraverso): rimbalza, anche se il colpo e' stato rifiutato
    //   (danno spento dal livello, troppo debole, tipo di danno non ammesso).
    //   Regge: un distruttibile col pezzo trovato e salute > 0 dopo il colpo; una
    //   mesh fratturabile con frammenti interi; una finestra di sabbia non
    //   scattata. Un pezzo gia' distrutto (collisione spenta) lo lascia passare.
    // - Rotto, con corpo PhysX che l'aveva gia' fermato: prosegue al 70% della
    //   velocita' del centro di prima (non quella del capo, con la rotazione).
    //   Non sulle mesh fratturabili: un colpo stacca un frammento solo, come
    //   un proiettile, e il resto e' ancora li' davanti.
    const char* motion = "";
    if (e.kind == HitKind::kFlying && ActorUsable(e.prop)) {
        bool holds = !broke;
        if (e.targetKind == kTgtDestructible) holds = sub >= 0 && (called ? after : before) > 0.0f;
        else if (e.targetKind == kTgtFractured) holds = !broke && (called ? after : before) > 0.0f;
        else holds = !broke && !(U32(tgt, kOffActorFlags9C) & kHidden);
        void* nx = CompNx(e.comp);
        float lin[3], ang[3];
        const bool bodyless = !hitComp || !U32(hitComp, kOffCompBodyInstance);
        if (nx && NxVelocities(nx, lin, ang) && std::isfinite(lin[0] + lin[1] + lin[2])) {
            float cur[3] = {lin[0] * 50.0f, lin[1] * 50.0f, lin[2] * 50.0f};  // PhysX m/s -> unita'/s
            // Normale verso il lato da cui arriva l'oggetto (la trace poligono per
            // poligono da' quella del triangolo, che puo' guardare dall'altra parte).
            float n[3] = {e.normal[0], e.normal[1], e.normal[2]};
            if (Dot3(e.dir, n) > 0.0f)
                for (float& c : n) c = -c;
            const float vn = Dot3(cur, n);
            const float cs = std::sqrt(Dot3(e.cvel, e.cvel));
            if (holds && bodyless && vn < 0.0f) {
                // Il vetro regge ma non ha corpo: l'oggetto rimbalza (30% lungo la normale, 80% di lato).
                float nv[3];
                for (int i = 0; i < 3; ++i) nv[i] = (cur[i] - vn * n[i]) * 0.8f - 0.3f * vn * n[i];
                SetLinVel(e.comp, nv);
                Wake(e.comp);
                motion = ", bounced off";
            } else if (called && broke && !bodyless && e.targetKind != kTgtFractured && cs > 1.0f && Dot3(cur, e.cvel) < 0.5f * cs * cs) {
                // Rotto, ma PhysX l'aveva gia' fermato o fatto rimbalzare: prosegue.
                float nv[3];
                for (int i = 0; i < 3; ++i) nv[i] = e.cvel[i] * 0.7f;
                SetLinVel(e.comp, nv);
                Wake(e.comp);
                motion = ", keeps flying";
            }
        }
    }
    static const char* const kTgt[] = {"", "", "fractured mesh", "sand window", "destructible"};
    char b[640];
    std::snprintf(b, sizeof(b), "PROPS: glass hit %s (%s%s%s%s): %.1f kg%s at %.1f m/s (impact %.1f%s) = %.0f J -> %d damage (%s%s), %s%s%s",
                  vr::script::ClassName(tgt).c_str(), kTgt[e.targetKind], e.glass ? ", glass" : "", part.empty() ? "" : ", piece ",
                  part.c_str(), w.kg, held ? " with the arm" : "", v, impact, suspect ? ", tracking jump at release" : "", w.joules,
                  refuse ? 0 : w.damage, w.heavy ? "heavy" : "light", w.capped ? ", capped" : "",
                  refuse || noFragment ? "no effect: " : (broke ? "broken, " : "damaged, "), refuse ? refuse : result.c_str(), motion);
    vr::log::Line(b);
}

// Qui le voci successive del piano attaccano gli effetti (danno, stordimento,
// vetri, finestre di sabbia, bottiglia rotta in testa). Voce 1: ThrowDamage;
// voce 2: WorldDamage; voce 4: HeldMelee.
// Finta granata (voce 7 del piano, 27/10). Un oggetto piccolo lanciato (o
// lasciato dal guanto) che atterra entro 3 m da un nemico: l'IA lo prende per
// una granata. Si rifa' quello che fa la granata vera a ogni impatto previsto
// (nativa di YProj_Physical, 0x00BA2EB5-0x00BA2FAE, letta il 27/10):
// 1. YAIEventPropagator.SendEvent_GrenadeImpact(InstigatorController, punto,
//    tempo all'impatto, m_aiImpactType, -1, false). Parametri dalla
//    riflessione: Instigator (Controller, non Pawn) @0, pos @4,
//    estimatedTimeToImpact @0x10, grenadeType (YE_GrenadeType, Frag 0) @0x14,
//    MaxRange @0x18, explosion (bool) @0x1C; 0x20 byte, iNative 0. Get()
//    restituisce l'oggetto di default: il singleton e'
//    Default__YAIEventPropagator. La nativa (0x00C22AA0 -> FUN_0099A570) lo
//    mette in coda; il gestore (FUN_00961D10) registra un pericolo (vita:
//    tempo + 5,2 s; scartato se ce n'e' gia' uno entro 75 unita' da meno di
//    3 s) e lo scrive in ogni bot Kynapse entro MaxRange (-1 = 325 unita') a
//    bot+0x164C, con l'evento 0x1B. Misurato in cpTutorialFight: i due
//    marauder ricevono il pericolo (posizione, vita 7,2 s con tempo 2).
// 2. Walker.GrenadeImpactImminent(punto, 300, tipo) (evento di YGamePawn,
//    iNative 0; la granata vera lo chiama su m_causerPawn):
//    PlaySpecialActionOnNearbyPawn(YSAST_IncomingGrenade 114, 300, ...) fa
//    gridare "granata!" al pawn vivo piu' vicino al punto, escluso Walker.
// Il primo urto di un lancio con un nemico entro 3 m decide (avviso, troppo
// grande, gruppo gia' avvisato, compagno vicino, tocco): poi niente piu'
// avvisi per quel lancio. Un oggetto che colpisce un nemico in pieno non e'
// una finta granata.
// Revisione (27/10): il gestore dell'evento non guarda la squadra (scrive il
// pericolo in ogni bot entro MaxRange, Adams e Lugo compresi) e il grido va
// al pawn vivo piu' vicino al punto, di qualunque parte. La granata vera fa
// lo stesso, ma le bottiglie non finiscono: con un compagno (Adams, Lugo, un
// civile) entro la portata, MaxRange scende a (sua distanza - 0,5 m), e se
// cosi' non arriva al nemico piu' vicino (con 0,5 m di margine) niente
// avviso. Allora il pawn piu' vicino al punto e' sempre il nemico: anche il
// grido resta suo.
constexpr float kFakeMaxRadius = 15.0f;     // raggio della sfera dei limiti (unita'): lattine, bottiglie, sassi
constexpr float kFakeNear = 150.0f;         // 3 m: nemico vicino al punto d'atterraggio (dal cilindro)
constexpr float kFakeGroup = 325.0f;        // gruppo avvisato = portata dell'evento (MaxRange -1 = 325 unita')
constexpr float kFakeFriendMargin = 25.0f;  // 0,5 m: il bot Kynapse sta fino a ~20 unita' dalla Location del pawn
constexpr double kFakeCooldown = 10.0;      // s per gruppo
constexpr float kFakeTime = 2.0f;           // tempo all'esplosione dichiarato all'IA (s)
constexpr float kFakeBarkRadius = 300.0f;   // come la granata vera (costante 0x0112CD40)
constexpr float kFakeLandBefore = 2.0f, kFakeLandDv = 2.5f;  // atterraggio: da oltre 2 m/s, cambio oltre 2,5 m/s in 50 ms
struct FakeStats {
    uint32_t sent = 0, cooldown = 0, tooBig = 0, enemyHit = 0, friendNear = 0, failed = 0;
} g_fakeStats;
std::vector<std::pair<void*, double>> g_fakeCooldown;  // nemico -> fino a quando niente finta granata vicino a lui

void* AiPropagator() {
    if (void* pf = Prefetched(kPfAiPropagator)) return pf;
    // Non ancora visto dalla scansione a pezzi: un giro intero, al piu' uno
    // ogni 30 s. Lo chiama solo la finta granata, con un nemico entro 3 m dal
    // punto; il rumore (voce 9) usa solo Prefetched (revisione: un urto
    // qualsiasi non deve costare un giro di GObjects sul thread del gioco).
    static void* cached = nullptr;
    static double nextTry = 0.0;
    static int32_t want = -1;
    if (want < 0) want = vr::script::NameIndex("Default__YAIEventPropagator");
    if (want < 0) return nullptr;
    if (cached && static_cast<int32_t>(U32(cached, kOffName)) == want) return cached;
    cached = nullptr;
    const double now = ProfMs() * 0.001;
    if (now < nextTry) return nullptr;
    nextTry = now + 30.0;
    uint32_t data = U32(reinterpret_cast<void*>(kGObjects), 0), num = U32(reinterpret_cast<void*>(kGObjects), 4);
    for (uint32_t i = 0; data && i < num && i < 2000000; ++i) {
        uint32_t o = 0;
        if (!TrySafeReadUint32(reinterpret_cast<void*>(data + 4 * i), &o) || !o) continue;
        if (static_cast<int32_t>(U32(reinterpret_cast<void*>(o), kOffName)) == want &&
            vr::script::ClassName(reinterpret_cast<void*>(o)) == "YAIEventPropagator") {
            cached = reinterpret_cast<void*>(o);
            break;
        }
    }
    return cached;
}

// Nemici vivi (PawnHostile) con il cilindro entro range dal punto (al piu' maxN, il piu' vicino primo).
int FakeEnemiesNear(void* walker, const float* at, float range, void** out, float* dist, int maxN, double now) {
    uint32_t wi = U32(walker, kOffActorWorldInfo);
    uint32_t q = wi ? U32(reinterpret_cast<void*>(wi), kOffWorldPawnList) : 0;
    int n = 0;
    for (int guard = 0; q && guard < 512 && n < maxN; ++guard) {
        void* pw = reinterpret_cast<void*>(q);
        q = U32(pw, kOffPawnNextPawn);
        if (pw == walker || static_cast<int32_t>(U32(pw, kOffPawnHealth)) <= 0 || (U32(pw, kOffActorFlags9C) & (kHidden | kDeleteMe)))
            continue;
        float loc[3], h = 0.0f, r = 0.0f;
        uint32_t cls = U32(pw, kOffClass), cyl = U32(pw, kOffPawnCylinder);
        if (!cls || !cyl || !SafeCopy(loc, static_cast<char*>(pw) + kOffActorLocation, 12) ||
            !SafeCopy(&h, reinterpret_cast<char*>(cyl) + kOffCylHeight, 4) || !SafeCopy(&r, reinterpret_cast<char*>(cyl) + kOffCylRadius, 4) ||
            !(h > 1.0f && h < 300.0f) || !(r > 1.0f && r < 200.0f))
            continue;
        // Distanza dal cilindro: in pianta dal bordo, in alto oltre la mezza altezza.
        const float dx = at[0] - loc[0], dy = at[1] - loc[1];
        const float dxy = (std::max)(0.0f, std::sqrt(dx * dx + dy * dy) - r);
        const float dz = (std::max)(0.0f, std::fabs(at[2] - loc[2]) - h);
        const float d = std::sqrt(dxy * dxy + dz * dz);
        if (d > range || !PawnHostile(walker, pw, cls, now)) continue;
        int k = n++;
        for (; k > 0 && dist[k - 1] > d; --k) {
            out[k] = out[k - 1];
            dist[k] = dist[k - 1];
        }
        out[k] = pw;
        dist[k] = d;
    }
    return n;
}

// Il compagno (YGamePawn vivo non nemico: Adams, Lugo, un civile; non Walker)
// piu' vicino al punto entro range, con la distanza dal centro in 3D, come la
// misura il gestore dell'evento (FUN_00961D10: bot entro MaxRange). Nessuno:
// *who nullo e ritorna range.
float FakeFriendNearest(void* walker, const float* at, float range, void** who, double now) {
    uint32_t wi = U32(walker, kOffActorWorldInfo);
    uint32_t q = wi ? U32(reinterpret_cast<void*>(wi), kOffWorldPawnList) : 0;
    float best = range;
    *who = nullptr;
    for (int guard = 0; q && guard < 512; ++guard) {
        void* pw = reinterpret_cast<void*>(q);
        q = U32(pw, kOffPawnNextPawn);
        if (pw == walker || static_cast<int32_t>(U32(pw, kOffPawnHealth)) <= 0 || (U32(pw, kOffActorFlags9C) & (kHidden | kDeleteMe)))
            continue;
        float loc[3];
        const uint32_t cls = U32(pw, kOffClass);
        if (!cls || !SafeCopy(loc, static_cast<char*>(pw) + kOffActorLocation, 12)) continue;
        const float dx = at[0] - loc[0], dy = at[1] - loc[1], dz = at[2] - loc[2];
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (!(d < best) || !PawnClass(cls).yGame || PawnHostile(walker, pw, cls, now)) continue;
        best = d;
        *who = pw;
    }
    return best;
}

const char* FakeSourceName(HitSource s) { return s == HitSource::kGlove ? "gravity glove" : "thrown"; }

// Un oggetto lanciato ha colpito un pawn in pieno (pipeline dei colpi): per
// questo lancio niente finta granata (lo ha visto arrivare addosso). Nel
// contatore dei 30 s solo i colpi sui nemici (revisione: kTgtPawn copre anche
// Adams, Lugo e i civili).
void FakeGrenadeOnPawnHit(const PropHit& e) {
    if (e.targetKind != kTgtPawn || e.kind != HitKind::kFlying) return;
    bool marked = false;
    for (Falling& f : g_falling)
        if (f.comp == e.comp && !f.fakeDone) {
            f.fakeDone = true;
            marked = true;
        }
    if (!marked || !vr::control::g_fakeGrenade.load() || !(e.source == HitSource::kThrown || e.source == HitSource::kGlove)) return;
    const uint32_t cls = e.target ? U32(e.target, kOffClass) : 0;
    if (g_tickPawn && cls && e.target != g_tickPawn && PawnHostile(g_tickPawn, e.target, cls, ProfMs() * 0.001)) ++g_fakeStats.enemyHit;
}

// Urto di un oggetto seguito in g_falling (lanciato o lasciato dal guanto):
// se un nemico e' entro 3 m, avviso all'IA come per una granata vera. dv e'
// il cambio di velocita' dell'urto (unita'/s, per il log). comp e source per
// valore (revisione): le chiamate al gioco qui sotto potrebbero in teoria
// aggiungere voci a g_falling, e un riferimento a una voce non varrebbe piu'.
void FakeGrenadeLanded(void* walker, void* comp, HitSource source, float dv, double now) {
    if (!vr::control::g_fakeGrenade.load() || vr::control::g_cinemaActive || vr::control::g_gameLocked) return;
    float b[7];
    if (!SafeCopy(b, static_cast<char*>(comp) + kOffCompBounds, sizeof(b)) || !std::isfinite(b[0] + b[1] + b[2] + b[6])) return;
    void* nearE[16];
    float nearD[16];
    const int n = FakeEnemiesNear(walker, b, kFakeNear, nearE, nearD, 16, now);
    if (n == 0) return;  // nessun nemico vicino: il prossimo rimbalzo puo' ancora contare
    for (Falling& o : g_falling)
        if (o.comp == comp) o.fakeDone = true;
    const float s = vr::control::g_posScale;
    const std::string mesh = vr::script::ObjectName(reinterpret_cast<void*>(U32(comp, kOffSmcStaticMesh)));
    const std::string who = vr::script::ClassName(nearE[0]);
    char b2[560];
    // Revisione (27/10): la sfera dei limiti che arriva al cilindro del nemico
    // all'urto vuol dire che l'oggetto ha urtato lui, non il terreno: e' un
    // colpo, non un atterraggio. Vale anche con propcombat spento (la pipeline
    // non gira) e per il tocco leggero sotto la sua soglia (manichino: 2,7 m/s
    // a 0,1 m dal cilindro contava come atterraggio).
    if (nearD[0] < b[6]) {
        ++g_fakeStats.enemyHit;
        std::snprintf(b2, sizeof(b2),
                      "PROPS: fake grenade: %s (r %.1f units, %s) touched %s (%.1f m from its cylinder, %.1f m/s change): a hit on him, "
                      "not a landing, no warning",
                      mesh.c_str(), b[6], FakeSourceName(source), who.c_str(), nearD[0] / s, dv / s);
        vr::log::Line(b2);
        return;
    }
    if (b[6] >= kFakeMaxRadius) {
        ++g_fakeStats.tooBig;
        std::snprintf(b2, sizeof(b2), "PROPS: fake grenade: %s (r %.1f units, over %.0f) landed %.1f m from %s: too big for a grenade, no warning",
                      mesh.c_str(), b[6], kFakeMaxRadius, nearD[0] / s, who.c_str());
        vr::log::Line(b2);
        return;
    }
    double left = 0.0;
    for (int i = 0; i < n; ++i)
        for (const auto& c : g_fakeCooldown)
            if (c.first == nearE[i] && c.second > now) left = (std::max)(left, c.second - now);
    if (left > 0.0) {
        ++g_fakeStats.cooldown;
        std::snprintf(b2, sizeof(b2), "PROPS: fake grenade: %s landed %.1f m from %s: that group was warned %.1f s ago (cooldown %.0f s), no warning",
                      mesh.c_str(), nearD[0] / s, who.c_str(), kFakeCooldown - left, kFakeCooldown);
        vr::log::Line(b2);
        return;
    }
    // Compagni: la portata dell'evento si ferma prima del piu' vicino (vedi
    // sopra). Il nemico piu' vicino dal centro, come lo misura il gestore.
    float enemyC = 1.0e9f;
    for (int i = 0; i < n; ++i) {
        float el[3];
        if (!SafeCopy(el, static_cast<char*>(nearE[i]) + kOffActorLocation, 12)) continue;
        const float dx = b[0] - el[0], dy = b[1] - el[1], dz = b[2] - el[2];
        enemyC = (std::min)(enemyC, std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    void* mate = nullptr;
    const float mateD = FakeFriendNearest(walker, b, kFakeGroup + kFakeFriendMargin, &mate, now);
    const float reach = mate ? mateD - kFakeFriendMargin : kFakeGroup;
    if (mate && enemyC + kFakeFriendMargin > reach) {
        ++g_fakeStats.friendNear;
        std::snprintf(b2, sizeof(b2),
                      "PROPS: fake grenade: %s landed %.1f m from %s (%.1f m from his centre), but %s is %.1f m from it: the warning "
                      "would reach him too, no warning",
                      mesh.c_str(), nearD[0] / s, who.c_str(), enemyC / s, vr::script::ClassName(mate).c_str(), mateD / s);
        vr::log::Line(b2);
        return;
    }
    // 1. Il pericolo per l'IA (come la granata vera: istigatore il Controller di chi lancia).
    void* prop = AiPropagator();
    void* pc = reinterpret_cast<void*>(static_cast<uintptr_t>(U32(walker, 0x214)));
    alignas(4) uint8_t ev[0x20] = {};
    const float t = kFakeTime, maxRange = mate ? reach : -1.0f;  // -1 = 325 unita', come la granata vera
    std::memcpy(ev + 0x00, &pc, 4);
    std::memcpy(ev + 0x04, b, 12);
    std::memcpy(ev + 0x10, &t, 4);
    ev[0x14] = 0;  // YGRENADE_Frag
    std::memcpy(ev + 0x18, &maxRange, 4);
    // explosion @0x1C: false (avviso, non scoppio)
    const bool sent = prop && vr::script::CallFunction(prop, "SendEvent_GrenadeImpact", ev);
    // 2. Il grido "granata!" del pawn piu' vicino al punto (YGamePawn.GrenadeImpactImminent su Walker):
    //    ogni compagno e' almeno 1 m piu' lontano del nemico (prova qui sopra), quindi grida un nemico.
    alignas(4) uint8_t bark[0x20] = {};
    const float rad = kFakeBarkRadius;
    std::memcpy(bark + 0x00, b, 12);
    std::memcpy(bark + 0x0C, &rad, 4);
    bark[0x10] = 0;  // YGRENADE_Frag
    const bool barked = vr::script::CallFunction(walker, "GrenadeImpactImminent", bark);
    // Il gruppo avvisato (i nemici entro la portata dell'evento) non ne riceve un altro per 10 s.
    void* grp[16];
    float grpD[16];
    const int ng = FakeEnemiesNear(walker, b, reach, grp, grpD, 16, now);
    g_fakeCooldown.erase(std::remove_if(g_fakeCooldown.begin(), g_fakeCooldown.end(),
                                        [&](const std::pair<void*, double>& c) { return c.second <= now; }),
                         g_fakeCooldown.end());
    if (sent || barked)
        for (int i = 0; i < ng; ++i) {
            bool found = false;
            for (auto& c : g_fakeCooldown)
                if (c.first == grp[i]) {
                    c.second = now + kFakeCooldown;
                    found = true;
                }
            if (!found && g_fakeCooldown.size() < 64) g_fakeCooldown.push_back({grp[i], now + kFakeCooldown});
        }
    ++(sent ? g_fakeStats.sent : g_fakeStats.failed);
    char reachTxt[120];
    if (mate)
        std::snprintf(reachTxt, sizeof(reachTxt), "reach %.1f m, short of %s %.1f m away", reach / s, vr::script::ClassName(mate).c_str(),
                      mateD / s);
    else
        std::snprintf(reachTxt, sizeof(reachTxt), "reach %.1f m, no squadmate in it", kFakeGroup / s);
    std::snprintf(b2, sizeof(b2),
                  "PROPS: fake grenade: %s (r %.1f units, %s) landed at (%.0f, %.0f, %.0f) (%.1f m/s change), %.1f m from %s (%d %s within "
                  "3 m): AI warned as by a frag grenade (%s, %s), IncomingGrenade bark (%s); %d %s on a %.0f s cooldown",
                  mesh.c_str(), b[6], FakeSourceName(source), b[0], b[1], b[2], dv / s, nearD[0] / s, who.c_str(), n,
                  n == 1 ? "enemy" : "enemies", sent ? "SendEvent_GrenadeImpact" : (prop ? "call FAILED" : "no YAIEventPropagator"), reachTxt,
                  barked ? "asked" : "call FAILED", ng, ng == 1 ? "enemy" : "enemies", kFakeCooldown);
    vr::log::Line(b2);
}

// Rumore per distrarre (voce 9 del piano, 27/10). Un oggetto lanciato,
// lasciato, urtato dalla mano o lasciato dal guanto che sbatte, o una
// bottiglia rotta in mano, si sente: YAIEventPropagator.SendHearEvent_Melee(
// Controller di Walker, punto dell'urto, 1), lo stesso evento che il gioco
// manda per il corpo a corpo di Walker (YGamePawn.PerformMeleeAttack:
// (self.Controller, self.Location, volume predefinito 1,0 a 0x011375D4)).
// Actor.MakeNoise non serve: e' una nativa a opcode (ProcessEvent la ignora)
// e va al vecchio HearNoise di UE3, che l'IA di Kynapse non usa.
// - Riflessione (27/10): final native, iNative 0, ctrl (Controller) @0, pos
//   @4, Volume (opzionale) @0x10; 0x14 byte.
// - La nativa (0x00475790 -> FUN_00439150) legge subito il raggio dal
//   singleton (m_hearEventMeleeAttack +0x58: 300 unita' = 6 m; gli spari e le
//   esplosioni hanno 0 = senza limite) e mette in coda l'evento
//   (FUN_0081CB10: tipo Hear, sottotipo 2, l'handle Kynapse del controller,
//   scartato se nullo; posizione, raggio, volume).
// - Il gestore (FUN_00FD23E0 -> FUN_00A36440, per ogni bot) lo accetta entro
//   il raggio x il fattore d'udito del bot (entita' +0x130 in pianta, +0x134
//   in altezza; FUN_00DCDB20) e registra uno stimolo d'udito con la posizione
//   passata (non quella di Walker), l'istigatore e il volume (FUN_00E428A0).
// Il raggio va col rumore (PropNoiseLoudness: 0,5-2,5 x 300 dall'energia
// dell'urto, il vetro che si rompe x1,5): per la sola chiamata +0x58 vale il
// raggio voluto, poi torna com'era. Tutto sul thread del gioco, e l'evento in
// coda ha la sua copia del raggio. Il volume resta 1 come nel gioco (non si e'
// trovato dove l'IA lo usa). Al piu' un evento al secondo.
// Misurato sull'IA vera (cpTutorialFight, 27/10): i marauder hanno la
// posizione vera di Walker come bersaglio (YAICtrl_Humanoid
// m_attackTargetPosition +0x504) 0,2-13 s (quasi sempre entro 5) dopo la
// comparsa, anche a 48 m e fuori dalla lotta, anche senza nessun rumore. In
// una lotta gia' iniziata non si e' vista nessuna differenza misurabile col
// rumore (ne' verso il punto, ne' verso Walker), ma non e' una prova che non
// lo tradisca: l'istigatore dello stimolo e' Walker (FUN_00E428A0 lo salva,
// FUN_007DA400 accetta l'evento solo per i bot ostili a lui), e nemici non
// ancora in combattimento non sono stati provati. Per questo l'opzione e'
// sperimentale e spenta. La sonda "noiseprobe" (qui sotto) conta i bot che
// accettano davvero l'evento.
constexpr float kNoiseBefore = 3.0f, kNoiseDv = 3.0f;  // urto: da oltre 3 m/s, cambio oltre 3 m/s in 50 ms
constexpr double kNoiseEvery = 1.0;                     // s tra due eventi
constexpr double kNoiseHold = 0.1;                      // s al piu' di attesa del cambio piu' grande di un urto
constexpr uintptr_t kOffPropagatorMeleeRange = 0x58;    // YAIEventPropagator.m_hearEventMeleeAttack
struct NoiseStats {
    uint32_t sent = 0, glass = 0, limited = 0, failed = 0;
} g_noiseStats;
double g_noiseNext = 0.0;

// Sonda di prova (revisione della voce 9, solo per i test: "noiseprobe
// on|off", niente menu). Conta gli stimoli d'udito che i bot registrano
// davvero per il nostro rumore: un hook (MinHook, messo alla prima
// accensione) su FUN_00E428A0, che riempie lo stimolo d'udito. La chiama
// FUN_00A36440 (il gestore dell'evento, per ogni bot) solo quando il bot
// accetta l'evento (FUN_007DA400: ostile all'istigatore; FUN_00DCDB20: entro
// il raggio x il suo fattore d'udito). Disassemblato (27/10):
// - __thiscall(stimolo, subtype, handle dell'istigatore, const float* pos,
//   float volume), ret 0x10; il prologo e' 56 8B F1 E8 (push esi; mov esi,
//   ecx; call), controllato prima di metterci l'hook.
// - pos e' quella dell'evento passata da FUN_004B61B0: (y, z, -x) delle
//   coordinate del gioco, senza scala (l'evento ha la posizione in unita').
// - Dal chiamante 0x00A367A3 (ritorno) ebp e' il gestore del bot (FUN_00A36440
//   lo usa come this) e [ebp+4] l'entita' Kynapse: posizione +0x34..+0x3C
//   nelle stesse coordinate (FUN_00DCDB20 la confronta cosi'), per sapere
//   QUALE bot ha sentito.
// Il gestore gira al giro dell'IA, dopo la chiamata: PropNoise arma la sonda
// col punto dell'urto, il Tick scrive il conteggio 1,5 s dopo.
namespace noiseprobe {
const uintptr_t& kHearStimFill = vr::addr::g[vr::addr::HearStimFill];
const uintptr_t& kRetFromBotHandler = vr::addr::g[vr::addr::RetFromBotHandler];
constexpr int kMaxBots = 16;
typedef void(__thiscall* HearStimFill_t)(void* stim, uint32_t subtype, uint32_t handle, const float* pos, float volume);
HearStimFill_t g_orig = nullptr;
bool g_installed = false, g_failed = false;
std::atomic<bool> g_armed{false};
float g_at[3] = {0, 0, 0};        // punto del nostro rumore (unita' del gioco)
double g_until = 0.0;             // quando scrivere il conteggio
std::atomic<int> g_match{0};      // stimoli col nostro punto (subtype 2)
std::atomic<int> g_other{0};      // altri stimoli d'udito nella finestra (spari, colpi, mischia altrui)
float g_botAt[kMaxBots][3] = {};  // posizione (unita' del gioco) dei bot che hanno sentito il nostro rumore
uint32_t g_handle = 0;            // handle dell'istigatore del primo stimolo nostro

// MSVC non ammette __thiscall su una funzione libera: __fastcall con edx
// finto ha lo stesso ingresso (this in ecx, argomenti sullo stack, li toglie
// la funzione).
void __fastcall Hooked(void* stim, void* /*edx*/, uint32_t subtype, uint32_t handle, const float* pos, float volume) {
    uint32_t callerEbp = 0;
    __asm {
        mov eax, [ebp]  // ebp del chiamante (salvato dal prologo: l'__asm obbliga il frame con ebp)
        mov callerEbp, eax
    }
    void* ret = _ReturnAddress();
    g_orig(stim, subtype, handle, pos, volume);
    if (!g_armed.load()) return;
    float p[3];
    if (!pos || !SafeCopy(p, pos, 12)) return;
    const float ue[3] = {-p[2], p[0], p[1]};
    const float dx = ue[0] - g_at[0], dy = ue[1] - g_at[1], dz = ue[2] - g_at[2];
    if ((subtype & 0xFF) != 2 || dx * dx + dy * dy + dz * dz > 4.0f) {
        g_other.fetch_add(1);
        return;
    }
    const int k = g_match.fetch_add(1);
    if (k == 0) g_handle = handle;
    if (k >= kMaxBots) return;
    float e[3] = {0, 0, 0};
    uint32_t ent = 0;
    if (reinterpret_cast<uintptr_t>(ret) == kRetFromBotHandler && callerEbp && TrySafeReadUint32(reinterpret_cast<char*>(callerEbp) + 4, &ent) &&
        ent && SafeCopy(e, reinterpret_cast<char*>(ent) + 0x34, 12)) {
        g_botAt[k][0] = -e[2];
        g_botAt[k][1] = e[0];
        g_botAt[k][2] = e[1];
    } else {
        g_botAt[k][0] = g_botAt[k][1] = g_botAt[k][2] = 0.0f;
    }
}

// Sul thread del gioco, dal Tick: mette l'hook alla prima accensione.
void Ensure() {
    if (g_installed || g_failed) return;
    void* t = reinterpret_cast<void*>(kHearStimFill);
    uint8_t code[4] = {};
    const uint8_t want[4] = {0x56, 0x8B, 0xF1, 0xE8};
    if (!SafeCopy(code, t, 4) || std::memcmp(code, want, 4) != 0) {
        g_failed = true;
        vr::log::Line("PROPS: noise probe: FUN_00E428A0 does not start as expected (56 8B F1 E8), no hook");
        return;
    }
    if (MH_CreateHook(t, reinterpret_cast<void*>(&Hooked), reinterpret_cast<void**>(&g_orig)) != MH_OK || MH_EnableHook(t) != MH_OK) {
        g_failed = true;
        vr::log::Line("PROPS: noise probe: MinHook failed on FUN_00E428A0");
        return;
    }
    g_installed = true;
    vr::log::Line("PROPS: noise probe: hook on FUN_00E428A0 (hear stimulus) installed");
}

// PropNoise, prima della chiamata: arma la finestra col punto dell'urto.
void Arm(const float* at, double now) {
    if (!g_installed) return;
    std::memcpy(g_at, at, 12);
    g_match = 0;
    g_other = 0;
    g_handle = 0;
    g_until = now + 1.5;
    g_armed = true;
}

// Dal Tick: a finestra chiusa, il conteggio nel log.
void Report(void* walker, double now) {
    if (!g_armed.load() || now < g_until) return;
    g_armed = false;
    const int n = g_match.load(), shown = (std::min)(n, kMaxBots);
    std::string bots;
    for (int i = 0; i < shown; ++i) {
        char b[160];
        const float* q = g_botAt[i];
        if (q[0] == 0.0f && q[1] == 0.0f && q[2] == 0.0f) {
            std::snprintf(b, sizeof(b), "%sbot ? (other caller)", i ? "; " : "");
        } else {
            // Il pawn piu' vicino al bot (il bot Kynapse sta a ~20 unita' dalla Location).
            uint32_t wi = U32(walker, kOffActorWorldInfo), qp = wi ? U32(reinterpret_cast<void*>(wi), kOffWorldPawnList) : 0;
            void* best = nullptr;
            float bd = 1.0e9f;
            for (int guard = 0; qp && guard < 512; ++guard) {
                void* pw = reinterpret_cast<void*>(qp);
                qp = U32(pw, kOffPawnNextPawn);
                float l[3];
                if (!SafeCopy(l, static_cast<char*>(pw) + kOffActorLocation, 12)) continue;
                const float dx = l[0] - q[0], dy = l[1] - q[1], dz = l[2] - q[2];
                const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (d < bd) {
                    bd = d;
                    best = pw;
                }
            }
            const float dx = q[0] - g_at[0], dy = q[1] - g_at[1];
            std::snprintf(b, sizeof(b), "%sbot at (%.0f, %.0f, %.0f) %.1f m from the noise, pawn %s 0x%p (%.0f units)", i ? "; " : "", q[0], q[1],
                          q[2], std::sqrt(dx * dx + dy * dy) / vr::control::g_posScale, best ? vr::script::ClassName(best).c_str() : "?", best, bd);
        }
        bots += b;
    }
    std::string line = "PROPS: noise probe: " + std::to_string(n) + " bot(s) stored a hear stimulus for the noise at (" +
                       std::to_string(static_cast<int>(g_at[0])) + ", " + std::to_string(static_cast<int>(g_at[1])) + ", " +
                       std::to_string(static_cast<int>(g_at[2])) + ") in 1.5 s";
    char h[64];
    std::snprintf(h, sizeof(h), ", instigator handle 0x%08X", g_handle);
    if (n) line += h;
    line += "; " + std::to_string(g_other.load()) + " other hear stimuli";
    if (!bots.empty()) line += ": " + bots;
    vr::log::Line(line.c_str());
}
}  // namespace noiseprobe

bool NoiseSource(HitSource s) {
    return s == HitSource::kThrown || s == HitSource::kDropped || s == HitSource::kGlove || s == HitSource::kKnocked;
}

const char* NoiseSourceName(HitSource s) {
    switch (s) {
        case HitSource::kThrown: return "thrown";
        case HitSource::kDropped: return "dropped";
        case HitSource::kGlove: return "gravity glove";
        case HitSource::kKnocked: return "knocked";
        default: return "other";
    }
}

// Urto di un oggetto (comp di actor) a dv unita'/s di cambio di velocita'
// (per l'energia); breaks = l'urto lo rompe. how per il log.
void PropNoise(void* walker, void* actor, void* comp, const char* how, float dv, bool breaks, double now) {
    if (!vr::control::g_propNoise.load() || vr::control::g_cinemaActive || vr::control::g_gameLocked || !walker || !comp) return;
    if (now < g_noiseNext) {
        ++g_noiseStats.limited;
        return;
    }
    float b[7];
    if (!SafeCopy(b, static_cast<char*>(comp) + kOffCompBounds, sizeof(b)) || !std::isfinite(b[0] + b[1] + b[2])) return;
    g_noiseNext = now + kNoiseEvery;
    const float s = vr::control::g_posScale;
    const std::string mesh = vr::script::ObjectName(reinterpret_cast<void*>(U32(comp, kOffSmcStaticMesh)));
    // Solo il singleton gia' visto dalla scansione a pezzi (revisione: niente
    // giro intero di GObjects per un urto; dopo un caricamento arriva in pochi s).
    void* prop = Prefetched(kPfAiPropagator);
    void* pc = reinterpret_cast<void*>(static_cast<uintptr_t>(U32(walker, 0x214)));
    char t[480];
    if (!prop || !pc) {
        ++g_noiseStats.failed;
        std::snprintf(t, sizeof(t), "PROPS: noise: %s (%s) at (%.0f, %.0f, %.0f): %s, no hear event", mesh.c_str(), how, b[0], b[1], b[2],
                      prop ? "Walker has no controller" : "no YAIEventPropagator yet");
        vr::log::Line(t);
        return;
    }
    // Vetro: la classe template di rottura del KActorFromStatic (YKAFST_GlassSmall/Medium/Large; +0x2F4 e' una
    // classe, quindi conta il suo nome: ClassName dava "Class" e le bottiglie del livello non erano vetro, 27/10).
    const uint32_t tmpl = breaks && actor ? U32(actor, kOffKafsTemplate) : 0;
    const bool glass = tmpl && vr::script::ObjectName(reinterpret_cast<void*>(tmpl)).find("Glass") != std::string::npos;
    const float m = BodyMass(comp), kg = m > 0.0f ? m * vr::control::g_massKgPerUnit.load() : -1.0f;
    const float loud = vr::pure::PropNoiseLoudness(kg, dv / s, glass);
    float base = 0.0f;
    const bool baseOk = SafeCopy(&base, static_cast<char*>(prop) + kOffPropagatorMeleeRange, 4) && std::isfinite(base) && base >= 0.0f &&
                        base < 100000.0f;
    // 0 = sentito da tutti (la nativa salta la prova del raggio): non si scala.
    const bool scale = baseOk && base > 0.0f;
    float radius = scale ? base * loud : base;
    if (scale) SafeCopy(static_cast<char*>(prop) + kOffPropagatorMeleeRange, &radius, 4);
    alignas(4) uint8_t ev[0x14] = {};
    const float volume = 1.0f;
    std::memcpy(ev + 0x00, &pc, 4);
    std::memcpy(ev + 0x04, b, 12);
    std::memcpy(ev + 0x10, &volume, 4);
    if (vr::control::g_noiseProbe.load()) noiseprobe::Arm(b, now);
    const bool sent = vr::script::CallFunction(prop, "SendHearEvent_Melee", ev);
    if (scale) SafeCopy(static_cast<char*>(prop) + kOffPropagatorMeleeRange, &base, 4);
    ++(sent ? g_noiseStats.sent : g_noiseStats.failed);
    if (sent && glass) ++g_noiseStats.glass;
    // Per il log: i nemici entro il raggio, dal cilindro (l'IA misura dal suo
    // centro, col suo fattore d'udito), e il piu' vicino entro 30 m.
    void* nearE[16];
    float nearD[16];
    const int n = FakeEnemiesNear(walker, b, 1500.0f, nearE, nearD, 16, now);
    int inR = 0;
    for (int i = 0; i < n; ++i) inR += !scale && baseOk ? 1 : (nearD[i] <= radius ? 1 : 0);
    char nearest[120] = "no enemy within 30 m";
    if (n > 0)
        std::snprintf(nearest, sizeof(nearest), "nearest enemy %s %.1f m away", vr::script::ClassName(nearE[0]).c_str(), nearD[0] / s);
    char kgs[24] = "?";
    if (kg > 0.0f) std::snprintf(kgs, sizeof(kgs), "%.2f", kg);
    std::snprintf(t, sizeof(t),
                  "PROPS: noise: %s (%s) at (%.0f, %.0f, %.0f), %.1f m/s change, %s kg%s -> loudness %.2f, heard within %.1f m "
                  "(%.0f x %.2f): %s; %d %s within it, %s",
                  mesh.c_str(), how, b[0], b[1], b[2], dv / s, kgs, glass ? ", glass breaks" : (breaks ? ", breaks" : ""), loud,
                  radius / s, base, scale ? loud : 1.0f, sent ? "SendHearEvent_Melee" : "call FAILED", inR, inR == 1 ? "enemy" : "enemies",
                  nearest);
    vr::log::Line(t);
}

void OnPropHit(const PropHit& e) {
    const float s = vr::control::g_posScale;
    std::string what = e.target ? vr::script::ClassName(e.target) : std::string("?");
    if (e.targetKind == kTgtPawn) {
        static const char* const kFrom[] = {"", " (trace)", " (side trace)", " (closest body)"};
        what += e.bone ? " bone " + vr::script::NameFromIndex(e.bone) +
                             ((e.boneFrom & 3) == 3 && !e.boneNear ? std::string(" (closest body, out of reach)") : std::string(kFrom[e.boneFrom & 3]))
                       : std::string(" (no bone)");
    } else {
        static const char* const kTgt[] = {"", "", "fractured mesh", "sand window", "destructible"};
        what += std::string(" (") + kTgt[e.targetKind <= kTgtDestructible ? e.targetKind : 0] + (e.glass ? ", glass)" : ")");
    }
    static const char* const kSrc[] = {"held", "thrown", "dropped", "glove", "shot", "knocked", "other"};
    std::string how = kSrc[static_cast<int>(e.source) <= 6 ? static_cast<int>(e.source) : 6];
    if (e.hand >= 0) how += std::string(", ") + (e.hand ? "left" : "right") + " hand";
    if (e.tip) how += e.kind == HitKind::kHeld ? ", tip" : ", end";
    char b[420];
    std::snprintf(b, sizeof(b), "PROPS: hit %s with %s (%s) at %.1f m/s, mass %.3f, at (%.0f, %.0f, %.0f)", what.c_str(),
                  vr::script::ObjectName(reinterpret_cast<void*>(U32(e.comp, kOffSmcStaticMesh))).c_str(), how.c_str(), e.speed / s,
                  e.mass, e.loc[0], e.loc[1], e.loc[2]);
    vr::log::Line(b);
    // Vibrazione del colpo con l'oggetto in mano (revisione della voce 3,
    // 26/10): il rilevatore fisico vibra solo se PhysX toglie all'oggetto
    // almeno hapdv, e i colpi che la pipeline vede con le trace (di striscio,
    // o sul nemico che il cilindro non ferma) non si sentivano: in 4 collaudi
    // hitthrow su 4 "hit ... bone head (held, left hand) at 1,5 m/s" e nessun
    // impulso. Ora il colpo vibra come un urto: massa ridotta oggetto-mano
    // per la velocita' del punto che colpisce. Refrattario condiviso: il
    // contatto fisico e il colpo dello stesso urto danno un impulso solo.
    // Le stesse finestre mute dell'oggetto tenuto (HeldHapticQuiet): la radio
    // creata accanto al manichino che vola nella presa lo "colpiva" 12 ms
    // dopo la presa (hitall, coperto solo dal refrattario).
    // Voce 4 (mischia): un colpo in mano contato ha la sua vibrazione di 70 ms
    // e puo' rompere l'oggetto (la mano lo lascia): prima della vibrazione
    // generica, che allora non c'e'.
    const bool meleePulsed = HeldMelee(e, ProfMs() * 0.001);
    if (!meleePulsed && e.source == HitSource::kHeld && e.hand >= 0 && e.hand <= 1 && HapticGain() > 0.0f) {
        const Held& held = g_held[e.hand];
        const float kg = held.comp == e.comp ? MassKg(held) : (e.mass > 0.0f ? e.mass * vr::control::g_massKgPerUnit.load() : 1.0f);
        const float v = e.speed / s;
        const float J = vr::pure::ReducedMass(kg, (std::max)(0.1f, g_tune.hapSphereKg)) * v;
        const double now = ProfMs() * 0.001;
        const char* why = held.comp == e.comp ? HeldHapticQuiet(e.hand, held, now) : (GunHandHaptics(e.hand) ? "hand on the gun" : nullptr);
        if (why) HapticMuted(e.hand, "hit", e.comp, J, v, why);
        else HapticSend(e.hand, "hit", e.comp, J, v, kg, vr::pure::ImpactPulse(J, kg, HapticGain(), g_tune.hapJ), now, true, e.loc);
    }
    ThrowDamage(e, ProfMs() * 0.001);
    WorldDamage(e, ProfMs() * 0.001);
    FakeGrenadeOnPawnHit(e);
}

void DisarmHandsReset();  // piu' avanti (voce 10)

void ClearHits() {
    g_tracks.clear();
    g_hitMemo.clear();
    g_throwRec.clear();
    g_knockCooldown.clear();
    g_meleeCooldown.clear();
    DisarmHandsReset();
}

// Uno dei segmenti di un oggetto (centro, punta o capo).
struct Sweep {
    float a[3], b[3];  // dal tick prima a ora (in volo: fin dove sarebbe arrivato senza urti)
    float dir[3];
    float ext;         // mezza estensione del box della trace (unita')
    float vel[3];      // velocita' del punto (in volo da PhysX, in mano dalla mano)
    float swing;       // in mano: |vel - FrameVelocity| (unita'/s, PropHit.swing); in volo |vel|
    float cvel[3];     // velocita' del centro (senza la rotazione dei capi)
    float half;        // mezza estensione dell'oggetto lungo il moto (unita')
    float box[3];      // mezze estensioni del punto che spazza sugli assi del mondo (contatto gia' in corso)
    bool tip;
    int hand;          // mano che lo tiene, -1 in volo (anche nel tick del rilascio)
};

// Attore che la trace attraversa (voce 4, 26/10): bBlockActors (+0xA0) spento,
// non geometria del mondo, non un nemico della lista e non un bersaglio del
// mondo. Le armi lasciate dai nemici morti (YPickup_WeaponSpawnable: +0xA0 =
// 0x0A020104, collisione una SkeletalMeshComponent con +0x14C = 0xC30069)
// fermavano la trace col box di una radio tenuta che ci passava sopra, "not a
// target", e il colpo sul manichino accanto non contava (e la trace a
// estensione zero che lo trovava stava "oltre" l'arma). Non sono muri: non
// fermano nemmeno l'oggetto.
bool TracePassThrough(void* a, const HitPawn* pawns, int nPawns) {
    if (!a || (U32(a, kOffActorFlagsA0) & kBlockActors) || (U32(a, kOffActorFlags9C) & kWorldGeometry)) return false;
    for (int i = 0; i < nPawns; ++i)
        if (pawns[i].pawn == a) return false;
    for (const WorldTarget& t : g_targets)
        if (t.actor == a) return false;
    const uint32_t cls = U32(a, kOffClass);
    const uint8_t ck = cls ? ClassKind(cls) : 0;
    return !(ck >= 4 && ck <= 7);  // bersagli del mondo e muri invisibili restano
}

// --- Disarmo (voce 10 del piano dei combattimenti, 27/10) ---------------------
// Colpire l'arma di un nemico, abbastanza forte, con l'oggetto tenuto, con
// uno lanciato o con la mano vuota gliela fa cadere, con la logica del gioco.
// - Volume colpito: la mesh dell'arma corrente, YGamePawn.m_currentWeapon
//   (+0x1594) -> YWeapon.m_weaponMeshComp (+0x2E0, SkeletalMeshComponent
//   attaccata alla mano, LocalToWorld aggiornata dall'animazione). Box
//   orientato dai limiti della SkeletalMesh (+0x20C -> Bounds +0x3C), che in
//   questo gioco sono il doppio della mesh: AK47 estensione X 45,5 (91 unita',
//   1,8 m) per un'arma di 0,9 m; dimezzata va da -10,9 a 34,6 e la volata
//   dell'AK sta a 34,6 (voce 6). Stesso conto su Steyr, SAW e M4 (letti
//   27/10). Prima una prova sui limiti nel mondo della componente (+0xEC, dai
//   corpi del PhysicsAsset, stretti). La mesh non blocca le trace (+0x14C =
//   0xC30020, senza CollideActors): la prova e' geometrica, come i cilindri
//   della pipeline (vr::pure::SegmentHitsObb).
// - Chi colpisce: gli sweep della pipeline (oggetti tenuti, e lanciati con
//   origine kThrown) e il palmo della mano libera (non la mano dell'arma:
//   GunHandHaptics; niente oggetto, guanto o arma raccolta), contro i nemici di
//   GatherPawns. Una volta per gesto (o lancio) e per arma: g_hitMemo con
//   l'arma come bersaglio, la mano con il suo stato come "componente".
// - Forza: vr::pure::DisarmHit (quantita' di moto). 1: barcolla, l'arma
//   resta; 2: l'arma cade.
// - Effetto, prima degli eventi della pipeline (il colpo sul corpo dello
//   stesso gesto trova le attese della voce 4 e non barcolla due volte):
//   1. la reazione della mischia di Walker, HandleMeleeAttack con 5 di danno
//      (fuoco e ricarica interrotti, il cambio di stato coperto), con le regole
//      della voce 4: un controller, un YPlayerReplicationInfo,
//      IsMeleeAttackable, al piu' una ogni 1,5 s per nemico (g_knockCooldown),
//      niente durante la mischia di Walker;
//   2. livello 2: YArsenalManager.ServerDropWeapon(arma, None), la strada
//      della morte del gioco (YGamePawn.OnTossWeapon -> ServerTossBestWeapon ->
//      ServerDropWeapon): CreatePickupActor (una YPickup_WeaponSpawnable del
//      pool, fisica, con munizioni; temporanea solo con
//      m_performDeathDBNOSpawning), poi RemoveWeapon (UnequipWeapon,
//      SetWeaponActive(None): pawn e controller senza arma; Cleanup, Destroy).
//      Non con le armi delle postazioni (m_turret), con
//      m_pickupDroppingDisabled o senza GetPickupClass (l'arma sparirebbe
//      senza pickup). L'arma caduta parte lungo il colpo (2,5 m/s e 1,5 in su,
//      con un giro).
// - Niente YAIController.RemoveWeapon (il cervello Kynapse): chiamata con
//   l'arma ancora nell'arsenale ha chiuso il gioco (0xC0000005 a 0x00DE7834,
//   FUN_00de7820 su una voce della lista delle armi, 27/10 08:29). Senza,
//   ne' 30 s di combattimento ne' una garbage collection forzata ("obj
//   garbage") danno problemi (sonde f10/probe.py, probe2.py).
// - L'IA disarmata (predoni veri in cpTutorialFight, 27/10) resta dov'e', al
//   riparo, e non spara piu' (nessuna riga SHOT della sua arma in 30 s): i
//   predoni non hanno un comportamento senza armi. Dopo 5 s, se la sua arma e'
//   ancora a terra entro 3 m e Walker non l'ha presa, la riprende con
//   YPickup_Weapon.DoPickup(pawn), la raccolta del gioco (ServerAddWeapon,
//   EquipWeapon, AddWeaponToAI), e torna a combattere (sonda: 32 colpi nei 20 s
//   dopo). Altrimenti resta disarmata (dopo 30 s la mod smette di guardare).
constexpr uintptr_t kOffPawnArsenal = 0x1590;    // YGamePawn.m_arsenalManager
constexpr uintptr_t kOffArsenalCurWeapon = 0x6C4;  // YArsenalManager.m_currentWeapon (riflessione, 27/10)
constexpr uintptr_t kOffPawnCurWeapon = 0x1594;  // YGamePawn.m_currentWeapon
constexpr uintptr_t kOffWeaponTurret = 0x2D0;    // YWeapon.m_turret
constexpr uintptr_t kOffWeaponMeshComp = 0x2E0;  // YWeapon.m_weaponMeshComp
constexpr uintptr_t kOffWeaponFlags268 = 0x268;  // YWeapon: m_pickupDroppingDisabled bit 0x1000000
constexpr uintptr_t kOffSkelCompMesh = 0x20C;    // SkeletalMeshComponent.SkeletalMesh
constexpr uintptr_t kOffSkelMeshBounds = 0x3C;   // SkeletalMesh.Bounds (Origin, BoxExtent, SphereRadius)
constexpr int32_t kDisarmDamage = 5;
constexpr double kRearmDelay = 5.0, kRearmGiveUp = 30.0;
constexpr float kRearmReachM = 3.0f;

struct WeaponBox {
    void* weapon = nullptr;
    void* mesh = nullptr;
    float m[4][4] = {};
    float lc[3] = {}, le[3] = {};  // box locale (meta' dei limiti della SkeletalMesh)
    float wc[3] = {}, we[3] = {};  // limiti nel mondo della componente
};

bool EnemyWeaponBox(void* pw, WeaponBox* out) {
    // Solo YGamePawn (revisione, 27/10): su un altro nemico +0x1594 e' un altro campo.
    const uint32_t cls = U32(pw, kOffClass);
    if (!(g_yGamePawnName >= 0 && cls && ClassIsA(cls, g_yGamePawnName))) return false;
    void* w = reinterpret_cast<void*>(static_cast<uintptr_t>(U32(pw, kOffPawnCurWeapon)));
    if (!w || !ActorAlive(w)) return false;
    void* mc = reinterpret_cast<void*>(static_cast<uintptr_t>(U32(w, kOffWeaponMeshComp)));
    if (!mc || !(U32(mc, kOffCompAttached) & 1)) return false;
    const uint32_t sk = U32(mc, kOffSkelCompMesh);
    float b[7], wb[7];
    if (!sk || !SafeCopy(b, reinterpret_cast<char*>(sk) + kOffSkelMeshBounds, sizeof(b)) ||
        !SafeCopy(wb, static_cast<char*>(mc) + kOffCompBounds, sizeof(wb)) || !ReadMatrix(mc, out->m))
        return false;
    for (int i = 0; i < 3; ++i) {
        const float e = 0.5f * b[3 + i];
        if (!std::isfinite(b[i]) || !(e > 0.2f && e < 120.0f) || !std::isfinite(wb[i]) || !(wb[3 + i] >= 0.0f && wb[3 + i] < 200.0f))
            return false;
        out->lc[i] = b[i];
        out->le[i] = e;
        out->wc[i] = wb[i];
        out->we[i] = wb[3 + i];
    }
    out->weapon = w;
    out->mesh = mc;
    return true;
}

bool HitsWeapon(const WeaponBox& wb, const float* a, const float* b, float margin, float* t) {
    for (int k = 0; k < 3; ++k) {
        const float lo = (std::min)(a[k], b[k]) - margin, hi = (std::max)(a[k], b[k]) + margin;
        if (hi < wb.wc[k] - wb.we[k] || lo > wb.wc[k] + wb.we[k]) return false;
    }
    return vr::pure::SegmentHitsObb(a, b, wb.m, wb.lc, wb.le, margin, t);
}

struct DisarmEvent {
    void* pawn = nullptr;
    void* weapon = nullptr;
    void* prop = nullptr;  // oggetto (nullptr per la mano)
    void* comp = nullptr;
    vr::pure::DisarmHow how = vr::pure::DisarmHow::kHand;
    int hand = -1;
    float kg = 0.0f;
    float speed = 0.0f;  // unita'/s: nel mondo (tenuto, mano) o rispetto al nemico (lanciato)
    float swing = 0.0f;  // unita'/s: rispetto a Walker (tenuto, mano)
    float dir[3] = {};
    float at[3] = {};    // punto del segmento che entra nel box
    uint32_t epoch = 0;  // il lancio o il gesto (HitTrack.epoch)
};
std::vector<DisarmEvent> g_disarmEvents;
struct DisarmStats {
    uint32_t hits = 0, dropped = 0, knocked = 0, soft = 0, refused = 0, noEffect = 0, rearmed = 0, unarmed = 0;
    uint32_t bodyFirst = 0;  // revisione: il corpo gia' colpito dallo stesso lancio o gesto in un tick prima
} g_disarmStats;
// Nemico disarmato in attesa: l'arma caduta (pickup), la spinta lungo il colpo, la ripresa dopo kRearmDelay.
struct Disarmed {
    void* pawn = nullptr;
    void* pickup = nullptr;
    uint32_t pickupClass = 0;
    float dropAt[3] = {};
    float kickV[3] = {}, kickW[3] = {};
    int kickTicks = 0;
    double at = 0.0;
};
std::vector<Disarmed> g_disarmed;
// Mano libera: gesto in corso (come HitTrack.swingOver) e palmo del tick prima.
struct DisarmHand {
    bool over = true;
    uint32_t epoch = 0;
    float prev[3] = {};
    bool prevValid = false;
    double prevAt = 0.0;  // ora del palmo del tick prima (oltre 0,1 s il segmento non vale)
} g_disarmHand[2];

// Revisione (27/10): dopo una pausa, una scena, propcombat spento o un cambio
// di livello (ClearHits, WeaponReset) il palmo del tick prima non vale: il
// primo segmento andava dal palmo di prima della pausa a quello di adesso,
// anche metri, e una mano veloce in quel tick disarmava un'arma mai toccata.
void DisarmHandsReset() {
    for (DisarmHand& hs : g_disarmHand) {
        hs.prevValid = false;
        hs.over = true;
    }
    vr::control::g_disarmNearMs[0] = 0;
    vr::control::g_disarmNearMs[1] = 0;
}

void WeaponSetVel(void* comp, const float* v, const float* w);  // piu' avanti (voce 6)

// L'arma appena caduta: una pickup del pool o della lista della voce 6 con la
// classe giusta, visibile, entro maxD unita' da dove era l'arma.
void* DisarmFindPickup(uint32_t pickupClass, const float* at, float maxD) {
    void* best = nullptr;
    float bestD = maxD * maxD;
    auto consider = [&](void* a) {
        if (!a || !vr::script::LiveObject(a) || WeaponPickupGone(a) || U32(a, kOffPickupWeaponClass) != pickupClass) return;
        float l[3];
        if (!SafeCopy(l, static_cast<char*>(a) + kOffActorLocation, 12)) return;
        const float d2 = (l[0] - at[0]) * (l[0] - at[0]) + (l[1] - at[1]) * (l[1] - at[1]) + (l[2] - at[2]) * (l[2] - at[2]);
        if (d2 < bestD) {
            bestD = d2;
            best = a;
        }
    };
    for (void* a : g_wpnPool) consider(a);
    for (const WeaponPick& wp : g_weapons) consider(wp.actor);
    return best;
}

// Segmento di un oggetto (pipeline) contro le armi dei nemici: eventi in g_disarmEvents.
void WeaponSweep(HitTrack& tr, const Sweep& sw, const HitPawn* pawns, int nPawns, double now) {
    if (!vr::control::g_disarm.load() || nPawns <= 0) return;
    const bool held = sw.hand >= 0;
    if (!held && tr.source != HitSource::kThrown) return;
    const float s = vr::control::g_posScale;
    const float sp = std::sqrt(Dot3(sw.vel, sw.vel));
    const vr::pure::DisarmParams prm;
    // Margine sulla soglia: il lanciato conta rispetto al nemico (deciso in Disarm).
    if ((held ? (std::min)(sw.swing, sp) : sp) < (held ? prm.minHeld : prm.minThrown) * s * 0.8f) return;
    for (int i = 0; i < nPawns; ++i) {
        const HitPawn& hp = pawns[i];
        const float lo[3] = {hp.loc[0], hp.loc[1], hp.loc[2] - hp.h}, hi[3] = {hp.loc[0], hp.loc[1], hp.loc[2] + hp.h};
        if (SegSegDist(sw.a, sw.b, lo, hi) > hp.r + 90.0f + sw.ext) continue;  // l'arma sta entro ~1,5 m dall'asse
        WeaponBox wb;
        float t = 0.0f;
        if (!EnemyWeaponBox(hp.pawn, &wb) || !HitsWeapon(wb, sw.a, sw.b, sw.ext + 2.0f, &t)) continue;
        bool seen = false;
        for (const HitMemo& m : g_hitMemo)
            if (m.comp == tr.comp && m.target == wb.weapon && (m.epoch == tr.epoch || now - m.at < 0.5)) seen = true;
        if (seen) continue;
        // Revisione (27/10): un colpo per lancio o gesto anche quando il corpo
        // viene prima. Lo stesso lancio (o gesto) ha gia' colpito questo nemico
        // in un tick prima (g_hitMemo di SweepHits: in mano solo un colpo da
        // mischia, sopra la sua soglia; un tocco piano non consuma il colpo,
        // come in SweepHits): l'arma che segue, una radio che rimbalza sul corpo
        // attraverso l'arma o un colpo che sfiora il cilindro e poi passa
        // sull'AK, non da' un secondo effetto e una seconda vibrazione. Nello
        // stesso tick vince l'arma (gli eventi sul corpo di quel gesto si
        // tolgono in HitPipeline).
        const float meleeMin = vr::pure::MeleeHitParams().minSpeed * s;
        const HitMemo* body = nullptr;
        for (const HitMemo& m : g_hitMemo)
            if (m.comp == tr.comp && m.target == hp.pawn && m.epoch == tr.epoch && m.at < now && (!held || m.speed >= meleeMin)) body = &m;
        g_hitMemo.push_back({tr.comp, wb.weapon, tr.epoch, now, sp});
        if (body) {
            char lb[300];
            std::snprintf(lb, sizeof(lb), "PROPS: disarm skipped: %s (%s) hit %s's %s, but this %s already hit his body %.0f ms ago",
                          vr::script::ObjectName(reinterpret_cast<void*>(static_cast<uintptr_t>(U32(tr.comp, kOffSmcStaticMesh)))).c_str(),
                          held ? "held" : "thrown", vr::script::ClassName(hp.pawn).c_str(), vr::script::ClassName(wb.weapon).c_str(),
                          held ? "swing" : "throw", (now - body->at) * 1000.0);
            vr::log::Line(lb);
            ++g_disarmStats.bodyFirst;
            continue;
        }
        DisarmEvent d;
        d.pawn = hp.pawn;
        d.weapon = wb.weapon;
        d.prop = tr.actor;
        d.comp = tr.comp;
        d.how = held ? vr::pure::DisarmHow::kHeld : vr::pure::DisarmHow::kThrown;
        d.hand = held ? sw.hand : tr.srcHand;
        if (tr.mass < 0.0f) tr.mass = BodyMass(tr.comp);
        d.kg = tr.mass > 0.0f ? tr.mass * kThrowKgPerUnit : 0.0f;
        if (held) {
            d.speed = sp;
            d.swing = sw.swing;
        } else {
            float pv[3] = {0, 0, 0};
            if (!SafeCopy(pv, static_cast<char*>(hp.pawn) + kOffActorVelocity, 12) || !std::isfinite(pv[0] + pv[1] + pv[2]))
                pv[0] = pv[1] = pv[2] = 0.0f;
            const float rv[3] = {sw.vel[0] - pv[0], sw.vel[1] - pv[1], sw.vel[2] - pv[2]};
            d.speed = d.swing = std::sqrt(Dot3(rv, rv));
        }
        std::memcpy(d.dir, sw.dir, sizeof(d.dir));
        for (int k = 0; k < 3; ++k) d.at[k] = sw.a[k] + (sw.b[k] - sw.a[k]) * t;
        d.epoch = tr.epoch;
        if (g_disarmEvents.size() < 16) g_disarmEvents.push_back(d);
    }
}

// Palmo della mano libera contro le armi dei nemici (ogni tick della pipeline).
void DisarmHands(void* walker, double now, HitPawn* pawns, int& nPawns) {
    const float s = vr::control::g_posScale;
    const vr::pure::DisarmParams prm;
    for (int h = 0; h < 2; ++h) {
        DisarmHand& hs = g_disarmHand[h];
        const bool free = vr::control::g_disarm.load() && g_lastHandValid[h] && !g_held[h].actor && !vr::control::g_propHeld[h].load() &&
                          g_glove[h].state < kGloveFlying && !GunHandHaptics(h);
        if (!free) {
            hs.prevValid = false;
            hs.over = true;
            continue;
        }
        float palm[3];
        for (int i = 0; i < 3; ++i) {
            palm[i] = g_lastHandPos[h][i];
            if (g_palmValid[h])
                palm[i] += g_palmLocal[h][0] * g_lastHandAxes[h][0][i] + g_palmLocal[h][1] * g_lastHandAxes[h][1][i] +
                           g_palmLocal[h][2] * g_lastHandAxes[h][2][i];
        }
        float hv[3] = {0, 0, 0}, fv[3];
        HandVelocity(h, 0.03, hv);
        FrameVelocity(fv);
        const float dr[3] = {hv[0] - fv[0], hv[1] - fv[1], hv[2] - fv[2]};
        const float rel = std::sqrt(Dot3(dr, dr)), world = std::sqrt(Dot3(hv, hv));
        if (!std::isfinite(rel + world)) {
            hs.prevValid = false;
            continue;
        }
        // Un gesto finisce sotto 1 m/s rispetto a Walker; uno nuovo parte da 1,5.
        if (rel < 1.0f * s) hs.over = true;
        if (rel >= 1.5f * s && hs.over) {
            hs.over = false;
            hs.epoch = ++g_hitEpoch;
        }
        float a[3];
        std::memcpy(a, hs.prev, sizeof(a));
        const bool had = hs.prevValid && now - hs.prevAt >= 0.0 && now - hs.prevAt <= 0.1;  // pipeline ferma: il palmo vecchio non vale
        std::memcpy(hs.prev, palm, sizeof(palm));
        hs.prevValid = true;
        hs.prevAt = now;
        const bool fast = (std::min)(rel, world) >= prm.minHand * s;
        // Gesti della sinistra (vr_input.cpp, revisione 27/10): la mano libera
        // che va sull'arma di un nemico non e' un pugno (B) ne' una granata
        // (LB). Da 1,5 m/s rispetto a Walker (il pugno scatta a 2,8 in avanti,
        // la granata a 3,2): il palmo entro 12 unita' (24 cm) dal box
        // dell'arma, adesso o lungo la sua velocita' nei prossimi 0,15 s. Vale
        // 0,3 s dopo l'ultima lettura (g_disarmNearMs).
        if (rel >= 1.5f * s) {
            if (nPawns < 0) nPawns = GatherPawns(walker, pawns, 64, now);
            float ahead[3];
            for (int k = 0; k < 3; ++k) ahead[k] = palm[k] + hv[k] * 0.15f;
            for (int i = 0; i < nPawns; ++i) {
                const HitPawn& hp = pawns[i];
                const float lo[3] = {hp.loc[0], hp.loc[1], hp.loc[2] - hp.h}, hi[3] = {hp.loc[0], hp.loc[1], hp.loc[2] + hp.h};
                if (SegSegDist(palm, ahead, lo, hi) > hp.r + 90.0f + 12.0f) continue;
                WeaponBox wb;
                float tn = 0.0f;
                if (!EnemyWeaponBox(hp.pawn, &wb) || !HitsWeapon(wb, palm, ahead, 12.0f, &tn)) continue;
                const unsigned long long ms = GetTickCount64();
                if (ms >= vr::control::g_disarmNearMs[h].load()) {
                    char lb[260];
                    std::snprintf(lb, sizeof(lb),
                                  "PROPS: disarm: the %s hand is going for %s's %s at %.1f m/s (%.1f relative to Walker): punch and grenade gestures wait",
                                  h ? "left" : "right", vr::script::ClassName(hp.pawn).c_str(), vr::script::ClassName(wb.weapon).c_str(), world / s,
                                  rel / s);
                    vr::log::Line(lb);
                }
                vr::control::g_disarmNearMs[h] = ms + 300ull;
                break;
            }
        }
        if (vr::control::g_hitLog > 0 && had && (std::min)(rel, world) >= 1.5f * s) {
            // Diagnostica (hitlog N): la mano libera veloce e le armi dei nemici vicini.
            vr::control::g_hitLog.fetch_sub(1);
            if (nPawns < 0) nPawns = GatherPawns(walker, pawns, 64, now);
            char lb[400];
            int n = std::snprintf(lb, sizeof(lb), "HITLOG: disarm %s hand palm (%.0f, %.0f, %.0f) at %.1f m/s (%.1f relative to Walker)%s, %d enemies",
                                  h ? "left" : "right", palm[0], palm[1], palm[2], world / s, rel / s, fast ? "" : " (too slow)", nPawns);
            for (int i = 0; i < nPawns && i < 3 && n > 0 && n < static_cast<int>(sizeof(lb)) - 80; ++i) {
                WeaponBox wb;
                if (!EnemyWeaponBox(pawns[i].pawn, &wb)) continue;
                float c[3];
                for (int k = 0; k < 3; ++k) c[k] = wb.lc[0] * wb.m[0][k] + wb.lc[1] * wb.m[1][k] + wb.lc[2] * wb.m[2][k] + wb.m[3][k];
                const float dd[3] = {c[0] - palm[0], c[1] - palm[1], c[2] - palm[2]};
                float tt = 0.0f;
                n += std::snprintf(lb + n, sizeof(lb) - n, "; %s weapon box centre %.0f units away, swept %s", vr::script::ClassName(wb.weapon).c_str(),
                                   std::sqrt(Dot3(dd, dd)), HitsWeapon(wb, a, palm, g_proxy[h].radius + 1.5f, &tt) ? "through it" : "past it");
            }
            vr::log::Line(lb);
        }
        if (!had || !fast || HandJumped(h, 0.05, 25.0f * s)) continue;
        if (nPawns < 0) nPawns = GatherPawns(walker, pawns, 64, now);
        if (nPawns <= 0) continue;
        float b[3];
        for (int i = 0; i < 3; ++i) b[i] = palm[i] + hv[i] / world * 2.0f;
        const float margin = g_proxy[h].radius + 1.5f;
        for (int i = 0; i < nPawns; ++i) {
            const HitPawn& hp = pawns[i];
            const float lo[3] = {hp.loc[0], hp.loc[1], hp.loc[2] - hp.h}, hi[3] = {hp.loc[0], hp.loc[1], hp.loc[2] + hp.h};
            if (SegSegDist(a, b, lo, hi) > hp.r + 90.0f + margin) continue;
            WeaponBox wb;
            float t = 0.0f;
            if (!EnemyWeaponBox(hp.pawn, &wb) || !HitsWeapon(wb, a, b, margin, &t)) continue;
            void* key = &hs;
            bool seen = false;
            for (const HitMemo& m : g_hitMemo)
                if (m.comp == key && m.target == wb.weapon && (m.epoch == hs.epoch || now - m.at < 0.5)) seen = true;
            if (seen) continue;
            g_hitMemo.push_back({key, wb.weapon, hs.epoch, now, rel});
            DisarmEvent d;
            d.pawn = hp.pawn;
            d.weapon = wb.weapon;
            d.how = vr::pure::DisarmHow::kHand;
            d.hand = h;
            d.speed = world;
            d.swing = rel;
            for (int k = 0; k < 3; ++k) {
                d.dir[k] = hv[k] / world;
                d.at[k] = a[k] + (b[k] - a[k]) * t;
            }
            if (g_disarmEvents.size() < 16) g_disarmEvents.push_back(d);
        }
    }
}

// Effetto di un colpo sull'arma (dopo la rilevazione di tutti gli oggetti, prima degli eventi della pipeline).
// true se il colpo conta (barcolla o l'arma cade, o almeno ci ha provato): per l'oggetto e' il colpo di
// quel lancio o gesto su quel nemico.
bool Disarm(const DisarmEvent& d, double now) {
    void* walker = g_tickPawn;
    void* pw = d.pawn;
    void* weapon = d.weapon;
    if (!walker || !pw || pw == walker || !ActorAlive(pw) || !ActorAlive(weapon)) return false;
    const uint32_t wcls = U32(walker, kOffClass), cls = U32(pw, kOffClass);
    if (!(g_yGamePawnName >= 0 && wcls && ClassIsA(wcls, g_yGamePawnName) && cls && ClassIsA(cls, g_yGamePawnName))) return false;
    const PawnDmgState st0 = ReadPawnDmgState(pw);
    if (st0.hp <= 0 || !PawnHostile(walker, pw, cls, now)) return false;
    if (U32(pw, kOffPawnCurWeapon) != static_cast<uint32_t>(reinterpret_cast<uintptr_t>(weapon))) return false;  // gia' cambiata o caduta
    ++g_disarmStats.hits;
    const float s = vr::control::g_posScale;
    const vr::pure::DisarmParams prm;
    const vr::pure::DisarmOut t = vr::pure::DisarmHit(d.how, d.kg, d.speed / s, d.swing / s, prm);
    const std::string wname = vr::script::ClassName(weapon);
    const std::string with = d.comp ? vr::script::ObjectName(reinterpret_cast<void*>(static_cast<uintptr_t>(U32(d.comp, kOffSmcStaticMesh))))
                                    : std::string(d.hand ? "the left hand" : "the right hand");
    static const char* const kHow[] = {"held", "thrown", "empty hand"};
    const char* how = kHow[static_cast<int>(d.how) <= 2 ? static_cast<int>(d.how) : 2];
    char sp[96];
    if (d.how == vr::pure::DisarmHow::kThrown) std::snprintf(sp, sizeof(sp), "%.1f m/s against the enemy", d.speed / s);
    else std::snprintf(sp, sizeof(sp), "%.1f m/s (swing %.1f relative to Walker)", d.speed / s, d.swing / s);
    char b[900];
    if (t.tier == 0) {
        ++g_disarmStats.soft;
        std::snprintf(b, sizeof(b), "PROPS: disarm hit %s's %s with %s (%s, %.1f kg): %s = %.1f kg m/s -> too %s, no effect",
                      vr::script::ClassName(pw).c_str(), wname.c_str(), with.c_str(), how, d.kg, sp, t.impulse, t.slow ? "slow" : "soft");
        vr::log::Line(b);
        return false;
    }
    auto prune = [&](std::vector<std::pair<void*, double>>& list) {
        list.erase(std::remove_if(list.begin(), list.end(), [&](const std::pair<void*, double>& c) { return now >= c.second; }), list.end());
    };
    prune(g_knockCooldown);
    prune(g_meleeCooldown);
    uint8_t wsm = 0;
    SafeCopy(&wsm, static_cast<char*>(walker) + kOffPawnSpecialMove, 1);
    const char* refuse = nullptr;
    if (now < g_walkerMeleeUntil || wsm == 7 || wsm == 8) {
        refuse = "Walker's own melee in progress or just done";
    } else {
        const int ma = MeleeAttackable(pw, walker);
        if (ma < 0) refuse = "IsMeleeAttackable call failed";
        else if (ma == 0) refuse = "IsMeleeAttackable false (down, dead or in a move the melee does not interrupt)";
    }
    void* ars = reinterpret_cast<void*>(static_cast<uintptr_t>(U32(pw, kOffPawnArsenal)));
    const char* noDrop = "";
    uint32_t pickupClass = 0;
    if (!refuse && t.tier == 2) {
        if (!ars || !ActorAlive(ars)) {
            noDrop = "no arsenal";
        } else if (U32(ars, kOffArsenalCurWeapon) != static_cast<uint32_t>(reinterpret_cast<uintptr_t>(weapon))) {
            // Revisione (27/10): YGamePawn.m_currentWeapon e' l'arma *attiva*;
            // col modo secondario (m_SecondaryWeapon) e' l'oggetto secondario,
            // e ServerDropWeapon lo distruggerebbe lasciando il primario nella
            // lista dell'arsenale. Si lascia cadere solo l'arma corrente
            // dell'arsenale (+0x6C4, uguale a quella del pawn in tutti i pawn
            // letti il 27/10: AK47, Steyr, SAW, M4).
            noDrop = "not the arsenal's current weapon (secondary mode?)";
        } else if (U32(weapon, kOffWeaponTurret)) {
            noDrop = "a turret's weapon";
        } else if (U32(weapon, kOffWeaponFlags268) & 0x1000000u) {
            noDrop = "m_pickupDroppingDisabled";
        } else {
            void* ret[4] = {};
            if (!vr::script::CallFunction(weapon, "GetPickupClass", ret) || !ret[0]) noDrop = "no pickup class (it would vanish)";
            else pickupClass = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ret[0]));
        }
    }
    void* ctrl = reinterpret_cast<void*>(static_cast<uintptr_t>(U32(pw, kOffPawnController)));
    const char* noKnock = "";
    if (!refuse) {
        if (!ctrl || !ActorAlive(ctrl)) {
            noKnock = "no controller: the game would kill it with any damage";
        } else if (!PawnHasYPri(pw)) {
            noKnock = "no YPlayerReplicationInfo: the game crashes on a knocked-down pawn without one";
        } else {
            for (const auto& c : g_knockCooldown)
                if (c.first == pw) noKnock = "knocked down less than 1.5 s ago";
            // Revisione (27/10): un colpo in mano sul corpo (voce 4) o un altro
            // disarmo meno di 0,6 s fa: come HeldMelee, niente seconda reazione
            // (l'arma puo' ancora cadere).
            if (!*noKnock)
                for (const auto& c : g_meleeCooldown)
                    if (c.first == pw) noKnock = "hit less than 0.6 s ago (item 4's melee wait)";
        }
    }
    // Direzione: il colpo in orizzontale (la mossa del gioco segue Momentum; il modulo non conta).
    float dir[3] = {d.dir[0], d.dir[1], 0.0f};
    float dl = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1]);
    if (dl < 1e-3f) {
        float wl[3] = {0, 0, 0}, tl[3] = {0, 0, 0};
        SafeCopy(wl, static_cast<char*>(walker) + kOffActorLocation, 12);
        SafeCopy(tl, static_cast<char*>(pw) + kOffActorLocation, 12);
        dir[0] = tl[0] - wl[0];
        dir[1] = tl[1] - wl[1];
        dl = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1]);
    }
    if (dl > 1e-3f) {
        dir[0] /= dl;
        dir[1] /= dl;
    }
    const float momentum[3] = {dir[0] * t.impulse * s, dir[1] * t.impulse * s, 0.0f};
    // Dove sta l'arma adesso (la pickup che il gioco crea parte da li').
    float wm[4][4];
    float dropAt[3] = {d.at[0], d.at[1], d.at[2]};
    void* mc = reinterpret_cast<void*>(static_cast<uintptr_t>(U32(weapon, kOffWeaponMeshComp)));
    if (mc && ReadMatrix(mc, wm) && std::isfinite(wm[3][0] + wm[3][1] + wm[3][2])) std::memcpy(dropAt, wm[3], sizeof(dropAt));
    bool knocked = false, dropped = false, dropCalled = false;
    if (!refuse && !*noKnock) {
        knocked = CallHandleMelee(pw, walker, momentum, kDisarmDamage);
        if (knocked) {
            if (g_knockCooldown.size() < 64) g_knockCooldown.push_back({pw, now + 1.5});
            g_meleeCooldown.push_back({pw, now + 0.6});  // voce 4: il colpo sul corpo dello stesso gesto non conta di nuovo
        }
    }
    if (!refuse && t.tier == 2 && !*noDrop && U32(pw, kOffPawnCurWeapon) == static_cast<uint32_t>(reinterpret_cast<uintptr_t>(weapon))) {
        struct {
            void* weapon;
            void* oldPickup;
            uint8_t spare[0x40];  // locali di ServerDropWeapon: Pickup @8, meshDir @0xC
        } q = {weapon, nullptr, {}};
        dropCalled = vr::script::CallFunction(ars, "ServerDropWeapon", &q);
        dropped = dropCalled && U32(pw, kOffPawnCurWeapon) != static_cast<uint32_t>(reinterpret_cast<uintptr_t>(weapon));
    }
    // L'arma caduta: trovata subito nel pool, spinta lungo il colpo (ora e al tick dopo).
    void* pickup = dropped ? DisarmFindPickup(pickupClass, dropAt, 75.0f) : nullptr;
    if (dropped) {
        Disarmed rec;
        rec.pawn = pw;
        rec.pickup = pickup;
        rec.pickupClass = pickupClass;
        std::memcpy(rec.dropAt, dropAt, sizeof(dropAt));
        for (int k = 0; k < 2; ++k) rec.kickV[k] = dir[k] * 2.5f * s;
        rec.kickV[2] = 1.5f * s;
        rec.kickW[0] = -dir[1] * 8.0f;  // giro attorno all'asse orizzontale di traverso al colpo
        rec.kickW[1] = dir[0] * 8.0f;
        rec.kickW[2] = 0.0f;
        rec.kickTicks = 2;
        rec.at = now;
        void* pm = pickup ? reinterpret_cast<void*>(static_cast<uintptr_t>(U32(pickup, kOffPickupMesh))) : nullptr;
        if (pm) {
            WeaponSetVel(pm, rec.kickV, rec.kickW);
            --rec.kickTicks;
        }
        if (g_disarmed.size() >= 16) g_disarmed.erase(g_disarmed.begin());
        g_disarmed.push_back(rec);
    }
    const PawnDmgState st1 = ReadPawnDmgState(pw);
    const bool effect = st1.hp != st0.hp || st1.sm != st0.sm || st1.knock != st0.knock;
    // Bottiglia, bicchiere, vaso tenuti: si rompono sull'arma come sul corpo
    // (voce 4); il colpo sul corpo dello stesso gesto non conta piu'.
    void* brkActor = nullptr;
    void* brkComp = nullptr;
    if (!refuse && d.how == vr::pure::DisarmHow::kHeld && d.hand >= 0 && d.hand <= 1 && g_held[d.hand].comp == d.comp &&
        Breakable(g_held[d.hand].actor)) {
        brkActor = g_held[d.hand].actor;
        brkComp = g_held[d.hand].comp;
    }
    if (refuse) ++g_disarmStats.refused;
    else if (dropped) ++g_disarmStats.dropped;
    else if (knocked && effect) ++g_disarmStats.knocked;
    else ++g_disarmStats.noEffect;
    // Vibrazione come il colpo della voce 4 (60 ms), mai sulla mano dell'arma.
    if (!refuse && d.hand >= 0 && d.hand <= 1 && d.how != vr::pure::DisarmHow::kThrown) {
        const float kgEff = d.how == vr::pure::DisarmHow::kHeld ? d.kg + prm.armKg : prm.handKg;
        const float v = t.speed;
        const float J = vr::pure::ReducedMass(kgEff, (std::max)(0.1f, g_tune.hapSphereKg)) * v;
        if (GunHandHaptics(d.hand)) {
            HapticMuted(d.hand, "disarm", d.comp, J, v, "hand on the gun");
        } else if (HapticGain() > 0.0f) {
            vr::pure::HapticPulseSpec ps = vr::pure::ImpactPulse(J, kgEff, HapticGain(), g_tune.hapJ);
            ps.ms = 60;
            ps.amp = (std::max)(ps.amp, 0.7f * HapticGain());
            HapticSend(d.hand, "disarm", d.comp, J, v, kgEff, ps, now, false, d.at);
        } else {
            vr::input::PulseHand(d.hand ? 0 : 1, 60);
        }
    }
    std::string tail;
    if (refuse) {
        tail = std::string("no effect: ") + refuse;
    } else {
        tail = knocked ? std::string(effect ? "HandleMeleeAttack (stagger)" : "HandleMeleeAttack: no effect")
                       : std::string("no stagger (") + noKnock + ")";
        if (t.tier == 2) {
            if (*noDrop) {
                tail += std::string(", the weapon stays: ") + noDrop;
            } else if (!dropCalled) {
                tail += ", ServerDropWeapon call failed";
            } else if (!dropped) {
                tail += ", ServerDropWeapon: the weapon is still in his hands";
            } else {
                const uint32_t now2 = U32(pw, kOffPawnCurWeapon);
                char pk[240];
                std::snprintf(pk, sizeof(pk), ", ServerDropWeapon: weapon %s -> %s, pickup %s%s", wname.c_str(),
                              now2 ? vr::script::ClassName(reinterpret_cast<void*>(static_cast<uintptr_t>(now2))).c_str() : "None",
                              pickup ? vr::script::ObjectName(pickup).c_str() : "not found yet",
                              pickup ? ((U32(pickup, kOffPickupTemp) & 1) ? " (temporary: DBNO drop)" : ", knocked along the hit") : "");
                tail += pk;
            }
        }
    }
    std::snprintf(b, sizeof(b),
                  "PROPS: disarm hit %s's %s with %s (%s, %.1f kg): %s = %.1f kg m/s -> %s, %s; health %d -> %d, special move %u -> %u, "
                  "knock flags %05X -> %05X",
                  vr::script::ClassName(pw).c_str(), wname.c_str(), with.c_str(), how, d.kg, sp, t.impulse, t.tier == 2 ? "drop" : "stagger",
                  tail.c_str(), st0.hp, st1.hp, st0.sm, st1.sm, st0.knock, st1.knock);
    vr::log::Line(b);
    if (brkActor) {
        Release(d.hand, "smashed on an enemy's weapon");
        BreakProp(brkActor, brkComp, "smashed on an enemy's weapon");
    }
    return !refuse;
}

// Ogni tick: l'arma caduta (trovata e spinta) e la ripresa dopo kRearmDelay.
void DisarmTick(double now) {
    const float s = vr::control::g_posScale;
    for (size_t i = 0; i < g_disarmed.size();) {
        Disarmed& e = g_disarmed[i];
        const char* end = nullptr;
        bool rearmed = false;
        float dist = -1.0f;
        void* pw = e.pawn;
        if (!vr::script::LiveObject(pw) || !ActorAlive(pw) || static_cast<int32_t>(U32(pw, kOffPawnHealth)) <= 0) {
            end = "dead or gone";
        } else {
            // Non nel pool (il pool vuoto ne crea una): dalla lista della voce 6 (un giro di Scan, 0,2-0,6 s), mentre cade.
            if (!e.pickup && now - e.at < 2.0) e.pickup = DisarmFindPickup(e.pickupClass, e.dropAt, 200.0f);
            const bool pickupOk = e.pickup && vr::script::LiveObject(e.pickup) && !WeaponPickupGone(e.pickup) &&
                                  U32(e.pickup, kOffPickupWeaponClass) == e.pickupClass && !IsHeldActor(e.pickup);
            void* pm = pickupOk ? reinterpret_cast<void*>(static_cast<uintptr_t>(U32(e.pickup, kOffPickupMesh))) : nullptr;
            if (pm && e.kickTicks > 0) {
                --e.kickTicks;
                WeaponSetVel(pm, e.kickV, e.kickW);
            }
            float pl[3], kl[3];
            if (pickupOk && SafeCopy(pl, static_cast<char*>(pw) + kOffActorLocation, 12) &&
                SafeCopy(kl, static_cast<char*>(e.pickup) + kOffActorLocation, 12))
                dist = std::sqrt((pl[0] - kl[0]) * (pl[0] - kl[0]) + (pl[1] - kl[1]) * (pl[1] - kl[1]) + (pl[2] - kl[2]) * (pl[2] - kl[2])) / s;
            if (U32(pw, kOffPawnCurWeapon)) {
                end = "armed again by the game";
            } else if (now - e.at >= kRearmDelay) {
                const PawnDmgState st = ReadPawnDmgState(pw);
                void* ctrl = reinterpret_cast<void*>(static_cast<uintptr_t>(U32(pw, kOffPawnController)));
                const bool ready = ctrl && ActorAlive(ctrl) && !(st.knock & 0x40000u) && st.sm == 0;
                if (ready && pickupOk && dist >= 0.0f && dist <= kRearmReachM) {
                    struct {
                        void* pawn;
                        uint32_t ret;
                        uint8_t spare[0x40];  // locale GI @8
                    } q = {pw, 0, {}};
                    vr::script::CallFunction(e.pickup, "DoPickup", &q);
                    rearmed = U32(pw, kOffPawnCurWeapon) != 0;
                    end = rearmed ? "picked his weapon up again (DoPickup)" : "DoPickup gave him nothing";
                } else if (e.pickup && !pickupOk) {
                    end = "stays unarmed (his weapon was taken or is gone)";
                } else if (now - e.at >= kRearmGiveUp) {
                    end = !e.pickup ? "stays unarmed (his weapon's pickup was never found)" : "stays unarmed (his weapon is out of reach)";
                }
            }
        }
        if (end) {
            if (rearmed) ++g_disarmStats.rearmed;
            else if (std::strncmp(end, "stays", 5) == 0) ++g_disarmStats.unarmed;
            char b[300];
            std::snprintf(b, sizeof(b), "PROPS: disarmed %s %.1f s ago: %s", vr::script::LiveObject(pw) ? vr::script::ClassName(pw).c_str() : "pawn",
                          now - e.at, end);
            if (dist >= 0.0f) {
                const size_t l = std::strlen(b);
                std::snprintf(b + l, sizeof(b) - l, " (weapon %.1f m from him)", dist);
            }
            vr::log::Line(b);
            g_disarmed.erase(g_disarmed.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

// Segmento contro nemici e bersagli: se la trace conferma un colpo nuovo lo mette in coda.
void SweepHits(void* walker, HitTrack& tr, const Sweep& sw, const HitPawn* pawns, int nPawns, double now,
               std::vector<PropHit>& out) {
    const bool held = sw.hand >= 0;
    // Candidati: cilindri dei nemici (tenuto in mano: solo se ci entra adesso) e box dei bersagli.
    const HitPawn* cand = nullptr;
    float candD = 1e30f;
    // Tenuto in mano (voce 4, 26/10): anche braccia e arma del nemico, fuori
    // dal cilindro. Su un predone vero (YPawn_MarauderSMG accucciato nel
    // pullman, cpTutorialFight) la bottiglia si fermava a 26 unita' dall'asse
    // (raggio 15) contro i corpi del PhysicsAsset delle braccia tese, si
    // rompeva in mano e la pipeline non vedeva niente: nessun candidato.
    // Entro kHeldArmReach oltre il raggio la trace a estensione zero qui sotto
    // cerca quei corpi (BlockZeroExtent; quella col box vede solo il cilindro).
    constexpr float kHeldArmReach = 20.0f;
    const HitPawn* armCand = nullptr;
    float armD = 1e30f;
    for (int i = 0; i < nPawns; ++i) {
        const HitPawn& hp = pawns[i];
        float lo[3] = {hp.loc[0], hp.loc[1], hp.loc[2] - hp.h}, hi[3] = {hp.loc[0], hp.loc[1], hp.loc[2] + hp.h};
        float d = SegSegDist(sw.a, sw.b, lo, hi);
        if (d > hp.r + sw.ext + 4.0f) {
            if (held && d <= hp.r + kHeldArmReach + sw.ext && d < armD) {
                armD = d;
                armCand = &hp;
            }
            continue;
        }
        if (held && InCylinder(sw.a, hp.loc, hp.r + sw.ext, hp.h + sw.ext)) continue;  // era gia' dentro
        if (!held) {
            // In volo (revisione 24/10): solo se si avvicina al nemico, e non
            // se e' gia' a contatto. Un oggetto fermo contro il manichino, o
            // spinto da un nemico che ci cammina contro nei suoi 4 s in
            // g_falling, partiva toccando la capsula e poteva dare un colpo.
            float nz = (std::max)(hp.loc[2] - hp.h, (std::min)(hp.loc[2] + hp.h, sw.a[2]));
            float to[3] = {hp.loc[0] - sw.a[0], hp.loc[1] - sw.a[1], nz - sw.a[2]};
            if (Dot3(sw.vel, to) <= 0.0f) continue;  // si allontana (rimbalzato, spinto via)
            float hd = std::sqrt(to[0] * to[0] + to[1] * to[1]);
            float proj = hd > 1e-3f ? (std::fabs(to[0]) * sw.box[0] + std::fabs(to[1]) * sw.box[1]) / hd : 0.0f;
            if (hd < hp.r + proj + 1.0f && std::fabs(sw.a[2] - hp.loc[2]) < hp.h + sw.box[2] + 1.0f) continue;  // gia' a contatto
        }
        if (d < candD) {
            candD = d;
            cand = &hp;
        }
    }
    bool nearTarget = false;
    for (const WorldTarget& t : g_targets) {
        bool overlap = true;
        for (int k = 0; k < 3 && overlap; ++k) {
            float lo = (std::min)(sw.a[k], sw.b[k]) - sw.ext - 4.0f, hi = (std::max)(sw.a[k], sw.b[k]) + sw.ext + 4.0f;
            overlap = hi >= t.mn[k] && lo <= t.mx[k];
        }
        if (overlap) {
            nearTarget = true;
            break;
        }
    }
    if (!cand && !armCand && !nearTarget) return;
    // Tenuto in mano vicino a un nemico: se la trace col box non trova niente,
    // una a estensione zero lungo lo stesso tratto, 6 unita' piu' lunga (il
    // braccio che ha fermato l'oggetto sta contro la sua faccia davanti).
    const bool armPass = held && (cand || armCand);
    // Inizio della trace fuori dalla capsula di Walker (l'oggetto tenuto ci sta
    // spesso dentro e la trace col box colpirebbe subito lui).
    float st[3];
    std::memcpy(st, sw.a, sizeof(st));
    float wl[3], wr = 0.0f, wh = 0.0f;
    uint32_t wcyl = U32(walker, kOffPawnCylinder);
    if (wcyl && SafeCopy(wl, static_cast<char*>(walker) + kOffActorLocation, 12) &&
        SafeCopy(&wh, reinterpret_cast<char*>(wcyl) + kOffCylHeight, 4) && SafeCopy(&wr, reinterpret_cast<char*>(wcyl) + kOffCylRadius, 4)) {
        float te = ExitCylinder(sw.a, sw.b, wl, wr + sw.ext + 2.0f, wh + sw.ext + 2.0f);
        if (te >= 1.0f) return;
        for (int i = 0; i < 3; ++i) st[i] = sw.a[i] + (sw.b[i] - sw.a[i]) * te;
    }
    TraceResult res;
    void* hitActor = nullptr;
    uint8_t kind = kTgtNone;
    bool glass = false;
    const HitPawn* hitPawn = nullptr;
    float usedExt = sw.ext;
    // Prima col box dell'oggetto; vicino a un bersaglio del mondo, se non ne
    // trova uno, anche a estensione zero: c'e' chi blocca solo quella
    // (BlockZeroExtent senza BlockNonZeroExtent: il pannello m01a_166_166 del
    // livello 1, PrimitiveComponent +0x14C = 0xC3002D). Il bersaglio della
    // seconda trace vale solo se non sta oltre cio' che ha fermato la prima
    // (un volume che blocca solo i box ferma anche l'oggetto).
    float firstAt = 1e30f;
    float bArm[3];
    for (int i = 0; i < 3; ++i) bArm[i] = sw.b[i] + sw.dir[i] * 6.0f;
    for (int pass = 0; pass < (nearTarget || armPass ? 2 : 1) && kind == kTgtNone; ++pass) {
        const float ext = pass ? 0.0f : sw.ext;
        const float* bEnd = pass && armPass && !nearTarget ? bArm : sw.b;
        float s0[3];
        std::memcpy(s0, st, sizeof(s0));
        float segLen = 0.0f;
        for (int i = 0; i < 3; ++i) segLen += (bEnd[i] - s0[i]) * (bEnd[i] - s0[i]);
        segLen = std::sqrt(segLen);
        void* got = nullptr;
        TraceResult r;
        for (int k = 0; k < 3 && segLen > 0.5f; ++k) {
            ++g_hitStats.traces;
            if (!EngineTrace(tr.actor, s0, bEnd, ext, &r, pass && nearTarget ? kHitTraceFlags | kTraceComplex : kHitTraceFlags)) break;
            // Walker, le sfere delle mani, gli oggetti tenuti e chi non blocca
            // (armi a terra: TracePassThrough) non contano: si riparte oltre.
            bool skip = r.actor == walker || r.actor == g_proxyActor[0] || r.actor == g_proxyActor[1] ||
                        r.actor == g_held[0].actor || r.actor == g_held[1].actor || TracePassThrough(r.actor, pawns, nPawns);
            if (!skip) {
                got = r.actor;
                break;
            }
            float adv = 0.0f;
            for (int i = 0; i < 3; ++i) {
                s0[i] = r.loc[i] + sw.dir[i] * (ext + 3.0f);
                adv += (bEnd[i] - s0[i]) * sw.dir[i];
            }
            segLen = adv;
        }
        if (!got) continue;
        if (!pass || !hitActor) {
            hitActor = got;  // per il log: quello che ha trovato la trace col box
            res = r;
            usedExt = ext;
        }
        hitPawn = nullptr;
        for (int i = 0; i < nPawns; ++i)
            if (pawns[i].pawn == got) hitPawn = &pawns[i];
        if (hitPawn) {
            kind = kTgtPawn;
        } else {
            for (const WorldTarget& t : g_targets)
                if (t.actor == got) {
                    kind = t.kind;
                    glass = t.glass;
                }
            if (kind == kTgtNone) {
                // Bersaglio non (ancora) nella lista di Scan: lo dice la classe.
                uint32_t cls = U32(got, kOffClass);
                uint8_t ck = cls ? ClassKind(cls) : 0;
                kind = ck == 4 ? kTgtFractured : (ck == 5 ? kTgtSandBomb : (ck == 6 ? kTgtDestructible : kTgtNone));
            }
        }
        float at[3] = {r.loc[0] - st[0], r.loc[1] - st[1], r.loc[2] - st[2]};
        const float along = Dot3(at, sw.dir);
        if (vr::control::g_hitLog > 0 && (nearTarget || pass)) {
            // Diagnostica dei bersagli del mondo: cosa trova ogni trace (mesh del componente).
            uint32_t rc = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(r.comp));
            uint32_t rm = rc && g_smcName >= 0 && ClassIsA(U32(r.comp, kOffClass), g_smcName) ? U32(r.comp, kOffSmcStaticMesh) : 0;
            char b[360];
            std::snprintf(b, sizeof(b), "HITLOG:   trace %d (extent %.0f): %s, mesh %s, material %s, phys material %s, %.0f units along%s",
                          pass + 1, ext, vr::script::ClassName(got).c_str(),
                          rm ? vr::script::ObjectName(reinterpret_cast<void*>(rm)).c_str() : "-",
                          r.material ? vr::script::ObjectName(r.material).c_str() : "-",
                          r.physMaterial ? vr::script::ObjectName(r.physMaterial).c_str() : "-", along,
                          pass && along > firstAt + 1.0f ? " (beyond the first trace's hit)" : "");
            vr::log::Line(b);
        }
        if (!pass) firstAt = along;
        else if (along > firstAt + 1.0f) kind = kTgtNone;
        if (kind != kTgtNone) {
            hitActor = got;
            res = r;
            usedExt = ext;
        }
    }
    if (vr::control::g_hitLog > 0) {
        vr::control::g_hitLog.fetch_sub(1);
        char b[400];
        static const char* const kSrcName[] = {"held", "thrown", "dropped", "glove", "shot", "knocked", "other"};
        const int src = held ? 0 : static_cast<int>(tr.source);
        std::snprintf(b, sizeof(b), "HITLOG: %s %s%s (%s) at %.1f m/s, candidate %s %.0f units from its axis%s -> trace hit %s%s",
                      vr::script::ObjectName(reinterpret_cast<void*>(U32(tr.comp, kOffSmcStaticMesh))).c_str(),
                      held ? "held" : "flying", sw.tip ? (held ? " tip" : " end") : "", kSrcName[src <= 6 ? src : 6],
                      std::sqrt(Dot3(sw.vel, sw.vel)) / vr::control::g_posScale,
                      cand ? vr::script::ClassName(cand->pawn).c_str() : (armCand ? vr::script::ClassName(armCand->pawn).c_str() : "-"),
                      cand ? candD : (armCand ? armD : 0.0f), cand ? (nearTarget ? ", near a world target" : "")
                                                                   : (armCand ? ", arm reach only" : (nearTarget ? ", near a world target" : "")),
                      hitActor ? vr::script::ClassName(hitActor).c_str() : "nothing", hitActor && !kind ? " (not a target)" : "");
        vr::log::Line(b);
    }
    if (kind == kTgtNone) return;
    // Un colpo per lancio (o per colpo in mano) e per bersaglio; comunque non
    // piu' di uno ogni 0,5 s per coppia (oggetto, bersaglio).
    // Eccezione (voce 4, 26/10): in mano, un primo contatto sotto la soglia
    // della mischia (2,5 m/s) non consuma il colpo. Con la portata delle
    // braccia la radio toccava piano la mano del manichino all'inizio del
    // gesto (Hand_Left a 1,9 m/s) e il colpo a 4,5 m/s nel petto, nello
    // stesso gesto, non contava piu'. Un solo passaggio: dopo, la memoria ha
    // la velocita' da mischia. In mano conta la velocita' della mischia
    // (HeldMelee): la minore tra quella nel mondo e quella rispetto a Walker
    // (sw.swing), cosi' un urto camminando non consuma il colpo che segue.
    const float sp = held ? (std::min)(sw.swing, std::sqrt(Dot3(sw.vel, sw.vel))) : std::sqrt(Dot3(sw.vel, sw.vel));
    const float meleeMin = vr::pure::MeleeHitParams().minSpeed * vr::control::g_posScale;
    bool upgraded = false;
    for (HitMemo& m : g_hitMemo)
        if (m.comp == tr.comp && m.target == hitActor && (m.epoch == tr.epoch || now - m.at < 0.5)) {
            if (!(held && kind == kTgtPawn && m.speed < meleeMin && sp >= meleeMin)) return;
            m.speed = sp;
            m.at = now;
            m.epoch = tr.epoch;
            upgraded = true;
        }
    if (!upgraded) g_hitMemo.push_back({tr.comp, hitActor, tr.epoch, now, sp});
    PropHit e;
    e.prop = tr.actor;
    e.comp = tr.comp;
    e.target = hitActor;
    e.targetKind = kind;
    // Vetro: dal nome della mesh (Scan) o, a colpo avvenuto, dal materiale del
    // triangolo colpito (FCheckResult.Material / PhysMaterial: solo la trace a
    // estensione zero li riempie). I vetri del pullman del livello 1
    // (m01a_166_166_single) non hanno "glass" nel nome della mesh.
    e.glass = glass || (kind != kTgtPawn && (NameHasGlass(res.material) || NameHasGlass(res.physMaterial)));
    std::memcpy(e.vel, sw.vel, sizeof(e.vel));
    e.speed = std::sqrt(Dot3(sw.vel, sw.vel));
    e.swing = held ? sw.swing : e.speed;
    std::memcpy(e.dir, sw.dir, sizeof(e.dir));
    // La trace col box da' il centro del box all'impatto: il punto di contatto
    // sta mezza estensione piu' in la', contro la normale.
    for (int i = 0; i < 3; ++i) e.loc[i] = res.loc[i] - res.normal[i] * usedExt;
    std::memcpy(e.normal, res.normal, sizeof(e.normal));
    e.targetComp = res.comp;
    e.item = res.item;
    e.boxTrace = usedExt > 0.0f;
    std::memcpy(e.cvel, sw.cvel, sizeof(e.cvel));
    e.kind = held ? HitKind::kHeld : HitKind::kFlying;
    e.source = held ? HitSource::kHeld : tr.source;
    e.hand = held ? sw.hand : tr.srcHand;
    e.tip = sw.tip;
    if (hitPawn) {
        // Osso: la trace col box prende il cilindro di collisione (BlockNonZeroExtent),
        // quella a estensione zero la mesh fisica e l'osso del suo corpo
        // (BlockZeroExtent, PhysicsAsset). Prima lungo il moto attraverso il
        // nemico, poi in orizzontale verso il suo asse alla quota dell'impatto;
        // vale solo se il corpo toccato e' entro la mezza misura dell'oggetto
        // (+12 unita') dal punto di contatto (una radio caduta sulla testa
        // non e' un colpo alla schiena). Altrimenti il corpo del PhysicsAsset
        // piu' vicino al contatto (SkeletalMeshComponent.FindClosestBone conta
        // tutte le ossa: sul manichino dava Camera_Bone_Left e FACE_LOBR).
        float bs[2][3], be[2][3];
        float ax[3] = {hitPawn->loc[0], hitPawn->loc[1],
                       (std::max)(hitPawn->loc[2] - hitPawn->h, (std::min)(hitPawn->loc[2] + hitPawn->h, e.loc[2]))};
        float toAx[3] = {ax[0] - e.loc[0], ax[1] - e.loc[1], 0.0f};
        float la = std::sqrt(Dot3(toAx, toAx));
        for (int i = 0; i < 3; ++i) {
            bs[0][i] = res.loc[i] - sw.dir[i] * (sw.ext + 4.0f);
            be[0][i] = res.loc[i] + sw.dir[i] * (2.0f * hitPawn->r + sw.ext + 10.0f);
            float u = la > 1.0f ? toAx[i] / la : sw.dir[i];
            bs[1][i] = ax[i] - u * (hitPawn->r + 10.0f);
            be[1][i] = ax[i] + u * hitPawn->r;
        }
        const float reach = sw.half + 12.0f;
        TraceResult rb;
        for (int k = 0; k < 2 && !e.bone; ++k) {
            ++g_hitStats.traces;
            bool got = EngineTrace(tr.actor, bs[k], be[k], 0.0f, &rb);
            float d[3] = {rb.loc[0] - e.loc[0], rb.loc[1] - e.loc[1], rb.loc[2] - e.loc[2]};
            float dist = std::sqrt(Dot3(d, d));
            if (vr::control::g_hitLog > 0) {
                char b[300];
                std::snprintf(b, sizeof(b), "HITLOG: bone trace %d hit %s, component %s, bone %s (body %d), %.0f units from the contact", k + 1,
                              got ? vr::script::ClassName(rb.actor).c_str() : "nothing",
                              got && rb.comp ? vr::script::ClassName(rb.comp).c_str() : "-",
                              got && rb.bone > 0 ? vr::script::NameFromIndex(rb.bone).c_str() : "None", rb.item, got ? dist : 0.0f);
                vr::log::Line(b);
            }
            if (got && rb.actor == hitActor && rb.bone > 0 && dist <= reach) {
                e.bone = rb.bone;
                e.boneFrom = static_cast<uint8_t>(k + 1);
                e.boneNear = true;
                e.targetComp = rb.comp;
                std::memcpy(e.loc, rb.loc, sizeof(e.loc));
                std::memcpy(e.normal, rb.normal, sizeof(e.normal));
            }
        }
        if (!e.bone) {
            uint32_t mesh = U32(hitActor, kOffPawnMesh);
            float bd = 1e30f;
            if (mesh && (e.bone = ClosestBodyBone(reinterpret_cast<void*>(mesh), e.loc, &bd)) != 0) {
                e.boneFrom = 3;
                e.boneNear = bd <= reach;
            }
        }
    }
    if (tr.mass < 0.0f) tr.mass = BodyMass(tr.comp);
    e.mass = tr.mass;
    out.push_back(e);
}

void HitPipeline(void* walker, double now, float dt) {
    const double t0 = ProfMs();
    const float s = vr::control::g_posScale;
    WalkerMeleeTrack(walker, now);  // voce 4: niente colpi in mano durante la mischia di Walker
    for (HitTrack& t : g_tracks) t.alive = false;
    static std::vector<PropHit> events;
    events.clear();
    g_disarmEvents.clear();
    HitPawn pawns[64];
    int nPawns = -1;  // letti solo se un oggetto si muove abbastanza
    auto follow = [&](void* actor, void* comp, int hand, HitSource source, int srcHand, uint32_t gen) {
        HitTrack* tr = nullptr;
        for (HitTrack& t : g_tracks)
            if (t.comp == comp) tr = &t;
        if (tr && tr->alive) return;  // gia' visto in questo tick (tenuto e in g_falling)
        if (!ActorUsable(actor)) return;
        Prop cur;
        void* nx = CompNx(comp);
        float lin[3], ang[3];
        if (!ReadBounds(comp, &cur) || !nx || !NxVelocities(nx, lin, ang)) return;
        float c[3], v[3];
        for (int i = 0; i < 3; ++i) {
            c[i] = 0.5f * (cur.boxMin[i] + cur.boxMax[i]);
            v[i] = lin[i] * 50.0f;  // PhysX m/s -> unita'/s
        }
        if (!std::isfinite(v[0] + v[1] + v[2]) || !std::isfinite(ang[0] + ang[1] + ang[2])) return;
        bool fresh = !tr || tr->actor != actor;
        if (fresh) {
            if (!tr) {
                g_tracks.emplace_back();
                tr = &g_tracks.back();
            }
            *tr = HitTrack();
            tr->actor = actor;
            tr->comp = comp;
            tr->epoch = ++g_hitEpoch;
            tr->hasEnds = LongAxisEnds(comp, tr->endLocal);
        } else if (tr->hand >= 0 && hand < 0) {
            tr->epoch = ++g_hitEpoch;  // rilasciato: un lancio nuovo
        } else if (hand < 0 && gen != tr->gen) {
            tr->epoch = ++g_hitEpoch;  // in volo con un'origine nuova (ripreso col guanto e lasciato)
        }
        tr->source = source;
        tr->srcHand = srcHand;
        tr->gen = gen;
        if (hand >= 0 && (fresh || tr->hand != hand)) {
            tr->hasTip = HeldTipLocal(comp, g_lastHandPos[hand], tr->tipLocal);
            tr->tipValid = false;
        }
        float tip[3] = {0, 0, 0};
        bool tipOk = hand >= 0 && tr->hasTip && LocalToWorld(comp, tr->tipLocal, tip);
        // Capi dell'asse maggiore degli oggetti lunghi, a ogni tick (in mano e in volo).
        float ends[2][3] = {};
        bool endsOk = tr->hasEnds && LocalToWorld(comp, tr->endLocal[0], ends[0]) && LocalToWorld(comp, tr->endLocal[1], ends[1]);
        // Mezze estensioni del box (assi del mondo): la trace usa un box con la
        // minore (al piu' 8 unita'); lungo il moto si aggiunge il resto.
        float ext[3] = {0.5f * (cur.boxMax[0] - cur.boxMin[0]), 0.5f * (cur.boxMax[1] - cur.boxMin[1]),
                        0.5f * (cur.boxMax[2] - cur.boxMin[2])};
        float e = (std::max)(1.0f, (std::min)(8.0f, (std::min)(ext[0], (std::min)(ext[1], ext[2]))));
        // Rilasciato in questo tick (era in mano): il tratto dalla mano si
        // spazza gia' come un lancio, con la velocita' di adesso.
        const bool released = !fresh && tr->hand >= 0 && hand < 0;
        if (!fresh && (tr->hand == hand || released)) {
            Sweep sw[3];
            int nSw = 0;
            if (hand < 0) {
                // In volo: dalla posizione del tick prima lungo la velocita' di
                // allora, fin dove sarebbe arrivato senza urti (o dove e' davvero,
                // se oltre). L'oggetto rimbalza sulla capsula cinematica del
                // nemico (YPawnKinematicRBCollider, con o senza
                // bIgnoreRigidBodyPawns: il flag lo legge solo FUN_008ba520) e
                // il suo centro non ci entra.
                const float* pv = released ? v : tr->v;
                const float* pw = released ? ang : tr->w;
                float sp = std::sqrt(Dot3(pv, pv));
                if (sp >= 1.0f * s) {
                    Sweep& w = sw[nSw++];
                    for (int i = 0; i < 3; ++i) w.dir[i] = pv[i] / sp;
                    float moved[3] = {c[0] - tr->c[0], c[1] - tr->c[1], c[2] - tr->c[2]};
                    float L = (std::max)(sp * dt, Dot3(moved, w.dir));
                    float along = std::fabs(w.dir[0]) * ext[0] + std::fabs(w.dir[1]) * ext[1] + std::fabs(w.dir[2]) * ext[2];
                    float look = (std::max)(0.0f, along - e) + 2.0f;
                    for (int i = 0; i < 3; ++i) {
                        w.a[i] = tr->c[i];
                        w.b[i] = tr->c[i] + w.dir[i] * (L + look);
                        w.vel[i] = pv[i];
                        w.cvel[i] = pv[i];
                        w.box[i] = ext[i];
                    }
                    w.ext = e;
                    w.half = along;
                    w.swing = sp;
                    w.tip = false;
                    w.hand = -1;
                }
                // Oggetto lungo lanciato di traverso: i capi possono toccare un
                // nemico che il centro passa lontano (box della trace al piu' 8
                // unita'). Anche i due capi, con la loro velocita' (centro piu'
                // rotazione PhysX del tick prima).
                if (endsOk && tr->endsValid) {
                    for (int k = 0; k < 2; ++k) {
                        const float* q = tr->ends[k];
                        float arm[3] = {q[0] - tr->c[0], q[1] - tr->c[1], q[2] - tr->c[2]}, spin[3], ve[3];
                        Cross3(pw, arm, spin);
                        for (int i = 0; i < 3; ++i) ve[i] = pv[i] + spin[i];
                        float se = std::sqrt(Dot3(ve, ve));
                        if (se < 1.0f * s || !std::isfinite(se)) continue;
                        Sweep& w = sw[nSw++];
                        for (int i = 0; i < 3; ++i) w.dir[i] = ve[i] / se;
                        float moved[3] = {ends[k][0] - q[0], ends[k][1] - q[1], ends[k][2] - q[2]};
                        float L = (std::max)(se * dt, Dot3(moved, w.dir));
                        w.ext = (std::min)(e, 4.0f);
                        for (int i = 0; i < 3; ++i) {
                            w.a[i] = q[i];
                            w.b[i] = q[i] + w.dir[i] * (L + 2.0f);
                            w.vel[i] = ve[i];
                            w.cvel[i] = pv[i];
                            w.box[i] = w.ext;
                        }
                        w.half = w.ext;
                        w.swing = se;
                        w.tip = true;
                        w.hand = -1;
                    }
                }
            } else {
                // In mano: la velocita' dei punti viene dalla mano (traslazione
                // piu' rotazione del polso, come in Drive), non da PhysX. Premuto
                // contro un nemico, l'oggetto tenuto trema e PhysX dava 24 m/s
                // con la mano ferma: un colpo falso ogni 0,5 s (24/10). Il
                // segmento parte dalla posizione vera del tick prima e va avanti
                // quanto la mano: l'oggetto fermato dalla capsula del nemico non
                // ci entra, la mano si'. Un salto del tracciamento (un passo
                // oltre 25 m/s tra due campioni negli ultimi 50 ms) non e' un
                // colpo; prima si scartava ogni mano oltre 10 m/s di media, e i
                // colpi piu' forti (la voce 4) non avrebbero dato nulla.
                float hv[3] = {0, 0, 0};
                HandVelocity(hand, 0.03, hv);
                if (!HandJumped(hand, 0.05, 25.0f * s)) {
                    const float* hp = g_lastHandPos[hand];
                    float pv[2][3] = {}, psp[2] = {0.0f, 0.0f};
                    bool has[2] = {true, tipOk && tr->tipValid};
                    // Con il peso (revisione 25/10) la velocita' e' quella data
                    // all'oggetto al tick prima (lastCmdV e lastCmdW, eseguite
                    // da PhysX in questo passo), non quella della mano: un
                    // oggetto pesante resta indietro e va piu' piano della mano,
                    // e colpiva alla velocita' della mano. Il comando e' pulito
                    // come la mano: premuto contro un nemico la base e' la
                    // velocita' misurata e il comando resta basso (Drive). Senza
                    // la forza del distacco (weightCmdV/W): un oggetto incastrato
                    // strappato via non colpisce alla velocita' dello strappo.
                    const Held& hd = g_held[hand];
                    const bool cmd = hd.weight && hd.hasCmd && hd.comp == comp;
                    for (int k = 0; k < 2; ++k) {
                        if (!has[k]) continue;
                        const float* p = k ? tip : c;
                        float spin[3];
                        if (cmd) {
                            float arm[3] = {p[0] - c[0], p[1] - c[1], p[2] - c[2]};
                            Cross3(hd.weightCmdW, arm, spin);
                            for (int i = 0; i < 3; ++i) pv[k][i] = hd.weightCmdV[i] + spin[i];
                        } else {
                            float arm[3] = {p[0] - hp[0], p[1] - hp[1], p[2] - hp[2]};
                            Cross3(g_heldHandW[hand], arm, spin);
                            for (int i = 0; i < 3; ++i) pv[k][i] = hv[i] + spin[i];
                        }
                        psp[k] = std::sqrt(Dot3(pv[k], pv[k]));
                    }
                    // Il colpo rispetto a Walker (revisione della voce 4, 26/10):
                    // la velocita' dei punti meno quella con cui Walker porta le
                    // mani (FrameVelocity, come il lancio). Camminando (3,7-3,8
                    // m/s) con l'oggetto fermo in mano contro un nemico la
                    // velocita' nel mondo passava la soglia della mischia: barcolla
                    // e la bottiglia si rompeva senza nessun colpo. Le sweep
                    // restano nel mondo (l'urto c'e', riga "hit" e vibrazione).
                    float fv[3];
                    FrameVelocity(fv);
                    float rsp[2] = {0.0f, 0.0f};
                    for (int k = 0; k < 2; ++k) {
                        if (!has[k]) continue;
                        const float d[3] = {pv[k][0] - fv[0], pv[k][1] - fv[1], pv[k][2] - fv[2]};
                        rsp[k] = std::sqrt(Dot3(d, d));
                    }
                    // Un colpo in mano finisce quando l'oggetto (centro e punta)
                    // scende sotto 1 m/s: uno fermato dal nemico mentre la mano
                    // passa oltre dava due colpi a 0,5 s nello stesso gesto (24/10).
                    // Rispetto a Walker: camminando con la mano ferma ogni gesto
                    // nuovo e' un colpo nuovo (nel mondo il colpo non finiva mai).
                    const float fastest = (std::max)(rsp[0], rsp[1]);
                    if (fastest < 1.0f * s) tr->swingOver = true;
                    if (fastest >= 1.5f * s && tr->swingOver) {
                        tr->swingOver = false;
                        tr->epoch = ++g_hitEpoch;
                    }
                    for (int k = 0; k < 2; ++k) {
                        const bool isTip = k == 1;
                        if (!has[k] || psp[k] < 1.5f * s) continue;
                        const float* p = isTip ? tip : c;
                        const float* q = isTip ? tr->tip : tr->c;
                        Sweep& w = sw[nSw++];
                        for (int i = 0; i < 3; ++i) w.dir[i] = pv[k][i] / psp[k];
                        float moved[3] = {p[0] - q[0], p[1] - q[1], p[2] - q[2]};
                        float L = (std::max)(psp[k] * dt, Dot3(moved, w.dir));
                        float along = std::fabs(w.dir[0]) * ext[0] + std::fabs(w.dir[1]) * ext[1] + std::fabs(w.dir[2]) * ext[2];
                        float look = isTip ? 2.0f : (std::max)(0.0f, along - e) + 2.0f;
                        w.ext = isTip ? (std::min)(e, 4.0f) : e;
                        for (int i = 0; i < 3; ++i) {
                            w.a[i] = q[i];
                            w.b[i] = q[i] + w.dir[i] * (L + look);
                            w.vel[i] = pv[k][i];
                            w.cvel[i] = pv[0][i];
                            w.box[i] = isTip ? w.ext : ext[i];
                        }
                        w.half = isTip ? w.ext : along;
                        w.swing = rsp[k];
                        w.tip = isTip;
                        w.hand = hand;
                    }
                }
            }
            if (nSw && nPawns < 0) nPawns = GatherPawns(walker, pawns, 64, now);
            for (int k = 0; k < nSw; ++k) {
                SweepHits(walker, *tr, sw[k], pawns, (std::max)(0, nPawns), now, events);
                WeaponSweep(*tr, sw[k], pawns, nPawns, now);  // voce 10: l'arma del nemico
            }
        }
        tr->hand = hand;
        std::memcpy(tr->c, c, sizeof(c));
        std::memcpy(tr->v, v, sizeof(v));
        std::memcpy(tr->w, ang, sizeof(ang));
        if (tipOk) std::memcpy(tr->tip, tip, sizeof(tip));
        tr->tipValid = tipOk;
        if (endsOk) std::memcpy(tr->ends, ends, sizeof(ends));
        tr->endsValid = endsOk;
        tr->alive = true;
    };
    for (int h = 0; h < 2; ++h)
        if (g_held[h].actor && !g_held[h].constrained) follow(g_held[h].actor, g_held[h].comp, h, HitSource::kHeld, h, 0);
    for (const Falling& f : g_falling) follow(f.actor, f.comp, -1, f.source, f.hand, f.gen);
    DisarmHands(walker, now, pawns, nPawns);  // voce 10: la mano libera sull'arma del nemico
    g_tracks.erase(std::remove_if(g_tracks.begin(), g_tracks.end(), [](const HitTrack& t) { return !t.alive; }), g_tracks.end());
    g_hitMemo.erase(std::remove_if(g_hitMemo.begin(), g_hitMemo.end(), [&](const HitMemo& m) { return now - m.at > 5.0; }),
                    g_hitMemo.end());
    // Eventi dopo la rilevazione di tutti gli oggetti (un effetto puo' distruggere attori).
    // Prima i colpi sulle armi (voce 10): la loro reazione mette le attese della
    // voce 4, e il colpo sul corpo dello stesso gesto non barcolla di nuovo.
    for (const DisarmEvent& d : g_disarmEvents) {
        if (!Disarm(d, now) || !d.comp) continue;
        // Il colpo sull'arma e' il colpo di questo lancio (o gesto) su questo
        // nemico: niente secondo colpo sul corpo (riga hit, voci 1 e 4). Una
        // radio lanciata sull'AK del manichino lo toccava 41 ms dopo e il
        // TakeDamage leggero della voce 1 interrompeva il barcollare (9 -> 0).
        g_hitMemo.push_back({d.comp, d.pawn, d.epoch, now, 1e9f});
        events.erase(std::remove_if(events.begin(), events.end(), [&](const PropHit& e) { return e.comp == d.comp && e.target == d.pawn; }),
                     events.end());
        // Revisione (27/10): tolto il colpo sul corpo, manca anche
        // FakeGrenadeOnPawnHit (voce 7). Un lancio che colpisce l'arma ha
        // colpito il nemico: niente finta granata quando la bottiglia (che
        // attraversa l'arma) atterra accanto a lui.
        if (d.how == vr::pure::DisarmHow::kThrown) {
            PropHit fe;
            fe.comp = d.comp;
            fe.target = d.pawn;
            fe.targetKind = kTgtPawn;
            fe.kind = HitKind::kFlying;
            fe.source = HitSource::kThrown;
            FakeGrenadeOnPawnHit(fe);
        }
    }
    g_disarmEvents.clear();
    g_hitStats.hits += static_cast<uint32_t>(events.size());
    for (const PropHit& e : events) OnPropHit(e);
    const double ms = ProfMs() - t0;
    g_hitStats.sum += ms;
    g_hitStats.worst = (std::max)(g_hitStats.worst, ms);
    ++g_hitStats.ticks;
    if (nPawns >= 0) {  // tick con oggetti in moto (segmenti e nemici letti)
        g_hitStats.movingSum += ms;
        ++g_hitStats.movingTicks;
    }
    g_hitStats.maxTracks = (std::max)(g_hitStats.maxTracks, static_cast<uint32_t>(g_tracks.size()));
    if (nPawns > 0) g_hitStats.maxPawns = (std::max)(g_hitStats.maxPawns, static_cast<uint32_t>(nPawns));
}

}  // namespace

double g_lastTickAt = 0.0;
struct HighlightBox {
    bool valid = false;
    void* comp = nullptr;
    int state = 0;
} g_highlight;

bool Highlight(float mn[3], float mx[3], int* state) {
    if (!g_highlight.valid || !g_highlight.comp) return false;
    Prop p;
    if (!ReadBounds(g_highlight.comp, &p)) return false;
    for (int i = 0; i < 3; ++i) {
        mn[i] = p.boxMin[i];
        mx[i] = p.boxMax[i];
    }
    *state = g_highlight.state;
    return true;
}

// --- Dita -------------------------------------------------------------------
namespace fingers {

// --- Dita sull'oggetto (FINGERSOLVE_BEGIN) -------------------------------------
struct Atom {  // FBoneAtom
    Quat rot;
    float trans[3];
    float scale;
};

// Oggetto tenuto nello spazio della mesh delle braccia: box con gli spigoli
// arrotondati (rho; la sezione di una bottiglia tonda e' un box con rho = raggio).
struct Shape {
    float c[3], ax[3][3], e[3], rho;  // centro, assi (righe), mezze estensioni
};

float ShapeDist(const Shape& s, const float* x) {  // < 0 dentro
    float out = 0.0f, in = -1e9f;
    for (int i = 0; i < 3; ++i) {
        float t = (x[0] - s.c[0]) * s.ax[i][0] + (x[1] - s.c[1]) * s.ax[i][1] + (x[2] - s.c[2]) * s.ax[i][2];
        float q = std::fabs(t) - (s.e[i] - s.rho);
        if (q > 0.0f) out += q * q;
        in = (std::max)(in, q);
    }
    return std::sqrt(out) + (std::min)(in, 0.0f) - s.rho;
}

float SegDist(const float* x, const float* a, const float* b) {
    float ab[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, ax[3] = {x[0] - a[0], x[1] - a[1], x[2] - a[2]};
    float l2 = Dot3(ab, ab), t = l2 > 1e-12f ? (std::max)(0.0f, (std::min)(1.0f, Dot3(ax, ab) / l2)) : 0.0f;
    float d[3] = {ax[0] - ab[0] * t, ax[1] - ab[1] * t, ax[2] - ab[2] * t};
    return std::sqrt(Dot3(d, d));
}

// Catena di un dito: tre ossa, le giunture e la punta stimata (la falange
// distale non ha un osso dopo: punta = 0,8 della falange media lungo l'asse
// dell'osso distale, preso dalla falange media).
struct Chain {
    Atom a[3];
    float pt[4][3];
    float bone[3];
    float l3;
};

void ChainTip(Chain& ch) {
    float d[3];
    Rotate(ch.a[2].rot, ch.bone, d);
    for (int i = 0; i < 3; ++i) ch.pt[3][i] = ch.pt[2][i] + d[i] * ch.l3;
}

bool ChainInit(Chain& ch) {
    for (int j = 0; j < 3; ++j) std::memcpy(ch.pt[j], ch.a[j].trans, 12);
    float s[3] = {ch.pt[2][0] - ch.pt[1][0], ch.pt[2][1] - ch.pt[1][1], ch.pt[2][2] - ch.pt[1][2]};
    float l2 = std::sqrt(Dot3(s, s));
    if (!(l2 > 1e-4f)) return false;
    for (float& v : s) v /= l2;
    Rotate(Conj(ch.a[1].rot), s, ch.bone);
    ch.l3 = 0.8f * l2;
    ChainTip(ch);
    return true;
}

// Piega la giuntura j (con le ossa dopo) di ang attorno al suo asse di flessione.
void Bend(Chain& ch, int j, const float* local, float ang) {
    float axis[3];
    Rotate(ch.a[j].rot, local, axis);
    if (!Norm3(axis)) return;
    Quat r = AxisAngle(axis, ang);
    for (int k = j; k < 3; ++k) ch.a[k].rot = Mul(r, ch.a[k].rot);
    for (int k = j + 1; k < 4; ++k) {
        float d[3] = {ch.pt[k][0] - ch.pt[j][0], ch.pt[k][1] - ch.pt[j][1], ch.pt[k][2] - ch.pt[j][2]}, rd[3];
        Rotate(r, d, rd);
        for (int i = 0; i < 3; ++i) ch.pt[k][i] = ch.pt[j][i] + rd[i];
    }
    for (int k = j + 1; k < 3; ++k) std::memcpy(ch.a[k].trans, ch.pt[k], 12);
}

// Ruota tutta la catena attorno alla base (pollice).
void Swing(Chain& ch, const float* axis, float ang) {
    Quat r = AxisAngle(axis, ang);
    for (int k = 0; k < 3; ++k) ch.a[k].rot = Mul(r, ch.a[k].rot);
    for (int k = 1; k < 4; ++k) {
        float d[3] = {ch.pt[k][0] - ch.pt[0][0], ch.pt[k][1] - ch.pt[0][1], ch.pt[k][2] - ch.pt[0][2]}, rd[3];
        Rotate(r, d, rd);
        for (int i = 0; i < 3; ++i) ch.pt[k][i] = ch.pt[0][i] + rd[i];
    }
    for (int k = 1; k < 3; ++k) std::memcpy(ch.a[k].trans, ch.pt[k], 12);
}

// Flessione della giuntura j (radianti, + = verso il palmo, come la chiusura):
// dal riferimento (j = 0: direzione delle dita della mano; poi il segmento
// precedente) al segmento j, attorno all'asse di flessione.
float Flex(const Chain& ch, int j, const float* local, const float* ref0) {
    float h[3], r[3], d[3], c[3];
    Rotate(ch.a[j].rot, local, h);
    if (!Norm3(h)) return 0.0f;
    for (int i = 0; i < 3; ++i) {
        r[i] = j == 0 ? ref0[i] : ch.pt[j][i] - ch.pt[j - 1][i];
        d[i] = ch.pt[j + 1][i] - ch.pt[j][i];
    }
    float rh = Dot3(r, h), dh = Dot3(d, h);
    for (int i = 0; i < 3; ++i) {
        r[i] -= rh * h[i];
        d[i] -= dh * h[i];
    }
    Cross3(r, d, c);
    return std::atan2(Dot3(c, h), Dot3(r, d));
}

// Distanza dall'oggetto e dalle capsule in piu' (dita gia' messe, per il pollice).
float Obstacle(const Shape& s, const float* x, const float (*caps)[2][3], int ncaps, float capRad) {
    float d = ShapeDist(s, x);
    for (int c = 0; c < ncaps; ++c) d = (std::min)(d, SegDist(x, caps[c][0], caps[c][1]) - capRad);
    return d;
}

// Distanza minima, meno il raggio del dito, dei segmenti j..last (< 0 = compenetra).
float ChainClear(const Chain& ch, int j, int last, const Shape& s, float rad, const float (*caps)[2][3], int ncaps, float capRad) {
    float m = 1e9f;
    for (int k = j; k <= last; ++k)
        for (int q = 1; q <= 5; ++q) {
            float u = 0.2f * q, x[3];
            for (int i = 0; i < 3; ++i) x[i] = ch.pt[k][i] + (ch.pt[k + 1][i] - ch.pt[k][i]) * u;
            m = (std::min)(m, Obstacle(s, x, caps, ncaps, capRad) - rad);
        }
    return m;
}

// Dito sull'oggetto, angoli assoluti (dal dito dritto, non sopra l'animazione:
// l'animazione sull'arma ha gia' la falange media piegata di ~70 gradi e la
// chiusura aggiunta mandava le dita dentro la bottiglia). Chiusura comune con
// le proporzioni del pugno fino al primo contatto, poi ogni giuntura dopo il
// segmento che tocca si chiude ancora finche' tocca anche lei.
// phi = flessione attuale (animazione); th = angoli voluti.
void SolveFinger(const Chain& ch, const float* local, const float phi[3], const Shape& s, float rad, const float fist[3],
                 const float hi[3], float th[3]) {
    Chain w;
    auto pose = [&](const float* t) {
        w = ch;
        for (int j = 0; j < 3; ++j) Bend(w, j, local, t[j] - phi[j]);
    };
    auto clearAt = [&](float c) {
        float t[3] = {fist[0] * c, fist[1] * c, fist[2] * c};
        pose(t);
        return ChainClear(w, 0, 2, s, rad, nullptr, 0, 0.0f) >= 0.0f;
    };
    float c = 0.0f;
    if (clearAt(1.0f)) {
        c = 1.0f;
    } else if (clearAt(0.0f)) {
        float lo = 0.0f, up = 1.0f;
        for (int it = 0; it < 12; ++it) {
            float m = 0.5f * (lo + up);
            if (clearAt(m)) lo = m;
            else up = m;
        }
        c = lo;
    }
    for (int j = 0; j < 3; ++j) th[j] = fist[j] * c;
    if (c >= 1.0f) return;  // non tocca nulla: pugno
    for (int j = 0; j < 3; ++j) {
        auto okj = [&](float x) {
            float t[3] = {th[0], th[1], th[2]};
            t[j] = x;
            pose(t);
            return ChainClear(w, j, 2, s, rad, nullptr, 0, 0.0f) >= 0.0f;
        };
        float a0 = th[j], a1 = hi[j];
        if (a1 <= a0 || !okj(a0)) continue;
        if (okj(a1)) {
            th[j] = a1;
            continue;
        }
        for (int it = 0; it < 12; ++it) {
            float m = 0.5f * (a0 + a1);
            if (okj(m)) a0 = m;
            else a1 = m;
        }
        th[j] = a0;
    }
}

// Pollice: l'animazione lo tiene opposto, proprio dove sta l'oggetto. Prima
// ruota tutto dalla base attorno a (base -> giuntura distale) x push: se
// compenetra, verso push quanto basta per uscirne; se e' fuori, verso
// l'oggetto finche' tocca (al massimo 40 gradi). push: dove deve stare il
// pollice (presa di forza: verso il polso, sulla faccia vicina; a pinza: sulla
// faccia lontana). Poi le giunture media e distale vanno verso l'oggetto (o le
// dita) finche' toccano: su quelli sottili si appoggia su indice e medio.
struct ThumbPose {
    float axis[3] = {0, 0, 1};
    float swing = 0.0f, d1 = 0.0f, d2 = 0.0f;
};

void SolveThumb(const Chain& base, const float* local, const float* push, const Shape& s, float rad, const float (*caps)[2][3], int ncaps,
                float capRad, float cmU, ThumbPose* tp) {
    *tp = ThumbPose();
    Chain ch = base;
    float arm[3] = {ch.pt[2][0] - ch.pt[0][0], ch.pt[2][1] - ch.pt[0][1], ch.pt[2][2] - ch.pt[0][2]};
    Cross3(arm, push, tp->axis);
    if (Norm3(tp->axis)) {
        const float Dg = 0.0174533f;
        auto clr = [&](float ang) {
            Chain t = base;
            Swing(t, tp->axis, ang);
            return ChainClear(t, 0, 1, s, rad, caps, ncaps, capRad);
        };
        float lo = 0.0f, up = 0.0f;
        if (clr(0.0f) < 0.0f) {
            up = 70.0f * Dg;  // dentro: fuori, il meno possibile (oltre 70 gradi resta a 70)
            if (clr(up) >= 0.0f)
                for (int it = 0; it < 12; ++it) {
                    float m = 0.5f * (lo + up);
                    if (clr(m) >= 0.0f) up = m;
                    else lo = m;
                }
            tp->swing = up;
        } else if (clr(-40.0f * Dg) < 0.0f) {
            up = -40.0f * Dg;  // fuori: verso l'oggetto fino al contatto
            for (int it = 0; it < 12; ++it) {
                float m = 0.5f * (lo + up);
                if (clr(m) >= 0.0f) lo = m;
                else up = m;
            }
            tp->swing = lo;
        }
        Swing(ch, tp->axis, tp->swing);
    } else {
        tp->axis[0] = 0.0f;
        tp->axis[1] = 0.0f;
        tp->axis[2] = 1.0f;
    }
    const float D = 0.0174533f, lo = -30.0f * D, hi = 45.0f * D, step = 5.0f * D;
    for (int j = 1; j <= 2; ++j) {
        auto clr = [&](float x) {
            Chain t = ch;
            Bend(t, j, local, x);
            return ChainClear(t, j, 2, s, rad, caps, ncaps, capRad);
        };
        float bestX = 0.0f, bestC = 1e9f, maxX = 0.0f, maxC = -1e9f;
        for (int q = 0; q <= 15; ++q) {
            float x = lo + step * q, cq = clr(x);
            if (cq > maxC) {
                maxC = cq;
                maxX = x;
            }
            if (cq >= 0.0f && cq < bestC) {
                bestC = cq;
                bestX = x;
            }
        }
        float x;
        if (bestC > 1e8f) {
            x = maxX;  // sempre dentro: il meno possibile
        } else if (bestC > 2.0f * cmU) {
            x = clr(0.0f) >= 0.0f ? 0.0f : bestX;  // niente vicino: come l'animazione
        } else {
            x = bestX;
            for (int sd = -1; sd <= 1; sd += 2) {  // fino al contatto, verso il vicino che compenetra
                float y = bestX + step * sd;
                if (y < lo - 1e-4f || y > hi + 1e-4f || clr(y) >= 0.0f) continue;
                float a0 = bestX, a1 = y;
                for (int it = 0; it < 10; ++it) {
                    float m = 0.5f * (a0 + a1);
                    if (clr(m) >= 0.0f) a0 = m;
                    else a1 = m;
                }
                x = a0;
                break;
            }
        }
        Bend(ch, j, local, x);
        (j == 1 ? tp->d1 : tp->d2) = x;
    }
}
// --- FINGERSOLVE_END -----------------------------------------------------------

struct Chains {
    void* mesh = nullptr;
    std::string suffix;
    int bone[5][3];  // mignolo, anulare, medio, indice, pollice; base -> punta
    bool ok = false;
};
Chains g_chains[2];
float g_curl[2] = {0.0f, 0.0f};        // peso della posa della mod sopra l'animazione (0..1), segue il bersaglio in ~0,12 s
float g_curlTarget[2] = {0.0f, 0.0f};  // scritta da Tick
float g_th[2][5][4] = {};              // angoli voluti seguiti in ~0,08 s (dita: 3 giunture; pollice: rotazione + 3 giunture)
float g_swingAxis[2][3] = {{0, 0, 1}, {0, 0, 1}};  // asse della rotazione del pollice (spazio della mesh)
bool g_thValid[2] = {false, false};

bool FindChains(void* mesh, const char* suffix, Chains* c) {
    static const char* kNames[5] = {"Pinky", "Ring", "Middle", "Index", "Thumb"};
    c->mesh = mesh;
    c->suffix = suffix;
    c->ok = false;
    int found[5] = {0, 0, 0, 0, 0};
    for (int i = 0; i < 256; ++i) {
        struct {
            int32_t index;
            int32_t ret[2];
        } parms = {i, {0, 0}};
        if (!vr::script::CallFunction(mesh, "GetBoneName", &parms) || parms.ret[0] <= 0) break;
        std::string n = vr::script::NameFromIndex(parms.ret[0]);
        for (int f = 0; f < 5; ++f)
            if (n == std::string(kNames[f]) + "_" + suffix && found[f] < 3) c->bone[f][found[f]++] = i;
    }
    for (int f = 0; f < 5; ++f)
        if (found[f] != 3) {
            vr::log::Line(std::string("FINGERS: no ") + kNames[f] + "_" + suffix + " chain on this mesh.");
            return false;
        }
    c->ok = true;
    char b[200];
    std::snprintf(b, sizeof(b), "FINGERS: %s hand chains pinky %d ring %d middle %d index %d thumb %d", suffix, c->bone[0][0],
                  c->bone[1][0], c->bone[2][0], c->bone[3][0], c->bone[4][0]);
    vr::log::Line(b);
    return true;
}

// Palmo e terna della presa dalle ossa (prima della chiusura). Le nocche (basi
// delle 4 dita) sono rigide con la mano, quindi la terna non dipende
// dall'animazione: prima la normale veniva dalla falange del medio, gia'
// piegata di ~35 gradi dall'animazione sull'arma, ed era inclinata verso il
// polso (la bottiglia finiva sopra il pollice). Convertiti negli assi della
// mano e mediati (le ossa seguono l'IK con un frame di ritardo).
void MeasurePalm(void* mesh, int side, uint32_t data, uint32_t num, const Chains& c) {
    if (!g_lastHandValid[side]) return;
    // Posa scritta oltre la portata del braccio (solo con solidhands, dove la
    // mano disegnata ruota attorno a questo palmo): le ossa restano indietro
    // (9-18 cm col braccio teso, radio tenuta) e le nocche negli assi della
    // mano davano un palmo spostato di altrettanto, (8,5, 0,3, 0,1) ->
    // (-8,6, -10,1, 1,0) cm; il polso disegnato, ruotato attorno a quel palmo,
    // finiva a 6,7 cm invece di 0,5 (p5x_reach_full, revisione 26/10). Il
    // palmo e' rigido con la mano: resta l'ultimo misurato.
    if (g_beyondReach[side]) return;
    float m[4][4];
    if (!SafeCopy(m, static_cast<char*>(mesh) + 0x70, 64)) return;  // LocalToWorld della mesh
    // Rispetto alla posa scritta nell'IK (voce 5: con solidhands la mano
    // disegnata non e' sul bersaglio); senza scostamento il bersaglio, come prima.
    float hpR[3], haR[3][3];
    RefHand(side, hpR, haR);
    const float(*ha)[3] = haR;
    const float* hp = hpR;
    float k[4][3];  // nocche di mignolo, anulare, medio, indice negli assi della mano
    Atom mid;
    for (int f = 0; f < 4; ++f) {
        Atom b;
        if (static_cast<uint32_t>(c.bone[f][0]) >= num || !SafeCopy(&b, reinterpret_cast<char*>(data) + 32 * c.bone[f][0], 32)) return;
        if (f == 2) mid = b;
        float wv[3];
        for (int i = 0; i < 3; ++i) wv[i] = b.trans[0] * m[0][i] + b.trans[1] * m[1][i] + b.trans[2] * m[2][i] + m[3][i] - hp[i];
        for (int r = 0; r < 3; ++r) k[f][r] = Dot3(wv, ha[r]);
    }
    float o[3], A[3], F[3], P[3];
    for (int i = 0; i < 3; ++i) {
        o[i] = 0.25f * (k[0][i] + k[1][i] + k[2][i] + k[3][i]);
        A[i] = k[3][i] - k[0][i];
    }
    // Le nocche stanno entro 15 cm dal bersaglio IK; oltre, mano non ancora sul bersaglio.
    float lim = 0.15f * vr::control::g_posScale;
    if (Dot3(o, o) > lim * lim || !Norm3(A)) return;
    float oa = Dot3(o, A);
    for (int i = 0; i < 3; ++i) F[i] = o[i] - oa * A[i];  // dal polso (bersaglio IK) alle nocche
    if (!Norm3(F)) return;
    Cross3(A, F, P);
    // Verso del palmo: dove la flessione del medio (asse "fingeraxis") porta f.
    int axisCode = vr::control::g_fingerAxis.load();
    float local[3] = {0, 0, 0};
    local[(std::abs(axisCode) - 1) % 3] = axisCode < 0 ? -1.0f : 1.0f;
    float ax[3], aw[3], al[3], fl[3];
    Rotate(mid.rot, local, ax);
    for (int i = 0; i < 3; ++i) aw[i] = ax[0] * m[0][i] + ax[1] * m[1][i] + ax[2] * m[2][i];
    for (int r = 0; r < 3; ++r) al[r] = Dot3(aw, ha[r]);
    if (!Norm3(al)) return;
    Cross3(al, F, fl);
    if (Dot3(fl, P) < 0.0f)
        for (float& v : P) v = -v;
    GripFrame& g = g_grip[side];
    float a = g.valid ? 0.1f : 1.0f;
    float s0 = Dot3(k[0], A) - oa, s1 = Dot3(k[3], A) - oa;
    float off = 0.015f * vr::control::g_posScale;
    for (int i = 0; i < 3; ++i) {
        g.o[i] += (o[i] - g.o[i]) * a;
        g.f[i] += (F[i] - g.f[i]) * a;
        g.a[i] += (A[i] - g.a[i]) * a;
        g.p[i] += (P[i] - g.p[i]) * a;
        // Palmo per la presa sul palmo e i guanti: 1,5 cm dalla nocca del medio verso il palmo.
        g_palmLocal[side][i] += (k[2][i] + P[i] * off - g_palmLocal[side][i]) * a;
        g_palmAxisLocal[side][i] += (al[i] - g_palmAxisLocal[side][i]) * a;
    }
    g.span[0] += (s0 - g.span[0]) * a;
    g.span[1] += (s1 - g.span[1]) * a;
    // Di nuovo ortonormale: a esatto, f perpendicolare, p = a x f col verso del palmo.
    Norm3(g.a);
    float fa = Dot3(g.f, g.a);
    for (int i = 0; i < 3; ++i) g.f[i] -= fa * g.a[i];
    Norm3(g.f);
    float pc[3];
    Cross3(g.a, g.f, pc);
    float sg = Dot3(pc, g.p) < 0.0f ? -1.0f : 1.0f;
    for (int i = 0; i < 3; ++i) {
        g.p[i] = pc[i] * sg;
        g_palmNormalLocal[side][i] = g.p[i];
    }
    Norm3(g_palmAxisLocal[side]);
    if (!g.valid) {
        char b[260];
        float kc = 100.0f / vr::control::g_posScale;
        std::snprintf(b, sizeof(b),
                      "FINGERS: knuckles %.1f %.1f %.1f cm, fingers %.2f %.2f %.2f, palm normal %.2f %.2f %.2f, span %.1f..%.1f cm (hand axes)",
                      g.o[0] * kc, g.o[1] * kc, g.o[2] * kc, g.f[0], g.f[1], g.f[2], g.p[0], g.p[1], g.p[2], g.span[0] * kc, g.span[1] * kc);
        vr::log::Line(b);
    }
    g.valid = true;
    g_palmValid[side] = true;
    // Voce 5 (solidlog): nocche di riferimento (assi della mano) prese
    // l'ultima volta che la mano era sul bersaglio (nessuno scostamento):
    // fisse durante lo scostamento, cosi' lo scarto delle ossa non si
    // nasconde nella media di g.o che lo segue.
    static float o0[2][3] = {};
    if (!g_render[side].deltaValid) std::memcpy(o0[side], g.o, sizeof(o0[side]));
    if (vr::control::g_solidLog > 0 && g_render[side].deltaValid) {
        // Nocche dalle ossa nel mondo contro la posa scritta (scarto, mm) e
        // contro il bersaglio (lo scostamento davvero disegnato, cm), con le
        // nocche di riferimento; palmo misurato negli assi della mano (deve restare fermo).
        const float* tp = g_lastHandPos[side];
        const float(*ta)[3] = g_lastHandAxes[side];
        const float* r0 = o0[side];
        float kw[3], pr[3], pt[3];
        for (int i = 0; i < 3; ++i) {
            kw[i] = hp[i] + o[0] * ha[0][i] + o[1] * ha[1][i] + o[2] * ha[2][i];
            pr[i] = hp[i] + r0[0] * ha[0][i] + r0[1] * ha[1][i] + r0[2] * ha[2][i];
            pt[i] = tp[i] + r0[0] * ta[0][i] + r0[1] * ta[1][i] + r0[2] * ta[2][i];
        }
        const float s = vr::control::g_posScale;
        const float dr[3] = {kw[0] - pr[0], kw[1] - pr[1], kw[2] - pr[2]}, dt3[3] = {kw[0] - pt[0], kw[1] - pt[1], kw[2] - pt[2]};
        const float dw[3] = {pr[0] - pt[0], pr[1] - pt[1], pr[2] - pt[2]};
        char b[300];
        std::snprintf(b, sizeof(b),
                      "SOLID: palm bones: knuckles %.1f mm from the written pose, %.2f cm from the target (written %.2f cm: dx %.2f dy %.2f dz %.2f cm)"
                      " | palm local (%.2f, %.2f, %.2f) cm",
                      std::sqrt(Dot3(dr, dr)) / s * 1000.0f, std::sqrt(Dot3(dt3, dt3)) / s * 100.0f, std::sqrt(Dot3(dw, dw)) / s * 100.0f,
                      dt3[0] / s * 100.0f, dt3[1] / s * 100.0f, dt3[2] / s * 100.0f, g_palmLocal[side][0] / s * 100.0f,
                      g_palmLocal[side][1] / s * 100.0f, g_palmLocal[side][2] / s * 100.0f);
        vr::log::Line(b);
    }
}

// Oggetto tenuto nello spazio della mesh delle braccia: nella posa voluta
// (mano + relPos/relAxes come Drive: stabile, senza il ritardo della fisica)
// o, per la diagnostica, in quella fisica.
bool HeldShape(int side, const float M[4][4], bool actual, Shape* o) {
    const Held& hd = g_held[side];
    float lc[3], le[3], om[4][4], sph = 0.0f;
    if (!hd.actor || !MeshLocalBounds(hd.comp, lc, le, &sph) || !ReadMatrix(hd.comp, om)) return false;
    float s[3], axw[3][3], ow[3];
    for (int i = 0; i < 3; ++i) s[i] = std::sqrt(om[i][0] * om[i][0] + om[i][1] * om[i][1] + om[i][2] * om[i][2]);
    if (actual) {
        if (!CompPose(hd.comp, ow, axw)) return false;
    } else {
        if (!g_lastHandValid[side]) return false;
        float rp[3], ra[3][3], hp[3], ha[3][3];
        RefHand(side, hp, ha);  // la mano come e' scritta (voce 5)
        // Con la mano che segue la fisica (voce 5) la mano disegnata tiene
        // l'oggetto nella presa senza abbassamento ne' piega (sono la mano che
        // cede, e si vedono nella mano): le dita su quella posa, cioe'
        // sull'oggetto vero quando lo scostamento e' intero.
        HeldTarget(hd, SolidHeld(hd) ? nullptr : ha, rp, ra);
        for (int c = 0; c < 3; ++c) {
            ow[c] = hp[c] + rp[0] * ha[0][c] + rp[1] * ha[1][c] + rp[2] * ha[2][c];
            for (int i = 0; i < 3; ++i) axw[i][c] = ra[i][0] * ha[0][c] + ra[i][1] * ha[1][c] + ra[i][2] * ha[2][c];
        }
    }
    float cw[3];
    for (int c = 0; c < 3; ++c) cw[c] = ow[c] + lc[0] * s[0] * axw[0][c] + lc[1] * s[1] * axw[1][c] + lc[2] * s[2] * axw[2][c];
    float ap[3], aa[3][3];
    if (!actual && hd.weight && !hd.constrained && CompPose(hd.comp, ap, aa)) {
        // Peso (revisione 25/10): l'oggetto vero resta indietro (cassa da
        // 52 kg: 12-20 cm e decine di gradi) mentre la mano disegnata e' sul
        // controller. Le dita lo seguono dalla posa voluta fin a fingerReach
        // (3 cm, 15 gradi): un ritardo piccolo si vede come l'oggetto che
        // scivola un po' nella presa; oltre la presa resta una presa (le
        // dita non si chiudono a pugno nel vuoto). La mano che segue la
        // fisica e' la voce 5 del piano.
        float reach = (std::max)(0.0f, g_tune.fingerReach) * 0.01f * vr::control::g_posScale;
        // Voce 5: la parte dello scostamento che la portata del braccio non
        // lascia disegnare (braccio teso) e' un ritardo tra la mano disegnata
        // e l'oggetto vero: le dita lo recuperano tutto, restano sull'oggetto
        // (revisione 26/10: prima si chiudevano nel vuoto sopra di lui).
        if (SolidHeld(hd) && g_render[side].deltaValid) reach += g_render[side].dCut;
        const float maxAng = (std::max)(0.0f, g_tune.fingerReach) * 5.0f * 0.0174533f;
        float ac[3], d[3];
        for (int c = 0; c < 3; ++c) {
            ac[c] = ap[c] + lc[0] * s[0] * aa[0][c] + lc[1] * s[1] * aa[1][c] + lc[2] * s[2] * aa[2][c];
            d[c] = ac[c] - cw[c];
        }
        const float dl = std::sqrt(Dot3(d, d));
        const float kd = dl > reach ? reach / dl : 1.0f;
        float ta[3][3], ra[3][3];
        std::memcpy(ta, axw, sizeof(ta));
        std::memcpy(ra, aa, sizeof(ra));
        const bool mir = Det3(ta) < 0.0f;  // mesh specchiata: la rotazione propria, come in HeldTarget
        if (mir)
            for (int c = 0; c < 3; ++c) {
                ta[2][c] = -ta[2][c];
                ra[2][c] = -ra[2][c];
            }
        const Quat qt = RowsToQuat(ta), qa = RowsToQuat(ra);
        const float qd = std::fabs(qt.w * qa.w + qt.x * qa.x + qt.y * qa.y + qt.z * qa.z);
        const float ang = 2.0f * std::acos((std::min)(1.0f, qd));
        QuatToRows(Slerp(qt, qa, ang > maxAng ? maxAng / ang : 1.0f), axw);
        if (mir)
            for (float& v : axw[2]) v = -v;
        for (int c = 0; c < 3; ++c) cw[c] += d[c] * kd;
    }
    float k2 = Dot3(M[0], M[0]), k = std::sqrt(k2);  // mondo = p * M, righe = k * R
    if (!(k2 > 1e-12f)) return false;
    for (int r = 0; r < 3; ++r) {
        o->c[r] = ((cw[0] - M[3][0]) * M[r][0] + (cw[1] - M[3][1]) * M[r][1] + (cw[2] - M[3][2]) * M[r][2]) / k2;
        for (int i = 0; i < 3; ++i) o->ax[i][r] = Dot3(axw[i], M[r]) / k;
    }
    int L = 0;
    for (int i = 0; i < 3; ++i) {
        o->e[i] = le[i] * s[i] / k;
        if (o->e[i] > o->e[L]) L = i;
    }
    float mn = 1e9f;
    for (int i = 0; i < 3; ++i)
        if (i != L) mn = (std::min)(mn, o->e[i]);
    o->rho = SectionRound(le, sph, L, g_tune.round) * mn;
    return true;
}

}  // namespace fingers

// Dita della mano di supporto, dopo la composizione dello scheletro. Peso
// g_curl (0 = animazione del gioco, 1 = posa della mod): con l'oggetto in mano
// le dita si chiudono fino a toccarlo (SolveFinger) e il pollice ne esce
// (SolveThumb); col grip a vuoto pugno. Angoli assoluti: la posa non dipende
// dall'animazione sotto (prima la chiusura si sommava all'animazione sull'arma).
void PoseFingers(void* mesh, int side, const char* handSuffix) {
    using namespace fingers;
    if (side < 0 || side > 1 || !mesh) return;
    static double lastT[2] = {0.0, 0.0};
    double now = ProfMs() * 0.001;
    float dt = lastT[side] > 0.0 ? static_cast<float>(now - lastT[side]) : 0.0f;
    lastT[side] = now;
    float target = vr::control::g_fingerCurlForce >= 0.0f ? vr::control::g_fingerCurlForce.load() : g_curlTarget[side];
    float k = (std::min)(1.0f, (std::max)(0.0f, dt) / 0.12f);
    g_curl[side] += (target - g_curl[side]) * k;
    Chains& c = g_chains[side];
    if (c.mesh != mesh || c.suffix != handSuffix) FindChains(mesh, handSuffix, &c);
    if (!c.ok) return;
    uint32_t data = U32(mesh, 0x274), num = U32(mesh, 0x278);  // SkeletalMeshComponent.SpaceBases
    if (!data || num == 0 || num > 1024) return;
    MeasurePalm(mesh, side, data, num, c);
    const float w = g_curl[side];
    const bool dump = vr::control::g_graspDump > 0;
    if ((w < 0.01f && !dump) || !g_grip[side].valid || !g_lastHandValid[side]) {
        g_thValid[side] = false;
        return;
    }
    float M[4][4];
    if (!SafeCopy(M, static_cast<char*>(mesh) + 0x70, 64)) return;  // LocalToWorld della mesh
    float mk = std::sqrt(Dot3(M[0], M[0]));
    if (!(mk > 1e-6f)) return;
    const float cmU = 0.01f * vr::control::g_posScale / mk;  // 1 cm in unita' della mesh
    // Direzione delle dita della mano (terna della presa) nello spazio della
    // mesh: il riferimento della flessione delle basi (0 = dito dritto). Assi
    // della mano come e' scritta nell'IK (voce 5; senza scostamento il bersaglio).
    float refP[3], refA[3][3];
    RefHand(side, refP, refA);
    const float(*ha)[3] = refA;
    const GripFrame& g = g_grip[side];
    float fw[3], fref[3];
    for (int i = 0; i < 3; ++i) fw[i] = g.f[0] * ha[0][i] + g.f[1] * ha[1][i] + g.f[2] * ha[2][i];
    for (int r = 0; r < 3; ++r) fref[r] = Dot3(fw, M[r]);
    if (!Norm3(fref)) return;
    Shape shape;
    const bool held = g_held[side].actor != nullptr;
    const bool solve = held && HeldShape(side, M, g_held[side].constrained, &shape);
    // Pugno (gradi dal dito dritto: base, media, distale) e limiti; il pollice a
    // pugno si piega in piu' dell'animazione, come prima.
    static const float kFist[4][3] = {{85, 100, 65}, {85, 100, 65}, {80, 100, 65}, {75, 95, 60}};
    static const float kHi[3] = {90, 110, 80};
    static const float kThumbFist[3] = {20, 35, 35};
    const float D = 0.0174533f;
    const float kk = (std::min)(1.0f, (std::max)(0.0f, dt) / 0.08f);
    int fa = vr::control::g_fingerAxis.load(), ta = vr::control::g_thumbAxis.load();
    float lf[3] = {0, 0, 0}, lt[3] = {0, 0, 0};
    lf[(std::abs(fa) - 1) % 3] = fa < 0 ? -1.0f : 1.0f;
    lt[(std::abs(ta) - 1) % 3] = ta < 0 ? -1.0f : 1.0f;
    const float rad = g_tune.finger * cmU, radT = g_tune.thumb * cmU;
    Chain posed[5];
    float phi[4][3] = {}, goal[5][4] = {};
    float caps[6][2][3];  // indice e medio gia' messi: ostacoli per il pollice
    int ncaps = 0;
    for (int f = 0; f < 5; ++f) {
        Chain& ch = posed[f];
        for (int j = 0; j < 3; ++j)
            if (static_cast<uint32_t>(c.bone[f][j]) >= num ||
                !SafeCopy(&ch.a[j], reinterpret_cast<char*>(data) + 32 * c.bone[f][j], sizeof(Atom)))
                return;
        if (!ChainInit(ch)) return;
        if (f < 4) {
            for (int j = 0; j < 3; ++j) phi[f][j] = Flex(ch, j, lf, fref);
            float th[3];
            if (solve) {
                float fist[3], hi[3];
                for (int j = 0; j < 3; ++j) {
                    fist[j] = kFist[f][j] * D;
                    hi[j] = kHi[j] * D;
                }
                SolveFinger(ch, lf, phi[f], shape, rad, fist, hi, th);
            } else {
                for (int j = 0; j < 3; ++j) th[j] = kFist[f][j] * D * (held ? 0.6f : 1.0f);  // oggetto senza limiti: mezza chiusura
            }
            for (int j = 0; j < 3; ++j) {
                float& s = g_th[side][f][j];
                if (!g_thValid[side]) s = phi[f][j];
                s += (th[j] - s) * kk;
                goal[f][j] = th[j];
                Bend(ch, j, lf, w * (s - phi[f][j]));
            }
            if (f >= 2)
                for (int q = 0; q < 3; ++q) {
                    std::memcpy(caps[ncaps][0], ch.pt[q], 12);
                    std::memcpy(caps[ncaps][1], ch.pt[q + 1], 12);
                    ++ncaps;
                }
        } else {
            float tgt[4] = {0.0f, kThumbFist[0] * D, kThumbFist[1] * D, kThumbFist[2] * D};  // rotazione, 3 giunture
            if (solve) {
                ThumbPose tp;
                // Presa di forza (e sul palmo): pollice sulla faccia verso il polso; a pinza: sulla faccia lontana.
                float push[3], pw[3];
                const float* gv = g_held[side].gripKind == 2 ? g.p : g.f;
                float sg = g_held[side].gripKind == 2 ? 1.0f : -1.0f;
                for (int i = 0; i < 3; ++i) pw[i] = sg * (gv[0] * ha[0][i] + gv[1] * ha[1][i] + gv[2] * ha[2][i]);
                for (int r = 0; r < 3; ++r) push[r] = Dot3(pw, M[r]);
                Norm3(push);
                SolveThumb(ch, lt, push, shape, radT, caps, ncaps, rad, cmU, &tp);
                tgt[0] = tp.swing;
                tgt[1] = 0.0f;
                tgt[2] = tp.d1;
                tgt[3] = tp.d2;
                if (tp.swing != 0.0f) std::memcpy(g_swingAxis[side], tp.axis, 12);
            } else if (held) {
                for (int j = 1; j < 4; ++j) tgt[j] *= 0.6f;
            }
            for (int j = 0; j < 4; ++j) {
                float& s = g_th[side][4][j];
                if (!g_thValid[side]) s = 0.0f;
                s += (tgt[j] - s) * kk;
                goal[4][j] = tgt[j];
            }
            Swing(ch, g_swingAxis[side], w * g_th[side][4][0]);
            for (int j = 0; j < 3; ++j) Bend(ch, j, lt, w * g_th[side][4][j + 1]);
        }
        for (int j = 0; j < 3; ++j) SafeCopy(reinterpret_cast<char*>(data) + 32 * c.bone[f][j], &ch.a[j], sizeof(Atom));
    }
    g_thValid[side] = true;
    if (dump) {
        // Diagnostica "graspdump N": giunture e punta vera nel mondo (unita'),
        // flessione (posa, animazione, voluta, gradi) e distanza dall'oggetto
        // nella posa fisica (cm, < 0 = dentro).
        vr::control::g_graspDump.fetch_sub(1);
        const float toCm = 100.0f / vr::control::g_posScale;
        auto Wp = [&](const float* pc, float* out) {
            for (int i = 0; i < 3; ++i) out[i] = pc[0] * M[0][i] + pc[1] * M[1][i] + pc[2] * M[2][i] + M[3][i];
        };
        Shape act;
        const bool haveAct = held && HeldShape(side, M, true, &act);
        static const char* kN[5] = {"pinky", "ring", "middle", "index", "thumb"};
        std::string line = "GRASPDUMP: weight " + std::to_string(w) + (solve ? " solve" : held ? " held, no bounds" : " fist");
        for (int f = 0; f < 5; ++f) {
            const Chain& ch = posed[f];
            float p4[4][3];
            for (int q = 0; q < 4; ++q) Wp(ch.pt[q], p4[q]);
            char b[420];
            int n = std::snprintf(b, sizeof(b), " | %s (%.1f,%.1f,%.1f)(%.1f,%.1f,%.1f)(%.1f,%.1f,%.1f)tip(%.1f,%.1f,%.1f)", kN[f], p4[0][0],
                                  p4[0][1], p4[0][2], p4[1][0], p4[1][1], p4[1][2], p4[2][0], p4[2][1], p4[2][2], p4[3][0], p4[3][1], p4[3][2]);
            if (n < 0 || n >= static_cast<int>(sizeof(b))) continue;
            if (f < 4)
                n += std::snprintf(b + n, sizeof(b) - n, " flex %.0f/%.0f/%.0f anim %.0f/%.0f/%.0f goal %.0f/%.0f/%.0f",
                                   Flex(ch, 0, lf, fref) / D, Flex(ch, 1, lf, fref) / D, Flex(ch, 2, lf, fref) / D, phi[f][0] / D,
                                   phi[f][1] / D, phi[f][2] / D, goal[f][0] / D, goal[f][1] / D, goal[f][2] / D);
            else
                n += std::snprintf(b + n, sizeof(b) - n, " swing %.0f joints %+.0f/%+.0f/%+.0f goal %.0f %+.0f/%+.0f",
                                   w * g_th[side][4][0] / D, w * g_th[side][4][1] / D, w * g_th[side][4][2] / D,
                                   w * g_th[side][4][3] / D, goal[4][0] / D, goal[4][2] / D, goal[4][3] / D);
            if (haveAct && n > 0 && n < static_cast<int>(sizeof(b))) {
                float r = f == 4 ? radT : rad;
                std::snprintf(b + n, sizeof(b) - n, " clear %.2f tip %.2f cm", ChainClear(ch, 0, 2, act, r, nullptr, 0, 0.0f) * mk * toCm,
                              (ShapeDist(act, ch.pt[3]) - r) * mk * toCm);
            }
            line += b;
        }
        vr::log::Line(line);
        const float* hp = refP;  // la mano come e' scritta (voce 5)
        float Pw[3], nw[3], aw[3];
        for (int i = 0; i < 3; ++i) {
            Pw[i] = hp[i] + g_palmLocal[side][0] * ha[0][i] + g_palmLocal[side][1] * ha[1][i] + g_palmLocal[side][2] * ha[2][i];
            nw[i] = g_palmNormalLocal[side][0] * ha[0][i] + g_palmNormalLocal[side][1] * ha[1][i] + g_palmNormalLocal[side][2] * ha[2][i];
            aw[i] = g_palmAxisLocal[side][0] * ha[0][i] + g_palmAxisLocal[side][1] * ha[1][i] + g_palmAxisLocal[side][2] * ha[2][i];
        }
        char b[700];
        std::snprintf(b, sizeof(b),
                      "GRASPDUMP: hand (%.1f,%.1f,%.1f) axes X(%.2f,%.2f,%.2f) Y(%.2f,%.2f,%.2f) Z(%.2f,%.2f,%.2f) | palm P(%.1f,%.1f,%.1f) "
                      "n(%.2f,%.2f,%.2f) W(%.2f,%.2f,%.2f) | grip (hand axes) knuckles (%.2f,%.2f,%.2f) cm f(%.3f,%.3f,%.3f) "
                      "a(%.3f,%.3f,%.3f) p(%.3f,%.3f,%.3f) span %.2f..%.2f cm",
                      hp[0], hp[1], hp[2], ha[0][0], ha[0][1], ha[0][2], ha[1][0], ha[1][1], ha[1][2], ha[2][0], ha[2][1], ha[2][2], Pw[0],
                      Pw[1], Pw[2], nw[0], nw[1], nw[2], aw[0], aw[1], aw[2], g.o[0] * toCm, g.o[1] * toCm, g.o[2] * toCm, g.f[0], g.f[1],
                      g.f[2], g.a[0], g.a[1], g.a[2], g.p[0], g.p[1], g.p[2], g.span[0] * toCm, g.span[1] * toCm);
        vr::log::Line(b);
        const Held& hd = g_held[side];
        float op[3], oa[3][3], lc[3], le[3], om[4][4], sph = 0.0f;
        if (hd.actor && CompPose(hd.comp, op, oa) && MeshLocalBounds(hd.comp, lc, le, &sph) && ReadMatrix(hd.comp, om)) {
            float s[3], cw[3], hc[3], rel[3], rp[3], ra[3][3], tc[3], A[3][3];
            for (int i = 0; i < 3; ++i) s[i] = std::sqrt(Dot3(om[i], om[i]));
            for (int i = 0; i < 3; ++i) cw[i] = op[i] + lc[0] * s[0] * oa[0][i] + lc[1] * s[1] * oa[1][i] + lc[2] * s[2] * oa[2][i];
            float d[3] = {cw[0] - hp[0], cw[1] - hp[1], cw[2] - hp[2]};
            for (int r = 0; r < 3; ++r) hc[r] = Dot3(d, ha[r]);
            for (int i = 0; i < 3; ++i)
                for (int r = 0; r < 3; ++r) A[i][r] = Dot3(oa[i], ha[r]);
            for (int i = 0; i < 3; ++i) rel[i] = hc[i] - g.o[i];
            HeldTarget(hd, ha, rp, ra);
            for (int r = 0; r < 3; ++r) tc[r] = rp[r] + lc[0] * s[0] * ra[0][r] + lc[1] * s[1] * ra[1][r] + lc[2] * s[2] * ra[2][r];
            float dtc[3] = {hc[0] - tc[0], hc[1] - tc[1], hc[2] - tc[2]};
            float tr = Dot3(A[0], ra[0]) + Dot3(A[1], ra[1]) + Dot3(A[2], ra[2]);
            int L = le[0] * s[0] >= le[1] * s[1] ? (le[0] * s[0] >= le[2] * s[2] ? 0 : 2) : (le[1] * s[1] >= le[2] * s[2] ? 1 : 2);
            int i1 = (L + 1) % 3, i2 = (L + 2) % 3;
            float q = (sph * sph - le[L] * le[L]) / (le[i1] * le[i1] + le[i2] * le[i2] + 1e-6f);
            std::snprintf(b, sizeof(b),
                          "GRASPDUMP: object origin (%.1f,%.1f,%.1f) center (%.1f,%.1f,%.1f) axes X(%.2f,%.2f,%.2f) Y(%.2f,%.2f,%.2f) "
                          "Z(%.2f,%.2f,%.2f) local ext (%.2f,%.2f,%.2f) halfThick %.1f cm grip %d | centre from knuckles f %.2f p %.2f a %.2f "
                          "cm, long axis to a %.1f deg | off target %.2f cm %.1f deg | sphere %.2f q %.2f round %.2f",
                          op[0], op[1], op[2], cw[0], cw[1], cw[2], oa[0][0], oa[0][1], oa[0][2], oa[1][0], oa[1][1], oa[1][2], oa[2][0],
                          oa[2][1], oa[2][2], le[0], le[1], le[2], hd.halfThickCm, hd.gripKind, Dot3(rel, g.f) * toCm,
                          Dot3(rel, g.p) * toCm, Dot3(rel, g.a) * toCm,
                          std::acos((std::min)(1.0f, std::fabs(Dot3(A[L], g.a)))) * 57.2958f, std::sqrt(Dot3(dtc, dtc)) * toCm,
                          std::acos((std::max)(-1.0f, (std::min)(1.0f, (tr - 1.0f) * 0.5f))) * 57.2958f, sph, q,
                          SectionRound(le, sph, L, g_tune.round));
            vr::log::Line(b);
        }
    }
}

// Comando "griptune <nome> <valore>": misure della presa al volo (cm; height
// e back frazioni, round -1 = automatico). Ritorna false se il nome non c'e'.
bool SetGripTune(const std::string& name, float v) {
    struct {
        const char* n;
        float* p;
    } k[] = {{"palm", &g_tune.palm},     {"gap", &g_tune.gap},   {"finger", &g_tune.finger}, {"thumb", &g_tune.thumb},
             {"height", &g_tune.height}, {"back", &g_tune.back}, {"round", &g_tune.round}, {"gain", &g_tune.gain},
             {"relgrace", &g_tune.relGrace}, {"minspeed", &g_tune.minSpeed}, {"pawnframe", &g_tune.pawnFrame},
             {"force", &g_tune.force}, {"torque", &g_tune.torque}, {"klin", &g_tune.kLin}, {"krot", &g_tune.kRot},
             {"sagcap", &g_tune.sagCap}, {"droopcap", &g_tune.droopCap}, {"brake", &g_tune.brake}, {"liftmin", &g_tune.liftMin},
             {"pawnacc", &g_tune.pawnAcc}, {"overshoot", &g_tune.overshoot}, {"overtime", &g_tune.overTime},
             {"alphamin", &g_tune.alphaMin}, {"fingerreach", &g_tune.fingerReach}, {"framevel", &g_tune.frameVel},
             {"breaktime", &g_tune.breakTime}, {"breakerr", &g_tune.breakErr}, {"breakratio", &g_tune.breakRatio},
             {"breakgive", &g_tune.breakGive}, {"breakturn", &g_tune.breakTurn}, {"passvolumes", &g_tune.passVolumes},
             {"wedgetime", &g_tune.wedgeTime}, {"breakkg", &g_tune.breakKg}, {"breakcap", &g_tune.breakCap},
             {"regriptime", &g_tune.regripTime}, {"regripreach", &g_tune.regripReach}, {"regripmove", &g_tune.regripMove},
             {"regripsettle", &g_tune.regripSettle}, {"stuckspeed", &g_tune.stuckSpeed}, {"regriperr", &g_tune.regripErr},
             {"regripspeed", &g_tune.regripSpeed}, {"regriprest", &g_tune.regripRest},
             {"hapdv", &g_tune.hapDv}, {"hapj", &g_tune.hapJ}, {"haprefr", &g_tune.hapRefr}, {"haprearm", &g_tune.hapRearm}, {"hapwindow", &g_tune.hapWindow},
             {"hapspherekg", &g_tune.hapSphereKg}, {"hapspeed", &g_tune.hapSpeed}, {"hapslapkg", &g_tune.hapSlapKg},
             {"hapapproach", &g_tune.hapApproach},
             {"peakwin", &g_tune.peakWin}, {"peakdt", &g_tune.peakDt}, {"peakjump", &g_tune.peakJump}, {"energy", &g_tune.throwEnergy},
             {"spinmax", &g_tune.spinMax}, {"spinjump", &g_tune.spinJump}, {"lever", &g_tune.lever},
             {"peakmin", &g_tune.peakMin}, {"peakfull", &g_tune.peakFull}, {"peakout", &g_tune.peakOut},
             {"shdead", &g_tune.shDead}, {"shturn", &g_tune.shTurn}, {"shcap", &g_tune.shCap}, {"shcapturn", &g_tune.shCapTurn},
             {"shin", &g_tune.shIn}, {"shout", &g_tune.shOut}, {"shtele", &g_tune.shTele}, {"shwall", &g_tune.shWall},
             {"shguard", &g_tune.shGuard},
             {"gripmin", &g_tune.gripMin}, {"griplo", &g_tune.gripLo}, {"griphi", &g_tune.gripHi}, {"gripturn", &g_tune.gripTurn},
             {"gripdroop", &g_tune.gripDroop}, {"griphold", &g_tune.gripHold}, {"gripease", &g_tune.gripEase},
             {"gripfloor", &g_tune.gripFloor}, {"griprise", &g_tune.gripRise}, {"gripthrow", &g_tune.gripThrow}};
    for (auto& e : k)
        if (name == e.n) {
            *e.p = v;
            char b[120];
            std::snprintf(b, sizeof(b), "GRIP: %s = %.2f (next grab)", e.n, v);
            vr::log::Line(b);
            return true;
        }
    return false;
}

namespace {

// --- Salti del riferimento delle mani (voce 2 del piano, 25/10) -------------
// Le mani stanno in center + Rz(yaw) x scostamento del controller (HandTarget
// in hooks.cpp). Un giro a scatti (snapturn 30/45), un ricentramento o uno
// spostamento di Walker che il suo moto non spiega (teletrasporto, mossa
// scriptata) sposta le mani di colpo: l'oggetto tenuto, guidato con forza
// limitata, restava indietro e spazzava l'arco (o scivolava via), e le sfere
// delle mani urtavano tutto sul percorso. Ora oggetto, sfere, storia delle mani
// e oggetto seguito dalla pipeline dei colpi fanno lo stesso moto rigido nello
// stesso tick.
// Revisione del 25/10:
// - imbardata effettiva = corpo meno riferimento della testa (SetHandFrame):
//   copertura e torretta girano corpo e riferimento insieme, le mani restano
//   ferme nel mondo e non e' un salto (prima ogni giro del gioco oltre 10 gradi
//   in un tick spostava oggetto, sfere e storia attorno alla testa);
// - il riferimento e' l'origine del tracciamento nel mondo (o), non il centro:
//   col visore il giro a scatti ruota attorno alla testa (compensatePivot);
// - il moto spiegato da Walker si sceglie tra niente, Actor.Velocity e lo
//   spostamento misurato di Walker (g_pawnHist, 30 ms), il piu' vicino:
//   Actor.Velocity ha picchi di un tick (oltre 20 m/s all'avvio della corsa,
//   poi 3,8) e lascia fuori i mezzi in moto; la soglia cresce con dt;
// - si porta solo la parte non spiegata: quella spiegata l'ha gia' fatta
//   PhysX con la velocita' data (che contiene quella di Walker); prima la si
//   contava due volte (6,6 cm a 6 m/s e 90 Hz) e la storia delle mani perdeva
//   il moto di Walker per 30 ms.
struct HandFrame {
    float c[3] = {0, 0, 0};
    float o[3] = {0, 0, 0};          // origine del tracciamento nel mondo
    int32_t posYaw = 0, axesYaw = 0;  // imbardate effettive (corpo meno riferimento della testa)
    int32_t rawYaw = 0, refYaw = 0;   // corpo e riferimento (solo per GRABLOG)
    uint32_t seq = 0;
};
HandFrame g_handFrame, g_handFrameTick;  // ultimo da UpdateArms; quello dell'ultimo tick di Tick
double g_handFrameTickAt = -1.0;         // quando g_handFrameTick e' stato usato

// Rotatore di UE3 dagli assi (righe X, Y, Z di LocalToWorld, normalizzate):
// X = (cp cy, cp sy, sp), Y.z = -sr cp, Z.z = cr cp (FRotationMatrix).
void RowsToRotator(const float a[3][3], int32_t rot[3]) {
    const float k = 65536.0f / 6.28318530718f;
    const float cp = std::sqrt(a[0][0] * a[0][0] + a[0][1] * a[0][1]);
    float pitch = std::atan2(a[0][2], cp), yaw, roll;
    if (cp > 1e-4f) {
        yaw = std::atan2(a[0][1], a[0][0]);
        roll = std::atan2(-a[1][2], a[2][2]);
    } else {
        // X verticale: rollio 0, imbardata da Y = (-sy, cy, 0)
        yaw = std::atan2(-a[1][0], a[1][1]);
        roll = 0.0f;
    }
    rot[0] = static_cast<int32_t>(std::lround(pitch * k));
    rot[1] = static_cast<int32_t>(std::lround(yaw * k));
    rot[2] = static_cast<int32_t>(std::lround(roll * k));
}

// SetRBRotation (NewRot @0, BoneName @12) e SetRBPosition (NewPos @0,
// BoneName @12): 20 byte di parametri, iNative 0 (riflessione, 25/10).
void SetRBPose(void* comp, const float pos[3], const int32_t* rot) {
    if (rot) {
        struct {
            int32_t rot[3];
            int32_t bone[2];
        } rr = {{rot[0], rot[1], rot[2]}, {0, 0}};
        vr::script::CallFunction(comp, "SetRBRotation", &rr);
    }
    struct {
        float pos[3];
        int32_t bone[2];
    } rp = {{pos[0], pos[1], pos[2]}, {0, 0}};
    vr::script::CallFunction(comp, "SetRBPosition", &rp);
}

// Trace contro il mondo soltanto (BSP, mesh del livello, terreno, mover;
// niente Pawn ne' altri oggetti): l'oggetto portato da un salto non va messo
// dentro o oltre un muro. true = qualcosa in mezzo. Le mesh piccole del
// livello (sfera dei limiti sotto 40 unita', 80 cm: bottiglie, scatole sulla
// cassa) non sono muri: oltre si ritraccia (al piu' 3 volte). Prima la
// bottiglia statica tra la mano e l'oggetto annullava il salto e l'oggetto,
// guidato lungo l'arco, la buttava giu' (snapknock2.py, 25/10).
// pawns (distacco, seguito della voce 2): anche i Pawn (TRACE_Pawns) fermano,
// qualunque sia la loro misura: un oggetto premuto contro un nemico non va
// spinto dentro la sua capsula. minWall (unita', 40 il predefinito): raggio
// dei limiti da cui una mesh del livello conta come muro (la sfera della mano
// della voce 5 usa shwall, 15: anche un tavolo o una cassa lo sono).
bool WorldBlocked(void* source, const float* a, const float* b, bool pawns, float minWall) {
    float st[3] = {a[0], a[1], a[2]};
    const float d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const float len = std::sqrt(Dot3(d, d));
    if (!(len > 1e-3f)) return false;
    const DWORD flags = 0x2 | 0x4 | 0x80 | 0x2000 | (pawns ? 0x1u : 0u);
    for (int k = 0; k < 4; ++k) {
        alignas(16) unsigned char hit[0x100] = {};
        __try {
            if (!kGWorld || !kSingleLineCheck) return false;
            void* world = *reinterpret_cast<void**>(kGWorld);
            if (!world) return false;
            const float ext[3] = {0.0f, 0.0f, 0.0f};
            reinterpret_cast<SingleLineCheckFn>(kSingleLineCheck)(world, hit, source, b, st, flags, ext, nullptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        void* hitActor = *reinterpret_cast<void**>(hit + 4);
        if (!hitActor) return false;
        if (pawns && g_yGamePawnName >= 0 && ClassIsA(U32(hitActor, kOffClass), g_yGamePawnName)) return true;
        void* comp = *reinterpret_cast<void**>(hit + 0x30);
        if (!comp) return true;  // BSP
        float rad = 1e9f;
        SafeCopy(&rad, static_cast<char*>(comp) + kOffCompBounds + 24, 4);
        if (!(rad < minWall)) return true;  // muro (o raggio illeggibile)
        if (k == 3) return false;
        // mesh piccola: oltre di 1 unita'
        const float* loc = reinterpret_cast<const float*>(hit + 8);
        const float t = ((loc[0] - a[0]) * d[0] + (loc[1] - a[1]) * d[1] + (loc[2] - a[2]) * d[2]) / (len * len) + 1.0f / len;
        if (t >= 1.0f) return false;
        for (int c = 0; c < 3; ++c) st[c] = a[c] + d[c] * t;
    }
    return false;
}

// Il salto: punti J(x) = o1 - e + Rz(dPos) (x - o0), con e la parte che il
// moto di Walker spiega; vettori che girano con gli assi delle mani: Rz(dAxes).
struct FrameXf {
    float o0[3], o1e[3];
    float cp, sp, ca, sa;
    void Pt(const float* x, float* out) const {
        const float d[3] = {x[0] - o0[0], x[1] - o0[1], x[2] - o0[2]};
        const float r[3] = {o1e[0] + d[0] * cp - d[1] * sp, o1e[1] + d[0] * sp + d[1] * cp, o1e[2] + d[2]};
        std::memcpy(out, r, sizeof(r));
    }
    void Turn(const float* v, float* out) const {
        const float r[3] = {v[0] * ca - v[1] * sa, v[0] * sa + v[1] * ca, v[2]};
        std::memcpy(out, r, sizeof(r));
    }
};

void FrameJump(const HandFrame& f0, const HandFrame& f1, float dPos, float dAxes, int32_t dAxesUnits, const float* e, float explainedCm,
               float unexplainedCm, double now) {
    FrameXf X;
    for (int c = 0; c < 3; ++c) {
        X.o0[c] = f0.o[c];
        X.o1e[c] = f1.o[c] - e[c];
    }
    X.cp = std::cos(dPos);
    X.sp = std::sin(dPos);
    X.ca = std::cos(dAxes);
    X.sa = std::sin(dAxes);
    int carried = 0, spheres = 0, blocked = 0;
    float residual[2] = {-1.0f, -1.0f};
    g_hapQuietUntil = now + 0.2;  // vibrazione degli urti: il salto non e' un urto (voce 3)
    for (int h = 0; h < 2; ++h) {
        Held& held = g_held[h];
        if (held.actor && held.weight && !held.constrained && ActorUsable(held.actor) && g_lastHandValid[h]) {
            float op[3], oa[3][3];
            if (CompPose(held.comp, op, oa)) {
                // Come il bersaglio: la mano va in J(mano), l'oggetto le resta
                // attaccato con la rotazione degli assi della mano.
                const float* hp0 = g_lastHandPos[h];
                float hp1[3];
                X.Pt(hp0, hp1);
                auto objPt = [&](const float* x, float* out) {
                    float d[3] = {x[0] - hp0[0], x[1] - hp0[1], x[2] - hp0[2]}, dd[3];
                    X.Turn(d, dd);
                    for (int c = 0; c < 3; ++c) out[c] = hp1[c] + dd[c];
                };
                float np[3], bc[3], nbc[3];
                objPt(op, np);
                if (!SafeCopy(bc, static_cast<char*>(held.comp) + kOffCompBounds, 12)) std::memcpy(bc, op, sizeof(bc));
                objPt(bc, nbc);
                // Davanti a un muro (giro a scatti verso una parete, una rete):
                // niente salto, l'oggetto resta guidato e la parete lo ferma,
                // come prima della voce 2.
                if (WorldBlocked(held.actor, hp1, nbc)) {
                    ++blocked;
                } else {
                    // Rotazione: un giro attorno a Z del mondo aggiunge solo allo
                    // yaw del rotatore. Le righe normalizzate bastano per una
                    // mesh normale; con una scala negativa una riga e' girata e
                    // il rotatore veniva 180 gradi fuori: si parte dalla
                    // Rotation dell'attore (quella del corpo, da SyncActorToRBPhysics).
                    int32_t rot[3];
                    bool rotOk = true;
                    if (Det3(oa) >= 0.0f) {
                        float na[3][3];
                        for (int i = 0; i < 3; ++i) X.Turn(oa[i], na[i]);
                        RowsToRotator(na, rot);
                    } else {
                        rotOk = SafeCopy(rot, static_cast<char*>(held.actor) + 0x60, 12);
                        rot[1] += dAxesUnits;
                    }
                    SetRBPose(held.comp, np, rotOk ? rot : nullptr);
                    void* nx = CompNx(held.comp);
                    float lin[3], angv[3];
                    if (nx && NxVelocities(nx, lin, angv)) {
                        float l2[3], a2[3];
                        for (float& c : lin) c *= 50.0f;
                        X.Turn(lin, l2);
                        X.Turn(angv, a2);
                        SetLinVel(held.comp, l2);
                        SetAngVel(held.comp, a2);
                    }
                    float t[3];
                    X.Turn(held.lastCmdV, t);
                    std::memcpy(held.lastCmdV, t, sizeof(t));
                    X.Turn(held.lastCmdW, t);
                    std::memcpy(held.lastCmdW, t, sizeof(t));
                    X.Turn(held.smashRef, t);
                    std::memcpy(held.smashRef, t, sizeof(t));
                    X.Turn(held.weightCmdV, t);
                    std::memcpy(held.weightCmdV, t, sizeof(t));
                    X.Turn(held.weightCmdW, t);
                    std::memcpy(held.weightCmdW, t, sizeof(t));
                    if (held.brkStart > 0.0) {
                        // Distacco interrotto dal salto (revisione 25/10): il
                        // comando di prima e' quello del peso, non quello gonfiato
                        // dello strappo (la base del tick dopo ne dipende).
                        std::memcpy(held.lastCmdV, held.weightCmdV, sizeof(held.lastCmdV));
                        std::memcpy(held.lastCmdW, held.weightCmdW, sizeof(held.lastCmdW));
                    }
                    held.brkStart = 0.0;  // distacco: si ricomincia nel riferimento nuovo
                    held.brkSince = -1.0;
                    held.jumpUntil = now + 0.2;  // energia delle compenetrazioni nuove ignorata (Drive)
                    held.stuckSince = -1.0;
                    // Pipeline dei colpi: lo stato del tick prima nel riferimento
                    // nuovo, se no il segmento del colpo in mano partiva dalla
                    // posizione vecchia e spazzava il salto (fino a ~46 cm).
                    for (HitTrack& tr : g_tracks) {
                        if (tr.comp != held.comp) continue;
                        objPt(tr.c, t);
                        std::memcpy(tr.c, t, sizeof(t));
                        if (tr.tipValid) {
                            objPt(tr.tip, t);
                            std::memcpy(tr.tip, t, sizeof(t));
                        }
                        if (tr.endsValid)
                            for (auto& q : tr.ends) {
                                objPt(q, t);
                                std::memcpy(q, t, sizeof(t));
                            }
                        X.Turn(tr.v, t);
                        std::memcpy(tr.v, t, sizeof(t));
                        X.Turn(tr.w, t);
                        std::memcpy(tr.w, t, sizeof(t));
                    }
                    float cp2[3], ca2[3][3];
                    if (CompPose(held.comp, cp2, ca2))  // SetRBPosition vale subito?
                        residual[h] = std::sqrt((cp2[0] - np[0]) * (cp2[0] - np[0]) + (cp2[1] - np[1]) * (cp2[1] - np[1]) +
                                                (cp2[2] - np[2]) * (cp2[2] - np[2])) / vr::control::g_posScale * 100.0f;
                    ++carried;
                }
            }
        }
        // Polso: gli assi del tick prima girano con la mano (niente velocita'
        // angolare finta di 100 rad/s nel tick del giro).
        float t[3];
        for (int i = 0; i < 3; ++i) {
            X.Turn(g_wristPrevHa[h][i], t);
            std::memcpy(g_wristPrevHa[h][i], t, sizeof(t));
            X.Turn(g_prevTa[h][i], t);
            std::memcpy(g_prevTa[h][i], t, sizeof(t));
        }
        X.Turn(g_wristW[h], t);
        std::memcpy(g_wristW[h], t, sizeof(t));
        X.Turn(g_heldHandW[h], t);
        std::memcpy(g_heldHandW[h], t, sizeof(t));
        // Sfera della mano: stesso moto, cosi' non spazza gli oggetti sull'arco
        // (non quelle parcheggiate sotto Walker).
        HandProxy& pr = g_proxy[h];
        float sp3[3], sa3[3][3];
        if (pr.actor && ActorUsable(pr.actor) && CompPose(pr.comp, sp3, sa3)) {
            const float dc[3] = {sp3[0] - f0.c[0], sp3[1] - f0.c[1], sp3[2] - f0.c[2]};
            if (Dot3(dc, dc) < 300.0f * 300.0f) {
                float np[3];
                X.Pt(sp3, np);
                SetRBPose(pr.comp, np, nullptr);
                pr.stuckSince = -1.0;
                ++spheres;
            }
        }
        // Storia delle mani: la velocita' della mano resta quella vera (lanci,
        // colpi in mano, sfere), Walker compreso, senza il salto.
        HandHist& hh = g_hist[h];
        for (int i = 0; i < hh.n; ++i) {
            const int k = HistIdx(hh, i);
            X.Pt(hh.pos[k], t);
            std::memcpy(hh.pos[k], t, sizeof(t));
            for (auto& a : hh.ax[k]) {  // assi (polso del lancio): stessa rotazione
                X.Turn(a, t);
                std::memcpy(a, t, sizeof(t));
            }
            if (hh.fok[k]) {  // centro del riferimento: stesso moto, la mano rispetto a lui solo girata
                X.Pt(hh.fc[k], t);
                std::memcpy(hh.fc[k], t, sizeof(t));
            }
        }
    }
    static int logged = 0;
    if (logged < 20 || vr::control::g_grabLog > 0) {
        ++logged;
        char b[360];
        std::snprintf(b, sizeof(b),
                      "GRAB: frame jump (hands turned %.1f deg, axes %.1f deg, moved %.1f cm beyond Walker's motion, %.1f cm explained by it): "
                      "%d held object(s) and %d hand sphere(s) carried, %d blocked by a wall, pose residual %.1f / %.1f cm",
                      dPos * 57.2958f, dAxes * 57.2958f, unexplainedCm, explainedCm, carried, spheres, blocked, residual[0], residual[1]);
        vr::log::Line(b);
    }
}

// Controllo del salto, una volta per tick di gioco prima della storia delle mani.
void CheckFrameJump(void* pawn, float dt, double now) {
    if (g_handFrame.seq == g_handFrameTick.seq) return;  // UpdateArms non ha girato in questo tick
    const HandFrame f0 = g_handFrameTick, f1 = g_handFrame;
    const double prevAt = g_handFrameTickAt;
    g_handFrameTick = g_handFrame;
    g_handFrameTickAt = now;
    // Velocita' del centro dei tick prima (moto di Walker atteso), poi il centro di adesso nella storia.
    float pf[3] = {0, 0, 0};
    const bool havePf = f0.seq && now - prevAt <= 0.1 && HistVelocity(g_frameHist, 0.03, pf) && std::isfinite(pf[0] + pf[1] + pf[2]);
    if (!f0.seq || prevAt < 0.0 || now - prevAt > 0.1) g_frameHist = HandHist();  // storia vecchia: da capo
    {
        HandHist& fh = g_frameHist;
        HistPush(fh, f1.c, now);
    }
    if (f0.seq && prevAt >= 0.0 && now - prevAt <= 0.1) {
        // Vibrazione degli urti (voce 3): un giro a scatti (o del riferimento)
        // oltre 10 gradi in un tick muta gli impulsi per 0,2 s, anche con il
        // peso spento (senza FrameJump: l'oggetto e la sfera inseguono l'arco).
        const float kr = 6.28318530718f / 65536.0f, lim = 10.0f * 0.0174533f;
        if (std::fabs(static_cast<int16_t>(f1.posYaw - f0.posYaw) * kr) > lim || std::fabs(static_cast<int16_t>(f1.axesYaw - f0.axesYaw) * kr) > lim)
            g_hapQuietUntil = now + 0.2;
    }
    if (!f0.seq || vr::control::g_objectWeight.load() == 0) return;  // spento: il percorso di prima, identico
    // Riferimento di piu' di 0,1 s fa (braccia spente, scena): niente confronto.
    if (prevAt < 0.0 || now - prevAt > 0.1) return;
    const float k = 6.28318530718f / 65536.0f, s = vr::control::g_posScale;
    const int32_t dAxesU = static_cast<int16_t>(f1.axesYaw - f0.axesYaw);
    const float dPos = static_cast<int16_t>(f1.posYaw - f0.posYaw) * k, dAxes = dAxesU * k;
    // Punto di misura: la mano che tiene qualcosa (sinistra prima), una mano
    // qualsiasi o il centro.
    int ph = -1;
    for (int h = 1; h >= 0; --h)
        if (ph < 0 && g_lastHandValid[h] && g_held[h].actor) ph = h;
    for (int h = 1; h >= 0; --h)
        if (ph < 0 && g_lastHandValid[h]) ph = h;
    const float* P = ph >= 0 ? g_lastHandPos[ph] : f0.c;
    const float cp = std::cos(dPos), sp = std::sin(dPos);
    const float r[3] = {P[0] - f0.o[0], P[1] - f0.o[1], P[2] - f0.o[2]};
    const float d[3] = {f1.o[0] + r[0] * cp - r[1] * sp - P[0], f1.o[1] + r[0] * sp + r[1] * cp - P[1], f1.o[2] + r[2] - P[2]};
    // Moto di Walker: niente, Actor.Velocity o lo spostamento misurato (al
    // piu' 20 m/s), quello che spiega meglio d.
    const float lim = 20.0f * s;
    float pv[3] = {0, 0, 0}, pm[3] = {0, 0, 0};
    if (!SafeCopy(pv, static_cast<char*>(pawn) + 0x140, 12) || !std::isfinite(pv[0] + pv[1] + pv[2])) pv[0] = pv[1] = pv[2] = 0.0f;
    if (!HistVelocity(g_pawnHist, 0.03, pm) || !std::isfinite(pm[0] + pm[1] + pm[2])) pm[0] = pm[1] = pm[2] = 0.0f;
    if (!havePf) pf[0] = pf[1] = pf[2] = 0.0f;
    for (float* v : {pv, pm, pf}) {
        const float l = std::sqrt(Dot3(v, v));
        if (l > lim)
            for (int c = 0; c < 3; ++c) v[c] *= lim / l;
    }
    const float zero[3] = {0, 0, 0};
    const float* cand[4] = {zero, pv, pm, pf};
    float e[3] = {0, 0, 0}, best = -1.0f;
    for (const float* v : cand) {
        const float u[3] = {d[0] - v[0] * dt, d[1] - v[1] * dt, d[2] - v[2] * dt};
        const float ul = std::sqrt(Dot3(u, u));
        if (best < 0.0f || ul < best) {
            best = ul;
            for (int c = 0; c < 3; ++c) e[c] = v[c] * dt;
        }
    }
    const float thr = (std::max)(0.10f * s, (std::max)(std::sqrt(Dot3(pv, pv)), (std::max)(std::sqrt(Dot3(pm, pm)), std::sqrt(Dot3(pf, pf)))) * dt);
    const float kJumpRad = 10.0f * 0.0174533f;
    if (vr::control::g_grabLog > 0) {
        char b[300];
        std::snprintf(b, sizeof(b),
                      "GRABFRAME: c (%.1f, %.1f, %.1f) o (%.1f, %.1f, %.1f) yaw %d/%d (body %d, reference %d) moved %.1f cm, unexplained %.1f cm (limit %.1f), "
                      "pawn v %.2f m/s, measured %.2f m/s, dt %.4f",
                      f1.c[0], f1.c[1], f1.c[2], f1.o[0], f1.o[1], f1.o[2], f1.posYaw, f1.axesYaw, f1.rawYaw, f1.refYaw, std::sqrt(Dot3(d, d)) / s * 100.0f,
                      best / s * 100.0f, thr / s * 100.0f, std::sqrt(Dot3(pv, pv)) / s, std::sqrt(Dot3(pm, pm)) / s, dt);
        vr::log::Line(b);
    }
    if (std::fabs(dPos) > kJumpRad || std::fabs(dAxes) > kJumpRad || best > thr) {
        FrameJump(f0, f1, dPos, dAxes, dAxesU, e, std::sqrt(Dot3(e, e)) / s * 100.0f, best / s * 100.0f, now);
        // Storia del centro: lo spostamento non spiegato del centro non e' una
        // velocita' di Walker (FrameVelocity); l'ultimo campione e' gia' nuovo.
        HandHist& fh = g_frameHist;
        const float uc[3] = {f1.c[0] - f0.c[0] - e[0], f1.c[1] - f0.c[1] - e[1], f1.c[2] - f0.c[2] - e[2]};
        for (int i = 1; i < fh.n; ++i) {
            const int kk = HistIdx(fh, i);
            for (int c = 0; c < 3; ++c) fh.pos[kk][c] += uc[c];
        }
    }
}

// --- Bombole esplosive da lanciare (voce 5 del piano dei combattimenti, 26/10) ---
// Gli esplosivi di Spec Ops sono solo YExplosiveActor (SRAssets:
// YExpl_PropaneBottle, YExpl_ScubaTank, YExpl_SmallGasBottleGroup...): attori
// del livello con bNoDelete, bWorldGeometry e bMovable spento. Il loro script
// (bytecode letto dalla memoria, f5/udis.py):
// - TakeDamage: m_currentHealth -= danno; il primo Controller che la colpisce
//   resta in m_firstControllerWhoHitMe; a vita <= 0 ServerExplode, subito se
//   m_numHits > 0, altrimenti fra 0,15 s (SetTimer); se resta viva m_numHits++
//   e al primo colpo notifyAI (SendEvent_GrenadeImpact a Location: avviso all'IA).
// - Tick: a m_numHits > 0 la vita cala di m_healthReductionPerSecondAndHit
//   al secondo (0 nella bombola: niente bombola che brucia, a differenza di HL2).
// - ServerExplode: m_exploded, Explode (particelle m_explPSC, suono, decalco,
//   nebbia di sabbia, mesh distrutta), poi il danno del gioco
//   (YDamageManager.AddDamageItem con YDamageEvaluationParams
//   m_damageParamsName) centrato sui limiti di m_statMeshComp (Bounds.Origin),
//   non su Location; solo se m_firstControllerWhoHitMe ha un Pawn con un'arma.
// Il portatore (CarrierCreate): KActorSpawnable con mesh, scala e materiali
// dell'originale nella sua posa; l'originale si nasconde senza collisione ne'
// corpo PhysX. Ogni tick la Location dello YExpl segue il portatore (la legge
// notifyAI); prima di un danno anche i componenti (ForceUpdateComponents:
// limiti, particelle, suono, mesh distrutta), poi il TakeDamage del gioco fa
// tutta la sua esplosione li'. Esplode con: un colpo (di Walker o di un nemico)
// col danno dell'arma, come l'originale (50 HP: l'M4 da 30 la intacca, il
// secondo colpo la fa saltare); un urto con un cambio di velocita' oltre
// 12 m/s in 50 ms (una caduta da circa 3 m: 2,5 m danno 10,9). Il piano diceva 8 m/s: misurato
// (26/10), la bombola lasciata cadere dalla mano alzata sopra la testa
// arrivava a terra con 8,4 m/s di cambio (7,5 di caduta piu' il rimbalzo) ed
// esplodeva in faccia a Walker; lanciata non supera 3,5 m/s (13,7 kg al limite
// di energia di 85 J), quindi l'urto conta solo per le cadute dall'alto
// (gettata da un tetto sui nemici). Il portatore sparisce
// (KActorSpawnable.RecycleInternal).
constexpr float kExplImpactDv = 12.0f;         // m/s: cambio di velocita' in 50 ms che la fa esplodere

Carrier* CarrierOfActor(const void* actor) {
    if (!actor) return nullptr;
    for (Carrier& c : g_carriers)
        if (c.actor == actor) return &c;
    return nullptr;
}

bool ObjAlive(void* obj, int32_t name) { return obj && vr::script::LiveObject(obj) && static_cast<int32_t>(U32(obj, kOffName)) == name; }
ObjId ObjIdOf(void* obj) {
    ObjId id;
    if (obj) {
        id.index = static_cast<int32_t>(U32(obj, kOffName));
        id.number = static_cast<int32_t>(U32(obj, kOffName + 4));
    }
    return id;
}
bool ObjAlive(void* obj, const ObjId& id) {
    if (!obj || !vr::script::LiveObject(obj)) return false;
    const ObjId now = ObjIdOf(obj);
    return now.index == id.index && now.number == id.number;
}
// L'originale nascosto e' ancora il m_statMeshComp della bombola? Solo allora
// si tocca (visibile, collisione): un puntatore vecchio puo' essere un altro
// componente vivo nello stesso blocco.
bool CarrierOrigOk(const Carrier& c) {
    return c.orig && U32(c.expl, kOffExplSmc) == static_cast<uint32_t>(reinterpret_cast<uintptr_t>(c.orig)) && vr::script::LiveObject(c.orig);
}

void CompSetCollision(void* comp, bool collide, bool block, bool always) {
    struct {
        uint32_t c, b, a;
    } sc = {collide ? 1u : 0u, block ? 1u : 0u, always ? 1u : 0u};
    vr::script::CallFunction(comp, "SetActorCollision", &sc);
}

void CompSetBool(void* comp, const char* fn, bool v) {
    struct {
        uint32_t b;
    } p = {v ? 1u : 0u};
    vr::script::CallFunction(comp, fn, &p);
}

// Centro dei limiti del componente (unita').
bool CompCenter(void* comp, float c[3]) { return SafeCopy(c, static_cast<char*>(comp) + kOffCompBounds, 12) && std::isfinite(c[0] + c[1] + c[2]); }

// Location dello YExpl dove sta il portatore (centro della mesh sul centro del
// portatore); components = anche i componenti (limiti per il danno,
// particelle, suono, mesh distrutta), solo prima di un danno.
bool CarrierSync(Carrier& c, bool components) {
    float cc[3];
    if (!CompCenter(c.comp, cc)) return false;
    const float loc[3] = {cc[0] - c.centerOff[0], cc[1] - c.centerOff[1], cc[2] - c.centerOff[2]};
    if (!SafeCopy(static_cast<char*>(c.expl) + kOffActorLocation, loc, 12)) return false;
    if (components) {
        struct {
            uint32_t collision, transformOnly;
        } fu = {0, 1};
        vr::script::CallFunction(c.expl, "ForceUpdateComponents", &fu);
        c.moved = true;
        std::memcpy(c.syncC, cc, sizeof(cc));
    }
    return true;
}

// Distanza (unita') del portatore dall'ultimo spostamento dei componenti; un
// valore enorme se non sono mai stati spostati.
float CarrierSyncGap(const Carrier& c) {
    float cc[3];
    if (!c.moved || !CompCenter(c.comp, cc)) return 1e9f;
    const float d[3] = {cc[0] - c.syncC[0], cc[1] - c.syncC[1], cc[2] - c.syncC[2]};
    return std::sqrt(Dot3(d, d));
}

// Il portatore se ne va (esplosione, bombola sparita): via dalle mani, dal
// guanto e dalle liste; il gioco lo nasconde, ferma e toglie dalle collisioni
// (RecycleInternal: SetHidden, SetPhysics(None), SetCollision(false, false)).
// Le voci di g_falling scadono da sole (ActorUsable: niente piu' corpo rigido).
void CarrierDrop(Carrier& c, const char* why) {
    for (int h = 0; h < 2; ++h) {
        if (g_held[h].actor == c.actor) Release(h, why);
        if (g_glove[h].actor == c.actor && g_glove[h].state != kGloveIdle) GloveReset(h, why);
    }
    g_props.erase(std::remove_if(g_props.begin(), g_props.end(), [&](const Prop& p) { return p.comp == c.comp; }), g_props.end());
    ProxyForget(c.comp);
    if (ObjAlive(c.actor, c.actorId)) {
        uint32_t none[2] = {0, 0};
        vr::script::CallFunction(c.actor, "RecycleInternal", none);
    }
}

// La bombola viva e intera torna com'era al suo posto: Location, componenti,
// e l'originale (solo se e' ancora il suo m_statMeshComp) visibile e solido.
void CarrierPutBack(Carrier& c, const char* why) {
    if (!ObjAlive(c.expl, c.explId) || (U32(c.expl, kOffExplFlags) & kExplExploded)) return;
    SafeCopy(static_cast<char*>(c.expl) + kOffActorLocation, c.home, 12);
    if (c.moved) {
        struct {
            uint32_t collision, transformOnly;
        } fu = {0, 1};
        vr::script::CallFunction(c.expl, "ForceUpdateComponents", &fu);
    }
    const bool origOk = CarrierOrigOk(c);
    if (origOk) {
        CompSetBool(c.orig, "SetHidden", false);
        CompSetCollision(c.orig, (c.origColl & 0x1) != 0, (c.origColl & 0x4) != 0, (c.origColl & 0x2) != 0);
        CompSetBool(c.orig, "SetBlockRigidBody", (c.origColl & 0x40) != 0);
    }
    vr::log::Line(std::string("PROPS: explosive ") + vr::script::ObjectName(c.expl) + " back at its spot (" + why + ")" +
                  (origOk ? "" : "; its mesh component changed, left as it is"));
}

// Portatore perso senza esplosione: la bombola torna al suo posto.
void CarrierRestore(Carrier& c, const char* why) {
    CarrierPutBack(c, why);
    CarrierDrop(c, why);
}

void* CarrierCreate(const Prop& p, HitSource source, int hand, void** compOut) {
    void* expl = p.actor;
    void* orig = p.comp;
    for (Carrier& c : g_carriers)
        if (c.expl == expl) {
            // Gia' portata (l'altra mano, il guanto): lo stesso portatore.
            if (!ObjAlive(c.actor, c.actorId) || !ActorUsable(c.actor)) return nullptr;
            if (compOut) *compOut = c.comp;
            return c.actor;
        }
    const uint32_t origU = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(orig));
    if (!vr::script::LiveObject(expl) || U32(expl, kOffExplSmc) != origU || (U32(expl, kOffExplFlags) & kExplExploded) ||
        !(U32(orig, kOffCompAttached) & 1))
        return nullptr;
    void* mesh = reinterpret_cast<void*>(static_cast<uintptr_t>(U32(orig, kOffSmcStaticMesh)));
    void* cls = KActorSpawnableClass();
    float M[4][4], home[3], oc[3];
    if (!mesh || !cls || !g_tickPawn || !ReadMatrix(orig, M) || !SafeCopy(home, static_cast<char*>(expl) + kOffActorLocation, 12) ||
        !CompCenter(orig, oc))
        return nullptr;
    float ax[3][3], sc[3];
    for (int r = 0; r < 3; ++r) {
        sc[r] = std::sqrt(M[r][0] * M[r][0] + M[r][1] * M[r][1] + M[r][2] * M[r][2]);
        if (!(sc[r] > 1e-4f) || !std::isfinite(sc[r])) return nullptr;
        for (int k = 0; k < 3; ++k) ax[r][k] = M[r][k] / sc[r];
    }
    int32_t rot[3];
    RowsToRotator(ax, rot);
    // L'originale sparisce prima: il corpo del portatore nascerebbe dentro il
    // suo (corpo PhysX fisso: BlockRigidBody spento, e niente trace).
    const uint32_t coll = U32(orig, kOffCompCollision);
    CompSetBool(orig, "SetHidden", true);
    CompSetCollision(orig, false, false, false);
    CompSetBool(orig, "SetBlockRigidBody", false);
    struct {
        void* cls;
        void* owner;
        int32_t tag[2];
        float loc[3];
        int32_t rot[3];
        void* tmpl;
        uint32_t noFail;
        void* ret;
    } sp = {cls, nullptr, {0, 0}, {M[3][0], M[3][1], M[3][2]}, {rot[0], rot[1], rot[2]}, nullptr, 1, nullptr};
    void* smc = nullptr;
    if (vr::script::CallFunction(g_tickPawn, "Spawn", &sp) && sp.ret)
        smc = reinterpret_cast<void*>(static_cast<uintptr_t>(U32(sp.ret, kOffKActorSmc)));
    bool ok = smc != nullptr;
    if (ok && (std::fabs(sc[0] - 1.0f) > 1e-3f || std::fabs(sc[1] - 1.0f) > 1e-3f || std::fabs(sc[2] - 1.0f) > 1e-3f)) {
        // Prima della mesh: il corpo nasce gia' scalato (come spawnprop).
        struct {
            float s[3];
        } s3 = {{sc[0], sc[1], sc[2]}};
        vr::script::CallFunction(smc, "SetScale3D", &s3);
    }
    if (ok) {
        struct {
            void* mesh;
            uint32_t force;
            uint32_t ret;
        } sm = {mesh, 1, 0};
        ok = vr::script::CallFunction(smc, "SetStaticMesh", &sm) && U32(smc, kOffCompBodyInstance);
    }
    if (!ok) {
        // Niente portatore: l'originale torna com'era (e resta fuori dalla lista).
        if (sp.ret) {
            uint32_t none[2] = {0, 0};
            vr::script::CallFunction(sp.ret, "RecycleInternal", none);
        }
        CompSetBool(orig, "SetHidden", false);
        CompSetCollision(orig, (coll & 0x1) != 0, (coll & 0x4) != 0, (coll & 0x2) != 0);
        CompSetBool(orig, "SetBlockRigidBody", (coll & 0x40) != 0);
        g_props.erase(std::remove_if(g_props.begin(), g_props.end(), [&](const Prop& q) { return q.comp == orig; }), g_props.end());
        vr::log::Line("PROPS: explosive " + vr::script::ObjectName(expl) + ": no carrier (" + (sp.ret ? "SetStaticMesh failed" : "Spawn failed") +
                      "), left in place");
        return nullptr;
    }
    // Materiali dell'istanza (MeshComponent.Materials +0x200), se il livello li cambia.
    uint32_t matData = 0, matNum = 0;
    if (TrySafeReadUint32(static_cast<char*>(orig) + 0x200, &matData) && TrySafeReadUint32(static_cast<char*>(orig) + 0x204, &matNum) && matData &&
        matNum < 16)
        for (uint32_t i = 0; i < matNum; ++i) {
            const uint32_t mat = U32(reinterpret_cast<void*>(matData), 4 * i);
            if (!mat) continue;
            struct {
                int32_t index;
                void* mat;
            } smat = {static_cast<int32_t>(i), reinterpret_cast<void*>(static_cast<uintptr_t>(mat))};
            vr::script::CallFunction(smc, "SetMaterial", &smat);
        }
    Wake(smc);
    Carrier c;
    c.expl = expl;
    c.orig = orig;
    c.actor = sp.ret;
    c.comp = smc;
    c.explId = ObjIdOf(expl);
    c.actorId = ObjIdOf(sp.ret);
    std::memcpy(c.home, home, sizeof(home));
    for (int k = 0; k < 3; ++k) c.centerOff[k] = oc[k] - home[k];
    c.origColl = coll;
    c.since = ProfMs() * 0.001;
    g_carriers.push_back(c);
    // L'originale nascosto esce subito dalle liste (lo Scan non lo vedra' piu').
    g_props.erase(std::remove_if(g_props.begin(), g_props.end(), [&](const Prop& q) { return q.comp == orig; }), g_props.end());
    g_noBody.erase(std::remove_if(g_noBody.begin(), g_noBody.end(), [&](const NoBody& nb) { return nb.comp == orig; }), g_noBody.end());
    SupportsAround(smc, 0.0f, 8);  // non cade dal mobile senza collisione su cui stava
    FallingAdd({sp.ret, smc, ProfMs() * 0.001 + 4.0, ProfMs() * 0.001 + 0.5, 1.0f * vr::control::g_posScale}, source, hand);
    float health = 0.0f;
    SafeCopy(&health, static_cast<char*>(expl) + kOffExplHealth, 4);
    float cc[3] = {0, 0, 0};
    CompCenter(smc, cc);
    char b[360];
    std::snprintf(b, sizeof(b),
                  "PROPS: explosive %s %s (%s, health %.0f) now carried by %s: mass %.3f units, scale %.2f %.2f %.2f, centre %.1f cm from the "
                  "original's, %u materials copied; original hidden, no collision (flags 0x%X -> 0x%X)",
                  vr::script::ClassName(expl).c_str(), vr::script::ObjectName(expl).c_str(), vr::script::ObjectName(mesh).c_str(), health,
                  vr::script::ObjectName(sp.ret).c_str(), BodyMass(smc), sc[0], sc[1], sc[2],
                  std::sqrt((cc[0] - oc[0]) * (cc[0] - oc[0]) + (cc[1] - oc[1]) * (cc[1] - oc[1]) + (cc[2] - oc[2]) * (cc[2] - oc[2])) /
                      vr::control::g_posScale * 100.0f,
                  matNum < 16 ? matNum : 0u, coll, U32(orig, kOffCompCollision));
    vr::log::Line(b);
    if (compOut) *compOut = smc;
    return sp.ret;
}

// Danno alla YExplosiveActor dove sta il portatore. damage <= 0: quanto basta
// per farla esplodere (vita + 1). Se il danno la fa esplodere, il portatore
// sparisce prima (le trace del danno del gioco partono dal centro della mesh,
// dove sta lui) e m_numHits va almeno a 1: ServerExplode subito, non fra
// 0,15 s con la bombola gia' sparita dalla mano. true = esplosa.
bool CarrierDamage(Carrier& c, float damage, void* ctrl, void* dmgType, const float* momentum, const char* why) {
    if (!ObjAlive(c.expl, c.explId) || !ObjAlive(c.actor, c.actorId)) return false;
    float health = 0.0f;
    SafeCopy(&health, static_cast<char*>(c.expl) + kOffExplHealth, 4);
    if (!std::isfinite(health)) health = 0.0f;
    const int32_t dmg = damage > 0.0f ? static_cast<int32_t>(std::lround(damage)) : static_cast<int32_t>(std::ceil((std::max)(0.0f, health))) + 1;
    const bool kills = static_cast<float>(dmg) >= health;
    float cc[3] = {0, 0, 0};
    CompCenter(c.comp, cc);
    float after[3] = {0, 0, 0};
    if (!CarrierSync(c, true)) return false;
    CompCenter(c.orig, after);
    if (kills) {
        CarrierDrop(c, "exploded");  // mani e guanto lo lasciano; il gioco lo nasconde e gli toglie la collisione
        int32_t hits = static_cast<int32_t>(U32(c.expl, kOffExplNumHits));
        if (hits < 1) {
            hits = 1;
            SafeCopy(static_cast<char*>(c.expl) + kOffExplNumHits, &hits, 4);
        }
    }
    static void* explDmg = nullptr;
    struct {
        int32_t damage;
        void* instigator;
        float loc[3];
        float momentum[3];
        void* damageType;
        uint8_t hitInfo[0x1C];
        void* causer;
        uint8_t spare[0x40];
    } td = {};
    td.damage = dmg;
    td.instigator = ctrl;
    std::memcpy(td.loc, cc, sizeof(cc));
    if (momentum) std::memcpy(td.momentum, momentum, sizeof(td.momentum));
    td.damageType = dmgType ? dmgType : ClassByName("YDamageType_Explosive", &explDmg);
    const int32_t noItem = -1;
    std::memcpy(td.hitInfo + 8, &noItem, 4);   // Item
    std::memcpy(td.hitInfo + 12, &noItem, 4);  // LevelIndex
    const bool called = vr::script::CallFunction(c.expl, "TakeDamage", &td);
    const bool exploded = (U32(c.expl, kOffExplFlags) & kExplExploded) != 0;
    float hAfter = 0.0f;
    SafeCopy(&hAfter, static_cast<char*>(c.expl) + kOffExplHealth, 4);
    const float s = vr::control::g_posScale;
    const float moved = std::sqrt((after[0] - c.home[0] - c.centerOff[0]) * (after[0] - c.home[0] - c.centerOff[0]) +
                                  (after[1] - c.home[1] - c.centerOff[1]) * (after[1] - c.home[1] - c.centerOff[1]) +
                                  (after[2] - c.home[2] - c.centerOff[2]) * (after[2] - c.home[2] - c.centerOff[2]));
    const float off = std::sqrt((after[0] - cc[0]) * (after[0] - cc[0]) + (after[1] - cc[1]) * (after[1] - cc[1]) + (after[2] - cc[2]) * (after[2] - cc[2]));
    char b[480];
    std::snprintf(b, sizeof(b),
                  "PROPS: explosive %s hit (%s): %d damage (%s) by %s, health %.0f -> %.0f%s%s; moved %.1f m from its spot, mesh centre %.1f cm from "
                  "the carrier (%.0f, %.0f, %.0f)%s",
                  vr::script::ObjectName(c.expl).c_str(), why, dmg, td.damageType ? vr::script::ObjectName(td.damageType).c_str() : "no type",
                  ctrl ? vr::script::ClassName(ctrl).c_str() : "nobody", health, hAfter,
                  exploded ? ", EXPLODED" : (kills ? ", NOT exploded" : ", still whole"), called ? "" : " (TakeDamage not called)", moved / s,
                  off / s * 100.0f, cc[0], cc[1], cc[2],
                  off > 2.0f ? " (components did NOT follow)" : "");
    vr::log::Line(b);
    if (exploded) {
        if (!kills) CarrierDrop(c, "exploded");
        for (size_t i = 0; i < g_carriers.size(); ++i)
            if (&g_carriers[i] == &c) {
                g_carriers.erase(g_carriers.begin() + i);
                break;
            }
    }
    return exploded;
}

// Colpo su una bombola portata (Walker da OnShot, un nemico da OnOtherShot):
// il danno dell'arma, come sull'originale. true = era un portatore.
bool CarrierShot(const ShotEvent& e, void* walker) {
    Carrier* c = CarrierOfActor(e.actor);
    if (!c) return false;
    // Anche con propexplosive spento: la bombola gia' portata resta esplosiva (l'opzione decide solo le prese nuove).
    void* ctrl = e.ctrl;
    if (!ctrl && walker) ctrl = reinterpret_cast<void*>(static_cast<uintptr_t>(U32(walker, 0x214)));
    const float mom[3] = {e.dir[0] * 1000.0f, e.dir[1] * 1000.0f, e.dir[2] * 1000.0f};
    // Senza il danno dell'arma (non letto): un colpo da 25, meta' bombola.
    // Tipo di danno: quello dell'arma (YWeapon.m_damageType), come per la
    // bombola colpita a casa sua; senza, Explosive. Il danno resta quello
    // grezzo dell'arma: niente calo con la distanza ne' scale per IA o
    // difficolta' del ciclo dei colpi del gioco (approssimazione).
    CarrierDamage(*c, e.damage > 0.0f ? e.damage : 25.0f, ctrl, e.dmgType, mom, e.why ? e.why : (e.other ? "enemy shot" : "Walker's shot"));
    return true;
}

// Ogni tick: Location dello YExpl sul portatore; portatore perso -> la
// bombola torna al suo posto; YExpl esploso da se' (il danno di un'altra
// esplosione, il timer di 0,15 s) -> il portatore sparisce. Componenti dello
// YExpl (limiti per il centro del danno, particelle, suono, detriti) sul
// portatore anche fuori da CarrierDamage (revisione della voce 5: prima una
// volta sola, e una bombola colpita e poi portata via esplodeva dov'era al
// primo colpo):
// - vita a 0 senza esplosione ancora (timer di 0,15 s in corso): appena il
//   portatore si sposta di 2 cm;
// - gia' colpita (m_numHits > 0: il prossimo danno letale del gioco, per
//   esempio l'esplosione di un'altra bombola, la fa esplodere subito, senza
//   passare dalla mod): ogni 20 cm di spostamento.
void CarrierTick() {
    for (size_t i = g_carriers.size(); i-- > 0;) {
        Carrier& c = g_carriers[i];
        const bool explOk = ObjAlive(c.expl, c.explId) && !(U32(c.expl, kOffActorFlags9C) & kDeleteMe);
        const bool carOk = ObjAlive(c.actor, c.actorId) && ActorUsable(c.actor);
        if (!explOk) {
            CarrierDrop(c, "explosive gone");
            vr::log::Line("PROPS: explosive carrier dropped: its YExplosiveActor is gone");
        } else if (U32(c.expl, kOffExplFlags) & kExplExploded) {
            vr::log::Line("PROPS: explosive " + vr::script::ObjectName(c.expl) + " exploded on its own (the game's damage): carrier removed");
            CarrierDrop(c, "exploded");
        } else if (!carOk) {
            CarrierRestore(c, "carrier lost");
        } else {
            float health = 1.0f;
            SafeCopy(&health, static_cast<char*>(c.expl) + kOffExplHealth, 4);
            const int32_t hits = static_cast<int32_t>(U32(c.expl, kOffExplNumHits));
            const float gap = health <= 0.0f || hits > 0 ? CarrierSyncGap(c) : 0.0f;
            CarrierSync(c, (health <= 0.0f && gap > 1.0f) || (hits > 0 && gap > 10.0f));
            continue;
        }
        g_carriers.erase(g_carriers.begin() + i);
    }
}

// Walker nuovo (livello caricato, checkpoint): le bombole ancora vive tornano
// al loro posto e i portatori vivi spariscono; mani e guanti li azzera chi
// chiama (puntatori vecchi: niente Release). Gli oggetti di un livello
// scaricato non sono piu' in GObjects (ObjAlive) e restano fuori. Solo per un
// Walker davvero nuovo (TickImpl, lastWalker): dopo una scena o una
// telecamera che non e' Walker le bombole restano dove sono.
void CarrierReset() {
    if (!g_carriers.empty())
        vr::log::Line("PROPS: new Walker (level or checkpoint): " + std::to_string(g_carriers.size()) + " explosive carrier(s) reset");
    for (Carrier& c : g_carriers) {
        CarrierPutBack(c, "new Walker: level or checkpoint");
        if (ObjAlive(c.actor, c.actorId)) {
            uint32_t none[2] = {0, 0};
            vr::script::CallFunction(c.actor, "RecycleInternal", none);
        }
    }
    g_carriers.clear();
}

// --- Armi dei nemici raccolte con la mano (voce 6 dei combattimenti, 27/10) ---
// Le armi lasciate dai nemici morti (YArsenalManager.ServerDropWeapon ->
// CreatePickupActor -> TossWeapon) sono YPickup_WeaponSpawnable presi dal pool
// del gioco (YOP): m_pickupMesh e' una SkeletalMeshComponent con il
// PhysicsAsset dell'arma, un corpo solo (AK47 WP_AK: AR_Root; ossa Magazine,
// Slide, Muzzle; canna lungo +X, carrello in +Z, caricatore in -Z; la radice e'
// il punto con cui l'arma si attacca a Weapon_Right). Ferma a terra il gioco la
// addormenta in ~0,4 s (OnSleepRBPhysics -> DoSleepActivation: PHYS_None,
// SetHasPhysicsAssetInstance(false), m_physicsDisabled);
// ConditionalReactivatePhysics la rimette fisica (misurato: PhysicsAssetInstance
// di nuovo nel tick stesso). La posa del corpo e' quella dell'osso radice: la
// componente resta con la rotazione dell'attore (zero) e l'origine sulla
// radice, le ossa (SpaceBases, spazio della componente) portano la rotazione.
// - Presa (WeaponTryGrab): grip chiuso con il palmo sinistro entro grabzone dai
//   limiti dell'arma. Se Walker ha gia' un'arma di quella classe
//   (YArsenalManager.FindWeaponInAresenal con l'archetipo m_weapon) e le sue
//   munizioni non sono piene (CanAddAmmoFromWeaponPickup), toccarla prende le
//   munizioni (AddAmmoFromWeaponPickup, le native 0x5AADB0 / 0xDAD000: tolgono
//   dalla pickup quello che entra, OnAmmoTakenByWeapon, la pickup vuota torna
//   nel pool); altrimenti la mano la prende.
// - In mano (WeaponDrive): velocita' del corpo verso la posa voluta della
//   radice negli assi della mano (g_wpnGrip), come la guida degli oggetti senza
//   peso: errore x gain / dt + velocita' della mano + polso. Coppie con i corpi
//   di Walker e con le sfere delle mani ignorate per ogni corpo dell'arma
//   (CompNxAll; l'M4 e le pistole ne hanno due), massa di PhysX da fucile
//   (WeaponLighten, 4 kg). Il timer di autodistruzione del gioco (SelfDestroyTimer, 15 s
//   per le armi dei nemici, InitializeFromWeaponToss) e' tolto alla presa
//   (ClearTimer) e rimesso al rilascio (InitializeSelfDestruction): il gioco
//   distrugge un'arma che il giocatore non guarda, anche quella in mano.
// - Passaggio alla destra (WeaponHandover): radice dell'arma entro 15 cm dalla
//   mano destra (il bersaglio IK della destra e' la radice dell'arma in mano)
//   per 0,1 s. Arma nuova: YPickup_Weapon.DoPickup(Walker), la raccolta del
//   gioco senza l'animazione: con due armi lascia cadere quella in mano
//   (ServerDropWeapon, una pickup nuova dove sta l'arma), aggiunge e impugna
//   quella raccolta con le sue munizioni (ServerAddWeapon, ServerInitWeaponAmmo).
//   Stessa arma di una che Walker ha: le munizioni, come al tocco.
// Opzione "weaponpickup on|off" (menu Basic). Collaudi: "weapontest" (la
// sinistra prende l'arma a terra piu' vicina, entro 3 m, senza zona) e
// "weapongrip f r u p y r" (posa della radice nella mano, cm e gradi).
// Posa della radice dell'arma negli assi del bersaglio della mano sinistra
// (l'osso Weapon_Left: X avanti, Y su, Z a sinistra, misurati il 27/10 con la
// mano finta dritta; non avanti-destra-su come la destra). Con gli assi della
// mano l'AK stava girato di 90 gradi (caricatore a destra) e il pugno sul
// caricatore: rollio -90 (carrello in su, caricatore in giu') e 6 cm avanti, 4
// su: l'impugnatura a pistola nel pugno, il calcio lungo l'avambraccio
// (scratchpad f6/out/grip, confronto di 4 pose).
struct WeaponGrip {
    float off[3] = {6.0f, 4.0f, 0.0f};    // cm lungo gli assi X, Y, Z della mano
    float rot[3] = {0.0f, 0.0f, -90.0f};  // gradi: beccheggio, imbardata, rollio
} g_wpnGrip;

struct WeaponHeld {
    void* actor = nullptr;  // YPickup_WeaponSpawnable
    void* comp = nullptr;   // la sua m_pickupMesh alla presa (il pool del gioco la cambia)
    int32_t name = 0;       // nome dell'attore (ObjAlive: un attore distrutto non passa)
    uint32_t weaponClass = 0;
    uint32_t walkerSig = 0;
    bool timerCleared = false;  // SelfDestroyTimer tolto alla presa: rimesso al rilascio
    double grabAt = 0.0;
    float blendT = 0.15f;
    float fromPos[3] = {0, 0, 0};  // posa della radice negli assi della mano alla presa
    Quat fromQ = {0, 0, 0, 1};
    float prevHa[3][3] = {};
    float handW[3] = {0, 0, 0};
    bool wristInit = false;
    double stuckSince = -1.0;
    double nearSince = -1.0;   // radice vicina alla mano destra da qui (passaggio)
    double ammoTryAt = -10.0;  // ultimo passaggio con munizioni piene (una riga, non a ogni tick)
    double staleSince = -1.0;  // ossa ferme (mesh non aggiornata): rotazione non corretta da qui
    float mass = 0.0f;
    int ticks = 0;
} g_wpn;

bool g_wpnTestReq = false;  // "weapontest" in coda (thread di gioco, sotto g_spawnMutex)
// "weapongrip" in coda (revisione: il comando arriva dal thread di
// rendering, control::Poll; la presa la legge il tick sul thread di gioco).
bool g_wpnGripReq = false;
WeaponGrip g_wpnGripNew;
// Ultimo tick con le prese accese (grabprops, niente cinema ne' gioco
// bloccato): la mano prende davvero le armi a terra (WeaponForHand, grabuse).
double g_wpnHandAt = -10.0;
double g_wpnGripTried = -1.0;  // chiusura del grip (gripDownAt) gia' usata su un'arma a terra

// Posa nel mondo dell'osso radice (indice 0) di una SkeletalMeshComponent:
// LocalToWorld della componente per la radice in spazio componente (SpaceBases
// +0x274, FBoneAtom: quaternione, traslazione, scala). Convenzione verificata
// sulla volata dell'AK a terra: Rotate(q, (34,56 0,02 3,22)) = (32,16 13,05
// -0,38) contro (32,2 13,1 -0,4) letti (27/10).
bool WeaponPose(void* comp, float pos[3], float axes[3][3]) {
    float M[4][4];
    if (!comp || !ReadMatrix(comp, M)) return false;
    const uint32_t data = U32(comp, 0x274), num = U32(comp, 0x278);
    if (!data || !num || num > 512) return false;
    struct {
        float q[4];
        float t[3];
        float s;
    } a = {};
    if (!SafeCopy(&a, reinterpret_cast<void*>(static_cast<uintptr_t>(data)), sizeof(a))) return false;
    const float ql = std::sqrt(a.q[0] * a.q[0] + a.q[1] * a.q[1] + a.q[2] * a.q[2] + a.q[3] * a.q[3]);
    if (!(ql > 0.5f && ql < 2.0f)) return false;
    const Quat q = {a.q[0] / ql, a.q[1] / ql, a.q[2] / ql, a.q[3] / ql};
    float R[3][3], sc[3];
    QuatToRows(q, R);
    for (int r = 0; r < 3; ++r) {
        sc[r] = std::sqrt(Dot3(M[r], M[r]));
        if (!(sc[r] > 1e-6f)) return false;
    }
    for (int i = 0; i < 3; ++i)
        for (int c = 0; c < 3; ++c) axes[i][c] = R[i][0] * M[0][c] / sc[0] + R[i][1] * M[1][c] / sc[1] + R[i][2] * M[2][c] / sc[2];
    for (int c = 0; c < 3; ++c) pos[c] = M[3][c] + a.t[0] * M[0][c] + a.t[1] * M[1][c] + a.t[2] * M[2][c];
    return std::isfinite(pos[0] + pos[1] + pos[2] + axes[0][0] + axes[1][1] + axes[2][2]);
}

// Posa voluta della radice negli assi della mano (g_wpnGrip): righe = assi
// dell'arma negli assi della mano. Assi dell'osso Weapon_Left: X avanti, Y su,
// Z a sinistra (Z = X x Y nei numeri del mondo). Imbardata attorno a Y (su),
// poi beccheggio attorno a Z (di lato), poi rollio attorno a X, come un
// rotatore di UE3; imbardata positiva = a destra, beccheggio positivo = in su.
// Revisione della voce 6: prima l'imbardata girava attorno a Z e il
// beccheggio attorno a Y, gli assi della mano destra (avanti-destra-su), cioe'
// scambiati in questo riferimento (la predefinita li ha a zero: invariata).
void WeaponGripRel(float rp[3], float ra[3][3]) {
    const float s = vr::control::g_posScale * 0.01f, D = 0.0174533f;
    for (int i = 0; i < 3; ++i) rp[i] = g_wpnGrip.off[i] * s;
    const float zAx[3] = {0, 0, 1}, yAx[3] = {0, 1, 0}, xAx[3] = {1, 0, 0};
    // Attorno a +Y un angolo positivo porta X verso -Z (a destra); attorno a
    // +Z porta X verso +Y (in su).
    const Quat q = Mul(AxisAngle(yAx, g_wpnGrip.rot[1] * D), Mul(AxisAngle(zAx, g_wpnGrip.rot[0] * D), AxisAngle(xAx, g_wpnGrip.rot[2] * D)));
    QuatToRows(q, ra);
}

// Pickup ancora quella presa, viva, visibile, con la stessa mesh e la stessa
// classe d'arma (revisione: il pool del gioco riusa attore e mesh; una pickup
// ridata con un'altra arma non e' piu' quella in mano).
bool WeaponAlive(const WeaponHeld& w) {
    if (!w.actor || !ObjAlive(w.actor, w.name)) return false;
    if (U32(w.actor, kOffActorFlags9C) & (kHidden | kDeleteMe)) return false;
    if (U32(w.actor, kOffPickupFlags) & kPickupHidden) return false;
    if (U32(w.actor, kOffPickupWeaponClass) != w.weaponClass) return false;
    return reinterpret_cast<void*>(static_cast<uintptr_t>(U32(w.actor, kOffPickupMesh))) == w.comp;
}

// Velocita' dell'arma: prima la rotazione, poi la lineare sommata. La
// SetRBAngularVelocity di una SkeletalMeshComponent (FUN_00e1b7f0, Ghidra
// 27/10) scrive a ogni corpo anche la velocita' lineare w x (corpo - radice),
// zero al corpo della radice: chiamata dopo, cancellava la velocita' lineare
// data prima (l'arma restava a terra: comando 29 m/s, misurata 1-3; un solo
// SetRBLinearVelocity di 5 m/s la alzava di 78 cm). Sommata (bAddToCurrent),
// la radice ha v e gli altri corpi v + w x r (l'M4 ne ha due): moto rigido.
void WeaponSetVel(void* comp, const float* v, const float* w) {
    SetAngVel(comp, w);
    struct {
        float v[3];
        uint32_t add;
    } p = {{v[0], v[1], v[2]}, 1};
    vr::script::CallFunction(comp, "SetRBLinearVelocity", &p);
}

std::string WeaponClassName(uint32_t cls) { return cls ? vr::script::ObjectName(reinterpret_cast<void*>(static_cast<uintptr_t>(cls))) : std::string("-"); }

// Munizioni dell'arma: YWeapon.m_currentAmmoCount (+0x764) e m_currentMagAmmo
// (+0x778), le stesse del polso. Raccolta: ServerInitWeaponAmmo mette
// m_ammoInMagazine della pickup nel primo e m_ammoInReserve nel secondo
// (misurato: pickup 25+11 -> AK47 25 e 11); l'HUD del gioco mostra il secondo.
void WeaponAmmo(void* weapon, int32_t* count, int32_t* magAmmo) {
    *count = *magAmmo = -1;
    if (!weapon) return;
    SafeCopy(count, static_cast<char*>(weapon) + 0x764, 4);
    SafeCopy(magAmmo, static_cast<char*>(weapon) + 0x778, 4);
}

// Arma di Walker della stessa classe della pickup (nullptr se non ce l'ha).
void* WeaponOwned(void* pawn, void* pickup) {
    void* ars = reinterpret_cast<void*>(static_cast<uintptr_t>(U32(pawn, 0x1590)));  // YGamePawn.m_arsenalManager
    void* arch = reinterpret_cast<void*>(static_cast<uintptr_t>(U32(pickup, kOffPickupWeaponArch)));
    if (!ars || !arch) return nullptr;
    struct {
        void* arch;
        void* ret;
    } fw = {arch, nullptr};
    if (!vr::script::CallFunction(ars, "FindWeaponInAresenal", &fw)) return nullptr;
    return fw.ret;
}

// Munizioni dalla pickup all'arma di Walker della stessa classe (natives del
// gioco). 1 prese, 0 piene (niente da prendere), -1 chiamata fallita.
int WeaponTakeAmmo(void* pawn, void* pickup, void* owned, const char* why) {
    void* ars = reinterpret_cast<void*>(static_cast<uintptr_t>(U32(pawn, 0x1590)));
    if (!ars || !pickup) return -1;
    struct {
        void* pickup;
        int32_t index, secondary;
        uint32_t ret;
    } can = {pickup, -1, -1, 0};
    if (!vr::script::CallFunction(ars, "CanAddAmmoFromWeaponPickup", &can)) return -1;
    int32_t pm0 = -1, pr0 = -1, wm0 = -1, wt0 = -1;
    SafeCopy(&pm0, static_cast<char*>(pickup) + kOffPickupAmmoMag, 4);
    SafeCopy(&pr0, static_cast<char*>(pickup) + kOffPickupAmmoRes, 4);
    WeaponAmmo(owned, &wm0, &wt0);
    char b[320];
    if (!can.ret) {
        std::snprintf(b, sizeof(b), "WEAPON: %s: %s full (%d+%d), the pickup keeps %d+%d", why,
                      vr::script::ClassName(owned).c_str(), wm0, wt0, pm0, pr0);
        vr::log::Line(b);
        return 0;
    }
    struct {
        void* pickup;
        uint32_t ret;
    } add = {pickup, 0};
    const bool called = vr::script::CallFunction(ars, "AddAmmoFromWeaponPickup", &add);
    int32_t pm1 = -1, pr1 = -1, wm1 = -1, wt1 = -1;
    SafeCopy(&pm1, static_cast<char*>(pickup) + kOffPickupAmmoMag, 4);
    SafeCopy(&pr1, static_cast<char*>(pickup) + kOffPickupAmmoRes, 4);
    WeaponAmmo(owned, &wm1, &wt1);
    // Vuota: OnPickupEmpty -> DisablePickup la rimette nel pool (m_pickupMesh a None).
    const bool emptied = (U32(pickup, kOffPickupFlags) & kPickupHidden) != 0 || !U32(pickup, kOffPickupMesh);
    std::snprintf(b, sizeof(b),
                  "WEAPON: %s: ammo for %s (slot %d%s): %d+%d in the weapon -> %d+%d (m_currentAmmoCount + m_currentMagAmmo), pickup %d+%d -> %d+%d%s%s", why,
                  vr::script::ClassName(owned).c_str(), can.index, can.secondary > 0 ? ", secondary" : "", wm0, wt0, wm1, wt1, pm0, pr0,
                  pm1, pr1, emptied ? ", the pickup is empty and gone" : "", called && add.ret ? "" : " (AddAmmoFromWeaponPickup returned false)");
    vr::log::Line(b);
    vr::input::PulseHand(0, 40);
    return called && add.ret ? 1 : -1;
}

// Fine della presa senza toccare la pickup (gia' tolta dal gioco o appena
// raccolta da DoPickup): solo lo stato della mano.
void WeaponForget() {
    if (!g_wpn.actor) return;
    g_wpnComp[1] = g_wpn.comp;  // le coppie con le sfere tornano solide da sole (ProxyTick)
    g_wpnComp[0] = nullptr;
    g_wpn = WeaponHeld();
    vr::control::g_propReleasedMs[1] = GetTickCount64();
    vr::control::g_propHeld[1] = false;
}

void WeaponRelease(const char* why) {
    WeaponHeld& w = g_wpn;
    if (!w.actor) return;
    const bool alive = WeaponAlive(w);
    float v[3] = {0, 0, 0}, speed = 0.0f;
    if (alive) {
        if (std::strcmp(why, "grip") == 0 && HandVelocity(1, 0.06, v)) {
            // Lancio: la velocita' media della mano negli ultimi 60 ms per
            // throwboost, al piu' 12 m/s; la rotazione del polso. Sul baricentro
            // (centro dei limiti): la radice parte con la velocita' della mano.
            for (float& c : v) c *= vr::control::g_throwBoost.load();
            const float cap = 12.0f * vr::control::g_posScale;
            speed = std::sqrt(Dot3(v, v));
            if (speed > cap)
                for (float& c : v) c *= cap / speed;
            speed = (std::min)(speed, cap);
            float wv[3] = {w.handW[0], w.handW[1], w.handW[2]};
            const float wl = std::sqrt(Dot3(wv, wv));
            if (wl > 20.0f)
                for (float& c : wv) c *= 20.0f / wl;
            float op[3], oa[3][3], bc[3];
            if (WeaponPose(w.comp, op, oa) && SafeCopy(bc, static_cast<char*>(w.comp) + kOffCompBounds, 12)) {
                const float arm[3] = {bc[0] - op[0], bc[1] - op[1], bc[2] - op[2]};
                float sp[3];
                Cross3(wv, arm, sp);
                for (int c = 0; c < 3; ++c) v[c] += sp[c];
            }
            WeaponSetVel(w.comp, v, wv);
            Wake(w.comp);
            // Velocita' vera dell'arma 3 tick dopo e a 0,1 s (la riga "thrown
            // object flies" di Release; CompNx legge il corpo da g_wpnComp[1]).
            g_throwCheck = ThrowCheck();
            g_throwCheck.actor = w.actor;
            g_throwCheck.comp = w.comp;
            g_throwCheck.ticks = 3;
            g_throwCheck.wanted = std::sqrt(Dot3(v, v));
            std::memcpy(g_throwCheck.wantV, v, sizeof(v));
            std::memcpy(g_throwCheck.wantW, wv, sizeof(wv));
            g_throwCheck.hasW = true;
            SafeCopy(g_throwCheck.from, static_cast<char*>(w.comp) + kOffCompBounds, 12);
            g_throwCheck.at = ProfMs() * 0.001;
        }
        // Le sfere delle mani lasciano uscire l'arma prima di tornare solide,
        // come gli oggetti (Release, relgrace): un lancio sopra 1 m/s rendeva
        // solida la coppia sfera / arma al tick dopo, con la sfera sinistra
        // ancora dentro l'arma (spinta di uscita e uno "schiaffo" della mano).
        const double t = ProfMs() * 0.001;
        if (g_tune.relGrace > 0.0f)
            for (int s = 0; s < 2; ++s) g_relGrace[s][1] = {w.comp, t, t + g_tune.relGrace, false};
        if (w.timerCleared) {
            uint32_t none[4] = {0, 0, 0, 0};
            vr::script::CallFunction(w.actor, "InitializeSelfDestruction", none);
        }
    }
    char b[260];
    std::snprintf(b, sizeof(b), "WEAPON: left hand released %s (%s), %.1f m/s%s", WeaponClassName(w.weaponClass).c_str(), why,
                  speed / vr::control::g_posScale,
                  !alive ? " - the pickup is gone" : (w.timerCleared ? ", self-destroy timer set again" : ""));
    vr::log::Line(b);
    WeaponForget();
}

// Massa di PhysX dell'arma raccolta come quella di un fucile vero (revisione
// della voce 6, regola dell'orchestratore): il PhysicsAsset delle armi ha la
// densita' predefinita (AK47 296 kg, M4 371), e l'arma guidata in velocita'
// spingeva oggetti e cadaveri come un barile. Massa e inerzia di ogni corpo x
// (kWeaponKg / massa totale): stessa distribuzione, quindi la stessa guida (la
// velocita' e' comandata), urti da 4 kg. Restano finche' esistono i corpi: il
// gioco li ricrea dal PhysicsAsset (massa di prima) quando addormenta l'arma
// ferma (DoSleepActivation) o la rimette nel pool. Con 4 kg i limiti del peso
// (forza 200 N, light 400) danno 50-100 m/s^2: l'arma non resterebbe indietro,
// e la guida dell'arma resta quella senza peso.
// NxActor::setMassSpaceInertiaTensor +0xBC (const NxVec3&) e
// getMassSpaceInertiaTensor +0xC0 (NxVec3 per puntatore nascosto): le
// asserzioni "Actor::setMassSpaceInertiaTensor: Actor must be dynamic!" e
// "Actor::getMassSpaceInertiaTensorVal: ..." di NpActor.cpp stanno nelle
// funzioni degli slot 47 e 48 (lette nel gioco il 27/10), come in NxActor.h
// dopo setMass / getMass (45 / 46).
constexpr float kWeaponKg = 4.0f;
using NxSetVecRef = void(__thiscall*)(void*, const float*);

bool NxMassOf(void* nx, float* m, float inertia[3]) {
    __try {
        void** vt = *static_cast<void***>(nx);
        if (!SameModule(vt, vt[45]) || !SameModule(vt, vt[46]) || !SameModule(vt, vt[47]) || !SameModule(vt, vt[48])) return false;
        *m = reinterpret_cast<NxGetFloat>(vt[46])(nx);
        reinterpret_cast<NxGetVec>(vt[48])(nx, inertia);
        return *m > 0.0f && *m < 1e6f && inertia[0] > 0.0f && inertia[1] > 0.0f && inertia[2] > 0.0f &&
               std::isfinite(inertia[0] + inertia[1] + inertia[2]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool NxSetMassOf(void* nx, float m, const float inertia[3]) {
    __try {
        void** vt = *static_cast<void***>(nx);
        reinterpret_cast<NxSetFloat>(vt[45])(nx, m);
        reinterpret_cast<NxSetVecRef>(vt[47])(nx, inertia);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Ritorna la massa totale dopo (unita'), before = prima; -1 se non letta.
float WeaponLighten(void* comp, float* before) {
    *before = -1.0f;
    void* nx[kMaxCompBodies];
    const int n = WeaponBodies(comp, nx);
    float m[kMaxCompBodies], I[kMaxCompBodies][3], total = 0.0f;
    for (int i = 0; i < n; ++i) {
        if (!NxMassOf(nx[i], &m[i], I[i])) return -1.0f;
        total += m[i];
    }
    if (!n || !(total > 0.0f)) return -1.0f;
    *before = total;
    const float want = kWeaponKg / (std::max)(1.0f, vr::control::g_massKgPerUnit.load());
    if (total <= want) return total;
    const float k = want / total;
    float after = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float In[3] = {I[i][0] * k, I[i][1] * k, I[i][2] * k};
        float mr = 0.0f, Ir[3] = {0, 0, 0};
        if (NxSetMassOf(nx[i], m[i] * k, In) && NxMassOf(nx[i], &mr, Ir)) after += mr;
        else after += m[i];
    }
    return after;
}

// Arma a terra resa fisica e dinamica: alla presa (qui sotto) e quando i
// guanti gravitazionali la agganciano (GloveTick). true = corpo rigido con la
// radice, si muove con WeaponSetVel.
bool WeaponMakePhysical(void* a, void* comp, uint8_t* physOut) {
    const uint32_t pf = U32(a, kOffPickupFlags);
    uint32_t none[4] = {0, 0, 0, 0};
    // Addormentata dal gioco: di nuovo fisica (PhysicsAssetInstance, PHYS_RigidBody).
    if (pf & kPickupPhysOff) vr::script::CallFunction(a, "ConditionalReactivatePhysics", none);
    uint8_t phys = 0;
    SafeCopy(&phys, static_cast<char*>(a) + kOffActorPhysics, 1);
    if (phys != kPhysRigidBody || !WeaponRootBody(comp)) {
        // Appena creata (StartPhysic in attesa) o messa ferma: la fisica del gioco.
        vr::script::CallFunction(a, "StartPhysic", none);
        SafeCopy(&phys, static_cast<char*>(a) + kOffActorPhysics, 1);
    }
    // Corpi dinamici: il PhysicsAssetInstance ricreato da ConditionalReactivatePhysics
    // ha i corpi fissi (cinematici) del PhysicsAsset, e SetRBLinearVelocity non
    // li muove (primo collaudo, 27/10: comando 23-36 m/s, l'arma ferma).
    // StartPhysic, alla caduta, li rende dinamici con SetComponentRBFixed(false).
    {
        struct {
            uint32_t fixed;
        } fx = {0};
        vr::script::CallFunction(comp, "SetComponentRBFixed", &fx);
    }
    if (physOut) *physOut = phys;
    return phys == kPhysRigidBody && WeaponRootBody(comp);
}

bool WeaponGrab(void* pawn, const WeaponPick& pick, const float* hp, const float ha[3][3], const char* how) {
    void* a = pick.actor;
    const uint32_t pf = U32(a, kOffPickupFlags);
    void* comp = reinterpret_cast<void*>(static_cast<uintptr_t>(U32(a, kOffPickupMesh)));
    if (!comp || comp != pick.comp || (pf & kPickupHidden) || !(pf & kPickupHasPhys)) return false;
    uint8_t phys = 0;
    WeaponMakePhysical(a, comp, &phys);
    const uint32_t body = WeaponRootBody(comp);
    float op[3], oa[3][3];
    if (phys != kPhysRigidBody || !body || !WeaponPose(comp, op, oa)) {
        char b[200];
        std::snprintf(b, sizeof(b), "WEAPON: %s: could not make the %s physical (Physics %u, body 0x%X)", how,
                      vr::script::ClassName(a).c_str(), phys, body);
        vr::log::Line(b);
        return false;
    }
    WeaponHeld& w = g_wpn;
    w = WeaponHeld();
    w.actor = a;
    w.comp = comp;
    w.name = static_cast<int32_t>(U32(a, kOffName));
    w.weaponClass = U32(a, kOffPickupWeaponClass);
    w.grabAt = ProfMs() * 0.001;
    g_wpnComp[0] = comp;
    if (g_wpnComp[1] == comp) g_wpnComp[1] = nullptr;
    // Posa di partenza negli assi della mano; arrivo nella presa in 0,10 s piu'
    // 0,10 s ogni 180 gradi (come la presa canonica degli oggetti).
    float d[3] = {op[0] - hp[0], op[1] - hp[1], op[2] - hp[2]}, rel[3][3], rp[3], ra[3][3];
    for (int r = 0; r < 3; ++r) w.fromPos[r] = Dot3(d, ha[r]);
    for (int i = 0; i < 3; ++i)
        for (int r = 0; r < 3; ++r) rel[i][r] = Dot3(oa[i], ha[r]);
    w.fromQ = RowsToQuat(rel);
    WeaponGripRel(rp, ra);
    const float tr = Dot3(ra[0], rel[0]) + Dot3(ra[1], rel[1]) + Dot3(ra[2], rel[2]);
    w.blendT = 0.10f + 0.10f * std::acos((std::max)(-1.0f, (std::min)(1.0f, (tr - 1.0f) * 0.5f))) / 3.14159265f;
    // Timer di autodistruzione del gioco: via finche' e' in mano.
    static int32_t timerName = -1;
    if (timerName < 0) timerName = vr::script::NameIndex("SelfDestroyTimer");
    float rate = 0.0f;
    if (timerName >= 0) {
        struct {
            int32_t fn[2];
            void* obj;
            uint32_t ret;
        } ta = {{timerName, 0}, nullptr, 0};
        if (vr::script::CallFunction(a, "IsTimerActive", &ta) && ta.ret) {
            struct {
                int32_t fn[2];
                void* obj;
                float ret;
            } tr2 = {{timerName, 0}, nullptr, 0.0f};
            vr::script::CallFunction(a, "GetTimerRate", &tr2);
            rate = tr2.ret;
            struct {
                int32_t fn[2];
                void* obj;
            } ct = {{timerName, 0}, nullptr};
            vr::script::CallFunction(a, "ClearTimer", &ct);
            ta.ret = 0;
            vr::script::CallFunction(a, "IsTimerActive", &ta);
            w.timerCleared = !ta.ret;
        }
    }
    w.walkerSig = IgnoreWalker(pawn, comp, true);
    RaiseMaxAngVel(comp);
    Wake(comp);
    float massBefore = -1.0f;
    w.mass = WeaponLighten(comp, &massBefore);
    vr::control::g_propHeld[1] = true;
    vr::input::PulseHand(0, 50);
    int32_t pm = -1, pr = -1;
    SafeCopy(&pm, static_cast<char*>(a) + kOffPickupAmmoMag, 4);
    SafeCopy(&pr, static_cast<char*>(a) + kOffPickupAmmoRes, 4);
    const uint32_t pai = U32(comp, kOffSkelPhysAssetInst);
    {
        float pw = 0.0f;
        SafeCopy(&pw, static_cast<char*>(comp) + 0x264, 4);  // SkeletalMeshComponent.PhysicsWeight
        char d[260];
        std::snprintf(d, sizeof(d),
                      "WEAPON: grab state: Physics %u, PhysicsWeight %.2f, bNoSkeletonUpdate %u, bHasPhysicsAssetInstance %u, body 0x%X nx %p; "
                      "pickup flags 0x%X (+0x210), m_initTemporarily %u (+0x2FC)",
                      phys, pw, U32(comp, 0x38C) & 1, (U32(comp, 0x3C4) & 0x1000) ? 1u : 0u, body, CompNx(comp), pf, U32(a, kOffPickupTemp) & 1);
        vr::log::Line(d);
    }
    char b[460];
    std::snprintf(b, sizeof(b),
                  "WEAPON: left hand took %s (%s, %s; ammo %d+%d, %d physics bod%s, mass %.1f kg = %.3f units, PhysX mass %.1f kg before the "
                  "rifle cap, arrives in %.2f s)%s",
                  WeaponClassName(w.weaponClass).c_str(), vr::script::ObjectName(reinterpret_cast<void*>(static_cast<uintptr_t>(U32(comp, 0x20C)))).c_str(),
                  how, pm, pr, pai ? static_cast<int>(U32(reinterpret_cast<void*>(static_cast<uintptr_t>(pai)), kOffPaiBodies + 4)) : 0,
                  pai && U32(reinterpret_cast<void*>(static_cast<uintptr_t>(pai)), kOffPaiBodies + 4) == 1 ? "y" : "ies",
                  w.mass * vr::control::g_massKgPerUnit.load(), w.mass, massBefore * vr::control::g_massKgPerUnit.load(), w.blendT,
                  w.timerCleared ? "; self-destroy timer cleared" : (rate > 0.0f ? "; self-destroy timer NOT cleared" : "; no self-destroy timer"));
    vr::log::Line(b);
    if (rate > 0.0f) {
        std::snprintf(b, sizeof(b), "WEAPON: the pickup's self-destroy timer had %.1f s (m_destroyTimer %.1f)", rate,
                      [&] {
                          float t = 0.0f;
                          SafeCopy(&t, static_cast<char*>(a) + 0x258, 4);
                          return t;
                      }());
        vr::log::Line(b);
    }
    return true;
}

// Arma a terra ancora valida: viva, visibile, non tornata nel pool, con la sua mesh.
bool WeaponPickUsable(const WeaponPick& wp) {
    return vr::script::LiveObject(wp.actor) && !(U32(wp.actor, kOffActorFlags9C) & (kHidden | kDeleteMe)) &&
           !(U32(wp.actor, kOffPickupFlags) & kPickupHidden) &&
           U32(wp.actor, kOffPickupMesh) == static_cast<uint32_t>(reinterpret_cast<uintptr_t>(wp.comp));
}

// Arma a terra piu' vicina al palmo entro zone (distanza dai limiti), -1 nessuna.
int WeaponNearest(const float* palm, float zone, float* outDist) {
    int best = -1;
    float bd = zone;
    for (size_t i = 0; i < g_weapons.size(); ++i) {
        const WeaponPick& wp = g_weapons[i];
        if (wp.comp == g_wpn.comp) continue;
        if (!WeaponPickUsable(wp)) continue;
        Prop cur;
        if (!ReadBounds(wp.comp, &cur)) continue;
        const float d = BoxDistance(cur, palm);
        if (d < bd) {
            bd = d;
            best = static_cast<int>(i);
            *outDist = d;
        }
    }
    return best;
}

// Grip chiuso nella zona di un'arma a terra: munizioni (stessa arma di Walker,
// non piene) o presa. true = la mano ha fatto qualcosa.
bool WeaponTryGrabPick(void* pawn, const WeaponPick& wp, const float* hp, const float ha[3][3], const char* how) {
    void* owned = WeaponOwned(pawn, wp.actor);
    if (owned && WeaponTakeAmmo(pawn, wp.actor, owned, "touch") == 1) return true;
    return WeaponGrab(pawn, wp, hp, ha, how);
}

bool WeaponTryGrab(void* pawn, int idx, const float* hp, const float ha[3][3], const char* how) {
    if (idx < 0 || idx >= static_cast<int>(g_weapons.size())) return false;
    const WeaponPick wp = g_weapons[idx];  // copia: le chiamate al gioco non devono vedere il vettore
    return WeaponTryGrabPick(pawn, wp, hp, ha, how);
}

// Arma passata alla mano destra.
void WeaponHandover(void* pawn, double now) {
    WeaponHeld& w = g_wpn;
    void* owned = WeaponOwned(pawn, w.actor);
    if (owned) {
        // Walker ha gia' quest'arma: le munizioni (DoPickup lascerebbe cadere
        // l'arma in mano e non aggiungerebbe niente: ServerAddWeapon ritorna
        // quella che c'e'). Piene: resta in mano, una riga al secondo.
        if (now - w.ammoTryAt < 1.0) return;
        w.ammoTryAt = now;
        void* actor = w.actor;
        WeaponTakeAmmo(pawn, actor, owned, "handover");
        if (!WeaponAlive(w)) WeaponForget();  // vuota: il gioco l'ha rimessa nel pool
        return;
    }
    void* cur = reinterpret_cast<void*>(static_cast<uintptr_t>(U32(pawn, 0x1594)));
    int32_t cm = -1, ct = -1, pm = -1, pr = -1;
    WeaponAmmo(cur, &cm, &ct);
    SafeCopy(&pm, static_cast<char*>(w.actor) + kOffPickupAmmoMag, 4);
    SafeCopy(&pr, static_cast<char*>(w.actor) + kOffPickupAmmoRes, 4);
    void* ars = reinterpret_cast<void*>(static_cast<uintptr_t>(U32(pawn, 0x1590)));
    struct {
        int32_t ret;
    } cnt = {-1};
    if (ars) vr::script::CallFunction(ars, "GetCurrentWeaponCount", &cnt);
    void* actor = w.actor;
    const uint32_t wcls = w.weaponClass;
    // Prima della chiamata: DoPickup rimette la pickup nel pool (DisablePickup:
    // niente piu' corpo, mesh restituita) e la mano non deve piu' guidarla.
    WeaponForget();
    struct {
        void* pawn;
        uint32_t ret;
    } dp = {pawn, 0};
    const bool ok = vr::script::CallFunction(actor, "DoPickup", &dp);
    void* nw = reinterpret_cast<void*>(static_cast<uintptr_t>(U32(pawn, 0x1594)));
    int32_t nm = -1, nt = -1;
    WeaponAmmo(nw, &nm, &nt);
    struct {
        int32_t ret;
    } cnt2 = {-1};
    if (ars) vr::script::CallFunction(ars, "GetCurrentWeaponCount", &cnt2);
    char b[400];
    std::snprintf(b, sizeof(b),
                  "WEAPON: handed to the right hand: DoPickup %s; in the hand %s (%d+%d) -> %s (%d+%d), pickup was %s with %d+%d; weapons %d -> %d",
                  ok ? (dp.ret ? "done" : "returned false") : "FAILED", cur ? vr::script::ClassName(cur).c_str() : "none", cm, ct,
                  nw ? vr::script::ClassName(nw).c_str() : "none", nm, nt, WeaponClassName(wcls).c_str(), pm, pr, cnt.ret, cnt2.ret);
    vr::log::Line(b);
    // Solo la sinistra (revisione): la destra e' la mano dell'arma, e la regola
    // dell'utente e' che la mano dell'arma non cambia mai (niente impulso nuovo).
    vr::input::PulseHand(0, 40);
}

// Diagnostica della revisione: al terzo tick in mano, per ogni corpo
// dell'arma, coppie con i corpi di Walker e con le due sfere delle mani
// ignorate davvero (getActorPairFlags) e rotazione massima alzata.
bool NxMaxAngOf(void* nx, float* v) {
    __try {
        void** vt = *static_cast<void***>(nx);
        if (!SameModule(vt, vt[61])) return false;
        *v = reinterpret_cast<NxGetFloat>(vt[61])(nx);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool NxPairIgnored(void* a, void* b) {
    __try {
        void** va = *static_cast<void***>(a);
        void* sa = reinterpret_cast<NxGetScene>(va[1])(a);
        if (!sa) return false;
        void** vs = *static_cast<void***>(sa);
        if (!SameModule(va, vs[42])) return false;
        return (reinterpret_cast<NxGetPairFlags>(vs[42])(sa, a, b) & kNxIgnorePair) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void WeaponBodiesReport(void* pawn, const WeaponHeld& w) {
    void* nx[kMaxCompBodies];
    const int n = WeaponBodies(w.comp, nx);
    void* wb[kMaxWalkerBodies];
    const int nw = WalkerBodies(pawn, wb);
    void* sp[2] = {g_proxy[0].actor ? CompNx(g_proxy[0].comp) : nullptr, g_proxy[1].actor ? CompNx(g_proxy[1].comp) : nullptr};
    int walkerOk = 0, sph[2] = {0, 0}, fast = 0;
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < nw; ++j) walkerOk += NxPairIgnored(nx[i], wb[j]) ? 1 : 0;
        for (int s = 0; s < 2; ++s)
            if (sp[s] && NxPairIgnored(sp[s], nx[i])) ++sph[s];
        float mv = 0.0f;
        if (NxMaxAngOf(nx[i], &mv) && mv >= 59.0f) ++fast;
    }
    char b[260];
    std::snprintf(b, sizeof(b),
                  "WEAPON: %d physics bod%s in the hand: Walker pairs ignored %d/%d, right hand sphere %d/%d%s, left hand sphere %d/%d%s, "
                  "max angular velocity 60 rad/s on %d/%d",
                  n, n == 1 ? "y" : "ies", walkerOk, n * nw, sph[0], n, sp[0] ? "" : " (no sphere)", sph[1], n, sp[1] ? "" : " (no sphere)", fast, n);
    vr::log::Line(b);
}

// Una volta per tick con l'arma nella sinistra e il grip chiuso.
void WeaponDrive(void* pawn, const float* hp, const float ha[3][3], const float* rightHp, bool rightValid, float dt, double now) {
    WeaponHeld& w = g_wpn;
    if (!WeaponAlive(w)) {
        WeaponRelease("pickup gone");
        return;
    }
    if (!WeaponRootBody(w.comp)) {
        // Il gioco l'ha addormentata (ferma con la mano ferma): di nuovo fisica.
        uint32_t none[4] = {0, 0, 0, 0};
        vr::script::CallFunction(w.actor, "ConditionalReactivatePhysics", none);
        struct {
            uint32_t fixed;
        } fx = {0};
        vr::script::CallFunction(w.comp, "SetComponentRBFixed", &fx);
        if (!WeaponRootBody(w.comp)) {
            WeaponRelease("no physics body");
            return;
        }
        w.walkerSig = IgnoreWalker(pawn, w.comp, true);
        RaiseMaxAngVel(w.comp);
        float before = -1.0f;
        w.mass = WeaponLighten(w.comp, &before);  // corpi nuovi dal PhysicsAsset: di nuovo la massa di un fucile
        char b[200];
        std::snprintf(b, sizeof(b), "WEAPON: the game had put the held weapon to sleep: physics on again (PhysX mass %.1f kg -> %.1f kg)",
                      before * vr::control::g_massKgPerUnit.load(), w.mass * vr::control::g_massKgPerUnit.load());
        vr::log::Line(b);
    } else {
        // Corpi di Walker ricreati (accucciata): di nuovo ignorati.
        const uint32_t sig = WalkerSig(pawn);
        if (sig && sig != w.walkerSig) w.walkerSig = IgnoreWalker(pawn, w.comp, true);
    }
    float op[3], oa[3][3];
    if (!WeaponPose(w.comp, op, oa)) {
        WeaponRelease("no pose");
        return;
    }
    ++w.ticks;
    if (w.ticks == 3) WeaponBodiesReport(pawn, w);  // dopo le coppie delle sfere (ProxyTick dei primi tick)
    // Ossa non aggiornate (mesh fuori vista: il gioco non ricompone lo
    // scheletro): la radice dalle ossa resta indietro rispetto all'attore, che
    // segue il corpo. Posizione dall'attore, rotazione non corretta.
    float al[3];
    bool stale = false;
    if (SafeCopy(al, static_cast<char*>(w.actor) + kOffActorLocation, 12)) {
        const float dd[3] = {al[0] - op[0], al[1] - op[1], al[2] - op[2]};
        stale = Dot3(dd, dd) > 4.0f * 4.0f;
        if (stale) std::memcpy(op, al, sizeof(op));
    }
    if (stale && w.staleSince < 0.0) w.staleSince = now;
    if (!stale) w.staleSince = -1.0;
    // Posa voluta: dalla posa della presa a quella dell'impugnatura in blendT.
    float rp[3], ra[3][3];
    WeaponGripRel(rp, ra);
    float bl = static_cast<float>((now - w.grabAt) / (std::max)(1e-3f, w.blendT));
    bl = (std::max)(0.0f, (std::min)(1.0f, bl));
    bl = bl * bl * (3.0f - 2.0f * bl);
    if (bl < 1.0f) {
        const Quat qt = RowsToQuat(ra);
        QuatToRows(Slerp(w.fromQ, qt, bl), ra);
        for (int i = 0; i < 3; ++i) rp[i] = w.fromPos[i] + (rp[i] - w.fromPos[i]) * bl;
    }
    float tp[3], ta[3][3];
    for (int c = 0; c < 3; ++c) tp[c] = hp[c] + rp[0] * ha[0][c] + rp[1] * ha[1][c] + rp[2] * ha[2][c];
    for (int i = 0; i < 3; ++i)
        for (int c = 0; c < 3; ++c) ta[i][c] = ra[i][0] * ha[0][c] + ra[i][1] * ha[1][c] + ra[i][2] * ha[2][c];
    // Velocita' angolare del polso (dal tick prima, smorzata come in Drive).
    if (!w.wristInit) {
        std::memcpy(w.prevHa, ha, sizeof(w.prevHa));
        w.wristInit = true;
    }
    {
        float Rh[3][3] = {}, axh[3], angh = 0.0f, wNow[3] = {0, 0, 0};
        for (int i = 0; i < 3; ++i)
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) Rh[r][c] += ha[i][r] * w.prevHa[i][c];
        if (AxisAngleOf(Rh, axh, &angh) && angh < 1.5f)
            for (int c = 0; c < 3; ++c) wNow[c] = axh[c] * angh / dt;
        for (int c = 0; c < 3; ++c) w.handW[c] += (wNow[c] - w.handW[c]) * 0.5f;
        std::memcpy(w.prevHa, ha, sizeof(w.prevHa));
    }
    const float s = vr::control::g_posScale;
    const float k = (std::max)(0.1f, (std::min)(1.0f, g_tune.gain));
    float err[3] = {tp[0] - op[0], tp[1] - op[1], tp[2] - op[2]};
    const float errLen = std::sqrt(Dot3(err, err));
    // Bloccata a oltre 60 cm dalla mano per 0,3 s: scivola via, come gli
    // oggetti. Non nell'arrivo (weapontest la prende fino a 3 m lontana).
    if (errLen > 0.6f * s && now - w.grabAt > w.blendT + 0.5) {
        if (w.stuckSince < 0.0) w.stuckSince = now;
        if (now - w.stuckSince > 0.3) {
            WeaponRelease("slipped");
            return;
        }
    } else {
        w.stuckSince = -1.0;
    }
    float hv[3] = {0, 0, 0}, pv[3];
    HandVelocity(1, 0.03, hv);
    float armH[3] = {tp[0] - hp[0], tp[1] - hp[1], tp[2] - hp[2]}, spin[3];
    Cross3(w.handW, armH, spin);
    float v[3];
    for (int c = 0; c < 3; ++c) v[c] = k * err[c] / dt + hv[c] + spin[c];
    PawnVelocity(pv);
    const float kc = CapScale(v, pv, 30.0f * s);
    for (float& c : v) c *= kc;
    float wcmd[3] = {w.handW[0], w.handW[1], w.handW[2]}, ang = 0.0f;
    if (!stale) {
        float R[3][3] = {}, ax[3];
        for (int i = 0; i < 3; ++i)
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) R[r][c] += ta[i][r] * oa[i][c];
        if (AxisAngleOf(R, ax, &ang)) {
            const float rate = (std::min)(k * ang / dt, 40.0f);
            for (int c = 0; c < 3; ++c) wcmd[c] += ax[c] * rate;
        }
    }
    // SetRBLinearVelocity muove il baricentro (circa il centro dei limiti),
    // non la radice: con la rotazione la radice va a v + w x (radice - baricentro).
    float bc[3];
    if (SafeCopy(bc, static_cast<char*>(w.comp) + kOffCompBounds, 12) && std::isfinite(bc[0] + bc[1] + bc[2])) {
        const float arm[3] = {bc[0] - op[0], bc[1] - op[1], bc[2] - op[2]};
        if (Dot3(arm, arm) < 60.0f * 60.0f) {
            float sp[3];
            Cross3(wcmd, arm, sp);
            for (int c = 0; c < 3; ++c) v[c] += sp[c];
        }
    }
    WeaponSetVel(w.comp, v, wcmd);
    Wake(w.comp);
    // Passaggio alla mano destra: radice entro 15 cm dal suo bersaglio per 0,1 s
    // (dopo l'arrivo nella presa: non un'arma presa accanto alla destra).
    float dr = -1.0f;
    if (rightValid && now - w.grabAt > w.blendT) {
        const float d[3] = {op[0] - rightHp[0], op[1] - rightHp[1], op[2] - rightHp[2]};
        dr = std::sqrt(Dot3(d, d));
        if (dr < 0.15f * s) {
            if (w.nearSince < 0.0) w.nearSince = now;
        } else {
            w.nearSince = -1.0;
        }
    } else {
        w.nearSince = -1.0;
    }
    if (vr::control::g_grabLog > 0) {
        vr::control::g_grabLog.fetch_sub(1);
        // Velocita' misurata dal corpo PhysX (prima del comando di questo tick).
        float ml[3] = {0, 0, 0}, mw[3] = {0, 0, 0};
        void* nx = CompNx(w.comp);
        const bool meas = nx && NxVelocities(nx, ml, mw);
        uint8_t phys = 0;
        SafeCopy(&phys, static_cast<char*>(w.actor) + kOffActorPhysics, 1);
        char b[480];
        std::snprintf(b, sizeof(b),
                      "WEAPONLOG: tick %d err %.1f cm %.1f deg%s, root (%.1f, %.1f, %.1f) target (%.1f, %.1f, %.1f), v %.2f m/s w %.1f rad/s "
                      "(measured %s%.2f m/s %.1f rad/s, Physics %u, actor %.2f m/s), hand %.2f m/s, right hand %.1f cm%s",
                      w.ticks, errLen / s * 100.0f, ang * 57.2958f, stale ? " (bones stale)" : "", op[0], op[1], op[2], tp[0], tp[1], tp[2],
                      std::sqrt(Dot3(v, v)) / s, std::sqrt(Dot3(wcmd, wcmd)), meas ? "" : "n/a ", std::sqrt(Dot3(ml, ml)), std::sqrt(Dot3(mw, mw)),
                      phys, [&] {
                          float av[3] = {0, 0, 0};
                          SafeCopy(av, static_cast<char*>(w.actor) + 0x140, 12);
                          return std::sqrt(Dot3(av, av)) / s;
                      }(), std::sqrt(Dot3(hv, hv)) / s, dr / s * 100.0f, w.nearSince >= 0.0 ? " (near)" : "");
        vr::log::Line(b);
    }
    if (w.nearSince >= 0.0 && now - w.nearSince >= 0.1) WeaponHandover(pawn, now);
}

void WeaponReset() {
    g_wpn = WeaponHeld();
    g_wpnComp[0] = g_wpnComp[1] = nullptr;
    g_weapons.clear();
    g_wpnPool.clear();   // voce 10
    g_disarmed.clear();  // voce 10: nemici del livello vecchio
    DisarmHandsReset();  // voce 10: il palmo del livello vecchio
    g_wpnTestReq = false;
}

// --- Granate nemiche rilanciate (voce 8 del piano dei combattimenti, 27/10) ---
// Le granate dell'IA sono YProj_Physical (YProj_FragGrenadeAI, ..StunGrenadeAI,
// ..StickyGrenadeAI, ..FragGrenadePlayerSquadAI di Adams e Lugo): non corpi
// PhysX ma la simulazione del proiettile (PHYS_Falling, rimbalzi in HitWall /
// Bounce) che segue Actor.Velocity. Misurato il 27/10 (scratchpad f8/,
// gprobe2.py): una frag dell'IA a mezz'aria con Velocity riscritta a (0, 0,
// 600) sale dritta e ricade con la gravita' (niente curva precalcolata:
// m_predictedLocation serve solo agli avvisi per l'IA, DoPrediction); ferma a
// terra, con bPerformGravity spento e Velocity = (punto - Location) / 0,03,
// sale di 60 unita' e resta sul punto finche' esplode. Da ferma la Velocity
// cresce con la gravita' (-3000 unita'/s dopo 3 s) senza muoverla: "a terra"
// si legge dagli spostamenti. La miccia e' il timer 'OnExplode' di
// TimeExplosion (Actor.Timers: frag dell'IA 4,0 s, quella di Walker 2,3; stun
// 1,1; fosforo 0,7) e in mano continua a correre.
// Chi la lancia: YGrenadeThrow.SpawnGrenade -> InitGrenade (Instigator = chi
// lancia, m_causer = il suo Controller, m_causerPawn; Owner resta None,
// InstigatorController None). Il danno dell'esplosione passa da
// YDamageManager.ApplyDamage, che con una YProj_Physical come causa usa
// m_causer come Controller del colpo: InitCauser(PC di Walker) al rilancio
// fa contare a Walker le uccisioni (LastHitBy dei colpiti). Esclusi Milkor,
// RPG e Mortar (proiettili di lanciatori) e quelle che esplodono al contatto.
constexpr uintptr_t kOffActorInstigator = 0xE8;
constexpr uintptr_t kOffActorTimers = 0xB0;       // Actor.Timers: TArray<TimerData> (bLoop, FuncName, Rate, Count, Dilation, TimerObj: 0x1C byte)
constexpr uintptr_t kOffProjInstCtrl = 0x224;     // Projectile.InstigatorController
constexpr uintptr_t kOffProjSkelMesh = 0x240;     // YProjectile.m_projSkelMesh (limiti ~7 x 9 x 7 cm, raggio 6,6 unita')
constexpr uintptr_t kOffProjMode = 0x29C;         // YProjectile.m_mode: 1 rotolata, 2 lanciata
constexpr uintptr_t kOffGrenFlags = 0x3E0;        // YProj_Physical, bit qui sotto
constexpr uint32_t kGrenGravity = 0x1;            // bPerformGravity
constexpr uint32_t kGrenExplodeContact = 0x8;     // m_explodeOnContact (percussione: non resta mai a terra)
constexpr uint32_t kGrenPredSliding = 0x40;       // m_predictionIsSliding
constexpr uint32_t kGrenSliding = 0x100;          // m_isSliding (ferma o che scivola a terra)
constexpr uint32_t kGrenJustBounced = 0x200;      // m_justBouncedInSim
constexpr uint32_t kGrenExploded = 0x1000;        // m_exploded
constexpr uintptr_t kOffGrenLifeRoll = 0x430, kOffGrenLifeThrow = 0x434;  // miccia (s), rotolata / lanciata
constexpr uintptr_t kOffGrenStartSpeedThrow = 0x458;                     // velocita' di lancio dell'IA (frag 1000 unita'/s)
constexpr uintptr_t kOffGrenPredVel = 0x4C8, kOffGrenPredLoc = 0x4D4;    // previsione dei rimbalzi (DoPrediction)
constexpr uintptr_t kOffGrenPredTime = 0x4E0, kOffGrenPredBounces = 0x4E4, kOffGrenStartTime = 0x4E8;
constexpr uintptr_t kOffGrenBounces = 0x518;      // m_numBounces
constexpr uintptr_t kOffGrenCauser = 0x5B4, kOffGrenCauserPawn = 0x5C8, kOffGrenCauserPri = 0x5CC;
constexpr uintptr_t kOffGrenStuckPawn = 0x644;    // m_stuckToPawn (adesiva attaccata a qualcuno)
constexpr uint32_t kCollideWorld = 0x10000000;    // Actor +0xA0 bCollideWorld
constexpr uintptr_t kOffCtrlPri = 0x1FC;          // Controller.PlayerReplicationInfo
constexpr uintptr_t kOffGrenKillCount = 0x4EC;    // m_killCount: uccisioni contate da ScoreKill, solo con un YPlayerController come m_causer
constexpr uintptr_t kOffPawnLastDamage = 0x1EA0;  // YGamePawn.m_lastDamageinfo (YS_LastDamageInfo): +0xC m_hitByEnemy (bit 0), +0x28 m_damageType
constexpr uintptr_t kOffWorldLevel = 0x50;        // UWorld: PersistentLevel (gli attori creati in gioco)
constexpr uintptr_t kOffLevelActors = 0x3C;       // ULevel.Actors (TArray; i nuovi in coda)
constexpr uint8_t kPhysFalling = 2;
constexpr float kGrenGroundSpeed = 200.0f;        // unita'/s (4 m/s): sotto, a terra
constexpr float kGrenHoldCap = 2000.0f;           // 40 m/s al piu' verso il palmo
constexpr float kGrenMarkRange = 8.0f;            // m da Walker: angoli rossi sulla granata a terra piu' vicina
constexpr float kGrenMinFuse = 1.0f;              // s: sotto, niente angoli ne' presa (revisione: esploderebbe in mano)
constexpr double kGrenLostGrace = 0.2;            // s: mano persa (tracciamento) con la granata, resta dov'era il palmo

struct LiveGrenade {
    void* actor = nullptr;
    ObjId id;
    void* mesh = nullptr;      // m_projSkelMesh: limiti per la zona e gli angoli
    void* thrower = nullptr;   // Instigator alla scoperta
    float pos[3] = {};
    double posT = 0.0;
    float speed = 0.0f;        // unita'/s dagli spostamenti tra due tick
    double seenAt = 0.0;
    bool ground = false;
    bool groundLogged = false;
    bool shortFuse = false;    // a terra con meno di kGrenMinFuse s: niente angoli ne' presa
    bool fuseLogged = false;
    bool back = false;         // rilanciata (o lasciata) dalla mano di Walker
    double backAt = 0.0;
    float backFrom[3] = {};
    float backV[3] = {};
    int checkTicks = 0;        // tick al controllo della velocita' dopo il rilancio
    double boomAt = 0.0;       // rilanciata ed esplosa: la riga nel log 0,5 s dopo (conti del gioco finiti)
};
std::vector<LiveGrenade> g_grenades;  // al piu' 8

struct GrenadeHeld {
    void* actor = nullptr;
    ObjId id;
    void* mesh = nullptr;
    uint32_t savedA0 = 0;      // Actor +0xA0 alla presa (bCollideWorld)
    double since = 0.0;
    float fuse = -1.0f;        // miccia rimasta alla presa (s)
    int32_t walkerHp = 0;      // vita di Walker alla presa (esplosione in mano)
    int ticks = 0;
    float errSum = 0.0f, errMax = 0.0f;  // distanza granata - palmo a ogni tick, prima della correzione (unita')
    float lastPalm[3] = {0, 0, 0};       // palmo dell'ultimo tick con la mano (mano persa: resta li')
    double lostAt = -1.0;                // mano persa da quando (-1: c'e')
    // Strada vera della granata (revisione): l'ultimo punto raggiunto senza
    // attraversare il mondo, con una trace a tick dal punto libero di prima.
    // Al rilascio riparte da li' se la mano e' oltre un muro.
    float clear[3] = {0, 0, 0};
    bool clearOk = false;
    int blockedTicks = 0;
} g_gren;

struct GrenadeStats {
    uint32_t seen = 0, ground = 0, taken = 0, thrown = 0, dropped = 0, inHand = 0;
    uint32_t notEnemy = 0, shortFuse = 0;  // revisione: ignorate (non di un nemico), a terra con poca miccia
} g_grenStats;
struct GrenadeIgnored {
    void* actor = nullptr;
    ObjId id;
};
std::vector<GrenadeIgnored> g_grenIgnored;  // granate non di un nemico gia' viste (al piu' 16)

struct GrenadeScanState {
    uint32_t level = 0, num = 0;
    double full = 0.0;
    double nameTry = 0.0;
    int32_t physName = -1, onExplode = -1;
} g_grenScan;
std::unordered_map<uint32_t, uint8_t> g_grenClass;  // classe -> 0 altro, 1 granata da prendere, 2 esclusa
double g_grenGripTried = -1.0;
int g_grenZone = -1;  // indice in g_grenades della granata nella zona della sinistra (questo tick)

uint8_t GrenadeClassKind(uint32_t cls) {
    auto it = g_grenClass.find(cls);
    if (it != g_grenClass.end()) return it->second;
    if (g_grenScan.physName < 0) return 0;
    uint8_t k = 0;
    if (ClassIsA(cls, g_grenScan.physName)) {
        const std::string n = vr::script::ObjectName(reinterpret_cast<void*>(static_cast<uintptr_t>(cls)));
        k = (n.find("Milkor") != std::string::npos || n.find("RPG") != std::string::npos || n.find("Mortar") != std::string::npos) ? 2 : 1;
    }
    if (g_grenClass.size() < 4096) g_grenClass[cls] = k;
    return k;
}

bool GrenadeAlive(void* a, const ObjId& id) {
    if (!ObjAlive(a, id)) return false;
    if (U32(a, kOffActorFlags9C) & (kHidden | kDeleteMe)) return false;
    return !(U32(a, kOffGrenFlags) & kGrenExploded);
}

// Miccia rimasta (s): il timer 'OnExplode' (Rate - Count). -1 se non c'e'.
float GrenadeFuse(void* a) {
    if (g_grenScan.onExplode < 0) return -1.0f;
    const uint32_t data = U32(a, kOffActorTimers), num = U32(a, kOffActorTimers + 4);
    if (!data || num > 16) return -1.0f;
    for (uint32_t i = 0; i < num; ++i) {
        unsigned char t[0x1C];
        if (!SafeCopy(t, reinterpret_cast<void*>(static_cast<uintptr_t>(data + 0x1C * i)), sizeof(t))) break;
        int32_t fn = 0;
        float rate = 0.0f, count = 0.0f;
        std::memcpy(&fn, t + 4, 4);
        std::memcpy(&rate, t + 0xC, 4);
        std::memcpy(&count, t + 0x10, 4);
        if (fn == g_grenScan.onExplode && std::isfinite(rate) && std::isfinite(count)) return (std::max)(0.0f, rate - count);
    }
    return -1.0f;
}

float WorldTime(void* pawn) {
    float t = -1.0f;
    const uint32_t wi = pawn ? U32(pawn, kOffActorWorldInfo) : 0;
    if (wi) SafeCopy(&t, reinterpret_cast<char*>(static_cast<uintptr_t>(wi)) + kOffWorldTimeSeconds, 4);
    return t;
}

std::string GrenadeWho(void* pw) {
    if (!pw || !vr::script::LiveObject(pw)) return "nobody";
    return vr::script::ClassName(pw);
}

// Nuove granate: in coda alla lista degli attori del livello persistente
// (UWorld::SpawnActor aggiunge in fondo; misurato: la frag appena creata e'
// la 195 di 196). Ogni tick gli ultimi attori dopo il conteggio di prima, un
// giro intero ogni 0,5 s (o se la lista si accorcia: pulizia del GC). Niente
// giri di GObjects.
void GrenadeScan(void* walker, double now) {
    if (g_grenScan.physName < 0 || g_grenScan.onExplode < 0) {
        if (now < g_grenScan.nameTry) return;
        g_grenScan.nameTry = now + 1.0;  // NameIndex non tiene in cache i nomi che mancano
        if (g_grenScan.physName < 0) g_grenScan.physName = vr::script::NameIndex("YProj_Physical");
        if (g_grenScan.onExplode < 0) g_grenScan.onExplode = vr::script::NameIndex("OnExplode");
        if (g_grenScan.physName < 0) return;
    }
    const uint32_t world = U32(reinterpret_cast<void*>(kGWorld), 0);
    const uint32_t level = world ? U32(reinterpret_cast<void*>(static_cast<uintptr_t>(world)), kOffWorldLevel) : 0;
    if (!level) return;
    const uint32_t data = U32(reinterpret_cast<void*>(static_cast<uintptr_t>(level)), kOffLevelActors);
    const uint32_t num = U32(reinterpret_cast<void*>(static_cast<uintptr_t>(level)), kOffLevelActors + 4);
    if (!data || num > 100000) return;
    uint32_t start = g_grenScan.num > 4 ? g_grenScan.num - 4 : 0;
    if (level != g_grenScan.level || num < g_grenScan.num || now >= g_grenScan.full) {
        start = 0;
        g_grenScan.full = now + 0.5;
    }
    if (start == 0)  // giro intero: via le ignorate che non ci sono piu' (indirizzi riusati)
        g_grenIgnored.erase(std::remove_if(g_grenIgnored.begin(), g_grenIgnored.end(),
                                           [](const GrenadeIgnored& x) { return !ObjAlive(x.actor, x.id); }),
                            g_grenIgnored.end());
    g_grenScan.level = level;
    g_grenScan.num = num;
    for (uint32_t i = start; i < num && g_grenades.size() < 8; ++i) {
        const uint32_t o = U32(reinterpret_cast<void*>(static_cast<uintptr_t>(data + 4 * i)), 0);
        if (!o) continue;
        void* a = reinterpret_cast<void*>(static_cast<uintptr_t>(o));
        if (GrenadeClassKind(U32(a, kOffClass)) != 1) continue;
        bool known = false;
        for (const LiveGrenade& g : g_grenades) known |= g.actor == a;
        for (const GrenadeIgnored& x : g_grenIgnored) known |= x.actor == a && ObjAlive(a, x.id);
        if (known) continue;
        void* inst = reinterpret_cast<void*>(static_cast<uintptr_t>(U32(a, kOffActorInstigator)));
        if (inst == walker) continue;  // di Walker (lanciate col gesto)
        const uint32_t fl = U32(a, kOffGrenFlags);
        if ((fl & (kGrenExploded | kGrenExplodeContact)) || U32(a, kOffGrenStuckPawn) || (U32(a, kOffActorFlags9C) & (kHidden | kDeleteMe)))
            continue;
        // Solo quelle di un nemico (revisione): le frag di Adams e Lugo
        // (YProj_FragGrenadePlayerSquadAI, Instigator della squadra) e quelle
        // degli script senza Instigator restano al gioco (niente angoli, niente
        // presa: rilanciate diventerebbero di Walker, e una granata di Kismet
        // spostata lascerebbe lo script senza il suo botto dov'era previsto).
        const bool instLive = inst && vr::script::LiveObject(inst);
        if (!instLive || !PawnHostile(walker, inst, U32(inst, kOffClass), now)) {
            if (g_grenIgnored.size() >= 16) g_grenIgnored.erase(g_grenIgnored.begin());
            g_grenIgnored.push_back({a, ObjIdOf(a)});
            ++g_grenStats.notEnemy;
            char b[220];
            std::snprintf(b, sizeof(b), "GRENADE: %s thrown by %s (not an enemy): ignored, left to the game", vr::script::ClassName(a).c_str(),
                          instLive ? GrenadeWho(inst).c_str() : "nobody (no Instigator)");
            vr::log::Line(b);
            continue;
        }
        LiveGrenade g;
        g.actor = a;
        g.id = ObjIdOf(a);
        g.mesh = reinterpret_cast<void*>(static_cast<uintptr_t>(U32(a, kOffProjSkelMesh)));
        g.thrower = inst;
        SafeCopy(g.pos, static_cast<char*>(a) + kOffActorLocation, 12);
        g.posT = now;
        g.seenAt = now;
        g_grenades.push_back(g);
        ++g_grenStats.seen;
        float wl[3] = {0, 0, 0};
        SafeCopy(wl, static_cast<char*>(walker) + kOffActorLocation, 12);
        const float d[3] = {g.pos[0] - wl[0], g.pos[1] - wl[1], g.pos[2] - wl[2]};
        char b[260];
        std::snprintf(b, sizeof(b), "GRENADE: %s thrown by %s (enemy), %.1f m from Walker, fuse %.1f s",
                      vr::script::ClassName(a).c_str(), GrenadeWho(inst).c_str(),
                      std::sqrt(Dot3(d, d)) / vr::control::g_posScale, GrenadeFuse(a));
        vr::log::Line(b);
    }
}

// Granata rilanciata esplosa (0,5 s dopo): dove, la sua m_killCount (le
// uccisioni che il gioco le conta: YProj_Physical.ScoreKill le conta solo con
// un YPlayerController come m_causer, per l'obiettivo delle tre uccisioni con
// una granata) e, per i pawn entro 8 m, la vita e l'ultimo danno ricevuto
// (YS_LastDamageInfo: m_hitByEnemy = colpo dal lato nemico del pawn, cioe' da
// Walker; con la granata non toccata il Controller di chi l'ha lanciata da' 0).
// Pawn.LastHitBy qui non serve: resta None anche dopo un colpo di Walker.
void GrenadeBackReport(void* walker, const LiveGrenade& g) {
    const float* at = g.pos;
    float wl[3] = {0, 0, 0};
    if (walker) SafeCopy(wl, static_cast<char*>(walker) + kOffActorLocation, 12);
    const int32_t kills = ObjAlive(g.actor, g.id) ? static_cast<int32_t>(U32(g.actor, kOffGrenKillCount)) : -1;
    std::string around;
    int dead = 0, byWalker = 0;
    const uint32_t wi = walker ? U32(walker, kOffActorWorldInfo) : 0;
    uint32_t q = wi ? U32(reinterpret_cast<void*>(static_cast<uintptr_t>(wi)), kOffWorldPawnList) : 0;
    for (int k = 0; q && k < 64; ++k, q = U32(reinterpret_cast<void*>(static_cast<uintptr_t>(q)), kOffPawnNextPawn)) {
        void* pw = reinterpret_cast<void*>(static_cast<uintptr_t>(q));
        if (pw == walker) continue;
        float pl[3];
        if (!SafeCopy(pl, static_cast<char*>(pw) + kOffActorLocation, 12)) continue;
        const float d[3] = {pl[0] - at[0], pl[1] - at[1], pl[2] - at[2]};
        const float dm = std::sqrt(Dot3(d, d)) / vr::control::g_posScale;
        if (dm > 8.0f) continue;
        const int32_t hp = static_cast<int32_t>(U32(pw, kOffPawnHealth));
        const bool yPawn = g_yGamePawnName >= 0 && ClassIsA(U32(pw, kOffClass), g_yGamePawnName);
        const bool byEnemy = yPawn && (U32(pw, kOffPawnLastDamage + 0xC) & 1);
        const uint32_t dmgType = yPawn ? U32(pw, kOffPawnLastDamage + 0x28) : 0;
        dead += hp <= 0;
        byWalker += hp <= 0 && byEnemy;
        if (around.size() < 400) {
            char e[160];
            std::snprintf(e, sizeof(e), "%s%s %.1f m health %d (last damage %s%s)", around.empty() ? "" : ", ", vr::script::ClassName(pw).c_str(),
                          dm, hp, dmgType ? vr::script::ObjectName(reinterpret_cast<void*>(static_cast<uintptr_t>(dmgType))).c_str() : "none",
                          byEnemy ? " from his enemy side" : "");
            around += e;
        }
    }
    const float d[3] = {at[0] - wl[0], at[1] - wl[1], at[2] - wl[2]};
    char b[700];
    std::snprintf(b, sizeof(b),
                  "GRENADE: the grenade Walker threw back exploded at (%.0f, %.0f, %.0f), %.1f m from Walker, %.1f s after the throw; m_killCount %d; "
                  "pawns within 8 m: %s; %d dead, %d of them last damaged from their enemy side (Walker)",
                  at[0], at[1], at[2], std::sqrt(Dot3(d, d)) / vr::control::g_posScale, g.boomAt - g.backAt, kills,
                  around.empty() ? "none" : around.c_str(), dead, byWalker);
    vr::log::Line(b);
}

// Ogni tick (thread di gioco): granate nuove, posizione, "a terra".
void GrenadeTick(void* walker, double now) {
    GrenadeScan(walker, now);
    float wl[3] = {0, 0, 0};
    SafeCopy(wl, static_cast<char*>(walker) + kOffActorLocation, 12);
    for (size_t i = 0; i < g_grenades.size();) {
        LiveGrenade& g = g_grenades[i];
        if (!GrenadeAlive(g.actor, g.id)) {
            // Rilanciata: la riga 0,5 s dopo l'esplosione (morti e conti del gioco fatti).
            if (g.back && g.boomAt == 0.0) g.boomAt = now;
            g.ground = false;
            if (g.back && now - g.boomAt < 0.5) {
                ++i;
                continue;
            }
            if (g.back) GrenadeBackReport(walker, g);
            g_grenades.erase(g_grenades.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        float loc[3];
        if (SafeCopy(loc, static_cast<char*>(g.actor) + kOffActorLocation, 12)) {
            const double dtp = now - g.posT;
            const float dd[3] = {loc[0] - g.pos[0], loc[1] - g.pos[1], loc[2] - g.pos[2]};
            if (dtp > 1e-4) g.speed = std::sqrt(Dot3(dd, dd)) / static_cast<float>(dtp);
            std::memcpy(g.pos, loc, sizeof(loc));
            g.posT = now;
        }
        uint8_t phys = 0;
        SafeCopy(&phys, static_cast<char*>(g.actor) + kOffActorPhysics, 1);
        const bool held = g.actor == g_gren.actor;
        g.ground = !held && phys == kPhysFalling && !U32(g.actor, kOffGrenStuckPawn) &&
                   vr::pure::GrenadeOnGround(static_cast<int32_t>(U32(g.actor, kOffGrenBounces)), (U32(g.actor, kOffGrenFlags) & kGrenSliding) != 0,
                                             g.speed, kGrenGroundSpeed);
        if (g.ground && !g.groundLogged) {
            g.groundLogged = true;
            ++g_grenStats.ground;
            const float d[3] = {g.pos[0] - wl[0], g.pos[1] - wl[1], g.pos[2] - wl[2]};
            char b[220];
            std::snprintf(b, sizeof(b), "GRENADE: %s on the ground %.1f s after it was seen, %.1f m from Walker, fuse %.1f s left%s",
                          vr::script::ClassName(g.actor).c_str(), now - g.seenAt, std::sqrt(Dot3(d, d)) / vr::control::g_posScale,
                          GrenadeFuse(g.actor), g.back ? " (thrown back by Walker)" : "");
            vr::log::Line(b);
        }
        // Poca miccia (revisione): niente angoli ne' presa, una riga per granata.
        g.shortFuse = false;
        if (g.ground) {
            const float fuse = GrenadeFuse(g.actor);
            g.shortFuse = !vr::pure::GrenadeFuseGrabbable(fuse, kGrenMinFuse);
            if (g.shortFuse && !g.fuseLogged) {
                g.fuseLogged = true;
                ++g_grenStats.shortFuse;
                char b[220];
                std::snprintf(b, sizeof(b), "GRENADE: %s on the ground with %.2f s of fuse left (under %.1f s): no red corners, no grab",
                              vr::script::ClassName(g.actor).c_str(), fuse, kGrenMinFuse);
                vr::log::Line(b);
            }
        }
        if (g.back && g.checkTicks > 0 && --g.checkTicks == 0) {
            // Velocita' vera qualche tick dopo il rilancio (quella data e' nella riga del rilancio).
            float v[3] = {0, 0, 0};
            SafeCopy(v, static_cast<char*>(g.actor) + kOffActorVelocity, 12);
            const float dd[3] = {g.pos[0] - g.backFrom[0], g.pos[1] - g.backFrom[1], g.pos[2] - g.backFrom[2]};
            char b[220];
            std::snprintf(b, sizeof(b), "GRENADE: thrown-back grenade flies at %.2f m/s (given %.2f) after %.0f ms, %.0f cm from the release point",
                          std::sqrt(Dot3(v, v)) / vr::control::g_posScale, std::sqrt(Dot3(g.backV, g.backV)) / vr::control::g_posScale,
                          (now - g.backAt) * 1000.0, std::sqrt(Dot3(dd, dd)) / vr::control::g_posScale * 100.0f);
            vr::log::Line(b);
        }
        ++i;
    }
}

// La granata a terra piu' vicina al palmo entro la zona (limiti della mesh).
int GrenadeNearest(const float* palm, float zone, float* outDist) {
    int best = -1;
    float bd = zone;
    for (size_t i = 0; i < g_grenades.size(); ++i) {
        const LiveGrenade& g = g_grenades[i];
        if (!g.ground || g.shortFuse || !g.mesh || g.actor == g_gren.actor) continue;
        Prop cur;
        if (!ReadBounds(g.mesh, &cur)) continue;
        const float d = BoxDistance(cur, palm);
        if (d < bd) {
            bd = d;
            best = static_cast<int>(i);
            *outDist = d;
        }
    }
    return best;
}

void GrenadeForgetHeld() {
    g_gren = GrenadeHeld();
    vr::control::g_propReleasedMs[1] = GetTickCount64();
    vr::control::g_propHeld[1] = false;
}

// Grip chiuso nella zona di una granata a terra: in mano. Gravita' e
// collisione col mondo spente (in mano non rimbalza nei muri ne' cade), e
// Instigator = Walker: bBlockedByInstigator e' spento (InitGrenade), quindi
// il cilindro di Walker non la ferma. m_causer resta quello del nemico finche'
// e' in mano: se esplode li' ferisce Walker come una granata nemica.
bool GrenadeGrab(void* pawn, int idx, const float* palm, double now) {
    if (idx < 0 || idx >= static_cast<int>(g_grenades.size())) return false;
    LiveGrenade& g = g_grenades[idx];
    if (!GrenadeAlive(g.actor, g.id)) return false;
    GrenadeHeld h;
    h.actor = g.actor;
    h.id = g.id;
    h.mesh = g.mesh;
    h.savedA0 = U32(g.actor, kOffActorFlagsA0);
    h.since = now;
    h.fuse = GrenadeFuse(g.actor);
    h.walkerHp = static_cast<int32_t>(U32(pawn, kOffPawnHealth));
    std::memcpy(h.lastPalm, palm, sizeof(h.lastPalm));
    std::memcpy(h.clear, g.pos, sizeof(h.clear));  // ferma a terra: il primo punto libero della strada
    h.clearOk = true;
    const uint32_t me = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pawn));
    SafeCopy(static_cast<char*>(g.actor) + kOffActorInstigator, &me, 4);
    const uint32_t fl = U32(g.actor, kOffGrenFlags) & ~kGrenGravity;
    SafeCopy(static_cast<char*>(g.actor) + kOffGrenFlags, &fl, 4);
    const uint32_t a0 = h.savedA0 & ~kCollideWorld;
    SafeCopy(static_cast<char*>(g.actor) + kOffActorFlagsA0, &a0, 4);
    g_gren = h;
    g.ground = false;
    vr::control::g_propHeld[1] = true;
    vr::input::PulseHand(0, 50);
    ++g_grenStats.taken;
    const float d[3] = {g.pos[0] - palm[0], g.pos[1] - palm[1], g.pos[2] - palm[2]};
    char b[260];
    std::snprintf(b, sizeof(b), "GRENADE: left hand took %s (thrown by %s, seen %.1f s ago, fuse %.1f s left, %.0f cm from the palm)",
                  vr::script::ClassName(g.actor).c_str(), GrenadeWho(g.thrower).c_str(), now - g.seenAt, h.fuse,
                  std::sqrt(Dot3(d, d)) / vr::control::g_posScale * 100.0f);
    vr::log::Line(b);
    return true;
}

// La granata in mano non c'e' piu': esplosa (il danno lo fa il gioco: a
// Walker con 1000 di vita e senza god mode e' arrivata a 0, 27/10) o sparita.
// La mano resta vuota. Da ogni strada (guida, rilascio, mano persa, morte).
void GrenadeGoneInHand(void* pawn, double now) {
    GrenadeHeld& h = g_gren;
    const bool boom = ObjAlive(h.actor, h.id) && (U32(h.actor, kOffGrenFlags) & kGrenExploded);
    char b[260];
    if (boom) {
        ++g_grenStats.inHand;
        vr::input::PulseHand(0, 150);  // solo la mano che la teneva
        std::snprintf(b, sizeof(b), "GRENADE: exploded in the left hand after %.2f s (fuse was %.1f s at the grab); Walker's health %d at the grab, %d now",
                      now - h.since, h.fuse, h.walkerHp, pawn ? static_cast<int32_t>(U32(pawn, kOffPawnHealth)) : -1);
    } else {
        std::snprintf(b, sizeof(b), "GRENADE: the grenade in the left hand is gone after %.2f s", now - h.since);
    }
    vr::log::Line(b);
    GrenadeForgetHeld();
}

// In mano, ogni tick: la velocita' che la porta sul palmo nel tick dopo.
void GrenadeDrive(void* pawn, const float* palm, const float* hv, float dt, double now) {
    GrenadeHeld& h = g_gren;
    if (!GrenadeAlive(h.actor, h.id)) {
        GrenadeGoneInHand(pawn, now);
        return;
    }
    float loc[3];
    if (!SafeCopy(loc, static_cast<char*>(h.actor) + kOffActorLocation, 12)) return;
    if (palm != h.lastPalm) std::memcpy(h.lastPalm, palm, sizeof(h.lastPalm));  // mano persa: arriva gia' lastPalm
    // Strada della granata (revisione): dal punto libero di prima a qui. Se la
    // trace urta il mondo la mano e' oltre un muro (o la copertura): il punto
    // libero resta quello di prima finche' la mano non torna di qua. Prima la
    // trace partiva dalla testa: accucciato dietro una copertura con la mano
    // appena sopra il suo bordo la linea testa-mano la tagliava, e la granata
    // ripartiva dalla parte di Walker, contro la copertura.
    if (pawn && h.clearOk) {
        TraceResult tr;
        if (EngineTrace(pawn, h.clear, loc, 0.0f, &tr, 0x2 | 0x4 | 0x80 | 0x2000)) {
            ++h.blockedTicks;
        } else {
            std::memcpy(h.clear, loc, sizeof(h.clear));
            h.blockedTicks = 0;
        }
    }
    const float err[3] = {palm[0] - loc[0], palm[1] - loc[1], palm[2] - loc[2]};
    const float e = std::sqrt(Dot3(err, err));
    ++h.ticks;
    h.errSum += e;
    h.errMax = (std::max)(h.errMax, e);
    float v[3];
    vr::pure::GrenadeHoldVel(err, dt, hv, kGrenHoldCap, v);
    SafeCopy(static_cast<char*>(h.actor) + kOffActorVelocity, v, 12);
    // Ogni tick: la simulazione non deve rimettere gravita' o collisione.
    const uint32_t fl = U32(h.actor, kOffGrenFlags) & ~kGrenGravity;
    SafeCopy(static_cast<char*>(h.actor) + kOffGrenFlags, &fl, 4);
    const uint32_t a0 = U32(h.actor, kOffActorFlagsA0) & ~kCollideWorld;
    SafeCopy(static_cast<char*>(h.actor) + kOffActorFlagsA0, &a0, 4);
    if (vr::control::g_grabLog > 0) {
        vr::control::g_grabLog.fetch_sub(1);
        char b[240];
        std::snprintf(b, sizeof(b), "GRENLOG: palm %.1f cm from the grenade, velocity (%.0f, %.0f, %.0f), fuse %.2f s%s", e / vr::control::g_posScale * 100.0f,
                      v[0], v[1], v[2], GrenadeFuse(h.actor), h.blockedTicks ? ", beyond the world on its path (blocked)" : "");
        vr::log::Line(b);
    }
}

// Mano aperta (o mano persa, prese spente): la granata parte con la
// velocita' della mano ("grip") o cade. Da qui e' di Walker: Instigator,
// InstigatorController, m_causer (InitCauser), m_causerPawn e il suo PRI;
// m_numBounces a 0, gravita' e collisione col mondo di nuovo accese, e la
// previsione dei rimbalzi riparte dal punto di rilascio (avvisi all'IA e
// "granata!" del nemico piu' vicino al punto d'arrivo, come per le granate
// di Walker).
void GrenadeRelease(void* pawn, const char* why, double now) {
    GrenadeHeld& h = g_gren;
    if (!h.actor) return;
    // Dal watchdog arriva l'ultimo Walker visto: solo se e' ancora vivo (un
    // m_causer spazzatura lo userebbe il gioco all'esplosione).
    if (pawn && !vr::script::LiveObject(pawn)) pawn = nullptr;
    if (!GrenadeAlive(h.actor, h.id)) {
        GrenadeGoneInHand(pawn, now);
        return;
    }
    void* a = h.actor;
    // Velocita' (revisione della voce 8, 27/10): lo stesso modello del lancio
    // degli oggetti (Release). throwmode peak: PeakThrowVel, il picco degli
    // ultimi ~110 ms rispetto a Walker (chi lancia frena prima di aprire la
    // mano: la media di 60 ms al rilascio, usata prima, la lasciava cadere
    // vicino a Walker), con la leva del centro della granata; classic: la
    // media di 60 ms meno FrameVelocity. throwboost solo sulla parte della
    // mano, poi la velocita' di Walker di adesso (prima la corsa x1,3).
    // Anche "disabled" (mossa speciale, scena, prese spente) la lascia con la
    // velocita' della mano: niente granata ferma ai piedi dopo uno scavalco.
    const bool moving = std::strcmp(why, "grip") == 0 || std::strcmp(why, "disabled") == 0;
    float v[3] = {0, 0, 0}, hand[3] = {0, 0, 0}, base[3] = {0, 0, 0};
    const char* model = "none";
    ThrowPeak tp;
    if (moving && pawn && vr::control::g_throwMode.load() != 0) {
        Held tmp;  // solo per la leva: la mesh della granata (limiti) e niente altro
        tmp.comp = h.mesh;
        if (PeakThrowVel(1, tmp, &tp)) {
            if (tp.rel) {
                std::memcpy(hand, tp.v, sizeof(hand));
                std::memcpy(base, tp.base, sizeof(base));
            } else {
                PawnVelocity(base);  // storia nel mondo (pawnframe 0): la mano e' quella meno Walker
                for (int c = 0; c < 3; ++c) hand[c] = tp.v[c] - base[c];
            }
            model = "peak";
        }
    }
    if (moving && pawn && std::strcmp(model, "none") == 0 && HandVelocity(1, 0.06, hand)) {
        FrameVelocity(base);
        for (int c = 0; c < 3; ++c) hand[c] -= base[c];
        model = "60 ms average";
    }
    const bool thrown = std::strcmp(model, "none") != 0;
    float startSpeed = 0.0f;
    SafeCopy(&startSpeed, static_cast<char*>(a) + kOffGrenStartSpeedThrow, 4);
    if (!(startSpeed > 100.0f && startSpeed < 5000.0f)) startSpeed = 1000.0f;
    const float cap = 1.3f * startSpeed;
    const float speed = thrown ? vr::pure::GrenadeThrowVel(hand, base, vr::control::g_throwBoost.load(), cap, v) : 0.0f;
    // Nel log e nei conti: rilanciata se la mano la spinge (almeno 0,5 m/s rispetto a Walker), se no lasciata.
    const bool tossed = thrown && std::sqrt(Dot3(hand, hand)) * vr::control::g_throwBoost.load() >= 0.5f * vr::control::g_posScale;
    float loc[3] = {0, 0, 0};
    SafeCopy(loc, static_cast<char*>(a) + kOffActorLocation, 12);
    // In mano non urta il mondo: se la mano e' oltre un muro (la strada della
    // granata dall'ultimo punto libero lo attraversa) riparte dal muro, dalla
    // parte da cui e' arrivata. Senza strada (mai successo: la presa la
    // comincia) la trace di prima, dalla testa.
    bool moved = false;
    if (pawn) {
        float from[3];
        const bool viewOk = ProfMs() * 0.001 - g_viewOriginAt < 0.5;
        if (h.clearOk) {
            std::memcpy(from, h.clear, sizeof(from));
        } else if (viewOk) {
            std::memcpy(from, g_viewOrigin, sizeof(from));
        } else {
            SafeCopy(from, static_cast<char*>(pawn) + kOffActorLocation, 12);
            from[2] += 30.0f;
        }
        TraceResult tr;
        if (EngineTrace(pawn, from, loc, 0.0f, &tr, 0x2 | 0x4 | 0x80 | 0x2000)) {
            const float d[3] = {loc[0] - from[0], loc[1] - from[1], loc[2] - from[2]};
            const float l = std::sqrt(Dot3(d, d));
            if (l > 1e-3f) {
                for (int c = 0; c < 3; ++c) loc[c] = tr.loc[c] - d[c] / l * 6.0f;
                SafeCopy(static_cast<char*>(a) + kOffActorLocation, loc, 12);
                moved = true;
            }
        }
    }
    SafeCopy(static_cast<char*>(a) + kOffActorVelocity, v, 12);
    const int32_t zero = 0;
    SafeCopy(static_cast<char*>(a) + kOffGrenBounces, &zero, 4);
    const uint32_t fl = (U32(a, kOffGrenFlags) | kGrenGravity) & ~(kGrenSliding | kGrenJustBounced | kGrenPredSliding);
    SafeCopy(static_cast<char*>(a) + kOffGrenFlags, &fl, 4);
    const uint32_t a0 = U32(a, kOffActorFlagsA0) | (h.savedA0 & kCollideWorld);
    SafeCopy(static_cast<char*>(a) + kOffActorFlagsA0, &a0, 4);
    uint32_t pc = pawn ? U32(pawn, kOffPawnController) : 0;
    if (pc && !vr::script::LiveObject(reinterpret_cast<void*>(static_cast<uintptr_t>(pc)))) pc = 0;
    bool causerOk = false;
    if (pc) {
        const uint32_t me = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pawn));
        SafeCopy(static_cast<char*>(a) + kOffActorInstigator, &me, 4);
        SafeCopy(static_cast<char*>(a) + kOffProjInstCtrl, &pc, 4);
        struct {
            uint32_t c;
        } ic = {pc};
        vr::script::CallFunction(a, "InitCauser", &ic);
        causerOk = U32(a, kOffGrenCauser) == pc;
        if (!causerOk) SafeCopy(static_cast<char*>(a) + kOffGrenCauser, &pc, 4);  // la chiamata non e' passata: a mano
        SafeCopy(static_cast<char*>(a) + kOffGrenCauserPawn, &me, 4);
        const uint32_t pri = U32(reinterpret_cast<void*>(static_cast<uintptr_t>(pc)), kOffCtrlPri);
        SafeCopy(static_cast<char*>(a) + kOffGrenCauserPri, &pri, 4);
        // Previsione dei rimbalzi da qui (DoPrediction riparte: si ferma solo
        // oltre la miccia, m_currentPredictionTime > m_lifeTimeThrow).
        uint8_t mode = 2;
        SafeCopy(&mode, static_cast<char*>(a) + kOffProjMode, 1);
        float life = 0.0f, st = 0.0f;
        SafeCopy(&life, static_cast<char*>(a) + (mode == 1 ? kOffGrenLifeRoll : kOffGrenLifeThrow), 4);
        SafeCopy(&st, static_cast<char*>(a) + kOffGrenStartTime, 4);
        const float pt = vr::pure::GrenadePredTime(WorldTime(pawn), st, life);
        SafeCopy(static_cast<char*>(a) + kOffGrenPredLoc, loc, 12);
        SafeCopy(static_cast<char*>(a) + kOffGrenPredVel, v, 12);
        SafeCopy(static_cast<char*>(a) + kOffGrenPredTime, &pt, 4);
        SafeCopy(static_cast<char*>(a) + kOffGrenPredBounces, &zero, 4);
    }
    for (LiveGrenade& g : g_grenades) {
        if (g.actor != a) continue;
        g.back = true;
        g.backAt = now;
        std::memcpy(g.backFrom, loc, sizeof(loc));
        std::memcpy(g.backV, v, sizeof(v));
        g.checkTicks = tossed ? 3 : 0;
        g.groundLogged = false;
    }
    if (tossed) ++g_grenStats.thrown;
    else ++g_grenStats.dropped;
    const float sc = vr::control::g_posScale;
    char peak[200] = "";
    if (std::strcmp(model, "peak") == 0)
        std::snprintf(peak, sizeof(peak), "; peak %.2f m/s %.0f ms before release, %.2f m/s at release (60 ms average), peak weight %.2f%s",
                      tp.handPeak / sc, tp.age * 1000.0, tp.handAvg / sc, tp.blend, tp.rel ? "" : ", history in the world");
    char b[760];
    std::snprintf(b, sizeof(b),
                  "GRENADE: left hand %s %s (%s) at %.1f m/s (hand %.2f m/s relative to Walker, %s, x throwboost %.2f, at most %.0f m/s, + Walker %.2f m/s)%s, "
                  "fuse %.1f s left, held %.2f s, palm-grenade distance %.1f cm on average, %.1f at most over %d ticks; now Walker's (InitCauser %s)%s",
                  tossed ? "threw back" : "let go of", vr::script::ClassName(a).c_str(), why, speed / sc, std::sqrt(Dot3(hand, hand)) / sc, model,
                  vr::control::g_throwBoost.load(), cap / sc, std::sqrt(Dot3(base, base)) / sc, peak, GrenadeFuse(a), now - h.since,
                  h.ticks ? h.errSum / h.ticks / sc * 100.0f : 0.0f, h.errMax / sc * 100.0f, h.ticks,
                  !pc ? "skipped, no controller" : (causerOk ? "ok" : "failed, written directly"),
                  moved ? "; the hand was beyond the world on the grenade's path: it starts from there, on its side" : "");
    vr::log::Line(b);
    GrenadeForgetHeld();
}

void GrenadeReset() {
    g_grenades.clear();
    g_grenIgnored.clear();
    g_gren = GrenadeHeld();
    g_grenScan.level = g_grenScan.num = 0;
    g_grenScan.full = 0.0;
    g_grenClass.clear();  // classi del livello vecchio
    g_grenZone = -1;
}

// --- Cadaveri spinti e trascinati (voce 11 dei combattimenti, 27/10) ---------
// I nemici morti sono YGamePawn in stato Dying con la mesh in ragdoll
// (Physics 10, i corpi del PhysicsAsset nel PhysicsAssetInstance: 16 per i
// predoni). Letto nel bytecode e misurato sul manichino ucciso (27/10):
// - Dying.Tick congela il cadavere quando PhysX lo addormenta, oltre 3 s dalla
//   morte (TimeOfDeath +0x8BC) e fuori dalle mosse 36/37: m_ragdollIsDisabled
//   (+0x5E8 bit 0x20000000), FinishKill (timer DisableMeshUpdates 0,1 s, la
//   native 0x009B4710 -> vtable +0x7C8: PhysicsAssetInstance tolto,
//   bNoSkeletonUpdate) e CreateBloodPool (una pozza di sangue sotto il
//   bacino). Il manichino: congelato 0,4 s dopo essersi fermato, PAI 0.
// - YGamePawn.ConditionalReanimateRagdoll (script, iNative 0) lo rimette
//   fisico: con m_deathDBNOState (+0x48A) 0 o 4 e m_ragdollIsDisabled fa
//   SetHasPhysicsAssetInstance(true), SetComponentRBFixed(false),
//   WakeRigidBody, bNoSkeletonUpdate spento (i corpi nascono nel tick stesso,
//   misurato). Il gioco la chiama da solo (YDestructibleObject.
//   CheckSleepingObjectReactivation su tutti i pawn vicini, YSeqAct_WakeKActors).
//   Con m_deathDBNOState 1-3 (morte "a terra", il nemico striscia ancora) la
//   stessa funzione lo farebbe morire: quei pawn non si toccano mai.
// - Fermo, il gioco lo ricongela da solo (un'altra pozza di sangue dove sta).
// Spinta: la sfera della mano libera (fase 3) urta i corpi del ragdoll come
// un oggetto; un cadavere congelato non ha corpi, quindi la mano sinistra
// che gli arriva addosso (entro 10 cm da un arto, dalle forme sulle ossa,
// oltre 0,3 m/s) lo rianima, e resta sveglio finche' la mano gli sta entro 30 cm (WakeRigidBody
// ogni 0,3 s): niente congelamenti a meta' spinta. Al piu' una rianimazione al
// secondo per cadavere.
// Trascinamento (la sinistra, grip): il corpo del ragdoll con la superficie
// piu' vicina al palmo, entro grabzone (le forme del PhysicsAsset: capsule,
// box, sfere negli assi del corpo; per un congelato le stesse forme sulle
// sue ossa, e si rianima alla stretta). Il punto preso (la superficie piu' vicina, o il palmo se e' gia'
// dentro) vive negli assi del corpo (NxActor::getGlobalPose). A ogni tick,
// come le portiere (ConstrainedDrive), dalla velocita' voluta del punto (mano
// + errore in 80 ms, al piu' 1,5 m/s) meno quella misurata dallo spostamento:
// il corpo intero vorrebbe massa totale x dv, al piu' forza x dt (il peso);
// il corpo preso ne riceve la sua parte (AddImpulse nel punto, sul suo osso)
// e il resto, solo orizzontale, va al bacino (la trazione che il braccio
// passa al busto: CorpseImpulseSplit in pure_utils.h). La forza e' quella
// della mano del peso (griptune force, 200 N: full x1, light x2, off x4); se
// il punto non viene (errore oltre 10 cm che non cala) raddoppia ogni 0,4 s
// fino a x4, e torna normale quando arriva (entro 6 cm): pesante, mai
// immobile (regola del piano Boneworks; CorpsePullStep). Oltre 40 cm dal
// palmo per 0,3 s la mano lo lascia. Dita a pugno; la mano disegnata
// (solidhands) sul punto preso; coppie della sfera sinistra con i corpi del
// cadavere ignorate finche' la mano lo tiene, e mentre si avvicina piano
// (sotto 1 m/s) a un suo corpo nella zona di presa. Solo nemici (mai la
// squadra, i personaggi della storia o i civili: PawnHostile). Mai i pawn
// vivi (Health > 0) ne' quelli che stanno morendo (morte "a terra", mossa
// speciale in corso, meno di 1 s dalla morte, corpi fissi, congelamento non
// ancora finito): ogni tick si ricontrolla tutto e alla prima differenza la
// mano lascia.
constexpr uintptr_t kOffPawnDbnoState = 0x48A;     // YGamePawn.m_deathDBNOState (byte)
constexpr uintptr_t kOffPawnFlags5E8 = 0x5E8;      // YGamePawn: bit di m_ragdollIsDisabled e bScalingToZero
constexpr uint32_t kRagdollDisabled = 0x20000000;  // m_ragdollIsDisabled: cadavere congelato dal gioco
constexpr uint32_t kScalingToZero = 0x80000000;    // bScalingToZero: il cadavere si rimpicciolisce e sparisce
constexpr uintptr_t kOffPawnTimeOfDeath = 0x8BC;   // YGamePawn.TimeOfDeath (WorldInfo.TimeSeconds)
constexpr uintptr_t kOffSkelFlags3C4 = 0x3C4;      // SkeletalMeshComponent: bSkelCompFixed 0x8000 (corpi fissi)
constexpr uint32_t kSkelFixed = 0x8000;
constexpr uintptr_t kOffBodyIndex = 0x40;          // RB_BodyInstance.BodyIndex (in PhysicsAsset.BodySetup)
constexpr float kCorpseRange = 4.0f;               // m da Walker: cadaveri candidati
constexpr float kCorpseSlip = 40.0f;               // cm dal palmo per kCorpseSlipT: la mano lascia
constexpr double kCorpseSlipT = 0.3;
// m/s: velocita' voluta del punto preso al piu'. Con 3 m/s (prima prova,
// 27/10) uno strappo della mano a 3 m/s trascinava il manichino (30 kg di
// PhysX) per 58 cm e, scivolato dalla mano, lo lanciava a 2 m: un corpo
// non si strattona. Con 1,5 la mano piu' veloce lo perde (oltre 40 cm per
// 0,3 s) e lui si ferma poco dopo.
constexpr float kCorpseVMax = 1.5f;
constexpr double kCorpseDeadFor = 1.0;             // s dalla morte prima di toccarlo
constexpr int kMaxCorpseBodies = 32;

struct CorpseCand {
    void* pawn = nullptr;
    ObjId id;
    void* mesh = nullptr;
    bool frozen = false;
};
std::vector<CorpseCand> g_corpses;  // cadaveri entro kCorpseRange, rifatti ogni 0,25 s (CorpseScan)
double g_corpseScanAt = 0.0;
// Revisione della voce 11: un cadavere congelato non ha corpi PhysX, ma la
// posa resta (bNoSkeletonUpdate). Le sue forme stanno sulle ossa
// (GetBoneLocation / GetBoneQuaternion, le chiamate della verifica della
// posa), lette una volta per congelamento (FrozenShapes). Prima la zona di un
// congelato era il riquadro della mesh intera, con molto vuoto tra gli arti:
// angoli azzurri, e al grip una rianimazione (con una pozza di sangue in piu'
// quando torna a riposo) per una presa nel vuoto.
struct FrozenBody {
    uint32_t setup = 0;  // RB_BodySetup (forme)
    float p[3] = {0, 0, 0};
    float R[3][3] = {};  // assi del corpo (righe), come CorpseBodyPose
};
struct FrozenPose {
    void* pawn = nullptr;
    ObjId id;
    bool ok = false;  // ossa lette; se no, la zona torna il riquadro della mesh
    std::vector<FrozenBody> bodies;
};
std::vector<FrozenPose> g_frozenPoses;  // solo i congelati dell'ultima CorpseScan
struct CorpseHeld {
    void* pawn = nullptr;
    ObjId id;
    void* mesh = nullptr;
    uint32_t pai = 0, bi = 0;  // PhysicsAssetInstance e RB_BodyInstance alla presa (il gioco li toglie congelandolo)
    void* nx = nullptr;
    int body = -1;
    int32_t bone[2] = {0, 0};        // osso del corpo preso (AddImpulse, nel log)
    float grabLocal[3] = {0, 0, 0};  // punto preso negli assi del corpo (unita')
    float mass = 0.0f;               // massa del corpo preso (unita' di PhysX)
    float totalMass = 0.0f;          // tutto il ragdoll (unita' di PhysX)
    float totalKg = 0.0f;            // tutto il ragdoll (log)
    int32_t rootBone[2] = {0, 0};    // osso del corpo radice (Bodies[0], il bacino): la trazione orizzontale
    float pt[3] = {0, 0, 0};         // punto preso nel mondo all'ultimo tick (mano disegnata, SolidSource)
    bool ptValid = false;
    float prevPt[3] = {0, 0, 0};
    bool hasPrev = false;
    float root0[3] = {0, 0, 0};  // radice del ragdoll alla presa (spostamento nel log)
    bool hasRoot0 = false;
    double since = 0.0, stuckSince = -1.0, growAt = -1.0, wakeAt = 0.0;
    float mul = 1.0f, maxMul = 1.0f, lastEl = 0.0f, errSum = 0.0f, errMax = 0.0f, forceN = 0.0f;
    int ticks = 0;
} g_corpse;
struct CorpseStats {
    uint32_t woken = 0, grabs = 0, slips = 0, lost = 0, letGo = 0;
    uint32_t capped = 0;   // tick con corpi rallentati dopo una spinta (CorpsePushCap)
    float moved = 0.0f;    // cm: spostamento piu' grande della radice in una presa
    float fastest = 0.0f;  // m/s: corpo piu' veloce rallentato
} g_corpseStats;
std::vector<std::pair<void*, double>> g_corpseWake;  // pawn -> niente rianimazione fino a qui
std::pair<void*, double> g_corpseLastWake{nullptr, 0.0};  // ultima rianimazione riuscita (log della spinta)
int g_corpseTestReq = 0;                              // "corpsetest" in coda (1 presa, 2 "bones"; sotto g_spawnMutex)
double g_corpseGripTried = -1.0;                      // chiusura del grip gia' usata su un cadavere
bool g_corpsePoseOk = false;                          // getGlobalPose verificato sull'osso (una volta per sessione)
int g_nxCols = -1;                                    // assi del corpo: colonne (1) o righe (0) della NxMat33

// NxActor::getGlobalPose (slot 9, +0x24), NxMat34 per puntatore nascosto come
// getLinearVelocity: ordine di NxActor.h (PhysX 2.8) dopo getScene (1):
// saveToDesc, setName, getName, i quattro setGlobal*, getGlobalPose (9), ...;
// setMass e' il 45 come gia' verificato (voce 6). Verificato nel gioco prima
// di ogni altro uso (CorpsePoseCheck, una volta per sessione: la posa del
// corpo coincide con quella del suo osso; 27/10 sul manichino: 0,0 cm, assi
// 3,000 su 3 come colonne della NxMat33), e la vtable deve stare nello stesso
// modulo (PhysXCore.dll).
struct NxMat34 {
    float m[3][3];
    float t[3];
};
using NxGetMat34 = void*(__thiscall*)(void*, NxMat34*);

bool NxPoseRaw(void* nx, NxMat34* out) {
    __try {
        void** vt = *static_cast<void***>(nx);
        reinterpret_cast<NxGetMat34>(vt[9])(nx, out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Vtable di un NxActor con lo slot 9 nello stesso modulo (una volta per vtable).
bool NxPoseSlotOk(void* nx) {
    static uint32_t okVt = 0;
    uint32_t vt = 0, s9 = 0;
    if (!nx || !TrySafeReadUint32(nx, &vt) || !vt) return false;
    if (vt == okVt) return true;
    if (!TrySafeReadUint32(reinterpret_cast<char*>(static_cast<uintptr_t>(vt)) + 9 * 4, &s9) ||
        !SameModule(reinterpret_cast<void*>(static_cast<uintptr_t>(vt)), reinterpret_cast<void*>(static_cast<uintptr_t>(s9))))
        return false;
    okVt = vt;
    return true;
}

// Posa del corpo in unita' (assi = righe: R[k] e' l'asse k del corpo nel
// mondo). Solo dopo la verifica (CorpsePoseCheck); falso se i numeri non
// sono una rotazione.
bool CorpseBodyPose(void* nx, float pos[3], float R[3][3]) {
    if (!g_corpsePoseOk || !NxPoseSlotOk(nx)) return false;
    NxMat34 M = {};
    if (!NxPoseRaw(nx, &M)) return false;
    const bool cols = g_nxCols != 0;
    for (int k = 0; k < 3; ++k)
        for (int j = 0; j < 3; ++j) R[k][j] = cols ? M.m[j][k] : M.m[k][j];
    for (int k = 0; k < 3; ++k) pos[k] = M.t[k] * 50.0f;
    const float det = Det3(R);
    return std::isfinite(pos[0] + pos[1] + pos[2] + det) && det > 0.9f && det < 1.1f;
}

// --- Forme dei corpi (PhysicsAsset) ---
// RB_BodySetup eredita KMeshProps.AggGeom (+0x48): TArray di KSphereElem,
// KBoxElem, KSphylElem (+0x48, +0x54, +0x60), passo 80 byte (FMatrix TM
// allineata a 16; KSphylElem 0x4C, KBoxElem 0x50 letti dalle ScriptStruct).
// TM (righe: assi x, y, z, origine) negli assi del corpo (l'osso), unita'
// x la scala della mesh. Sfera: Radius +0x40; box: X, Y, Z +0x40 (misure
// intere); capsula: Radius +0x40, Length +0x44 (il cilindro, lungo z).
// Letto sul manichino (CH_ENEMY_SK_PHYS, 16 corpi): bacino capsula r 6, busto
// r 8,5-9, braccia r 3-3,9 lunghe 9-10, gambe r 4,3-5 lunghe 15-16, mani e
// piedi box, testa tre capsule.
using CorpseShape = vr::pure::BodyShape;  // forme e distanza in pure_utils.h (collaudate)
std::unordered_map<uint32_t, std::vector<CorpseShape>> g_corpseShapes;  // RB_BodySetup -> forme (scala 1)

const std::vector<CorpseShape>& BodyShapes(uint32_t setup) {
    auto it = g_corpseShapes.find(setup);
    if (it != g_corpseShapes.end()) return it->second;
    std::vector<CorpseShape> out;
    for (int kind = 0; kind < 3; ++kind) {
        struct {
            uint32_t data;
            int32_t num, max;
        } arr = {};
        if (!SafeCopy(&arr, reinterpret_cast<char*>(static_cast<uintptr_t>(setup)) + 0x48 + 12 * kind, sizeof(arr)) || !arr.data || arr.num <= 0 ||
            arr.num > 8)
            continue;
        for (int i = 0; i < arr.num; ++i) {
            float e[19];
            if (!SafeCopy(e, reinterpret_cast<char*>(static_cast<uintptr_t>(arr.data)) + 80 * i, sizeof(e))) continue;
            CorpseShape s;
            s.kind = kind;
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) s.ax[r][c] = e[4 * r + c];
            for (int c = 0; c < 3; ++c) s.o[c] = e[12 + c];
            if (kind == 0) s.a = e[16];
            if (kind == 1) {
                s.a = e[16] * 0.5f;
                s.b = e[17] * 0.5f;
                s.c = e[18] * 0.5f;
            }
            if (kind == 2) {
                s.a = e[16];
                s.b = e[17] * 0.5f;
            }
            // Forme non sensate (raggi negativi o enormi, assi non unitari): saltate.
            const float axl = Dot3(s.ax[0], s.ax[0]) + Dot3(s.ax[1], s.ax[1]) + Dot3(s.ax[2], s.ax[2]);
            if (!(s.a > 0.1f && s.a < 100.0f) || !(s.b >= 0.0f && s.b < 100.0f) || !(s.c >= 0.0f && s.c < 100.0f) ||
                !std::isfinite(s.o[0] + s.o[1] + s.o[2]) || !(axl > 2.9f && axl < 3.1f))
                continue;
            out.push_back(s);
        }
    }
    if (g_corpseShapes.size() > 512) g_corpseShapes.clear();
    return g_corpseShapes[setup] = out;
}

float ShapeDist(const CorpseShape& s, float sc, const float q[3], float nearOut[3]) { return vr::pure::BodyShapeDist(s, sc, q, nearOut); }

// Scala delle forme: mesh (Scale x Scale3D.X) x attore (DrawScale x DrawScale3D.X); 1 se non sensata.
float CorpseScale(void* pw, void* mesh) {
    float ms = 1.0f, ms3 = 1.0f, as = 1.0f, as3 = 1.0f;
    SafeCopy(&ms, static_cast<char*>(mesh) + 0x1D8, 4);
    SafeCopy(&ms3, static_cast<char*>(mesh) + 0x1DC, 4);
    SafeCopy(&as, static_cast<char*>(pw) + 0x6C, 4);
    SafeCopy(&as3, static_cast<char*>(pw) + 0x70, 4);
    const float s = ms * ms3 * as * as3;
    return std::isfinite(s) && s > 0.3f && s < 3.0f ? s : 1.0f;
}

// 0 no (why), 1 ragdoll fisico, 2 congelato dal gioco (senza corpi). Per i
// cadaveri soltanto: mai un pawn vivo ne' uno che sta morendo.
int CorpseState(void* walker, void* pw, float worldT, void** meshOut, const char** why) {
    *why = "";
    *meshOut = nullptr;
    if (!pw || pw == walker || !ActorAlive(pw)) {
        *why = "gone";
        return 0;
    }
    if (U32(pw, kOffActorFlags9C) & (kHidden | kDeleteMe)) {
        *why = "hidden or deleted";
        return 0;
    }
    const uint32_t cls = U32(pw, kOffClass);
    if (!(g_yGamePawnName >= 0 && cls && ClassIsA(cls, g_yGamePawnName))) {
        *why = "not a YGamePawn";
        return 0;
    }
    if (static_cast<int32_t>(U32(pw, kOffPawnHealth)) > 0) {
        *why = "alive";
        return 0;
    }
    // Solo nemici (revisione della voce 11): la regola dei bersagli
    // (PawnHostile: m_squadMemberType, squadra, classe da YPawn_Enemy). Mai un
    // personaggio della squadra o della storia (Lugo, Adams, Riggs, Gould,
    // YPawn_LugoCanDie del linciaggio), mai i civili (YPawn_Civilian) ne' i
    // pawn delle scene. Un morto di solito non ha piu' controller ne' PRI:
    // decide la classe. In cache per pawn (2 s), come per i colpi.
    if (!PawnHostile(walker, pw, cls, ProfMs() * 0.001)) {
        *why = "not an enemy (squad, story character or civilian)";
        return 0;
    }
    uint8_t dbno = 0xFF, sm = 0xFF, phys = 0;
    SafeCopy(&dbno, static_cast<char*>(pw) + kOffPawnDbnoState, 1);
    SafeCopy(&sm, static_cast<char*>(pw) + kOffPawnSpecialMove, 1);
    SafeCopy(&phys, static_cast<char*>(pw) + kOffActorPhysics, 1);
    if (dbno != 0 && dbno != 4) {
        *why = "still dying on the ground (death DBNO)";
        return 0;
    }
    const uint32_t f5e8 = U32(pw, kOffPawnFlags5E8);
    if (f5e8 & kScalingToZero) {
        *why = "vanishing";
        return 0;
    }
    if (phys != kPhysRigidBody) {
        *why = "not a ragdoll";
        return 0;
    }
    float tod = 0.0f;
    SafeCopy(&tod, static_cast<char*>(pw) + kOffPawnTimeOfDeath, 4);
    if (!(tod > 0.0f) || !(worldT - tod >= kCorpseDeadFor)) {
        *why = "just died";
        return 0;
    }
    void* mesh = reinterpret_cast<void*>(static_cast<uintptr_t>(U32(pw, kOffPawnMesh)));
    if (!mesh || !vr::script::LiveObject(mesh) || (U32(mesh, kOffCompVisFlags) & 0x4)) {
        *why = "no visible mesh";
        return 0;
    }
    *meshOut = mesh;
    if (f5e8 & kRagdollDisabled) {
        // Congelato solo quando il gioco ha finito (revisione della voce 11):
        // m_ragdollIsDisabled arriva prima, il PhysicsAssetInstance lo toglie
        // FinishKill 0,1 s dopo (timer DisableMeshUpdates; nel gioco vero
        // congelato a 3,48 s, PAI 0 a 3,58 s). Svegliato in quella finestra
        // il timer poteva togliere i corpi sotto la mano (presa persa, corpo
        // escluso fino al congelamento successivo).
        if (U32(mesh, kOffSkelPhysAssetInst)) {
            *why = "being laid to rest by the game";
            return 0;
        }
        return 2;
    }
    if (sm != 0) {
        *why = "special move (death animation)";
        return 0;
    }
    if (!U32(mesh, kOffSkelPhysAssetInst)) {
        *why = "no physics bodies";
        return 0;
    }
    if (U32(mesh, kOffSkelFlags3C4) & kSkelFixed) {
        *why = "bodies fixed (animated)";
        return 0;
    }
    return 1;
}

// Cadaveri entro kCorpseRange da Walker, dalla PawnList, ogni 0,25 s (al piu' 8).
void CorpseScan(void* walker, double now) {
    if (now < g_corpseScanAt) return;
    g_corpseScanAt = now + 0.25;
    g_corpses.clear();
    float wl[3];
    const uint32_t wi = U32(walker, kOffActorWorldInfo);
    if (!wi || !SafeCopy(wl, static_cast<char*>(walker) + kOffActorLocation, 12)) return;
    const float wt = WorldTime(walker);
    const float range = kCorpseRange * vr::control::g_posScale;
    uint32_t q = U32(reinterpret_cast<void*>(static_cast<uintptr_t>(wi)), kOffWorldPawnList);
    for (int guard = 0; q && guard < 512 && g_corpses.size() < 8; ++guard) {
        void* pw = reinterpret_cast<void*>(static_cast<uintptr_t>(q));
        q = U32(pw, kOffPawnNextPawn);
        if (pw == walker || static_cast<int32_t>(U32(pw, kOffPawnHealth)) > 0) continue;
        float loc[3];
        if (!SafeCopy(loc, static_cast<char*>(pw) + kOffActorLocation, 12)) continue;
        const float d[3] = {loc[0] - wl[0], loc[1] - wl[1], loc[2] - wl[2]};
        if (Dot3(d, d) > range * range) continue;
        void* mesh = nullptr;
        const char* why = "";
        const int st = CorpseState(walker, pw, wt, &mesh, &why);
        if (!st) continue;
        CorpseCand c;
        c.pawn = pw;
        c.id = ObjIdOf(pw);
        c.mesh = mesh;
        c.frozen = st == 2;
        g_corpses.push_back(c);
    }
    // Pose dalle ossa solo per i congelati di adesso (rianimato o sparito: via).
    g_frozenPoses.erase(std::remove_if(g_frozenPoses.begin(), g_frozenPoses.end(),
                                       [](const FrozenPose& f) {
                                           for (const CorpseCand& c : g_corpses)
                                               if (c.frozen && c.pawn == f.pawn && c.id.index == f.id.index && c.id.number == f.id.number)
                                                   return false;
                                           return true;
                                       }),
                        g_frozenPoses.end());
}

// Rianima un cadavere congelato (ConditionalReanimateRagdoll del gioco):
// solo se e' ancora un cadavere congelato adesso, al piu' una volta al secondo.
bool CorpseReanimate(void* walker, void* pw, const char* why, double now) {
    g_corpseWake.erase(std::remove_if(g_corpseWake.begin(), g_corpseWake.end(), [&](const std::pair<void*, double>& w) { return now >= w.second; }),
                       g_corpseWake.end());
    for (const auto& w : g_corpseWake)
        if (w.first == pw) return false;
    void* mesh = nullptr;
    const char* no = "";
    if (CorpseState(walker, pw, WorldTime(walker), &mesh, &no) != 2) return false;
    if (g_corpseWake.size() < 32) g_corpseWake.push_back({pw, now + 1.0});
    uint32_t none[4] = {0, 0, 0, 0};
    const double t0 = ProfMs();
    const bool called = vr::script::CallFunction(pw, "ConditionalReanimateRagdoll", none);
    const double ms = ProfMs() - t0;
    void* all[kMaxCompBodies];
    const int nb = U32(mesh, kOffSkelPhysAssetInst) ? WeaponBodies(mesh, all) : 0;
    const bool back = called && nb > 0 && !(U32(pw, kOffPawnFlags5E8) & kRagdollDisabled);
    if (back) {
        // Revisione della voce 11: fisico subito anche nella lista, non alla
        // prossima CorpseScan (fino a 0,25 s): prima il limite della spinta
        // (CorpsePushCap) saltava il cadavere appena svegliato proprio mentre
        // la sfera lo attraversava.
        for (CorpseCand& c : g_corpses)
            if (c.pawn == pw) {
                c.frozen = false;
                c.mesh = mesh;
            }
        g_frozenPoses.erase(std::remove_if(g_frozenPoses.begin(), g_frozenPoses.end(), [&](const FrozenPose& f) { return f.pawn == pw; }),
                            g_frozenPoses.end());
        g_corpseLastWake = {pw, now};
    }
    ++g_corpseStats.woken;
    static int logged = 0;
    if (logged < 40) {
        ++logged;
        char b[240];
        std::snprintf(b, sizeof(b), "CORPSE: %s woke up the ragdoll of %s (ConditionalReanimateRagdoll): %s, %d physics bodies, %.2f ms", why,
                      vr::script::ClassName(pw).c_str(), back ? "physical again" : "STILL FROZEN", nb, ms);
        vr::log::Line(b);
    }
    return back;
}

// RB_BodyInstance -> PhysicsAsset.BodySetup[BodyIndex] (RB_BodySetup) e il
// suo osso (BoneName +0x84). 0 se non leggibile.
uint32_t BodySetupOf(void* mesh, uint32_t bi, int32_t bone[2]) {
    bone[0] = bone[1] = 0;
    const uint32_t pa = U32(mesh, kOffSkelPhysAsset);
    const int32_t bidx = bi ? static_cast<int32_t>(U32(reinterpret_cast<void*>(static_cast<uintptr_t>(bi)), kOffBodyIndex)) : -1;
    struct {
        uint32_t data;
        int32_t num, max;
    } setups = {};
    if (!pa || !SafeCopy(&setups, reinterpret_cast<char*>(static_cast<uintptr_t>(pa)) + 0x40, sizeof(setups)) || !setups.data || bidx < 0 ||
        bidx >= setups.num)
        return 0;
    const uint32_t bs = U32(reinterpret_cast<void*>(static_cast<uintptr_t>(setups.data)), 4u * static_cast<uint32_t>(bidx));
    if (!bs || !SafeCopy(bone, reinterpret_cast<char*>(static_cast<uintptr_t>(bs)) + 0x84, 8) || bone[0] <= 0) return 0;
    return bs;
}

// Corpi del ragdoll (PhysicsAssetInstance.Bodies): RB_BodyInstance per indice, al piu' kMaxCorpseBodies.
int CorpseBodyList(void* mesh, uint32_t out[kMaxCorpseBodies]) {
    const uint32_t pai = U32(mesh, kOffSkelPhysAssetInst);
    struct {
        uint32_t data;
        int32_t num, max;
    } arr = {};
    if (!pai || !SafeCopy(&arr, reinterpret_cast<char*>(static_cast<uintptr_t>(pai)) + kOffPaiBodies, sizeof(arr)) || !arr.data || arr.num <= 0 ||
        arr.num > kMaxCorpseBodies)
        return 0;
    return SafeCopy(out, reinterpret_cast<void*>(static_cast<uintptr_t>(arr.data)), 4u * static_cast<uint32_t>(arr.num)) ? arr.num : 0;
}

// Una volta per sessione, prima di ogni uso di getGlobalPose: la posa del
// corpo deve essere quella del suo osso (GetBoneLocation / GetBoneQuaternion,
// spazio del mondo): posizione entro 3 unita' e assi entro ~8 gradi. Decide
// anche se gli assi sono le colonne o le righe della NxMat33. Senza, nessuna
// presa (la spinta resta: e' la fisica della sfera). Al piu' un tentativo al
// secondo, 5 righe nel log.
bool CorpsePoseCheck(void* mesh) {
    if (g_corpsePoseOk) return true;
    static double nextTry = 0.0;
    static int logged = 0;
    const double now = ProfMs() * 0.001;
    if (now < nextTry) return false;
    nextTry = now + 1.0;
    uint32_t bodies[kMaxCorpseBodies];
    if (!CorpseBodyList(mesh, bodies)) return false;
    int32_t bone[2];
    void* nx = BodyNx(bodies[0]);
    if (!nx || !BodySetupOf(mesh, bodies[0], bone) || !NxPoseSlotOk(nx)) return false;
    NxMat34 M = {};
    if (!NxPoseRaw(nx, &M)) return false;
    struct {
        int32_t name[2];
        int32_t space;
        float ret[3];
    } q = {{bone[0], bone[1]}, 0, {0, 0, 0}};
    struct {
        int32_t name[2];
        int32_t space;
        int32_t pad;
        float ret[4];
        uint8_t spare[16];
    } qq = {{bone[0], bone[1]}, 0, 0, {0, 0, 0, 1}, {}};
    if (!vr::script::CallFunction(mesh, "GetBoneLocation", &q) || !vr::script::CallFunction(mesh, "GetBoneQuaternion", &qq)) return false;
    const float ql = std::sqrt(qq.ret[0] * qq.ret[0] + qq.ret[1] * qq.ret[1] + qq.ret[2] * qq.ret[2] + qq.ret[3] * qq.ret[3]);
    if (!(ql > 0.5f && ql < 2.0f)) return false;
    float B[3][3];
    QuatToRows({qq.ret[0] / ql, qq.ret[1] / ql, qq.ret[2] / ql, qq.ret[3] / ql}, B);
    float sumRows = 0.0f, sumCols = 0.0f;
    for (int k = 0; k < 3; ++k) {
        const float row[3] = {M.m[k][0], M.m[k][1], M.m[k][2]}, col[3] = {M.m[0][k], M.m[1][k], M.m[2][k]};
        sumRows += Dot3(row, B[k]);
        sumCols += Dot3(col, B[k]);
    }
    const float d[3] = {M.t[0] * 50.0f - q.ret[0], M.t[1] * 50.0f - q.ret[1], M.t[2] * 50.0f - q.ret[2]};
    const float dl = std::sqrt(Dot3(d, d));
    const float best = (std::max)(sumRows, sumCols);
    const bool ok = std::isfinite(dl + best) && dl < 3.0f && best > 2.97f;  // 3 cos(8 gradi) = 2,97
    if (ok) {
        g_nxCols = sumCols >= sumRows ? 1 : 0;
        g_corpsePoseOk = true;
    }
    if (ok || logged < 5) {
        ++logged;
        char b[300];
        std::snprintf(b, sizeof(b),
                      "CORPSE: PhysX pose check on bone %s: body at %.1f cm from the bone, axes match %.3f as columns, %.3f as rows (3 = same) -> %s",
                      vr::script::NameFromIndex(bone[0]).c_str(), dl / vr::control::g_posScale * 100.0f, sumCols, sumRows,
                      ok ? (g_nxCols ? "ok, columns" : "ok, rows") : "NOT VERIFIED, no grab");
        vr::log::Line(b);
    }
    return ok;
}

// Il corpo del ragdoll con la superficie piu' vicina al palmo (forme del
// PhysicsAsset): indice in Bodies (-1 nessuno), distanza dalla superficie
// (unita', negativa dentro) e punto da prendere negli assi del corpo (la
// superficie piu' vicina; il palmo stesso se e' gia' dentro: niente strappo).
// Pose dei corpi di un cadavere congelato dalle sue ossa (vedi FrozenPose),
// lette alla prima richiesta e tenute finche' resta congelato: 2 chiamate
// per corpo (16 corpi: 32), una volta per congelamento. nullptr se non ci
// sono (allora la zona e' il riquadro della mesh, come prima).
const FrozenPose* FrozenShapes(const CorpseCand& c) {
    for (const FrozenPose& f : g_frozenPoses)
        if (f.pawn == c.pawn && f.id.index == c.id.index && f.id.number == c.id.number) return f.ok ? &f : nullptr;
    if (g_frozenPoses.size() >= 8) g_frozenPoses.erase(g_frozenPoses.begin());
    FrozenPose f;
    f.pawn = c.pawn;
    f.id = c.id;
    const double t0 = ProfMs();
    const uint32_t pa = U32(c.mesh, kOffSkelPhysAsset);
    struct {
        uint32_t data;
        int32_t num, max;
    } setups = {};
    if (pa && SafeCopy(&setups, reinterpret_cast<char*>(static_cast<uintptr_t>(pa)) + 0x40, sizeof(setups)) && setups.data && setups.num > 0 &&
        setups.num <= kMaxCorpseBodies) {
        for (int i = 0; i < setups.num; ++i) {
            const uint32_t bs = U32(reinterpret_cast<void*>(static_cast<uintptr_t>(setups.data)), 4u * static_cast<uint32_t>(i));
            int32_t bone[2] = {0, 0};
            if (!bs || !SafeCopy(bone, reinterpret_cast<char*>(static_cast<uintptr_t>(bs)) + 0x84, 8) || bone[0] <= 0) continue;
            struct {
                int32_t name[2];
                int32_t space;
                float ret[3];
            } q = {{bone[0], bone[1]}, 0, {0, 0, 0}};
            struct {
                int32_t name[2];
                int32_t space;
                int32_t pad;
                float ret[4];
                uint8_t spare[16];
            } qq = {{bone[0], bone[1]}, 0, 0, {0, 0, 0, 1}, {}};
            if (!vr::script::CallFunction(c.mesh, "GetBoneLocation", &q) || !vr::script::CallFunction(c.mesh, "GetBoneQuaternion", &qq)) continue;
            const float ql = std::sqrt(qq.ret[0] * qq.ret[0] + qq.ret[1] * qq.ret[1] + qq.ret[2] * qq.ret[2] + qq.ret[3] * qq.ret[3]);
            if (!(ql > 0.5f && ql < 2.0f) || !std::isfinite(q.ret[0] + q.ret[1] + q.ret[2])) continue;
            FrozenBody b;
            b.setup = bs;
            std::memcpy(b.p, q.ret, sizeof(b.p));
            QuatToRows({qq.ret[0] / ql, qq.ret[1] / ql, qq.ret[2] / ql, qq.ret[3] / ql}, b.R);
            f.bodies.push_back(b);
        }
    }
    // Ossa sensate: almeno meta' dei corpi, tutte entro i limiti della mesh (+50 cm).
    Prop box;
    bool inBox = ReadBounds(c.mesh, &box);
    for (const FrozenBody& b : f.bodies)
        if (inBox && BoxDistance(box, b.p) > 0.5f * vr::control::g_posScale) inBox = false;
    f.ok = inBox && setups.num > 0 && static_cast<int>(f.bodies.size()) * 2 >= setups.num;
    static int logged = 0;
    if (logged < 12) {
        ++logged;
        char t[220];
        std::snprintf(t, sizeof(t), "CORPSE: frozen %s: %d of %d bodies placed from their bones (%.2f ms)%s", vr::script::ClassName(c.pawn).c_str(),
                      static_cast<int>(f.bodies.size()), setups.num, ProfMs() - t0, f.ok ? "" : ": NOT USABLE, its bounds instead");
        vr::log::Line(t);
    }
    g_frozenPoses.push_back(f);
    return g_frozenPoses.back().ok ? &g_frozenPoses.back() : nullptr;
}

// Distanza del palmo dalle forme di un congelato (unita', negativa dentro); 1e30 senza ossa.
float FrozenDist(const CorpseCand& c, const FrozenPose& f, const float* palm, int* bodyOut = nullptr) {
    const float sc = CorpseScale(c.pawn, c.mesh);
    float best = 1e30f;
    for (size_t i = 0; i < f.bodies.size(); ++i) {
        const FrozenBody& b = f.bodies[i];
        const float v[3] = {palm[0] - b.p[0], palm[1] - b.p[1], palm[2] - b.p[2]};
        const float q[3] = {Dot3(v, b.R[0]), Dot3(v, b.R[1]), Dot3(v, b.R[2])};
        for (const CorpseShape& s : BodyShapes(b.setup)) {
            float nearL[3];
            const float d = ShapeDist(s, sc, q, nearL);
            if (d < best) {
                best = d;
                if (bodyOut) *bodyOut = static_cast<int>(i);
            }
        }
    }
    return best;
}

int CorpseNearestBody(void* pw, void* mesh, const float* palm, float* outD, float local[3]) {
    *outD = 1e30f;
    if (!CorpsePoseCheck(mesh)) return -1;
    uint32_t bodies[kMaxCorpseBodies];
    const int nb = CorpseBodyList(mesh, bodies);
    const float sc = CorpseScale(pw, mesh);
    int best = -1;
    for (int i = 0; i < nb; ++i) {
        void* nx = BodyNx(bodies[i]);
        int32_t bone[2];
        const uint32_t setup = nx ? BodySetupOf(mesh, bodies[i], bone) : 0;
        float p[3], R[3][3];
        if (!setup || !CorpseBodyPose(nx, p, R)) continue;
        const float v[3] = {palm[0] - p[0], palm[1] - p[1], palm[2] - p[2]};
        const float q[3] = {Dot3(v, R[0]), Dot3(v, R[1]), Dot3(v, R[2])};
        for (const CorpseShape& s : BodyShapes(setup)) {
            float nearL[3];
            const float d = ShapeDist(s, sc, q, nearL);
            if (d < *outD) {
                *outD = d;
                best = i;
                for (int k = 0; k < 3; ++k) local[k] = d > 0.0f ? nearL[k] : q[k];
            }
        }
    }
    return best;
}

struct CorpsePick {
    int idx = -1;   // in g_corpses
    int body = -1;  // in Bodies (-1: congelato, il cadavere intero)
    float d = 1e30f;
};
// Cadavere nella zona del palmo (zone: unita'); i congelati dai limiti della mesh.
CorpsePick CorpseNearest(const float* palm, float zone) {
    CorpsePick best;
    for (size_t i = 0; i < g_corpses.size(); ++i) {
        const CorpseCand& c = g_corpses[i];
        if (!ObjAlive(c.pawn, c.id) || !vr::script::LiveObject(c.mesh)) continue;
        Prop b;
        if (!ReadBounds(c.mesh, &b)) continue;
        const float bd = BoxDistance(b, palm);
        if (bd > zone) continue;  // fuori dai limiti del corpo intero: nessun suo corpo e' nella zona
        if (c.frozen) {
            // Dalle forme sulle ossa (revisione); senza ossa leggibili, i limiti.
            const FrozenPose* f = FrozenShapes(c);
            const float fd = f ? FrozenDist(c, *f, palm) : bd;
            if (fd <= zone && fd < best.d) best = {static_cast<int>(i), -1, fd};
            continue;
        }
        float d = 0.0f, local[3];
        const int body = CorpseNearestBody(c.pawn, c.mesh, palm, &d, local);
        if (body >= 0 && d <= zone && d < best.d) best = {static_cast<int>(i), body, d};
    }
    return best;
}

// Forza della mano sul cadavere (N): quella del peso (griptune force) per il
// modo di objectweight: full x1, light x2, off x4.
float CorpseForceN() {
    const int w = vr::control::g_objectWeight.load();
    const float mul = w == 2 ? 1.0f : (w == 1 ? 2.0f : 4.0f);
    return mul * (std::max)(1.0f, g_tune.force);
}

// maxD (unita'): il corpo piu' vicino deve avere la superficie entro questa
// distanza dal palmo (la zona di presa; un congelato e' nella zona coi limiti
// del corpo intero, e solo dopo la rianimazione si sa dove sono gli arti).
bool CorpseGrab(void* walker, const CorpsePick& pick, const float* palm, double now, const char* how, float maxD) {
    if (pick.idx < 0 || pick.idx >= static_cast<int>(g_corpses.size())) return false;
    const CorpseCand cand = g_corpses[pick.idx];
    if (!ObjAlive(cand.pawn, cand.id)) return false;
    void* mesh = nullptr;
    const char* why = "";
    int st = CorpseState(walker, cand.pawn, WorldTime(walker), &mesh, &why);
    if (st == 2) {
        // Congelato: di nuovo fisico (nel tick stesso), poi il corpo piu' vicino.
        if (!CorpseReanimate(walker, cand.pawn, "grip", now)) return false;
        st = CorpseState(walker, cand.pawn, WorldTime(walker), &mesh, &why);
    }
    if (st != 1 || !mesh) {
        char b[200];
        std::snprintf(b, sizeof(b), "CORPSE: %s: %s not taken (%s)", how, vr::script::ClassName(cand.pawn).c_str(), why);
        vr::log::Line(b);
        return false;
    }
    float d = 0.0f, local[3];
    const int body = CorpseNearestBody(cand.pawn, mesh, palm, &d, local);
    if (body < 0 || d > maxD) {
        char b[200];
        std::snprintf(b, sizeof(b), "CORPSE: %s: no body of %s within reach (nearest surface %.1f cm from the palm)", how,
                      vr::script::ClassName(cand.pawn).c_str(), body < 0 ? -1.0f : d / vr::control::g_posScale * 100.0f);
        vr::log::Line(b);
        return false;
    }
    const uint32_t pai = U32(mesh, kOffSkelPhysAssetInst);
    uint32_t list[kMaxCorpseBodies];
    if (body >= CorpseBodyList(mesh, list)) return false;
    const uint32_t bi = list[body];
    void* nx = BodyNx(bi);
    int32_t bone[2] = {0, 0};
    if (!nx || !BodySetupOf(mesh, bi, bone)) return false;
    float p[3], R[3][3];
    if (!CorpseBodyPose(nx, p, R)) return false;
    CorpseHeld h;
    h.pawn = cand.pawn;
    h.id = cand.id;
    h.mesh = mesh;
    h.pai = pai;
    h.bi = bi;
    h.nx = nx;
    h.body = body;
    h.bone[0] = bone[0];
    h.bone[1] = bone[1];
    std::memcpy(h.grabLocal, local, sizeof(local));  // punto preso negli assi del corpo (CorpseNearestBody)
    float m = 0.0f, I[3];
    h.mass = NxMassOf(nx, &m, I) ? m : 0.1f;
    {
        void* all[kMaxCompBodies];
        const int nb = WeaponBodies(mesh, all);
        float tot = 0.0f;
        for (int i = 0; i < nb; ++i)
            if (NxMassOf(all[i], &m, I)) tot += m;
        h.totalMass = tot;
        h.totalKg = tot * vr::control::g_massKgPerUnit.load();
        uint32_t list[kMaxCorpseBodies];
        if (CorpseBodyList(mesh, list)) BodySetupOf(mesh, list[0], h.rootBone);
        float rp[3], rR[3][3];
        if (nb && CorpseBodyPose(all[0], rp, rR)) {
            std::memcpy(h.root0, rp, sizeof(rp));
            h.hasRoot0 = true;
        }
    }
    h.since = now;
    h.forceN = CorpseForceN();
    g_corpse = h;
    g_corpseComp = mesh;
    vr::control::g_propHeld[1] = true;
    Wake(mesh);
    ++g_corpseStats.grabs;
    vr::input::PulseHand(0, 40);  // la sinistra (mai la mano dell'arma: la presa non parte con la sinistra sull'arma)
    char fz[64] = "";
    if (pick.body < 0 && pick.d < 1e29f)
        std::snprintf(fz, sizeof(fz), "; while frozen %.1f cm from its bones", (std::max)(0.0f, pick.d) / vr::control::g_posScale * 100.0f);
    char b[380];
    std::snprintf(b, sizeof(b),
                  "CORPSE: left hand took %s by %s (%s; body %d, %.1f kg of %.1f kg, palm %.1f cm from its surface%s), hand force %.0f N",
                  vr::script::ClassName(cand.pawn).c_str(), vr::script::NameFromIndex(bone[0]).c_str(), how, body,
                  h.mass * vr::control::g_massKgPerUnit.load(), h.totalKg, (std::max)(0.0f, d) / vr::control::g_posScale * 100.0f, fz, h.forceN);
    vr::log::Line(b);
    return true;
}

// Il cadavere tenuto e' ancora quello, ancora un cadavere fisico, con lo
// stesso corpo nello stesso PhysicsAssetInstance.
bool CorpseHeldValid(void* walker, const char** why) {
    const CorpseHeld& c = g_corpse;
    if (!ObjAlive(c.pawn, c.id)) {
        *why = "the body is gone";
        return false;
    }
    void* mesh = nullptr;
    const int st = CorpseState(walker, c.pawn, WorldTime(walker), &mesh, why);
    if (st != 1) {
        if (st == 2) *why = "frozen by the game";
        return false;
    }
    if (mesh != c.mesh || U32(mesh, kOffSkelPhysAssetInst) != c.pai) {
        *why = "physics bodies rebuilt";
        return false;
    }
    const uint32_t bodies = U32(reinterpret_cast<void*>(static_cast<uintptr_t>(c.pai)), kOffPaiBodies);
    if (U32(reinterpret_cast<void*>(static_cast<uintptr_t>(bodies + 4u * static_cast<uint32_t>(c.body))), 0) != c.bi || BodyNx(c.bi) != c.nx) {
        *why = "physics bodies rebuilt";
        return false;
    }
    return true;
}

void CorpseRelease(const char* why) {
    CorpseHeld& c = g_corpse;
    if (!c.pawn) return;
    const char* no = "";
    float moved = -1.0f;
    if (c.hasRoot0 && g_tickPawn && CorpseHeldValid(g_tickPawn, &no)) {
        void* all[kMaxCompBodies];
        float rp[3], rR[3][3];
        if (WeaponBodies(c.mesh, all) && CorpseBodyPose(all[0], rp, rR)) {
            const float d[3] = {rp[0] - c.root0[0], rp[1] - c.root0[1], rp[2] - c.root0[2]};
            moved = std::sqrt(Dot3(d, d)) / vr::control::g_posScale * 100.0f;
            g_corpseStats.moved = (std::max)(g_corpseStats.moved, moved);
        }
    }
    if (std::strcmp(why, "slipped off") == 0) ++g_corpseStats.slips;
    else if (std::strcmp(why, "grip") == 0) ++g_corpseStats.letGo;
    else ++g_corpseStats.lost;
    char b[340];
    std::snprintf(b, sizeof(b),
                  "CORPSE: left hand let go of %s (%s) after %.2f s: body moved %.0f cm, palm-point distance %.1f cm on average, %.1f at most, "
                  "force up to %.0f N",
                  vr::script::NameFromIndex(c.bone[0]).c_str(), why, ProfMs() * 0.001 - c.since, moved,
                  c.ticks ? c.errSum / c.ticks / vr::control::g_posScale * 100.0f : 0.0f, c.errMax / vr::control::g_posScale * 100.0f,
                  c.forceN * c.maxMul);
    vr::log::Line(b);
    // Le coppie con le sfere tornano solide da sole (ProxyTick): nella lista
    // finche' ne resta una da rimettere, anche se intanto se ne prende un altro.
    if (c.mesh && std::find(g_corpsePairMeshes.begin(), g_corpsePairMeshes.end(), c.mesh) == g_corpsePairMeshes.end())
        g_corpsePairMeshes.push_back(c.mesh);
    g_corpseComp = nullptr;
    g_corpse = CorpseHeld();
    vr::control::g_propReleasedMs[1] = GetTickCount64();
    vr::control::g_propHeld[1] = false;
}

// Un tick della presa: impulso nel punto preso verso il palmo, col limite di forza del peso.
void CorpseDrive(void* walker, const float* palm, float dt, double now) {
    CorpseHeld& c = g_corpse;
    const char* why = "";
    if (!CorpseHeldValid(walker, &why)) {
        CorpseRelease(why);
        return;
    }
    float p[3], R[3][3];
    if (!CorpseBodyPose(c.nx, p, R)) {
        CorpseRelease("pose not readable");
        return;
    }
    const float s = vr::control::g_posScale;
    float pt[3];
    for (int k = 0; k < 3; ++k) pt[k] = p[k] + c.grabLocal[0] * R[0][k] + c.grabLocal[1] * R[1][k] + c.grabLocal[2] * R[2][k];
    float err[3] = {palm[0] - pt[0], palm[1] - pt[1], palm[2] - pt[2]};
    const float el = std::sqrt(Dot3(err, err));
    std::memcpy(c.pt, pt, sizeof(pt));
    c.ptValid = true;
    if (el > kCorpseSlip * 0.01f * s) {
        if (c.stuckSince < 0.0) c.stuckSince = now;
        if (now - c.stuckSince > kCorpseSlipT) {
            CorpseRelease("slipped off");
            return;
        }
    } else {
        c.stuckSince = -1.0;
    }
    // Peso: la forza raddoppia ogni 0,4 s mentre il punto resta oltre 10 cm
    // senza avvicinarsi; arrivato entro 6 cm torna quella della mano.
    {
        vr::pure::CorpsePull pull;
        pull.mul = c.mul;
        pull.growAt = c.growAt;
        vr::pure::CorpsePullStep(pull, el, c.lastEl, s, now);
        c.mul = pull.mul;
        c.growAt = pull.growAt;
    }
    c.lastEl = el;
    c.maxMul = (std::max)(c.maxMul, c.mul);
    float hv[3] = {0, 0, 0};
    HandVelocity(1, 0.03, hv);
    float vpt[3] = {0, 0, 0};
    if (c.hasPrev && dt > 0.0f)
        for (int k = 0; k < 3; ++k) vpt[k] = (pt[k] - c.prevPt[k]) / dt;
    std::memcpy(c.prevPt, pt, sizeof(pt));
    c.hasPrev = true;
    float vwant[3], dv[3];
    for (int k = 0; k < 3; ++k) vwant[k] = err[k] / 0.08f + hv[k];
    const float vl = std::sqrt(Dot3(vwant, vwant)), vmax = kCorpseVMax * s;
    if (vl > vmax)
        for (float& v : vwant) v *= vmax / vl;
    for (int k = 0; k < 3; ++k) dv[k] = vwant[k] - vpt[k];
    // Impulso di AddImpulse (unita' di UE3): massa di PhysX x cambio di
    // velocita' (unita'/s). Al piu' forza x dt: in PhysX N s / masskg, x 50
    // per le unita' di UE3 (AddImpulse moltiplica per 0,02). Il corpo intero
    // vorrebbe massa totale x dv; dentro il limite, il corpo preso ne riceve
    // la sua parte (massa del corpo x dv: segue la mano) e il resto, solo
    // orizzontale, va alla radice (il bacino): e' la trazione che il braccio
    // passa al busto. Prima tutto andava al corpo preso: un braccio da 0,4 kg
    // tirato a 0,3 m/s non spostava il manichino (1 cm, scivolato via con la
    // forza a x4: i giunti annullavano l'impulso), il busto da 5 kg si'.
    // Verticale solo sul corpo preso: tirato su per un braccio il cadavere
    // alza il braccio e struscia, non levita dal bacino.
    const float dvl = std::sqrt(Dot3(dv, dv));
    const float jmax = c.forceN * c.mul * dt / (0.02f * (std::max)(1.0f, vr::control::g_massKgPerUnit.load()));
    const vr::pure::CorpseImpulse ji = vr::pure::CorpseImpulseSplit(c.mass, c.totalMass, dvl, jmax);
    const float jl = ji.limb + ji.root;
    float J[3] = {0, 0, 0}, Jr[3] = {0, 0, 0};
    if (dvl > 1e-4f) {
        for (int k = 0; k < 3; ++k) J[k] = dv[k] / dvl * ji.limb;
        if (c.rootBone[0] > 0) {
            Jr[0] = dv[0] / dvl * ji.root;
            Jr[1] = dv[1] / dvl * ji.root;
        }
    }
    struct {
        float impulse[3];
        float pos[3];
        int32_t bone[2];
        uint32_t velChange;
    } ai = {{J[0], J[1], J[2]}, {pt[0], pt[1], pt[2]}, {c.bone[0], c.bone[1]}, 0};
    vr::script::CallFunction(c.mesh, "AddImpulse", &ai);
    if (Jr[0] != 0.0f || Jr[1] != 0.0f) {
        // Posizione nulla: AddImpulse spinge il corpo nel baricentro (senza coppia).
        struct {
            float impulse[3];
            float pos[3];
            int32_t bone[2];
            uint32_t velChange;
        } ar = {{Jr[0], Jr[1], 0.0f}, {0, 0, 0}, {c.rootBone[0], c.rootBone[1]}, 0};
        vr::script::CallFunction(c.mesh, "AddImpulse", &ar);
    }
    if (now >= c.wakeAt) {
        Wake(c.mesh);  // tutti i corpi: il gioco non lo congela mentre la mano lo tiene
        c.wakeAt = now + 0.25;
    }
    c.errSum += el;
    c.errMax = (std::max)(c.errMax, el);
    ++c.ticks;
    if (vr::control::g_grabLog > 0) {
        vr::control::g_grabLog.fetch_sub(1);
        char b[300];
        std::snprintf(b, sizeof(b),
                      "GRABLOG: L corpse %s err %.1f cm, point speed %.2f m/s, wanted %.2f m/s, impulse %.0f%% of the limit, force %.0f N (x%.0f)",
                      vr::script::NameFromIndex(c.bone[0]).c_str(), el / s * 100.0f, std::sqrt(Dot3(vpt, vpt)) / s,
                      std::sqrt(Dot3(vwant, vwant)) / s, jmax > 0.0f ? (std::min)(jl, jmax) / jmax * 100.0f : 0.0f, c.forceN * c.mul, c.mul);
        vr::log::Line(b);
    }
}

// Mano sinistra libera vicino a un cadavere: congelato e urtato (entro 10 cm
// dai limiti, oltre 0,3 m/s) si rianima perche' la sfera lo spinga; fisico e
// con la mano entro 30 cm resta sveglio (WakeRigidBody ogni 0,3 s).
void CorpseNearHand(void* walker, const float* palm, float speed, double now) {
    static double wakeAt = 0.0;
    const float s = vr::control::g_posScale;
    bool woke = false;
    for (const CorpseCand& c : g_corpses) {
        if (!ObjAlive(c.pawn, c.id) || !vr::script::LiveObject(c.mesh)) continue;
        Prop b;
        if (!ReadBounds(c.mesh, &b)) continue;
        const float bd = BoxDistance(b, palm);
        if (c.frozen) {
            // Entro 10 cm da un arto (forme sulle ossa; senza, dai limiti): non
            // piu' ogni passaggio veloce nel vuoto del riquadro (revisione).
            if (bd < 0.10f * s && speed > 0.3f * s) {
                const FrozenPose* f = FrozenShapes(c);
                if (!f || FrozenDist(c, *f, palm) < 0.10f * s) CorpseReanimate(walker, c.pawn, "the left hand pushed it and", now);
            }
        } else if (bd < 0.30f * s && now >= wakeAt) {
            void* mesh = nullptr;
            const char* why = "";
            if (CorpseState(walker, c.pawn, WorldTime(walker), &mesh, &why) == 1) {
                Wake(mesh);
                woke = true;
            }
        }
    }
    if (woke) wakeAt = now + 0.3;
}

// Peso nella spinta (regola del piano Boneworks: pesante): la sfera della
// mano (4 kg, rimessa a ogni tick alla velocita' della mano) urta un corpo
// del ragdoll da 0,3-6 kg alla volta e i giunti si portano dietro il resto:
// nella serie finale una spazzata a 1,5 m/s attraverso il busto ha spinto il
// manichino (30 kg) di 1 m. Per ogni cadavere fisico non tenuto entro 40 cm
// dal palmo sinistro, e per 0,5 s dopo, ogni corpo va al piu' a
// kCorpsePushMax in orizzontale (AddImpulse con bVelChange sull'osso del
// corpo: il cambio di velocita' che manca; la verticale resta, cade come
// prima). Velocita' lette da NxActor::getLinearVelocity (slot 58, gia' in uso).
constexpr float kCorpsePushMax = 0.6f;  // m/s
std::vector<std::pair<void*, double>> g_corpsePushed;  // pawn -> limite attivo fino a qui

void CorpsePushCap(void* walker, const float* palm, double now) {
    const float s = vr::control::g_posScale;
    g_corpsePushed.erase(std::remove_if(g_corpsePushed.begin(), g_corpsePushed.end(), [&](const std::pair<void*, double>& e) { return now >= e.second; }),
                         g_corpsePushed.end());
    for (const CorpseCand& c : g_corpses) {
        // Anche i "congelati" della lista (revisione della voce 11): uno
        // svegliato adesso e' fisico subito (CorpseReanimate aggiorna la voce,
        // e CorpseState qui sotto decide comunque).
        if ((g_corpse.pawn && c.pawn == g_corpse.pawn) || !ObjAlive(c.pawn, c.id) || !vr::script::LiveObject(c.mesh)) continue;
        Prop b;
        const bool nearNow = ReadBounds(c.mesh, &b) && BoxDistance(b, palm) < 0.40f * s;
        auto it = std::find_if(g_corpsePushed.begin(), g_corpsePushed.end(), [&](const std::pair<void*, double>& e) { return e.first == c.pawn; });
        if (nearNow) {
            if (it != g_corpsePushed.end()) it->second = now + 0.5;
            else if (g_corpsePushed.size() < 16) g_corpsePushed.push_back({c.pawn, now + 0.5});
        } else if (it == g_corpsePushed.end()) {
            continue;
        }
        void* mesh = nullptr;
        const char* why = "";
        if (CorpseState(walker, c.pawn, WorldTime(walker), &mesh, &why) != 1) continue;
        uint32_t list[kMaxCorpseBodies];
        const int nb = CorpseBodyList(mesh, list);
        float fastest = 0.0f;
        int capped = 0;
        for (int i = 0; i < nb; ++i) {
            void* nx = BodyNx(list[i]);
            float lin[3], ang[3];
            if (!nx || !NxVelocities(nx, lin, ang) || !std::isfinite(lin[0] + lin[1] + lin[2])) continue;
            const float hz = std::sqrt(lin[0] * lin[0] + lin[1] * lin[1]);  // m/s di PhysX
            if (!(hz > kCorpsePushMax)) continue;
            int32_t bone[2];
            if (!BodySetupOf(mesh, list[i], bone)) continue;
            const float k = kCorpsePushMax / hz - 1.0f;
            struct {
                float impulse[3];
                float pos[3];
                int32_t bone[2];
                uint32_t velChange;
            } ai = {{lin[0] * k * 50.0f, lin[1] * k * 50.0f, 0.0f}, {0, 0, 0}, {bone[0], bone[1]}, 1};
            vr::script::CallFunction(mesh, "AddImpulse", &ai);
            fastest = (std::max)(fastest, hz);
            ++capped;
        }
        if (capped) {
            ++g_corpseStats.capped;
            g_corpseStats.fastest = (std::max)(g_corpseStats.fastest, fastest);
            static int logged = 0;
            static double lastAt = -10.0;
            // Anche sotto 1 m/s nel primo mezzo secondo dopo la rianimazione (prova del limite sul cadavere appena svegliato).
            const bool justWoken = g_corpseLastWake.first == c.pawn && now - g_corpseLastWake.second < 0.5;
            if (logged < 30 && (fastest > 1.0f || justWoken) && now - lastAt > 0.5) {
                ++logged;
                lastAt = now;
                char t[240], w[64] = "";
                if (g_corpseLastWake.first == c.pawn && now - g_corpseLastWake.second < 2.0)
                    std::snprintf(w, sizeof(w), ", %.3f s after its wake", now - g_corpseLastWake.second);
                std::snprintf(t, sizeof(t), "CORPSE: %s shoved by the left hand: %d bodies held to %.1f m/s (fastest %.2f m/s%s)",
                              vr::script::ClassName(c.pawn).c_str(), capped, kCorpsePushMax, fastest, w);
                vr::log::Line(t);
            }
        }
    }
}

void CorpseReset() {
    g_corpses.clear();
    g_corpsePushed.clear();
    g_corpsePairMeshes.clear();
    g_corpseShapes.clear();  // forme dei PhysicsAsset del livello vecchio
    g_corpse = CorpseHeld();
    g_corpseComp = nullptr;
    g_corpseWake.clear();
    g_corpseLastWake = {nullptr, 0.0};
    g_frozenPoses.clear();
    g_corpseScanAt = 0.0;
}

}  // namespace

void SetHandFrame(const float center[3], int32_t posYaw, int32_t axesYaw, int32_t refYaw, const float originOff[3]) {
    HandFrame& f = g_handFrame;
    std::memcpy(f.c, center, sizeof(f.c));
    // Origine del tracciamento nel mondo, come le mani in HandTarget.
    const float b = posYaw * (6.28318530718f / 65536.0f), cb = std::cos(b), sb = std::sin(b);
    f.o[0] = center[0] + originOff[0] * cb - originOff[1] * sb;
    f.o[1] = center[1] + originOff[0] * sb + originOff[1] * cb;
    f.o[2] = center[2] + originOff[2];
    f.posYaw = posYaw - refYaw;
    f.axesYaw = axesYaw - refYaw;
    f.rawYaw = posYaw;
    f.refYaw = refYaw;
    if (++f.seq == 0) f.seq = 1;
}

// Anche le sfere delle mani (revisione 25/10): la mano sinistra davanti al
// petto (35 cm avanti, 15 sotto gli occhi) stava sulla trace bassa del muro
// davanti, e la vista arretrava di 25 unita' (50 cm); spostando la mano la
// vista tornava avanti in 0,2 s, e con lei le mani e l'oggetto tenuto
// (l'assestamento "bimodale" dei collaudi del peso).
bool IsHeldActor(const void* actor) {
    return actor && (g_held[0].actor == actor || g_held[1].actor == actor || g_proxyActor[0] == actor || g_proxyActor[1] == actor ||
                     g_proxy[0].actor == actor || g_proxy[1].actor == actor || g_wpn.actor == actor);
}

bool IsHandSphere(const void* actor) {
    return actor && (g_proxyActor[0] == actor || g_proxyActor[1] == actor || g_proxy[0].actor == actor || g_proxy[1].actor == actor);
}

bool IsThrownBackGrenade(const void* actor) {
    if (!actor) return false;
    if (g_gren.actor == actor) return true;
    for (const LiveGrenade& g : g_grenades)
        if (g.actor == actor && g.back) return true;
    return false;
}

// Una trace ha colpito la sfera di una mano (i suoi bit delle trace erano
// accesi: ProxyCreate li spegne): di nuovo spenti, e nel log (al piu' 20 righe).
void HandSphereTraced(const void* actor, const char* who) {
    uint32_t was = 0;
    for (int h = 0; h < 2; ++h) {
        HandProxy& pr = g_proxy[h];
        if (!actor || pr.actor != actor || !ActorUsable(pr.actor)) continue;
        ProxyNoTraces(pr.comp, &was);
        static int logged = 0;
        if (logged < 20) {
            ++logged;
            char b[200];
            std::snprintf(b, sizeof(b), "PROPS: WARNING %s hit the %s hand sphere (collision flags were 0x%X): ignored, traces turned off",
                          who ? who : "a trace", h ? "left" : "right", was);
            vr::log::Line(b);
        }
    }
}

// --- Mano disegnata che segue la fisica (voce 5 del piano, "solidhands") ----
// In Boneworks il corpo disegnato segue la fisica, non il controller. Qui la
// fisica resta guidata dal controller (g_handWorld, Drive, sfere) e cambia
// solo dove si disegna la mano libera, da tre fonti:
// (a) oggetto tenuto: la mano sta dove la presa (senza abbassamento ne'
//     piega: quelli sono la mano che cede) mette la mano rispetto all'oggetto
//     vero. Si scompone nel palmo: spostamento del palmo + rotazione attorno
//     al palmo, ognuno con la sua zona morta e il suo tetto (una bottiglia
//     incastrata girata di 100 gradi non porta il polso a 20 cm). Premuta su
//     un tavolo la mano resta sull'oggetto; un barile pesante la tira giu' e
//     resta indietro.
// (b) oggetto vincolato (portiera, cartello): il palmo sul punto afferrato
//     (pt - palmo di ConstrainedDrive), senza rotazione.
// (c) mano vuota, solo "full": il palmo sulla sfera della mano, che tavoli e
//     muri fermano (ProxyTick); con un muro tra sfera e palmo la sfera non
//     torna sul palmo prima di shtele cm, e la zona di presa parte dalla sfera.
// Lo scostamento vive negli assi del bersaglio (un giro a scatti o la
// scivolata in copertura lo portano con se', niente da spostare) e passa per
// zona morta (SolidShape), tetto e filtro (SolidFilterAlpha, 40 ms entrando,
// 100 uscendo). Mai la mano dell'arma ne' la testa.
namespace {

// Scostamento voluto (assi del bersaglio, unita' e quaternione) dalla fonte
// attiva, prima della zona morta. pivotT = palmo del bersaglio nel mondo.
int SolidSource(int side, const float tp[3], const float ta[3][3], const float* palmL, const float* pivotT, float wantP[3], Quat* wantQ,
                float* rawCm, float* rawDeg) {
    wantP[0] = wantP[1] = wantP[2] = 0.0f;
    *wantQ = {0.0f, 0.0f, 0.0f, 1.0f};
    *rawCm = *rawDeg = 0.0f;
    const int mode = vr::control::g_solidHands.load();
    if (mode <= 0 || side != 1) return 0;
    const float s = vr::control::g_posScale;
    const Held& hd = g_held[side];
    float d[3] = {0, 0, 0};
    int src = 0;
    if (hd.actor) {
        float op[3], oa[3][3];
        if (!SolidHeld(hd) || !ActorUsable(hd.actor) || !CompPose(hd.comp, op, oa)) return 0;
        if (!hd.constrained) {
            // La mano implicita dall'oggetto vero: assi haI = ra^T oa, origine
            // op - haI^T rp (la presa senza peso: HeldTarget senza assi).
            float rp[3], ra[3][3], haI[3][3], hpI[3];
            HeldTarget(hd, nullptr, rp, ra);
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) haI[r][c] = ra[0][r] * oa[0][c] + ra[1][r] * oa[1][c] + ra[2][r] * oa[2][c];
            for (int c = 0; c < 3; ++c) hpI[c] = op[c] - (rp[0] * haI[0][c] + rp[1] * haI[1][c] + rp[2] * haI[2][c]);
            float Q[3][3];
            for (int j = 0; j < 3; ++j)
                for (int k = 0; k < 3; ++k) Q[j][k] = Dot3(haI[j], ta[k]);
            if (Det3(Q) < 0.5f) return 0;  // non una rotazione (assi letti male): niente
            *wantQ = RowsToQuat(Q);
            for (int c = 0; c < 3; ++c) d[c] = hpI[c] + palmL[0] * haI[0][c] + palmL[1] * haI[1][c] + palmL[2] * haI[2][c] - pivotT[c];
            *rawDeg = 2.0f * std::acos((std::min)(1.0f, std::fabs(wantQ->w))) * 57.2958f;
            src = 1;
        } else {
            // Portiera, cartello: il palmo sul punto afferrato.
            for (int c = 0; c < 3; ++c)
                d[c] = op[c] + hd.grabLocal[0] * oa[0][c] + hd.grabLocal[1] * oa[1][c] + hd.grabLocal[2] * oa[2][c] - pivotT[c];
            src = 2;
        }
    } else if (g_wpn.actor) {
        // Arma raccolta (revisione della voce 6): la mano disegnata segue
        // l'arma vera, come un oggetto tenuto (premuta a terra o contro un muro
        // il pugno restava sul controller, fino a 60 cm prima dello
        // scivolamento). La mano implicita: assi ra^T oa, origine op - haI^T rp
        // con la presa g_wpnGrip. Non nell'arrivo (l'arma vola verso la presa)
        // ne' con le ossa ferme (radice letta male fuori vista): nessuna fonte.
        const WeaponHeld& w = g_wpn;
        float op[3], oa[3][3];
        if (ProfMs() * 0.001 - w.grabAt < w.blendT || w.staleSince >= 0.0 || !WeaponAlive(w) || !WeaponPose(w.comp, op, oa)) return 0;
        float rp[3], ra[3][3], haI[3][3], hpI[3];
        WeaponGripRel(rp, ra);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) haI[r][c] = ra[0][r] * oa[0][c] + ra[1][r] * oa[1][c] + ra[2][r] * oa[2][c];
        for (int c = 0; c < 3; ++c) hpI[c] = op[c] - (rp[0] * haI[0][c] + rp[1] * haI[1][c] + rp[2] * haI[2][c]);
        float Q[3][3];
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) Q[j][k] = Dot3(haI[j], ta[k]);
        if (Det3(Q) < 0.5f) return 0;
        *wantQ = RowsToQuat(Q);
        for (int c = 0; c < 3; ++c) d[c] = hpI[c] + palmL[0] * haI[0][c] + palmL[1] * haI[1][c] + palmL[2] * haI[2][c] - pivotT[c];
        *rawDeg = 2.0f * std::acos((std::min)(1.0f, std::fabs(wantQ->w))) * 57.2958f;
        src = 1;
    } else if (g_corpse.pawn) {
        // Cadavere trascinato (voce 11): il palmo sul punto preso, come una
        // portiera (punto dell'ultimo tick di CorpseDrive; nessuno al primo).
        if (!g_corpse.ptValid) return 0;
        for (int c = 0; c < 3; ++c) d[c] = g_corpse.pt[c] - pivotT[c];
        src = 2;
    } else if (mode >= 2) {
        // Mano vuota: il palmo sulla sfera (oltre shtele + 5 cm, o parcheggiata, niente).
        const HandProxy& pr = g_proxy[side];
        float sc[3], sa[3][3];
        if (!pr.actor || !ActorUsable(pr.actor) || !CompPose(pr.comp, sc, sa)) return 0;
        for (int c = 0; c < 3; ++c) d[c] = sc[c] - pivotT[c];
        if (Dot3(d, d) > ((std::max)(5.0f, g_tune.shTele) + 5.0f) * 0.01f * s * ((std::max)(5.0f, g_tune.shTele) + 5.0f) * 0.01f * s) return 0;
        src = 3;
    } else {
        return 0;
    }
    for (int k = 0; k < 3; ++k) wantP[k] = Dot3(d, ta[k]);
    *rawCm = std::sqrt(Dot3(d, d)) / s * 100.0f;
    return src;
}

}  // namespace

float RenderHandPose(int side, const float tgtPos[3], const float tgtAxes[3][3], const float* shoulder, float reach, float outPos[3],
                     float outAxes[3][3]) {
    std::memcpy(outPos, tgtPos, 12);
    std::memcpy(outAxes, tgtAxes, 36);
    if (side < 0 || side > 1) return 0.0f;
    if (side == 1 && GunHandHaptics(1)) {
        // La sinistra sull'arma, a due mani o nella ricarica fa parte della
        // presa dell'arma (regola dell'utente: mano dell'arma e mira non
        // cambiano mai): nessuno scostamento, subito (revisione dell'orchestratore, 26/10).
        RenderHandNone(side);
        return 0.0f;
    }
    RenderHand& rh = g_render[side];
    const double now = ProfMs() * 0.001;
    const float dt = rh.lastT > 0.0 ? static_cast<float>((std::min)(0.1, (std::max)(0.0, now - rh.lastT))) : 0.0f;
    rh.lastT = now;
    rh.deltaValid = false;
    const int mode = vr::control::g_solidHands.load();
    if (mode <= 0 && !rh.active) {
        rh.src = 0;
        if (vr::control::g_solidLog > 0) {
            // Solo diagnostica (confronto con off): il palmo misurato negli assi della mano.
            vr::control::g_solidLog.fetch_sub(1);
            const float s = vr::control::g_posScale;
            char b[160];
            std::snprintf(b, sizeof(b), "SOLID: off | palm local (%.2f, %.2f, %.2f) cm", g_palmLocal[side][0] / s * 100.0f,
                          g_palmLocal[side][1] / s * 100.0f, g_palmLocal[side][2] / s * 100.0f);
            vr::log::Line(b);
        }
        return 0.0f;  // spento: il bersaglio, identico
    }
    const float s = vr::control::g_posScale;
    const float(*ta)[3] = tgtAxes;
    const float zero[3] = {0, 0, 0};
    const float* palmL = g_palmValid[side] ? g_palmLocal[side] : zero;
    float pivotT[3];
    for (int c = 0; c < 3; ++c) pivotT[c] = tgtPos[c] + palmL[0] * ta[0][c] + palmL[1] * ta[1][c] + palmL[2] * ta[2][c];
    float wantP[3];
    Quat wantQ;
    float rawCm = 0.0f, rawDeg = 0.0f;
    rh.src = SolidSource(side, tgtPos, tgtAxes, palmL, pivotT, wantP, &wantQ, &rawCm, &rawDeg);
    // Zona morta e tetto: posizione (cm) e rotazione (gradi) separate.
    const float rawP = std::sqrt(Dot3(wantP, wantP));
    if (rawP > 1e-5f) {
        const float m = vr::pure::SolidShape(rawP / s * 100.0f, g_tune.shDead, g_tune.shCap) * 0.01f * s;
        for (float& c : wantP) c *= m / rawP;
    }
    {
        const float a = 2.0f * std::acos((std::min)(1.0f, std::fabs(wantQ.w)));
        if (a > 1e-5f) {
            const float aS = vr::pure::SolidShape(a * 57.2958f, g_tune.shTurn, g_tune.shCapTurn) * 0.0174533f;
            wantQ = Slerp({0.0f, 0.0f, 0.0f, 1.0f}, wantQ, aS / a);
        }
    }
    // Filtro (assi del bersaglio): entra in fretta, esce piano.
    {
        const float lw = std::sqrt(Dot3(wantP, wantP)), lf = std::sqrt(Dot3(rh.fp, rh.fp));
        const float a = rh.active ? vr::pure::SolidFilterAlpha(dt, lw >= lf, g_tune.shIn, g_tune.shOut) : 0.0f;
        for (int c = 0; c < 3; ++c) rh.fp[c] += (wantP[c] - rh.fp[c]) * a;
        const Quat fq0 = {rh.fq[0], rh.fq[1], rh.fq[2], rh.fq[3]};
        const float aw = 2.0f * std::acos((std::min)(1.0f, std::fabs(wantQ.w))), af = 2.0f * std::acos((std::min)(1.0f, std::fabs(fq0.w)));
        const float ar = rh.active ? vr::pure::SolidFilterAlpha(dt, aw >= af, g_tune.shIn, g_tune.shOut) : 0.0f;
        const Quat fq = Slerp(fq0, wantQ, ar);
        rh.fq[0] = fq.x;
        rh.fq[1] = fq.y;
        rh.fq[2] = fq.z;
        rh.fq[3] = fq.w;
    }
    // Il filtro parte da zero quando una fonte compare (frame dopo); finito
    // di uscire (sotto 0,5 mm e 0,05 gradi, nessuna fonte) torna esattamente zero.
    const float fl = std::sqrt(Dot3(rh.fp, rh.fp));
    const float fa = 2.0f * std::acos((std::min)(1.0f, std::fabs(rh.fq[3])));
    if (rh.src == 0 && fl < 0.0005f * s && fa < 0.0009f) {
        rh = RenderHand();
        rh.lastT = now;
        return 0.0f;
    }
    rh.active = true;
    if (fl < 1e-6f && fa < 1e-6f) return 0.0f;  // primo frame della fonte: il filtro parte da qui
    // Posa disegnata: assi Q x bersaglio, palmo spostato di fp, polso dal palmo.
    float Q[3][3];
    QuatToRows({rh.fq[0], rh.fq[1], rh.fq[2], rh.fq[3]}, Q);
    float haR[3][3], pivR[3], hpR[3];
    for (int j = 0; j < 3; ++j)
        for (int c = 0; c < 3; ++c) haR[j][c] = Q[j][0] * ta[0][c] + Q[j][1] * ta[1][c] + Q[j][2] * ta[2][c];
    for (int c = 0; c < 3; ++c) {
        pivR[c] = pivotT[c] + rh.fp[0] * ta[0][c] + rh.fp[1] * ta[1][c] + rh.fp[2] * ta[2][c];
        hpR[c] = pivR[c] - (palmL[0] * haR[0][c] + palmL[1] * haR[1][c] + palmL[2] * haR[2][c]);
    }
    // Entro la portata del braccio (armstretch): il polso disegnato non va piu'
    // lontano dalla spalla della portata o del bersaglio, se e' piu' lontano.
    // Riportato sulla sfera di quel raggio attorno alla spalla lungo la sua
    // direzione: si toglie solo la parte che allunga il braccio, quella di
    // lato (l'abbassamento della radio col braccio teso) resta. Revisione
    // 26/10: prima lo scostamento si accorciava verso il bersaglio, e col
    // bersaglio sul bordo della portata (braccio teso, frequente allungandosi)
    // spariva tutto (radio premuta: 40 frame di "wrist 0.00 cm"), e vicino al
    // bordo il tremolio del controller lo accendeva e spegneva. La proiezione
    // e' continua (mai piu' lontana del punto di partenza): niente scatti.
    const float cut = shoulder ? vr::pure::ReachClamp(shoulder, tgtPos, reach, hpR) : 0.0f;
    const bool reachCut = cut > 0.0f;
    std::memcpy(outPos, hpR, 12);
    std::memcpy(outAxes, haR, 36);
    const float od[3] = {hpR[0] - tgtPos[0], hpR[1] - tgtPos[1], hpR[2] - tgtPos[2]};
    for (int k = 0; k < 3; ++k) rh.dPos[k] = Dot3(od, ta[k]);
    std::memcpy(rh.dRot, Q, sizeof(Q));
    rh.dCut = cut;
    rh.deltaValid = true;
    if (vr::control::g_solidLog > 0) {
        vr::control::g_solidLog.fetch_sub(1);
        static const char* kSrc[4] = {"none", "held object", "constrained point", "hand sphere"};
        const float pd[3] = {pivR[0] - pivotT[0], pivR[1] - pivotT[1], pivR[2] - pivotT[2]};
        // Polso disegnato rispetto al bersaglio negli assi del bersaglio (cm): il tremolio si legge qui.
        char cutTxt[64] = "";
        if (reachCut) std::snprintf(cutTxt, sizeof(cutTxt), " | cut %.2f cm to the arm's reach", cut / s * 100.0f);
        char b[600];
        std::snprintf(b, sizeof(b),
                      "SOLID: %s, raw %.2f cm %.1f deg, shaped %.2f cm, drawn palm %.2f cm (%.2f, %.2f, %.2f) %.1f deg, wrist %.2f cm"
                      " (%.3f, %.3f, %.3f)%s | target palm (%.1f, %.1f, %.1f) dt %.1f ms | palm local (%.2f, %.2f, %.2f) cm",
                      kSrc[rh.src & 3], rawCm, rawDeg, std::sqrt(Dot3(wantP, wantP)) / s * 100.0f, std::sqrt(Dot3(pd, pd)) / s * 100.0f,
                      pd[0] / s * 100.0f, pd[1] / s * 100.0f, pd[2] / s * 100.0f, fa * 57.2958f, std::sqrt(Dot3(od, od)) / s * 100.0f,
                      rh.dPos[0] / s * 100.0f, rh.dPos[1] / s * 100.0f, rh.dPos[2] / s * 100.0f, cutTxt, pivotT[0], pivotT[1], pivotT[2],
                      dt * 1000.0f, palmL[0] / s * 100.0f, palmL[1] / s * 100.0f, palmL[2] / s * 100.0f);
        vr::log::Line(b);
    }
    return std::sqrt(Dot3(od, od)) / s * 100.0f;
}

void RenderHandNone(int side) {
    if (side < 0 || side > 1) return;
    const double t = g_render[side].lastT;
    g_render[side] = RenderHand();
    g_render[side].lastT = t;
    g_beyondReach[side] = false;  // niente posa scritta da noi: la misura del palmo torna libera
}

void SetHandBeyondReach(int side, bool beyond) {
    if (side >= 0 && side <= 1) g_beyondReach[side] = beyond;
}

void Watchdog() {
    // Le collisioni con Walker si ripristinano al ritorno di Tick (serve
    // sapere se l'oggetto e' fuori dalla sua capsula).
    if (!g_held[0].actor && !g_held[1].actor && !g_gren.actor) return;
    if (ProfMs() * 0.001 - g_lastTickAt < 0.3) return;
    for (int h = 0; h < 2; ++h) Release(h, "no hands");
    // Voce 8: senza tick la granata in mano (gravita' e collisione col mondo
    // spente, ultima velocita' scritta) volerebbe via attraverso i muri: cade.
    GrenadeRelease(g_tickPawn, "no hands", ProfMs() * 0.001);
}

void QueueSpawn(const std::string& mesh, float distCm, const float* scale3) {
    std::lock_guard<std::mutex> lock(g_spawnMutex);
    g_spawnMesh = mesh;
    g_spawnDistCm = distCm;
    for (int c = 0; c < 3; ++c) g_spawnScale[c] = scale3 ? scale3[c] : 1.0f;
}

// Voce 5: danno dell'arma (YWeapon.m_damage +0x704, come gli scudi), il suo
// tipo (m_damageType +0x248, classe: letto sui default, YDamageType_AK47,
// _AssaultRifle...; solo se e' davvero un DamageType vivo) e il Controller di
// chi spara (Instigator dell'arma +0xE8 -> Pawn.Controller).
void ShotWeapon(ShotEvent& e, void* weapon) {
    if (!weapon) return;
    float dmg = 0.0f;
    SafeCopy(&dmg, static_cast<char*>(weapon) + 0x704, 4);
    e.damage = std::isfinite(dmg) && dmg > 0.0f && dmg < 100000.0f ? dmg : 0.0f;
    const uint32_t pw = U32(weapon, 0xE8);
    e.ctrl = pw ? reinterpret_cast<void*>(static_cast<uintptr_t>(U32(reinterpret_cast<void*>(pw), 0x214))) : nullptr;
    static int32_t dtName = -1;
    if (dtName < 0) dtName = vr::script::NameIndex("DamageType");
    const uint32_t dt = U32(weapon, 0x248);
    e.dmgType = dt && dtName >= 0 && vr::script::LiveObject(reinterpret_cast<void*>(dt)) && ClassIsA(dt, dtName) ? reinterpret_cast<void*>(dt) : nullptr;
}

void OnShot(const float* start, const float* hit, void* hitActor, void* hitComp, void* weapon) {
    bool ownerOk = hitActor && hitComp && U32(hitComp, kOffCompOwner) == static_cast<uint32_t>(reinterpret_cast<uintptr_t>(hitActor));
    static int logged = 0;
    if (logged < 4) {
        ++logged;
        vr::log::Line("PROPS: shot hit " + (hitActor ? vr::script::ClassName(hitActor) : std::string("none")) + " / " +
                      (hitComp ? vr::script::ClassName(hitComp) : std::string("none")) + (ownerOk ? " (component of the actor)" : " (component NOT of the actor)"));
    }
    if (!ownerOk) return;
    ShotEvent e;
    e.actor = hitActor;
    e.comp = hitComp;
    float l = 0.0f;
    for (int i = 0; i < 3; ++i) {
        e.hit[i] = hit[i];
        e.dir[i] = hit[i] - start[i];
        l += e.dir[i] * e.dir[i];
    }
    l = std::sqrt(l);
    if (!(l > 1.0f)) return;
    for (float& c : e.dir) c /= l;
    ShotWeapon(e, weapon);
    std::lock_guard<std::mutex> lock(g_shotMutex);
    if (g_shots.size() < 16) g_shots.push_back(e);
}

// --- Scudi con oggetti tenuti (voce 3 del piano dei combattimenti, 26/10) ---
// Il ciclo dei colpi istantanei del gioco (FUN_0093F510, slot +0x424 della
// vtable di ogni YWeapon, IA compresa) chiama CalcWeaponFire (0x004A3D50:
// SingleLineCheck con Source = l'arma e flag 0x22CB7 = Pawns, Movers, Level,
// Others, OnlyProjActor, LevelGeometry, SingleResult, Material, Terrain,
// ComplexCollision), poi fa danno all'attore colpito (FUN_009F7300) e gli
// effetti (FUN_00F5E6B0) dall'FImpactInfo; il colpo prosegue solo attraverso
// gli YDestructibleObject con il bit 0x800 (+0x1FC, Ghidra). Un KActor tenuto
// ha bProjTarget (+0xA0 bit 0x80000000, letto sui KActor del livello 1 e sui
// default di KActorFromStatic e KActorSpawnable): la trace lo prende anche con
// bBlockActors spento dalla presa, e il colpo si ferma li'. La mod aggiunge
// spinta, vibrazione e usura (ShieldTick) e riscrive sull'oggetto il colpo
// che ha preso Walker attraversando prima la scatola dell'oggetto tenuto
// (oggetto dentro il corpo di Walker, dove la trace trova prima il corpo).
// Chi spara e' un nemico? (revisione della voce 3: prima contavano anche i
// colpi di Adams e Lugo.) Instigator dell'arma (Actor +0xE8), deciso come
// nella pipeline dei colpi (PawnHostile: squadra, tabella delle squadre,
// classe; in cache per pawn 2 s, le chiamate al gioco solo allora). Senza
// Instigator (o non leggibile) conta come nemico: nessun amico spara senza.
bool ShooterHostile(void* weapon, void* walker) {
    void* pw = reinterpret_cast<void*>(static_cast<uintptr_t>(U32(weapon, 0xE8)));
    const uint32_t cls = pw ? U32(pw, kOffClass) : 0;
    if (!pw || !cls || !ActorAlive(pw)) return true;
    return PawnHostile(walker, pw, cls, ProfMs() * 0.001);
}

int OnOtherShot(void* weapon, void* walker, const float* start, const float* end, void* impact, bool bullet) {
    if (!weapon || !walker || !impact) return 0;
    const uint32_t walkerU = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(walker));
    if (U32(weapon, 0xE8) == walkerU) return 0;  // Instigator: un'altra arma di Walker (postazione fissa)
    ShieldStats& st = g_shieldStats;
    if (!bullet) {
        // Chiamata da uno script (AYWeapon_execCalcWeaponFire), non dal ciclo
        // dei colpi: un controllo, non un proiettile. Nessun effetto.
        ++st.script;
        return 0;
    }
    uint32_t hitA = 0;
    float hit[3];
    if (!TrySafeReadUint32(impact, &hitA) || !SafeCopy(hit, static_cast<char*>(impact) + 4, 12)) return 0;
    ++st.shots;
    const bool onWalker = hitA == walkerU;
    if (onWalker) ++st.onWalker;
    // Spento dall'opzione: il colpo passa (2). In una scena o a gioco bloccato
    // (oggetti gia' lasciati da TickImpl) la mod non tocca nulla.
    const bool enabled = vr::control::g_propShield.load();
    const bool on = enabled && !vr::control::g_cinemaActive && !vr::control::g_gameLocked;
    // Direzione del colpo: verso il punto colpito (o la fine della trace).
    float dir[3] = {hit[0] - start[0], hit[1] - start[1], hit[2] - start[2]};
    float len = std::sqrt(Dot3(dir, dir));
    if (!(len > 1.0f)) {
        for (int c = 0; c < 3; ++c) dir[c] = end[c] - start[c];
        len = std::sqrt(Dot3(dir, dir));
        if (!(len > 1.0f)) return 0;
    }
    for (float& c : dir) c /= len;
    // Voce 5: una bombola portata (in mano, in volo o posata) colpita dall'arma
    // di un nemico prende il danno del colpo come l'originale e puo' esplodere
    // (CarrierShot nel tick). I colpi di Adams e Lugo no, come per gli scudi.
    // Due strade: il gioco ha preso il portatore (colpo diretto), o ha preso
    // Walker attraversando prima la scatola della bombola tenuta (riscrittura
    // qui sotto: il caso solito con la bombola davanti al petto; revisione
    // della voce 5, prima solo i colpi diretti la facevano esplodere).
    const bool sceneFree = !vr::control::g_cinemaActive && !vr::control::g_gameLocked;
    auto queueCarrier = [&](const void* actor, const float* at, const char* why) {
        const Carrier* c = CarrierOfActor(actor);
        if (!c) return;
        ShotEvent e;
        e.actor = c->actor;
        e.comp = c->comp;
        std::memcpy(e.hit, at, sizeof(e.hit));
        std::memcpy(e.dir, dir, sizeof(e.dir));
        ShotWeapon(e, weapon);
        e.other = true;
        e.why = why;
        std::lock_guard<std::mutex> lock(g_shotMutex);
        if (g_shots.size() < 16) g_shots.push_back(e);
    };
    {
        const void* hitActor = reinterpret_cast<const void*>(static_cast<uintptr_t>(hitA));
        if (sceneFree && CarrierOfActor(hitActor) && ShooterHostile(weapon, walker)) queueCarrier(hitActor, hit, "enemy shot");
    }
    auto queue = [&](int h, const float* at, bool redirected) {
        ShieldEvent e;
        e.hand = h;
        e.actor = g_held[h].actor;
        e.comp = g_held[h].comp;
        e.weapon = weapon;
        std::memcpy(e.hit, at, sizeof(e.hit));
        std::memcpy(e.dir, dir, sizeof(e.dir));
        float dmg = 0.0f;
        SafeCopy(&dmg, static_cast<char*>(weapon) + 0x704, 4);  // YWeapon.m_damage
        e.damage = std::isfinite(dmg) ? dmg : 0.0f;
        e.redirected = redirected;
        e.at = ProfMs() * 0.001;
        std::lock_guard<std::mutex> lock(g_shotMutex);
        if (g_shieldEvents.size() < 64) g_shieldEvents.push_back(e);
    };
    for (int h = 0; h < 2; ++h) {
        const Held& hd = g_held[h];
        if (!hd.actor || hd.constrained || hitA != static_cast<uint32_t>(reinterpret_cast<uintptr_t>(hd.actor))) continue;
        if (!enabled) {
            // propshield off: chi chiama rifa' la trace senza gli oggetti tenuti.
            // Una bombola tenuta colpita cosi' ha gia' il suo colpo in coda (sopra):
            // propshield decide solo se gli oggetti tenuti fermano i proiettili, non
            // se la bombola e' colpita; il proiettile la buca e prosegue.
            ++st.passed;
            return 2;
        }
        if (!on) return 0;
        if (!ShooterHostile(weapon, walker)) {
            // Un compagno: il gioco ferma il colpo sull'oggetto (bProjTarget),
            // ma niente usura, spinta, vibrazione ne' oggetto che vola via.
            ++st.friendly;
            return 0;
        }
        ++st.onHeld;
        queue(h, hit, false);
        return 0;
    }
    if (!onWalker || (!g_held[0].actor && !g_held[1].actor)) return 0;
    // Con propshield spento si guarda solo una bombola tenuta: il colpo che
    // prende Walker attraversandone la scatola la colpisce lo stesso (e arriva
    // a Walker, come il colpo diretto che la buca, sopra).
    const bool carrierOnly = !on && !enabled && sceneFree && (CarrierOfActor(g_held[0].actor) || CarrierOfActor(g_held[1].actor));
    if (!on && !carrierOnly) return 0;
    // Il colpo ha preso Walker: attraversa prima la scatola di un oggetto
    // tenuto (limiti della StaticMesh negli assi del componente)? La trace a
    // estensione zero prende i corpi della PhysicsAsset di Walker, braccia
    // comprese (26/10, manichino a 6 m: mirando alla testa i colpi finivano
    // sul braccio davanti al viso, 41 unita' dall'asse; al centro del Pawn sul
    // petto, 18 unita'). In VR il corpo vero e' dietro: l'oggetto tenuto
    // davanti al viso o premuto sul petto, con la mano o il braccio dentro la
    // sua scatola, puo' stare oltre il punto colpito. Il segmento va quindi
    // fino al punto della linea del colpo piu' vicino all'asse di Walker (dove
    // il colpo arriverebbe al corpo), almeno 4 unita' oltre il punto colpito e
    // al piu' 60: un colpo nella schiena non incontra l'oggetto tenuto davanti.
    float wl[3];
    if (!SafeCopy(wl, static_cast<char*>(walker) + kOffActorLocation, 12)) return 0;
    float reach = len + 4.0f;
    {
        const float dxy = dir[0] * dir[0] + dir[1] * dir[1];
        if (dxy > 0.01f) {
            const float sAxis = ((wl[0] - start[0]) * dir[0] + (wl[1] - start[1]) * dir[1]) / dxy;
            reach = (std::max)(reach, (std::min)(sAxis, len + 60.0f));
        }
    }
    const float b[3] = {start[0] + dir[0] * reach, start[1] + dir[1] * reach, start[2] + dir[2] * reach};
    int best = -1;
    float bestT = 2.0f, bestN[3] = {0, 0, 0};
    for (int h = 0; h < 2; ++h) {
        const Held& hd = g_held[h];
        if (!hd.actor || hd.constrained || !ActorUsable(hd.actor)) continue;
        if (carrierOnly && !CarrierOfActor(hd.actor)) continue;
        float lc[3], le[3], m[4][4];
        if (!MeshLocalBounds(hd.comp, lc, le) || !ReadMatrix(hd.comp, m)) continue;
        float ax[3][3], e[3], c[3];
        bool okAx = true;
        for (int i = 0; i < 3; ++i) {
            const float sc = std::sqrt(m[i][0] * m[i][0] + m[i][1] * m[i][1] + m[i][2] * m[i][2]);
            if (!(sc > 1e-6f) || !std::isfinite(sc)) okAx = false;
            for (int k = 0; k < 3; ++k) ax[i][k] = okAx ? m[i][k] / sc : 0.0f;
            e[i] = le[i] * sc;
        }
        if (!okAx) continue;
        for (int k = 0; k < 3; ++k) c[k] = m[3][k] + lc[0] * m[0][k] + lc[1] * m[1][k] + lc[2] * m[2][k];
        float t = 0.0f, n[3];
        if (vr::pure::SegmentObbEntry(start, b, c, ax, e, &t, n) && t < bestT) {
            best = h;
            bestT = t;
            std::memcpy(bestN, n, sizeof(bestN));
        }
    }
    if (best < 0) return 0;
    if (!ShooterHostile(weapon, walker)) {
        // Colpo di un compagno attraverso l'oggetto tenuto: resta com'e'.
        ++st.friendly;
        return 0;
    }
    float entry[3];
    for (int k = 0; k < 3; ++k) entry[k] = start[k] + (b[k] - start[k]) * bestT;
    if (carrierOnly) {
        // propshield off: la bombola prende il colpo, il proiettile prosegue fino a Walker.
        queueCarrier(g_held[best].actor, entry, "enemy shot through the hold, propshield off");
        return 0;
    }
    // FImpactInfo: HitActor +0, HitLocation +4, HitNormal +0x10; HitInfo +0x34:
    // Material, PhysMaterial, Item (+0x3C, -1 = nessuno), LevelIndex, BoneName
    // (+0x44, FName), HitComponent (+0x4C). Danno ed effetti del gioco vanno
    // all'oggetto (gli oggetti rompibili si rompono da se').
    char* ip = static_cast<char*>(impact);
    const uint32_t actorU = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_held[best].actor));
    const uint32_t compU = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_held[best].comp));
    const uint32_t zero2[2] = {0, 0};
    const int32_t noItem = -1;
    if (!SafeCopy(ip, &actorU, 4) || !SafeCopy(ip + 4, entry, 12) || !SafeCopy(ip + 0x10, bestN, 12) || !SafeCopy(ip + 0x34, zero2, 8) ||
        !SafeCopy(ip + 0x3C, &noItem, 4) || !SafeCopy(ip + 0x44, zero2, 8) || !SafeCopy(ip + 0x4C, &compU, 4))
        return 0;
    ++st.redirected;
    queue(best, entry, true);
    queueCarrier(g_held[best].actor, entry, "enemy shot redirected from Walker");  // voce 5: bombola tenuta davanti al petto
    return 1;
}

void HeldTraceable(bool on, uint32_t saved[2]) {
    for (int h = 0; h < 2; ++h) {
        void* comp = g_held[h].comp;
        uint32_t f = 0;
        if (!g_held[h].actor || !comp || !TrySafeReadUint32(static_cast<char*>(comp) + kOffCompCollision, &f)) continue;
        if (!on) {
            saved[h] = f;
            f &= ~0x8u;  // BlockZeroExtent
        } else {
            f = (f & ~0x8u) | (saved[h] & 0x8u);
        }
        SafeCopy(static_cast<char*>(comp) + kOffCompCollision, &f, 4);
    }
}

void SetViewOrigin(const float* loc) {
    if (!loc || !std::isfinite(loc[0]) || !std::isfinite(loc[1]) || !std::isfinite(loc[2])) return;
    std::memcpy(g_viewOrigin, loc, sizeof(g_viewOrigin));
    g_viewOriginAt = ProfMs() * 0.001;
}

void SetShieldTraceTest(bool traceable) {
    g_shieldNoTraceTest = !traceable;
    vr::log::Line(traceable ? "PROPS: shieldtrace on: held objects block traces again (bit restored in the next tick)"
                            : "PROPS: shieldtrace off (test): held objects are left out of zero-extent traces");
}

namespace {
// Collaudo "shieldtrace off" a ogni tick: spegne BlockZeroExtent agli oggetti
// tenuti (salvando il bit di prima, una volta per componente) e lo rimette a
// quelli non piu' tenuti, o a tutti quando il collaudo finisce.
void ShieldTraceTestTick() {
    const bool test = g_shieldNoTraceTest.load();
    if (!test && g_noTraceComps.empty()) return;
    for (size_t i = 0; i < g_noTraceComps.size();) {
        NoTraceComp& n = g_noTraceComps[i];
        bool held = false;
        for (int h = 0; h < 2; ++h)
            if (test && g_held[h].actor == n.actor && g_held[h].comp == n.comp) held = true;
        if (held) {
            ++i;
            continue;
        }
        uint32_t f = 0;
        if (ActorAlive(n.actor) && TrySafeReadUint32(static_cast<char*>(n.comp) + kOffCompCollision, &f)) {
            f = (f & ~0x8u) | n.bit;
            SafeCopy(static_cast<char*>(n.comp) + kOffCompCollision, &f, 4);
            vr::log::Line("PROPS: shieldtrace: " + vr::script::ObjectName(reinterpret_cast<void*>(U32(n.comp, kOffSmcStaticMesh))) +
                          " blocks zero-extent traces again");
        }
        g_noTraceComps.erase(g_noTraceComps.begin() + static_cast<std::ptrdiff_t>(i));
    }
    if (!test) return;
    for (int h = 0; h < 2; ++h) {
        void* comp = g_held[h].comp;
        uint32_t f = 0;
        if (!g_held[h].actor || !comp || !TrySafeReadUint32(static_cast<char*>(comp) + kOffCompCollision, &f)) continue;
        bool known = false;
        for (const NoTraceComp& n : g_noTraceComps)
            if (n.comp == comp) known = true;
        if (!known) g_noTraceComps.push_back({g_held[h].actor, comp, f & 0x8u});
        if (f & 0x8u) {
            f &= ~0x8u;
            SafeCopy(static_cast<char*>(comp) + kOffCompCollision, &f, 4);
        }
    }
}
}  // namespace

void QueueEnemyFire(uint32_t pawn, int shots, const float* target) {
    std::lock_guard<std::mutex> lock(g_spawnMutex);
    g_enemyFireReq = EnemyFireReq();
    g_enemyFireReq.pawn = pawn;
    g_enemyFireReq.shots = shots;
    if (target) {
        g_enemyFireReq.hasTarget = true;
        std::memcpy(g_enemyFireReq.target, target, sizeof(g_enemyFireReq.target));
    }
}

void QueueWeaponTest() {
    std::lock_guard<std::mutex> lock(g_spawnMutex);
    g_wpnTestReq = true;
}

void QueueCorpseTest(bool bones) {
    std::lock_guard<std::mutex> lock(g_spawnMutex);
    g_corpseTestReq = bones ? 2 : 1;
}

void SetWeaponGrip(const float off[3], const float rot[3]) {
    // In coda: la applica il prossimo tick, sul thread di gioco.
    {
        std::lock_guard<std::mutex> lock(g_spawnMutex);
        for (int i = 0; i < 3; ++i) {
            g_wpnGripNew.off[i] = off[i];
            g_wpnGripNew.rot[i] = rot[i];
        }
        g_wpnGripReq = true;
    }
    char b[200];
    std::snprintf(b, sizeof(b), "WEAPON: grip of a picked-up weapon in the left hand: %.1f %.1f %.1f cm, pitch %.0f yaw %.0f roll %.0f deg", off[0],
                  off[1], off[2], rot[0], rot[1], rot[2]);
    vr::log::Line(b);
}

bool WeaponForHand(void* actor, int h) {
    if (!actor) return false;
    if (actor == g_wpn.actor) return true;  // quella in mano: mai il tasto A, con nessuna mano
    if (h != 1 || !vr::control::g_weaponPickup.load() || !vr::control::g_grabProps.load()) return false;
    if (ProfMs() * 0.001 - g_wpnHandAt > 0.5) return false;  // tick delle prese fermo (cinema, gioco bloccato)
    if (g_wpnPickupName < 0 || !vr::script::LiveObject(actor)) return false;
    // Le stesse condizioni di ScanObject (tipo 8): classe o sottoclasse, con
    // il PhysicsAsset, non temporanea (DBNO).
    const uint32_t cls = U32(actor, kOffClass);
    if (!cls || !ClassIsA(cls, g_wpnPickupName)) return false;
    return (U32(actor, kOffPickupFlags) & kPickupHasPhys) && !(U32(actor, kOffPickupTemp) & 1);
}

bool WeaponPickupGone(void* actor) {
    if (!actor || g_wpnPickupName < 0 || !vr::script::LiveObject(actor)) return false;
    const uint32_t cls = U32(actor, kOffClass);
    if (!cls || !ClassIsA(cls, g_wpnPickupName)) return false;
    return !U32(actor, kOffPickupMesh) || (U32(actor, kOffPickupFlags) & kPickupHidden) || (U32(actor, kOffActorFlags9C) & (kHidden | kDeleteMe));
}

void QueueGrabTest(const std::string& meshPart) {
    std::lock_guard<std::mutex> lock(g_spawnMutex);
    g_grabTestMesh = meshPart;
}

struct GlassHitReq {
    uint32_t actor = 0;
    float kg = 0.0f, speed = 0.0f;
} g_glassHitReq;

void QueueGlassHit(uint32_t actor, float kg, float speed) {
    std::lock_guard<std::mutex> lock(g_spawnMutex);
    g_glassHitReq = {actor, kg, speed};
}

float g_explHitReq = -1.0f;  // collaudo "explhit" (voce 5): danno in attesa, -1 nessuno

void QueueExplHit(float damage) {
    std::lock_guard<std::mutex> lock(g_spawnMutex);
    g_explHitReq = damage;
}

// Collaudo "glasshit": evento finto di un oggetto lanciato (senza oggetto) contro
// il centro del box del bersaglio, nella direzione Walker -> bersaglio.
void GlassHitTest(void* walker, const GlassHitReq& r) {
    void* tgt = reinterpret_cast<void*>(static_cast<uintptr_t>(r.actor));
    const uint32_t cls = U32(tgt, kOffClass);
    const uint8_t ck = cls ? ClassKind(cls) : 0;
    const uint32_t cc = U32(tgt, kOffActorCollisionComp);
    Prop b;
    float wl[3];
    if (!ActorAlive(tgt) || ck < 4 || ck > 6 || !cc || !ReadBounds(reinterpret_cast<void*>(cc), &b) ||
        !SafeCopy(wl, static_cast<char*>(walker) + kOffActorLocation, 12)) {
        vr::log::Line("PROPS: glasshit: not a fractured mesh, sand window or destructible with bounds");
        return;
    }
    const float s = vr::control::g_posScale;
    PropHit e;
    e.target = tgt;
    e.targetKind = ck == 4 ? kTgtFractured : (ck == 5 ? kTgtSandBomb : kTgtDestructible);
    e.targetComp = reinterpret_cast<void*>(cc);
    float l = 0.0f;
    for (int i = 0; i < 3; ++i) {
        e.loc[i] = 0.5f * (b.boxMin[i] + b.boxMax[i]);
        e.dir[i] = e.loc[i] - wl[i];
        l += e.dir[i] * e.dir[i];
    }
    l = std::sqrt(l);
    if (!(l > 1.0f)) return;
    for (int i = 0; i < 3; ++i) {
        e.dir[i] /= l;
        e.normal[i] = -e.dir[i];
        e.vel[i] = e.dir[i] * r.speed * s;
        e.cvel[i] = e.vel[i];
    }
    e.speed = r.speed * s;
    const float k = vr::control::g_massKgPerUnit.load();
    e.mass = k > 0.0f ? r.kg / k : 0.0f;
    e.kind = HitKind::kFlying;
    e.source = HitSource::kThrown;
    // Come la pipeline: trace a estensione zero (poligoni) da Walker al centro;
    // se prende il bersaglio, punto, normale, componente e Item vengono da li'.
    TraceResult tr;
    const float st[3] = {wl[0], wl[1], wl[2] + 40.0f};
    float en[3];
    for (int i = 0; i < 3; ++i) en[i] = e.loc[i] + e.dir[i] * 100.0f;
    if (EngineTrace(walker, st, en, 0.0f, &tr, kHitTraceFlags | kTraceComplex) && tr.actor == tgt) {
        std::memcpy(e.loc, tr.loc, sizeof(e.loc));
        std::memcpy(e.normal, tr.normal, sizeof(e.normal));
        e.targetComp = tr.comp;
        e.item = tr.item;
    }
    char line[200];
    std::snprintf(line, sizeof(line), "PROPS: glasshit %s (test event, no object; trace %s, item %d)", vr::script::ClassName(tgt).c_str(),
                  tr.actor == tgt ? "hit it" : (tr.actor ? vr::script::ClassName(tr.actor).c_str() : "nothing"), e.item);
    vr::log::Line(line);
    WorldDamage(e, ProfMs() * 0.001);
}

namespace {

// Scudi (voce 3 dei combattimenti): i colpi raccolti da OnOtherShot. Per ogni
// colpo contato (i pallini della stessa arma entro 40 ms una volta sola):
// - usura: ShieldWear(danno) colpi in piu'; regge ShieldDurability(materiale
//   dal nome della mesh, kg) colpi, poi vola via dalla mano (o si rompe, se il
//   gioco lo sa rompere);
// - spinta: AddImpulse nel punto colpito (velocita' ShieldDv) e la posa voluta
//   spostata e riportata (ShieldKick in Drive);
// - vibrazione: ImpactPulse dell'impulso del proiettile (ShieldShotImpulse),
//   piena e lunga (90 ms) quando vola via; con impacthaptics off impulsi fissi.
void ShieldTick(void* walker, double now) {
    std::vector<ShieldEvent> evs;
    {
        std::lock_guard<std::mutex> lock(g_shotMutex);
        evs.swap(g_shieldEvents);
    }
    if (evs.empty()) return;
    const float s = vr::control::g_posScale;
    ShieldStats& st = g_shieldStats;
    for (const ShieldEvent& e : evs) {
        if (e.hand < 0 || e.hand > 1) continue;
        Held& hd = g_held[e.hand];
        if (hd.actor != e.actor || hd.comp != e.comp || !ActorUsable(e.actor)) {
            // Rotto dal danno del gioco (una bottiglia) o lasciato nel frattempo.
            if (!ActorUsable(e.actor)) {
                ++st.broke;
                g_shieldWear.erase(e.comp);
                vr::log::Line(std::string("PROPS: shield gone under a shot (") + (e.redirected ? "redirected" : "stopped by the game") +
                              "): the game's damage destroyed it or it was released");
            }
            continue;
        }
        if (e.weapon == g_shieldVolley.weapon && e.comp == g_shieldVolley.comp && e.at - g_shieldVolley.at < 0.04) {
            ++st.pellets;  // stesso colpo (pallini): gia' contato
            continue;
        }
        g_shieldVolley = {e.weapon, e.comp, e.at};
        const std::string mesh = vr::script::ObjectName(reinterpret_cast<void*>(U32(e.comp, kOffSmcStaticMesh)));
        const float kg = MassKg(hd);
        if (g_shieldWear.size() > 256) g_shieldWear.clear();
        ShieldWearState& w = g_shieldWear[e.comp];
        if (w.cap <= 0) {
            std::string low = mesh;
            for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            const vr::pure::ShieldMaterial mat = vr::pure::ShieldMaterialOf(low);
            w.material = static_cast<int>(mat);
            w.cap = vr::pure::ShieldDurability(mat, kg);
        }
        w.actor = e.actor;
        w.hits += vr::pure::ShieldWear(e.damage);
        const bool out = w.hits >= w.cap;
        const float J = vr::pure::ShieldShotImpulse(e.damage);
        const float dv = vr::pure::ShieldDv(J, kg);
        // Spinta nel punto colpito (come OnShot) e posa voluta spostata e riportata.
        struct {
            float impulse[3];
            float pos[3];
            int32_t bone[2];
            uint32_t velChange;
        } ai = {{e.dir[0] * dv * s, e.dir[1] * dv * s, e.dir[2] * dv * s}, {e.hit[0], e.hit[1], e.hit[2]}, {0, 0}, 1};
        vr::script::CallFunction(e.comp, "AddImpulse", &ai);
        float axis[3] = {0, 0, 0};
        bool hasAxis = false;
        {
            Prop cur;
            if (ReadBounds(e.comp, &cur)) {
                const float r[3] = {e.hit[0] - 0.5f * (cur.boxMin[0] + cur.boxMax[0]), e.hit[1] - 0.5f * (cur.boxMin[1] + cur.boxMax[1]),
                                    e.hit[2] - 0.5f * (cur.boxMin[2] + cur.boxMax[2])};
                Cross3(r, e.dir, axis);
                const float l = std::sqrt(Dot3(axis, axis)), rl2 = Dot3(r, r);
                hasAxis = l > 1e-3f && rl2 > 1.0f;
                if (hasAxis) {
                    for (float& c : axis) c /= l;
                    hd.kickAng = (std::min)(0.15f, vr::pure::ShieldKickCm(dv) * 0.01f * s * l / (std::max)(rl2, 25.0f));
                }
            }
        }
        if (!hasAxis) hd.kickAng = 0.0f;
        hd.kickAt = now;
        std::memcpy(hd.kickDir, e.dir, sizeof(hd.kickDir));
        std::memcpy(hd.kickAxis, axis, sizeof(hd.kickAxis));
        hd.kickAmp = vr::pure::ShieldKickCm(dv) * 0.01f * s;
        // Vibrazione (prima del rilascio: il log legge il nome dal componente).
        const float gain = HapticGain();
        if (gain > 0.0f) {
            vr::pure::HapticPulseSpec sp = vr::pure::ImpactPulse(J, kg, gain, g_tune.hapJ);
            if (out) {
                sp.amp = gain;
                sp.ms = 90;
                sp.freq = 90.0f;
            }
            HapticSend(e.hand, out ? "shield shot out" : "shield shot", e.comp, J, dv, kg, sp, now, false, e.hit);
        } else {
            vr::input::PulseHand(e.hand ? 0 : 1, out ? 90u : 30u);
        }
        static const char* const kMat[3] = {"normal", "fragile", "metal or sand"};
        char line[420];
        std::snprintf(line, sizeof(line),
                      "PROPS: shield %s (%s hand) took a shot from %s (damage %.0f, %s): hit %d of %d (%s, %.1f kg), pushed %.2f m/s, kick %.1f cm%s",
                      mesh.c_str(), e.hand ? "left" : "right", e.weapon ? vr::script::ClassName(e.weapon).c_str() : "?", e.damage,
                      e.redirected ? "redirected from Walker" : "stopped by the game", w.hits, w.cap, kMat[w.material % 3], kg, dv,
                      vr::pure::ShieldKickCm(dv), out ? ", knocked out of the hand" : "");
        vr::log::Line(line);
        if (!out) continue;
        ++st.knocked;
        void* a = hd.actor;
        void* c = hd.comp;
        const bool brk = Breakable(a);
        Release(e.hand, "shot out of the hand");
        if (brk) {
            ++st.broke;
            g_shieldWear.erase(c);
            BreakProp(a, c, "worn out as a shield");
            continue;
        }
        // Vola via lontano dalla testa (revisione della voce 3: lungo il colpo
        // tornava indietro attraverso il viso e la camera): il colpo senza la
        // parte verso la testa, di lato dalla parte della mano e in basso
        // (ShieldKnockDir), alla velocita' della spinta (ShieldKnockSpeed:
        // 0,8 + 1,2 dv m/s), girando attorno all'asse del colpo.
        float head[3];
        if (now - g_viewOriginAt < 0.5) {
            std::memcpy(head, g_viewOrigin, sizeof(head));
        } else {
            // Senza la vista recente: occhi sopra il centro di Walker.
            float wl[3] = {e.hit[0], e.hit[1], e.hit[2]};
            if (walker) SafeCopy(wl, static_cast<char*>(walker) + kOffActorLocation, 12);
            head[0] = wl[0];
            head[1] = wl[1];
            head[2] = wl[2] + vr::control::g_fpEyeHeight.load();
        }
        float oc[3] = {e.hit[0], e.hit[1], e.hit[2]};
        {
            Prop cur;
            if (ReadBounds(c, &cur))
                for (int k = 0; k < 3; ++k) oc[k] = 0.5f * (cur.boxMin[k] + cur.boxMax[k]);
        }
        float kd[3];
        vr::pure::ShieldKnockDir(e.dir, oc, head, e.hand ? -1.0f : 1.0f, kd);
        const float ks = vr::pure::ShieldKnockSpeed(dv);
        float v[3] = {kd[0] * ks * s, kd[1] * ks * s, kd[2] * ks * s};
        SetLinVel(c, v);
        const float spin = (std::min)(8.0f, 2.5f * ks);
        float wv[3] = {axis[0] * spin, axis[1] * spin, axis[2] * spin};
        if (hasAxis) SetAngVel(c, wv);
        Wake(c);
        {
            char kb[260];
            const float th[3] = {head[0] - oc[0], head[1] - oc[1], head[2] - oc[2]};
            const float thl = std::sqrt(th[0] * th[0] + th[1] * th[1]);
            std::snprintf(kb, sizeof(kb),
                          "PROPS: shield knocked away at %.2f m/s, direction (%.2f, %.2f, %.2f), %.2f towards the head (horizontal), "
                          "object %.0f units from the eyes (%s)",
                          ks, kd[0], kd[1], kd[2], thl > 1.0f ? (kd[0] * th[0] + kd[1] * th[1]) / thl : 0.0f, std::sqrt(Dot3(th, th)),
                          now - g_viewOriginAt < 0.5 ? "view" : "pawn");
            vr::log::Line(kb);
        }
    }
}

// Collaudo "enemyfire": il ciclo dei colpi istantanei del gioco su un'arma
// qualsiasi (FUN_0093F510, __thiscall(arma, colpi); verificato: slot +0x424
// della vtable delle YWeapon). Con un Pawn IA (flag +0x5F4 0x8000 spento)
// parte da YGamePawn.m_fireStartLoc (+0x17F8) verso m_aimAtPosition
// (+0x140C), con il cono di dispersione dell'arma.
typedef float(__thiscall* InstantFire_t)(void* weapon, int shots);
const uintptr_t& kInstantFireAddr = vr::addr::g[vr::addr::InstantFire];
constexpr uintptr_t kOffPawnWeapon = 0x1594;  // YGamePawn.m_currentWeapon

bool CallInstantFire(void* weapon, int shots) {
    if (!kInstantFireAddr) return false;
    __try {
        reinterpret_cast<InstantFire_t>(kInstantFireAddr)(weapon, shots);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void EnemyFireTest(void* walker, const EnemyFireReq& r) {
    void* ep = reinterpret_cast<void*>(static_cast<uintptr_t>(r.pawn));
    char b[420];
    if (!ActorAlive(ep) || ep == walker) {
        vr::log::Line("PROPS: enemyfire: not a live pawn");
        return;
    }
    void* w = reinterpret_cast<void*>(static_cast<uintptr_t>(U32(ep, kOffPawnWeapon)));
    const uint32_t vt = w ? U32(w, 0) : 0;
    const std::string wc = w ? vr::script::ClassName(w) : std::string("none");
    const uint32_t slot = vt ? U32(reinterpret_cast<void*>(static_cast<uintptr_t>(vt)), 0x424) : 0;
    if (!w || wc.rfind("YWeapon", 0) != 0 || !kInstantFireAddr || slot != kInstantFireAddr) {
        std::snprintf(b, sizeof(b), "PROPS: enemyfire: the pawn has no YWeapon with the instant-hit routine (weapon %s, vtable +0x424 = %08X)", wc.c_str(),
                      slot);
        vr::log::Line(b);
        return;
    }
    if (!U32(w, 0x2C8) || U32(w, 0x2CC) != r.pawn) {
        std::snprintf(b, sizeof(b), "PROPS: enemyfire: %s is not set up (m_weaponAttachment %08X, m_pawn %08X)", wc.c_str(), U32(w, 0x2C8),
                      U32(w, 0x2CC));
        vr::log::Line(b);
        return;
    }
    float pl[3], eye = 0.0f;
    if (!SafeCopy(pl, static_cast<char*>(ep) + kOffActorLocation, 12)) return;
    SafeCopy(&eye, static_cast<char*>(ep) + 0x2E4, 4);  // Pawn.BaseEyeHeight
    float tgt[3];
    if (r.hasTarget) {
        std::memcpy(tgt, r.target, sizeof(tgt));
    } else {
        // L'oggetto tenuto (sinistra, poi destra), altrimenti gli occhi di Walker.
        Prop cur;
        const void* hc = g_held[1].comp ? g_held[1].comp : g_held[0].comp;
        if (hc && ReadBounds(const_cast<void*>(hc), &cur)) {
            for (int k = 0; k < 3; ++k) tgt[k] = 0.5f * (cur.boxMin[k] + cur.boxMax[k]);
        } else {
            float wl[3], we = 0.0f;
            if (!SafeCopy(wl, static_cast<char*>(walker) + kOffActorLocation, 12)) return;
            SafeCopy(&we, static_cast<char*>(walker) + 0x2E4, 4);
            tgt[0] = wl[0];
            tgt[1] = wl[1];
            tgt[2] = wl[2] + (we > 0.0f && we < 100.0f ? we : 40.0f);
        }
    }
    float st[3] = {pl[0], pl[1], pl[2] + (eye > 0.0f && eye < 100.0f ? eye : 40.0f)};
    float d[3] = {tgt[0] - st[0], tgt[1] - st[1], tgt[2] - st[2]};
    const float dl = std::sqrt(Dot3(d, d));
    if (!(dl > 30.0f)) {
        vr::log::Line("PROPS: enemyfire: target too close");
        return;
    }
    for (int k = 0; k < 3; ++k) st[k] += d[k] / dl * 20.0f;  // fuori dal suo cilindro (raggio 15)
    SafeCopy(static_cast<char*>(ep) + 0x17F8, st, 12);  // m_fireStartLoc
    SafeCopy(static_cast<char*>(ep) + 0x140C, tgt, 12);  // m_aimAtPosition
    const int n = (std::max)(1, (std::min)(30, r.shots));
    const bool ok = CallInstantFire(w, n);
    std::snprintf(b, sizeof(b), "PROPS: enemyfire: %s of %s fired %d shot(s) from (%.0f, %.0f, %.0f) at (%.0f, %.0f, %.0f), pawn flags +0x5F4 %08X%s",
                  wc.c_str(), vr::script::ClassName(ep).c_str(), n, st[0], st[1], st[2], tgt[0], tgt[1], tgt[2], U32(ep, 0x5F4),
                  ok ? "" : " - EXCEPTION in the game's routine");
    vr::log::Line(b);
}

}  // namespace

void TickImpl(void* pawn, const float handPos[2][3], const float handAxes[2][3][3], const bool valid[2]);
double g_capWaitMs = 0.0;  // attesa di "fpscap" in questo tick (TickImpl)

// Costo della mod degli oggetti per tick nel log ogni 30 s (fluidita' in VR).
void Tick(void* pawn, const float handPos[2][3], const float handAxes[2][3][3], const bool valid[2]) {
    static double sum = 0.0, worst = 0.0;
    static uint32_t n = 0;
    static ULONGLONG next = 0;
    const double t0 = ProfMs();
    g_capWaitMs = 0.0;
    TickImpl(pawn, handPos, handAxes, valid);
    const double ms = ProfMs() - t0 - g_capWaitMs;
    sum += ms;
    worst = (std::max)(worst, ms);
    ++n;
    if (GetTickCount64() >= next) {
        if (next && n) {
            char b[160];
            std::snprintf(b, sizeof(b), "PROPS: tick %.3f ms on average, %.2f ms at most (%u calls in 30 s)", sum / n, worst, n);
            vr::log::Line(b);
            HitStats& hs = g_hitStats;
            if (hs.ticks) {
                char h[320];
                std::snprintf(h, sizeof(h),
                              "PROPS: hits %.4f ms per tick on average, %.4f ms in the %u ticks with objects in motion, %.2f ms at most "
                              "(inside the tick above); %u traces, %u hits, up to %u objects followed and %u enemies near",
                              hs.sum / hs.ticks, hs.movingTicks ? hs.movingSum / hs.movingTicks : 0.0, hs.movingTicks, hs.worst, hs.traces,
                              hs.hits, hs.maxTracks, hs.maxPawns);
                vr::log::Line(h);
            }
            const ThrowDmgStats& ts = g_throwDmgStats;
            if (ts.hits) {
                char t[260];
                std::snprintf(t, sizeof(t),
                              "PROPS: thrown hits on enemies in 30 s: %u (%u knock-downs, %u light, %u stun, %u too soft, %u refused (no controller), "
                              "%u called with no effect; %u with a tracking jump at release)",
                              ts.hits, ts.heavy, ts.light, ts.stun, ts.none, ts.refused, ts.noEffect, ts.suspect);
                vr::log::Line(t);
            }
            const MeleeStats& ms = g_meleeStats;
            if (ms.hits || ms.soft) {
                char t[300];
                std::snprintf(t, sizeof(t),
                              "PROPS: melee hits with held objects in 30 s: %u (%u staggers, %u light, %u called with no effect, %u refused "
                              "(%u within 0.6 s, %u during Walker's melee, %u other); %u broke the object; %u too slow)",
                              ms.hits, ms.stagger, ms.light, ms.noEffect, ms.cooldown + ms.walkerMelee + ms.refused, ms.cooldown, ms.walkerMelee,
                              ms.refused, ms.broken, ms.soft);
                vr::log::Line(t);
            }
            const ShieldStats& ss = g_shieldStats;
            if (ss.shots || ss.script) {
                char t[400];
                std::snprintf(t, sizeof(t),
                              "PROPS: other weapons' shots in 30 s: %u (%u on Walker, %u stopped by a held object, %u redirected to a held "
                              "object, %u let through (propshield off), %u friendly on a held object (no effect); %u extra pellets, %u knocked "
                              "out of the hand, %u shields broken); %u script calls (not bullets, ignored)",
                              ss.shots, ss.onWalker, ss.onHeld, ss.redirected, ss.passed, ss.friendly, ss.pellets, ss.knocked, ss.broke, ss.script);
                vr::log::Line(t);
            }
            const FakeStats& fs = g_fakeStats;
            if (fs.sent || fs.cooldown || fs.tooBig || fs.enemyHit || fs.friendNear || fs.failed) {
                char t[260];
                std::snprintf(t, sizeof(t),
                              "PROPS: fake grenades in 30 s: %u warnings (%u failed), %u in a group's cooldown, %u objects too big, %u throws "
                              "that hit an enemy instead, %u with a squadmate as close as the enemy",
                              fs.sent, fs.failed, fs.cooldown, fs.tooBig, fs.enemyHit, fs.friendNear);
                vr::log::Line(t);
            }
            const NoiseStats& ns = g_noiseStats;
            if (ns.sent || ns.limited || ns.failed) {
                char t[200];
                std::snprintf(t, sizeof(t), "PROPS: object noises in 30 s: %u hear events (%u breaking glass, %u failed), %u impacts within 1 s of the last",
                              ns.sent, ns.glass, ns.failed, ns.limited);
                vr::log::Line(t);
            }
            const DisarmStats& ds = g_disarmStats;
            if (ds.hits || ds.rearmed || ds.unarmed || ds.bodyFirst) {
                char t[340];
                std::snprintf(t, sizeof(t),
                              "PROPS: hits on enemy weapons in 30 s: %u (%u dropped, %u staggered, %u too soft, %u refused, %u with no effect); "
                              "%u picked their weapon up again, %u stayed unarmed; %u skipped (the body was hit first in that swing or throw)",
                              ds.hits, ds.dropped, ds.knocked, ds.soft, ds.refused, ds.noEffect, ds.rearmed, ds.unarmed, ds.bodyFirst);
                vr::log::Line(t);
            }
            const GrenadeStats& gs = g_grenStats;
            if (gs.seen || gs.taken || gs.notEnemy) {
                char t[320];
                std::snprintf(t, sizeof(t),
                              "PROPS: enemy grenades in 30 s: %u seen, %u came to rest on the ground, %u picked up, %u thrown back, %u let go, "
                              "%u exploded in the hand; %u at rest with under %.1f s of fuse (not offered); %u not an enemy's (ignored)",
                              gs.seen, gs.ground, gs.taken, gs.thrown, gs.dropped, gs.inHand, gs.shortFuse, kGrenMinFuse, gs.notEnemy);
                vr::log::Line(t);
            }
            const CorpseStats& cs = g_corpseStats;
            if (cs.woken || cs.grabs || cs.capped) {
                char t[300];
                std::snprintf(t, sizeof(t),
                              "PROPS: corpses in 30 s: %u ragdolls woken up, %u grabbed (%u let go, %u slipped off, %u lost), largest move %.0f cm; "
                              "%u ticks with shoved bodies held to 0.6 m/s (fastest %.2f m/s)",
                              cs.woken, cs.grabs, cs.letGo, cs.slips, cs.lost, cs.moved, cs.capped, cs.fastest);
                vr::log::Line(t);
            }
            const WorldDmgStats& ws = g_worldDmgStats;
            if (ws.hits) {
                char t[200];
                std::snprintf(t, sizeof(t), "PROPS: object hits on glass, crates and sand windows in 30 s: %u (%u broken, %u damaged, %u too soft, %u no effect)",
                              ws.hits, ws.broken, ws.damaged, ws.soft, ws.refused);
                vr::log::Line(t);
            }
        }
        g_hitStats = HitStats();
        g_throwDmgStats = ThrowDmgStats();
        g_meleeStats = MeleeStats();
        g_worldDmgStats = WorldDmgStats();
        g_shieldStats = ShieldStats();
        g_fakeStats = FakeStats();
        g_noiseStats = NoiseStats();
        g_disarmStats = DisarmStats();
        g_grenStats = GrenadeStats();
        g_corpseStats = CorpseStats();
        next = GetTickCount64() + 30000;
        sum = worst = 0.0;
        n = 0;
    }
}

// Suggerimenti contestuali (regressione finale del gameplay, 27/10): bit
// 1 << vr::ui::ObjHint delle situazioni di questo tick, letti da vr_toast
// (HintSituations). g_hintSitHands: la parte del giro delle mani (oggetto o arma
// nella zona della sinistra, granata con gli angoli rossi).
unsigned g_hintSit = 0;
unsigned g_hintSitHands = 0;

// Le situazioni con un oggetto in mano: in mano, bombola esplosiva in mano,
// nemico vivo entro 2,5 m (controllato ogni 0,25 s, solo con un oggetto in mano).
unsigned HintSituationsHeld(void* pawn, double now) {
    static double nextCheck = 0.0;
    static bool enemyNear = false;
    void* held[2] = {g_held[0].actor, g_held[1].actor};
    if (!held[0] && !held[1]) {
        enemyNear = false;
        return 0;
    }
    unsigned sit = 1u << vr::ui::kObjHintHeld;
    for (void* a : held)
        if (a && CarrierOfActor(a)) sit |= 1u << vr::ui::kObjHintExplosive;
    if (now >= nextCheck) {
        nextCheck = now + 0.25;
        enemyNear = false;
        float wl[3];
        HitPawn pawns[64];
        const int n = pawn && SafeCopy(wl, static_cast<char*>(pawn) + kOffActorLocation, 12) ? GatherPawns(pawn, pawns, 64, now) : 0;
        const float reach = 2.5f * vr::control::g_posScale;
        for (int i = 0; i < n && !enemyNear; ++i) {
            const float d[3] = {pawns[i].loc[0] - wl[0], pawns[i].loc[1] - wl[1], pawns[i].loc[2] - wl[2]};
            enemyNear = Dot3(d, d) < reach * reach;
        }
    }
    if (enemyNear) sit |= 1u << vr::ui::kObjHintMelee;
    return sit;
}

unsigned HintSituations() { return g_hintSit; }

void TickImpl(void* pawn, const float handPos[2][3], const float handAxes[2][3][3], const bool valid[2]) {
    g_hintSit = g_hintSitHands = 0;  // ricalcolati in fondo (un ritorno anticipato li lascia a zero)
    static void* lastPawn = nullptr;
    static float lastTime = -1.0f;
    static bool gripWas[2] = {false, false};
    static double gripDownAt[2] = {-10.0, -10.0};
    g_lastTickAt = ProfMs() * 0.001;
    SynthGripTick();  // voce 6: valore finto del grip in attesa o in rampa (collaudi), anche a mani vuote
    g_tickPawn = pawn;
    if (!pawn) {
        for (int h = 0; h < 2; ++h) Release(h, "no pawn");
        WeaponRelease("no pawn");  // voce 6: arma nella sinistra
        GrenadeRelease(nullptr, "no pawn", ProfMs() * 0.001);  // voce 8: granata nella sinistra (cade)
        CorpseRelease("no pawn");  // voce 11: cadavere nella sinistra
        lastPawn = nullptr;
        return;
    }
    // Ultimo Walker visto (revisione della voce 5): un tick senza pawn (scena
    // nel motore, telecamera passata a un altro attore, postazione fissa)
    // azzera lastPawn, ma al ritorno puo' essere lo stesso Walker nello stesso
    // livello. Stesso oggetto (indirizzo e nome completo), stesso WorldInfo e
    // tempo del mondo non tornato indietro (un livello ricaricato riparte da 0).
    static struct {
        void* pawn = nullptr;
        ObjId id;
        uint32_t wi = 0;
        float t = -1.0f;
    } lastWalker;
    if (pawn != lastPawn) {
        const ObjId idNow = ObjIdOf(pawn);
        const uint32_t wiNow = U32(pawn, kOffActorWorldInfo);
        float tNow = -1.0f;
        if (wiNow) SafeCopy(&tNow, reinterpret_cast<char*>(wiNow) + kOffWorldTimeSeconds, 4);
        const bool sameWalker = pawn == lastWalker.pawn && idNow.index == lastWalker.id.index && idNow.number == lastWalker.id.number &&
                                wiNow && wiNow == lastWalker.wi && std::isfinite(tNow) && tNow >= lastWalker.t;
        if (!sameWalker) {
            CarrierReset();  // bombole portate (voce 5): al loro posto, se il livello e' ancora quello
        } else if (!g_carriers.empty()) {
            // Lo stesso Walker dopo una scena: le bombole portate restano dove
            // sono (CarrierTick sistema quelle sparite nel frattempo).
            char b[160];
            std::snprintf(b, sizeof(b), "PROPS: same Walker back after %.1f s without a pawn: %u explosive carrier(s) kept where they are",
                          tNow - lastWalker.t, static_cast<unsigned>(g_carriers.size()));
            vr::log::Line(b);
        }
        lastWalker.pawn = pawn;
        lastWalker.id = idNow;
        lastWalker.wi = wiNow;
        WeaponReset();   // voce 6: armi del livello vecchio
        GrenadeReset();  // voce 8: granate del livello vecchio
        if (g_corpse.pawn) CorpseRelease("new level or new Walker");
        CorpseReset();  // voce 11: cadaveri del livello vecchio
        for (int h = 0; h < 2; ++h) {
            g_held[h] = Held();  // livello nuovo: puntatori vecchi
            g_glove[h] = Glove();
            g_relGrace[h][0] = g_relGrace[h][1] = ReleaseGrace();
        }
        g_throwCheck = ThrowCheck();
        g_restore.clear();
        g_volRestore.clear();
        g_blockVols.clear();
        g_props.clear();
        g_noBody.clear();
        g_falling.clear();
        g_targets.clear();
        ClearHits();
        g_fakeCooldown.clear();
        g_noiseNext = 0.0;
        noiseprobe::g_armed = false;
        g_pawnClass.clear();
        g_pawnSide.clear();
        g_walkerTeam.walker = nullptr;
        g_glassMesh.clear();
        g_shieldWear.clear();  // scudi: usura degli oggetti del livello vecchio
        g_shieldVolley = ShieldVolley();
        {
            std::lock_guard<std::mutex> lock(g_shotMutex);
            g_shieldEvents.clear();
        }
        g_scan = ScanBuild();
        for (int h = 0; h < 2; ++h) {
            g_proxy[h] = HandProxy();  // gli attori del livello vecchio non ci sono piu'
            g_proxyActor[h] = nullptr;
        }
        g_meshAllowed.clear();  // le mesh del livello vecchio possono essere scaricate
        g_supportAllowed.clear();
        g_explClass.clear();
        for (auto& hh : g_hist) hh = HandHist();
        g_pawnHist = HandHist();
        g_handFrameTick = HandFrame();  // niente salto tra un livello e l'altro
        lastPawn = pawn;
        lastTime = -1.0f;
    }
    // Un passo per tick di gioco (Draw arriva una volta per occhio).
    uint32_t wi = U32(pawn, kOffActorWorldInfo);
    float t = 0.0f;
    if (!wi || !SafeCopy(&t, reinterpret_cast<char*>(wi) + kOffWorldTimeSeconds, 4)) return;
    if (t == lastTime) return;
    lastWalker.t = t;
    {
        // "fpscap <hz>" (collaudi senza visore, 25/10): un tick di gioco al
        // piu' ogni 1/hz s, attendendo qui (thread di gioco, una volta per
        // tick). Senza visore il gioco va a ~225 tick/s; il visore a 90/120/45.
        static double capLast = 0.0;
        const float hz = vr::control::g_fpsCap.load();
        if (hz > 1.0f) {
            const double until = capLast + 1000.0 / hz, w0 = ProfMs();
            while (ProfMs() < until) Sleep(0);
            g_capWaitMs += ProfMs() - w0;  // fuori dal tempo di "PROPS: tick"
        }
        capLast = ProfMs();
    }
    float dt = lastTime >= 0.0f ? t - lastTime : 0.011f;
    lastTime = t;
    // Vibrazione degli urti (voce 3): un tick lungo (scatto del gioco, 124 ms
    // misurati tenendo fermo) fa fare a PhysX un passo diverso da quello del
    // comando: la radio cadeva di 8 cm e sembrava 1,3 m/s tolti da un urto; e
    // premuta sul terreno uno scatto di 108 ms la spingeva su di 5 cm, poi la
    // guida la riportava giu' a 2 m/s 40 ms dopo (F2_radio): muto per 0,2 s.
    const bool hitch = dt > 0.05f;
    if (!(dt > 0.0005f) || dt > 0.1f) dt = 0.011f;
    double now = ProfMs() * 0.001;
    if (hitch) g_hapQuietUntil = (std::max)(g_hapQuietUntil, now + 0.2);
    // Giro a scatti o spostamento scriptato: oggetti tenuti, sfere e storia
    // delle mani nel riferimento nuovo (prima di aggiungere la posa di questo tick).
    CheckFrameJump(pawn, dt, now);
    {
        float pl[3];
        if (SafeCopy(pl, static_cast<char*>(pawn) + kOffActorLocation, 12)) HistPush(g_pawnHist, pl, now);
    }
    for (int h = 0; h < 2; ++h) {
        g_lastHandValid[h] = valid[h];
        if (valid[h]) {
            std::memcpy(g_lastHandPos[h], handPos[h], 12);
            std::memcpy(g_lastHandAxes[h], handAxes[h], 36);
        }
        if (!valid[h]) continue;
        // Con il centro del riferimento delle mani se UpdateArms l'ha dato in
        // questo tick (CheckFrameJump l'ha appena preso): stesso Draw delle mani.
        const bool frameNow = g_handFrame.seq != 0 && g_handFrameTick.seq == g_handFrame.seq && g_handFrameTickAt == now;
        HistPush(g_hist[h], handPos[h], now, handAxes[h], vr::control::g_poseStampMs.load() * 0.001, frameNow ? g_handFrame.c : nullptr);
    }
    for (size_t i = 0; i < g_restore.size();) {
        // Collisioni con Walker di nuovo attive solo con l'oggetto fuori dalla
        // sua capsula: dentro, PhysX lo espelle a velocita' assurde (una
        // bottiglia lasciata accanto al corpo e' finita a 34 m, 24/10).
        Restore& r = g_restore[i];
        const bool usable = ActorUsable(r.actor);
        if (!r.flagsBack && OutsidePawn(pawn, r.actor, false)) {
            // Fuori dalla capsula: i flag dell'attore subito (voce 0 del piano:
            // si temeva che bIgnoreRigidBodyPawns acceso per 0,5 s facesse
            // attraversare i nemici ai lanci; vedi la nota su Restore). Contro
            // Walker restano ignorate solo le coppie con i suoi corpi, compreso
            // il corpo-spinta (per questo qui basta la capsula, senza RBPushRadius).
            r.flagsBack = true;
            if (usable) {
                WriteFlags(r.actor, r.savedA0, r.saved9C);
                static int logged = 0;
                if (logged < 20) {
                    ++logged;
                    char b[160];
                    std::snprintf(b, sizeof(b), "GRAB: %s solid for pawns again %.2f s after release", vr::script::ObjectName(r.actor).c_str(),
                                  now - r.since);
                    vr::log::Line(b);
                }
            }
        }
        if (r.flagsBack && now >= r.at && OutsidePawn(pawn, r.actor)) {
            // Di nuovo solido per Walker (se i suoi corpi sono ancora quelli).
            if (usable && r.walkerSig) IgnoreWalker(pawn, PropComponent(r.actor), false, r.walkerSig);
            g_restore.erase(g_restore.begin() + i);
        } else {
            ++i;
        }
    }
    VolumesTick(now);
    // Sonda del rumore (voce 9, solo test): l'hook alla prima accensione, il conteggio a finestra chiusa.
    if (vr::control::g_noiseProbe.load()) noiseprobe::Ensure();
    noiseprobe::Report(pawn, now);
    // Colpi degli oggetti (pipeline condivisa): prima delle rotture per urto
    // qui sotto, che toglierebbero da g_falling la bottiglia che ha colpito.
    if (vr::control::g_propCombat && !vr::control::g_cinemaActive && !vr::control::g_gameLocked) HitPipeline(pawn, now, dt);
    else ClearHits();
    // Disarmo (voce 10): l'arma caduta e la ripresa dopo 5 s (fuori dalle scene).
    if (!g_disarmed.empty() && !vr::control::g_cinemaActive && !vr::control::g_gameLocked) DisarmTick(now);
    // Superfici solide attorno agli oggetti tenuti, in volo e appena lasciati
    // (prima della scansione: g_noBody e' quella dell'ultimo Scan).
    for (int h = 0; h < 2; ++h) {
        if (g_held[h].actor) SupportsAround(g_held[h].comp, 0.0f);
        if (g_glove[h].state >= kGloveLocked && g_glove[h].comp) SupportsAround(g_glove[h].comp, 1200.0f);
    }
    // Revisione della voce 7: le chiamate al gioco nel ciclo (finta granata,
    // rumore) passano per ProcessEvent; se un hook li' dentro aggiungesse una
    // voce (FallingAdd), il vettore non deve spostarsi sotto il riferimento f.
    // Con 16 posti liberi non si sposta (no-op se c'e' gia' posto).
    g_falling.reserve(g_falling.size() + 16);
    for (size_t i = 0; i < g_falling.size();) {
        Falling& f = g_falling[i];
        float v[3] = {0, 0, 0};
        if (now > f.until || !ActorUsable(f.actor) || !SafeCopy(v, static_cast<char*>(f.actor) + 0x140, 12)) {
            if (f.maxDv > 2.0f * vr::control::g_posScale && ActorUsable(f.actor) && Breakable(f.actor)) {
                char t[160];
                std::snprintf(t, sizeof(t), "PROPS: %s did not break (largest change %.1f m/s, threshold 5)",
                              vr::script::ObjectName(reinterpret_cast<void*>(U32(f.comp, kOffSmcStaticMesh))).c_str(),
                              f.maxDv / vr::control::g_posScale);
                vr::log::Line(t);
            } else if (f.maxDv > 2.0f * vr::control::g_posScale && ActorUsable(f.actor) && CarrierOfActor(f.actor) &&
                       std::count_if(g_falling.begin(), g_falling.end(), [&](const Falling& o) { return o.comp == f.comp; }) == 1) {
                // Bombola portata (voce 5): urto sotto la soglia, nel log per tararla (una riga
                // sola: presa e rilascio danno due voci dello stesso componente).
                char t[160];
                std::snprintf(t, sizeof(t), "PROPS: explosive landed without exploding (largest change %.1f m/s, threshold %.0f)",
                              f.maxDv / vr::control::g_posScale, kExplImpactDv);
                vr::log::Line(t);
            }
            g_falling[i] = g_falling.back();
            g_falling.pop_back();
            continue;
        }
        float sp = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        SupportsAround(f.comp, sp);
        bool held = g_held[0].comp == f.comp || g_held[1].comp == f.comp;
        // Voce 5 (revisione): una bombola agganciata, in volo o sospesa col
        // guanto, e per 0,15 s dopo (piu' della finestra di 50 ms dell'urto),
        // non esplode per urto. All'arrivo non presa il guanto la ferma davanti
        // al palmo (~1 m/s, sospesa): non e' un urto, ma un volo lungo e veloce
        // poteva superare la soglia accanto alla mano. Misurato (27/10, tre voli
        // di 7,5 m non presi): il guanto la rallenta piano (16 -> 2 m/s in
        // 0,25 s), sotto la soglia; la guardia resta per i casi non visti.
        bool gloved = false;
        for (const Glove& gl : g_glove) gloved |= gl.comp == f.comp && gl.state >= kGloveLocked;
        if (gloved) f.gloveUntil = now + 0.15;
        const bool gloveCalm = now < f.gloveUntil && CarrierOfActor(f.actor);
        if (!held && f.histN > 0 && now > f.clampUntil + 0.1) {
            // Urto (fase 4): cambio di velocita' oltre 5 m/s in 50 ms partendo
            // da oltre 3 m/s (lancio contro un muro, caduta da piu' di ~1 m).
            float d = 0.0f, before = 0.0f;
            for (int k = 0; k < f.histN; ++k) {
                if (now - f.histT[k] > 0.05) continue;
                const float* pv = f.histV[k];
                float dv[3] = {v[0] - pv[0], v[1] - pv[1], v[2] - pv[2]};
                float dk = std::sqrt(dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2]);
                if (dk > d) {
                    d = dk;
                    before = std::sqrt(pv[0] * pv[0] + pv[1] * pv[1] + pv[2] * pv[2]);
                }
            }
            const float s = vr::control::g_posScale;
            if (gloveCalm) {
                // Nel log il cambio ignorato (una riga per volo sopra la soglia).
                if (before > 3.0f * s && d > kExplImpactDv * s && !f.gloveLogged) {
                    char t[160];
                    std::snprintf(t, sizeof(t), "PROPS: explosive: %.1f m/s change while the glove moved it (from %.1f m/s), not an impact", d / s,
                                  before / s);
                    vr::log::Line(t);
                    f.gloveLogged = true;
                }
                d = before = 0.0f;
            }
            if (before > 3.0f * s) f.maxDv = (std::max)(f.maxDv, d);
            // Voce 7 (finta granata): un urto di un oggetto lanciato o lasciato
            // dal guanto (non mentre il guanto lo tiene, ne' per 0,15 s dopo:
            // nella finestra di 50 ms restano le velocita' date dal guanto, e
            // una virata brusca sembrerebbe un urto; revisione) e' un
            // atterraggio. Prima delle rotture qui sotto: una bottiglia che si
            // rompe atterrando conta anche lei.
            if (!f.fakeDone && now >= f.gloveUntil && (f.source == HitSource::kThrown || f.source == HitSource::kGlove) &&
                before > kFakeLandBefore * s && d > kFakeLandDv * s) {
                FakeGrenadeLanded(pawn, f.comp, f.source, d, now);
                if (i >= g_falling.size()) break;  // svuotato durante le chiamate al gioco (vedi reserve sopra il ciclo)
            }
            // Voce 9 (rumore): lo stesso urto si sente (al piu' un evento al
            // secondo). Prima delle rotture qui sotto, che tolgono la voce; la
            // rottura (stessa soglia del codice sotto) lo rende piu' forte.
            // Revisione: la finestra di 50 ms vede lo stesso urto per piu'
            // tick. Un urto solo (noiseLatch, su tutte le voci dello stesso
            // componente) e un evento solo, col cambio piu' grande della
            // finestra; parte a finestra chiusa (o dopo 0,1 s), o subito se
            // in questo tick l'urto lo rompe o lo fa esplodere.
            const bool noiseHit = !gloved && NoiseSource(f.source) && before > kNoiseBefore * s && d > kNoiseDv * s;
            if (noiseHit && !f.noiseLatch) {
                for (Falling& o : g_falling)
                    if (o.comp == f.comp) o.noiseLatch = true;
                f.noiseDv = d;
                f.noiseT = now;
            } else if (noiseHit && f.noiseDv > 0.0f) {
                f.noiseDv = (std::max)(f.noiseDv, d);
            }
            if (f.noiseDv > 0.0f) {
                const bool breaks = before > 3.0f * s && d > 5.0f * s && Breakable(f.actor);
                const bool blows = before > 3.0f * s && d > kExplImpactDv * s && CarrierOfActor(f.actor);
                if (!noiseHit || breaks || blows || now - f.noiseT >= kNoiseHold) {
                    void* const na = f.actor;
                    void* const nc = f.comp;
                    const float nd = f.noiseDv;
                    const char* const nh = NoiseSourceName(f.source);
                    for (Falling& o : g_falling)
                        if (o.comp == nc) o.noiseDv = 0.0f;
                    PropNoise(pawn, na, nc, nh, nd, breaks, now);
                    if (i >= g_falling.size()) break;  // come sopra
                }
            }
            if (!noiseHit) f.noiseLatch = false;
            if (before > 3.0f * s && d > kExplImpactDv * s) {
                // Bombola portata (voce 5): un urto forte la fa esplodere li' (anche con
                // propexplosive spento: l'opzione decide solo le prese nuove).
                if (Carrier* c = CarrierOfActor(f.actor)) {
                    char why[80];
                    std::snprintf(why, sizeof(why), "impact at %.1f m/s", d / s);
                    void* ctrl = reinterpret_cast<void*>(static_cast<uintptr_t>(U32(pawn, 0x214)));
                    CarrierDamage(*c, 0.0f, ctrl, nullptr, nullptr, why);
                    g_falling[i] = g_falling.back();
                    g_falling.pop_back();
                    continue;
                }
            }
            if (before > 3.0f * s && d > 5.0f * s && Breakable(f.actor)) {
                char why[80];
                std::snprintf(why, sizeof(why), "impact at %.1f m/s", d / s);
                BreakProp(f.actor, f.comp, why);
                g_falling[i] = g_falling.back();
                g_falling.pop_back();
                continue;
            }
        } else if (f.noiseDv > 0.0f || f.noiseLatch) {
            // Ripreso in mano o agganciato dal guanto entro 0,1 s dall'urto: il
            // rumore in attesa cade (non partirebbe dopo, da un altro punto).
            f.noiseDv = 0.0f;
            f.noiseLatch = false;
        }
        for (int k = 5; k > 0; --k) {
            std::memcpy(f.histV[k], f.histV[k - 1], 12);
            f.histT[k] = f.histT[k - 1];
        }
        std::memcpy(f.histV[0], v, sizeof(v));
        f.histT[0] = now;
        if (f.histN < 6) ++f.histN;
        bool locked = false;
        for (const Glove& gl : g_glove) locked |= gl.comp == f.comp && gl.state == kGloveLocked;
        if (locked) {
            f.clampUntil = now + 0.3;  // agganciato dal guanto: fermo finche' non vola
            f.maxSpeed = 0.3f * vr::control::g_posScale;
        }
        if (!held && now < f.clampUntil) {
            uint32_t bi = 0, nx = 0;
            float lin[3] = {0, 0, 0}, ang[3] = {0, 0, 0};
            if (TrySafeReadUint32(static_cast<char*>(f.comp) + kOffCompBodyInstance, &bi) && bi &&
                TrySafeReadUint32(reinterpret_cast<char*>(bi) + 0x60, &nx) && nx && NxVelocities(reinterpret_cast<void*>(nx), lin, ang)) {
                // lin in metri/s di PhysX (x 50 = unita'/s)
                float ls = std::sqrt(lin[0] * lin[0] + lin[1] * lin[1] + lin[2] * lin[2]) * 50.0f;
                float as = std::sqrt(ang[0] * ang[0] + ang[1] * ang[1] + ang[2] * ang[2]);
                if (ls > f.maxSpeed && ls > 0.0f) {
                    float k = f.maxSpeed / ls, nv[3] = {lin[0] * 50.0f * k, lin[1] * 50.0f * k, lin[2] * 50.0f * k};
                    SetLinVel(f.comp, nv);
                }
                if (as > 10.0f) {
                    float k = 10.0f / as, nw[3] = {ang[0] * k, ang[1] * k, ang[2] * k};
                    SetAngVel(f.comp, nw);
                }
            }
        }
        ++i;
    }
    DoSpawn(pawn, handPos[1], handAxes[1]);
    if (g_throwCheck.actor) {
        // 3 tick dopo il rilascio e di nuovo a 0,1 s: velocita' di PhysX
        // (quella dell'attore UE3 arriva un tick dopo) e scarto dalla
        // direzione data, tolta la gravita' del tempo passato.
        ThrowCheck& tc = g_throwCheck;
        const bool due = tc.stage == 0 ? --tc.ticks <= 0 : now - tc.at >= 0.1;
        if (due) {
            float v[3] = {0, 0, 0}, w[3] = {0, 0, 0};
            void* nx = ActorUsable(tc.actor) ? CompNx(tc.comp) : nullptr;
            bool ok = nx && NxVelocities(nx, v, w);
            const bool wOk = ok;  // rotazione solo da PhysX
            if (ok)
                for (float& c : v) c *= 50.0f;
            else if (ActorUsable(tc.actor))
                ok = SafeCopy(v, static_cast<char*>(tc.actor) + 0x140, 12);
            if (ok) {
                const float el = static_cast<float>(now - tc.at);
                float e[3] = {tc.wantV[0], tc.wantV[1], tc.wantV[2] + GravityZ(pawn) * el};
                auto angle = [](const float* a, const float* b) {
                    float la = std::sqrt(Dot3(a, a)), lb = std::sqrt(Dot3(b, b));
                    if (!(la > 1e-3f) || !(lb > 1e-3f)) return 0.0f;
                    return std::acos((std::max)(-1.0f, (std::min)(1.0f, Dot3(a, b) / (la * lb)))) * 57.2958f;
                };
                float at[3] = {tc.from[0], tc.from[1], tc.from[2]};
                SafeCopy(at, static_cast<char*>(tc.comp) + kOffCompBounds, 12);
                // Direzione solo per un lancio vero (sotto 0,5 m/s e' un oggetto posato).
                char dir[96] = "";
                if (tc.wanted >= 0.5f * vr::control::g_posScale)
                    std::snprintf(dir, sizeof(dir), ", %.1f deg off the throw (%.1f before gravity)", angle(v, e), angle(v, tc.wantV));
                // Voce 4, in coda (la parte di prima resta uguale per i collaudi che
                // la leggono): velocita' senza la gravita' del tempo passato e
                // rotazione misurata, con throwmode peak anche quella data.
                const float vg[3] = {v[0], v[1], v[2] - GravityZ(pawn) * el};
                char spin[120];
                std::snprintf(spin, sizeof(spin), "; %.2f m/s without gravity", std::sqrt(Dot3(vg, vg)) / vr::control::g_posScale);
                if (wOk) {
                    const size_t l = std::strlen(spin);
                    if (tc.hasW)
                        std::snprintf(spin + l, sizeof(spin) - l, ", spin %.1f rad/s (wanted %.1f)", std::sqrt(Dot3(w, w)), std::sqrt(Dot3(tc.wantW, tc.wantW)));
                    else
                        std::snprintf(spin + l, sizeof(spin) - l, ", spin %.1f rad/s", std::sqrt(Dot3(w, w)));
                }
                char b[400];
                std::snprintf(b, sizeof(b), "GRAB: thrown object flies at %.2f m/s (wanted %.2f)%s after %.0f ms, moved %.0f cm (dz %.0f)%s",
                              std::sqrt(Dot3(v, v)) / vr::control::g_posScale, tc.wanted / vr::control::g_posScale, dir, el * 1000.0f,
                              std::sqrt((at[0] - tc.from[0]) * (at[0] - tc.from[0]) + (at[1] - tc.from[1]) * (at[1] - tc.from[1]) +
                                        (at[2] - tc.from[2]) * (at[2] - tc.from[2])) / vr::control::g_posScale * 100.0f,
                              (at[2] - tc.from[2]) / vr::control::g_posScale * 100.0f, spin);
                vr::log::Line(b);
            }
            if (tc.stage == 0 && ok) tc.stage = 1;
            else tc.actor = nullptr;
        }
    }
    const bool enabled = vr::control::g_grabProps && !vr::control::g_cinemaActive && !vr::control::g_gameLocked;
    if (!enabled) {
        WeaponRelease("disabled");  // voce 6
        GrenadeRelease(pawn, "disabled", now);  // voce 8
        CorpseRelease("disabled");  // voce 11
        for (int h = 0; h < 2; ++h) {
            ProxyPark(h, pawn);
            Release(h, "disabled");
            if (g_glove[h].state != kGloveIdle) GloveReset(h, "disabled");
            vr::control::g_propZone[h] = false;
            fingers::g_curlTarget[h] = 0.0f;
        }
        return;
    }
    Scan(pawn);
    // Granate nemiche (voce 8): quelle nuove, dove sono, quali stanno a terra.
    if (vr::control::g_grenadeBack.load()) {
        GrenadeTick(pawn, now);
    } else if (g_gren.actor || !g_grenades.empty()) {
        GrenadeRelease(pawn, "grenadeback off", now);
        g_grenades.clear();
    }
    g_grenZone = -1;
    // Cadaveri (voce 11): quelli vicini a Walker ogni 0,25 s.
    if (vr::control::g_corpseDrag.load()) {
        CorpseScan(pawn, now);
    } else if (g_corpse.pawn || !g_corpses.empty()) {
        CorpseRelease("corpsedrag off");
        g_corpses.clear();
    }
    {
        // Colpi di Walker su oggetti di scena (fase 4).
        std::vector<ShotEvent> shots;
        {
            std::lock_guard<std::mutex> lock(g_shotMutex);
            shots.swap(g_shots);
        }
        for (const ShotEvent& e : shots) {
            // Voce 5: colpo su una bombola portata (di Walker o di un nemico).
            if (CarrierShot(e, pawn)) continue;
            if (e.other) continue;
            if (GameMakesDynamic(e.comp)) continue;  // le sue bottiglie le gestisce il gioco
            const Prop* hitProp = nullptr;
            for (const Prop& p : g_props)
                if (p.comp == e.comp && p.isStatic) hitProp = &p;
            // Bombola ancora al suo posto: il colpo lo gestisce il gioco (TakeDamage della YExplosiveActor).
            if (!hitProp || hitProp->explosive || hitProp->radius > 40.0f) continue;  // solo oggetti, non i mobili grandi
            void* a = MakePropDynamic(*hitProp, HitSource::kShot, -1);
            if (!a) continue;
            if (Breakable(a)) {
                BreakProp(a, e.comp, "shot");
                continue;
            }
            float m = BodyMass(e.comp);
            float dv = 6.0f * vr::control::g_posScale * (std::min)(1.0f, 0.1f / (m > 0.0f ? m : 0.1f));  // 6 m/s ai leggeri
            struct {
                float impulse[3];
                float pos[3];
                int32_t bone[2];
                uint32_t velChange;
            } ai = {{e.dir[0] * dv, e.dir[1] * dv, e.dir[2] * dv}, {e.hit[0], e.hit[1], e.hit[2]}, {0, 0}, 1};
            vr::script::CallFunction(e.comp, "AddImpulse", &ai);
            for (Falling& f : g_falling)
                if (f.comp == e.comp) f.maxSpeed = (std::max)(f.maxSpeed, dv * 1.5f);
            vr::log::Line("PROPS: shot moved " + vr::script::ObjectName(reinterpret_cast<void*>(U32(e.comp, kOffSmcStaticMesh))));
        }
    }
    // Bombole portate (voce 5): Location sul portatore, portatori persi o esplosi.
    CarrierTick();
    // Colpi delle altre armi sugli oggetti tenuti (scudi, voce 3 dei
    // combattimenti): prima di Drive, che parte con la spinta in questo tick.
    ShieldTick(pawn, now);
    ShieldTraceTestTick();  // collaudo "shieldtrace off"
    {
        // Collaudo enemyfire (voce 3): un Pawn spara con la sua arma.
        EnemyFireReq r;
        {
            std::lock_guard<std::mutex> lock(g_spawnMutex);
            std::swap(r, g_enemyFireReq);
        }
        if (r.pawn) EnemyFireTest(pawn, r);
    }
    {
        // Collaudo explhit (voce 5): danno alla prima bombola portata.
        float d = -1.0f;
        {
            std::lock_guard<std::mutex> lock(g_spawnMutex);
            std::swap(d, g_explHitReq);
        }
        if (d >= 0.0f) {
            if (g_carriers.empty()) vr::log::Line("PROPS: explhit: no explosive carried");
            else CarrierDamage(g_carriers.front(), d, reinterpret_cast<void*>(static_cast<uintptr_t>(U32(pawn, 0x214))), nullptr, nullptr, "test explhit");
        }
    }
    {
        // Collaudo glasshit (voce 2): colpo finto su un bersaglio del mondo.
        GlassHitReq r;
        {
            std::lock_guard<std::mutex> lock(g_spawnMutex);
            std::swap(r, g_glassHitReq);
        }
        if (r.actor) GlassHitTest(pawn, r);
    }
    {
        // Collaudo (grabtest <parte del nome della mesh>): presa senza zona.
        std::string want;
        {
            std::lock_guard<std::mutex> lock(g_spawnMutex);
            want.swap(g_grabTestMesh);
        }
        if (!want.empty() && valid[1] && !g_held[1].actor) {
            const Prop* best = nullptr;
            float bd = 1e30f;
            for (const Prop& p : g_props) {
                std::string n = vr::script::ObjectName(reinterpret_cast<void*>(U32(p.comp, kOffSmcStaticMesh)));
                if (want != "any" && n.find(want) == std::string::npos) continue;
                Prop cur = p;
                if (!ReadBounds(p.comp, &cur)) continue;
                float d = BoxDistance(cur, handPos[1]);
                if (d < bd) {
                    bd = d;
                    best = &p;
                }
            }
            if (best) {
                char b[160];
                std::snprintf(b, sizeof(b), "GRAB: grabtest %s at %.0f cm", want.c_str(), bd / vr::control::g_posScale * 100.0f);
                vr::log::Line(b);
                Grab(1, *best, handPos[1], handAxes[1]);
            } else {
                vr::log::Line("GRAB: grabtest " + want + ": nothing in range");
            }
        }
    }
    {
        // Collaudo weapontest (voce 6): la sinistra prende l'arma a terra piu'
        // vicina (entro 3 m), senza zona; munizioni se Walker ha gia' quell'arma.
        bool req = false;
        {
            std::lock_guard<std::mutex> lock(g_spawnMutex);
            std::swap(req, g_wpnTestReq);
            if (g_wpnGripReq) {
                g_wpnGrip = g_wpnGripNew;  // "weapongrip" (collaudo), qui sul thread di gioco
                g_wpnGripReq = false;
            }
        }
        g_wpnHandAt = now;  // prese accese: la mano prende le armi a terra (WeaponForHand)
        if (req && valid[1] && !g_held[1].actor && !g_wpn.actor) {
            float d = 0.0f;
            const int wi = WeaponNearest(handPos[1], 3.0f * vr::control::g_posScale, &d);
            if (wi >= 0) {
                char b[160];
                std::snprintf(b, sizeof(b), "WEAPON: weapontest: %s at %.0f cm", vr::script::ClassName(g_weapons[wi].actor).c_str(),
                              d / vr::control::g_posScale * 100.0f);
                vr::log::Line(b);
                WeaponTryGrab(pawn, wi, handPos[1], handAxes[1], "weapontest");
            } else {
                char b[120];
                std::snprintf(b, sizeof(b), "WEAPON: weapontest: no weapon on the ground within 3 m (%u listed)",
                              static_cast<unsigned>(g_weapons.size()));
                vr::log::Line(b);
            }
        }
    }
    {
        // Collaudo corpsetest (voce 11): la sinistra afferra il corpo del
        // cadavere piu' vicino al palmo (entro 2 m), senza zona.
        int req = 0;
        {
            std::lock_guard<std::mutex> lock(g_spawnMutex);
            std::swap(req, g_corpseTestReq);
        }
        if (req == 2) {
            // "corpsetest bones": i corpi del cadavere piu' vicino a Walker, osso e posa (per i collaudi).
            g_corpseScanAt = 0.0;
            CorpseScan(pawn, now);
            for (const CorpseCand& c : g_corpses) {
                if (!c.frozen) CorpsePoseCheck(c.mesh);
                uint32_t list[kMaxCorpseBodies];
                const int nb = c.frozen ? 0 : CorpseBodyList(c.mesh, list);
                char b[160];
                std::snprintf(b, sizeof(b), "CORPSE: bones of %s (%s): %d bodies", vr::script::ClassName(c.pawn).c_str(), c.frozen ? "frozen" : "physical", nb);
                vr::log::Line(b);
                float palmT[3];
                for (int k = 0; k < 3; ++k) {
                    palmT[k] = handPos[1][k];
                    if (g_palmValid[1])
                        palmT[k] += g_palmLocal[1][0] * handAxes[1][0][k] + g_palmLocal[1][1] * handAxes[1][1][k] + g_palmLocal[1][2] * handAxes[1][2][k];
                }
                const float sc = CorpseScale(c.pawn, c.mesh);
                if (c.frozen) {
                    // Congelato: le forme sulle ossa (FrozenShapes, revisione della voce 11).
                    const FrozenPose* f = FrozenShapes(c);
                    for (size_t i = 0; f && i < f->bodies.size(); ++i) {
                        const FrozenBody& fb = f->bodies[i];
                        int32_t bone[2] = {0, 0};
                        SafeCopy(bone, reinterpret_cast<char*>(static_cast<uintptr_t>(fb.setup)) + 0x84, 8);
                        const float v[3] = {palmT[0] - fb.p[0], palmT[1] - fb.p[1], palmT[2] - fb.p[2]};
                        const float q[3] = {Dot3(v, fb.R[0]), Dot3(v, fb.R[1]), Dot3(v, fb.R[2])};
                        float dmin = 1e30f, nearL[3];
                        for (const CorpseShape& s : BodyShapes(fb.setup)) dmin = (std::min)(dmin, ShapeDist(s, sc, q, nearL));
                        char t[240];
                        std::snprintf(t, sizeof(t), "CORPSE: body %d %s at (%.1f, %.1f, %.1f) from its bone (frozen), palm %.1f cm from it",
                                      static_cast<int>(i), vr::script::NameFromIndex(bone[0]).c_str(), fb.p[0], fb.p[1], fb.p[2],
                                      dmin / vr::control::g_posScale * 100.0f);
                        vr::log::Line(t);
                    }
                }
                for (int i = 0; i < nb; ++i) {
                    int32_t bone[2];
                    float bp[3], bR[3][3];
                    const uint32_t setup = BodySetupOf(c.mesh, list[i], bone);
                    if (!setup || !CorpseBodyPose(BodyNx(list[i]), bp, bR)) continue;
                    const float v[3] = {palmT[0] - bp[0], palmT[1] - bp[1], palmT[2] - bp[2]};
                    const float q[3] = {Dot3(v, bR[0]), Dot3(v, bR[1]), Dot3(v, bR[2])};
                    float dmin = 1e30f, nearL[3];
                    const std::vector<CorpseShape>& shapes = BodyShapes(setup);
                    for (const CorpseShape& s : shapes) dmin = (std::min)(dmin, ShapeDist(s, sc, q, nearL));
                    char t[240];
                    std::snprintf(t, sizeof(t), "CORPSE: body %d %s at (%.1f, %.1f, %.1f), %u shapes, palm %.1f cm from them (palm in the body's axes %.1f %.1f %.1f, scale %.2f)", i,
                                  vr::script::NameFromIndex(bone[0]).c_str(), bp[0], bp[1], bp[2], static_cast<unsigned>(shapes.size()),
                                  dmin / vr::control::g_posScale * 100.0f, q[0], q[1], q[2], sc);
                    vr::log::Line(t);
                }
                break;  // il primo (il piu' vicino della lista basta ai collaudi)
            }
        }
        if (req == 1 && valid[1] && !g_held[1].actor && !g_wpn.actor && !g_gren.actor && !g_corpse.pawn) {
            float palmT[3];
            for (int i = 0; i < 3; ++i) {
                palmT[i] = handPos[1][i];
                if (g_palmValid[1])
                    palmT[i] += g_palmLocal[1][0] * handAxes[1][0][i] + g_palmLocal[1][1] * handAxes[1][1][i] + g_palmLocal[1][2] * handAxes[1][2][i];
            }
            g_corpseScanAt = 0.0;
            CorpseScan(pawn, now);
            const CorpsePick pk = CorpseNearest(palmT, 2.0f * vr::control::g_posScale);
            char b[160];
            std::snprintf(b, sizeof(b), "CORPSE: corpsetest: %u corpse(s) within 4 m, nearest %s at %.0f cm", static_cast<unsigned>(g_corpses.size()),
                          pk.idx >= 0 ? vr::script::ClassName(g_corpses[pk.idx].pawn).c_str() : "-",
                          pk.idx >= 0 ? pk.d / vr::control::g_posScale * 100.0f : -1.0f);
            vr::log::Line(b);
            if (pk.idx >= 0) CorpseGrab(pawn, pk, palmT, now, "corpsetest", 1e30f);
        }
    }
    // Opzione spenta con l'arma in mano: la mano la lascia.
    if (g_wpn.actor && !vr::control::g_weaponPickup.load()) WeaponRelease("weaponpickup off");
    g_highlight = HighlightBox();
    const float zone = vr::control::g_grabZoneCm * 0.01f * vr::control::g_posScale;
    for (int h = 0; h < 2; ++h) {
        bool handAllowed = h == 1 || vr::control::g_grabRightHand;
        ULONGLONG offAt = vr::control::g_gripSimOffAt[h];
        if (offAt && GetTickCount64() >= offAt) {
            vr::control::g_gripSim[h] = false;
            vr::control::g_gripSimOffAt[h] = 0;
        }
        bool grip = vr::control::g_gripRaw[h].load() || vr::control::g_gripSim[h].load();
        if (grip && !gripWas[h]) gripDownAt[h] = now;
        // Sfera della mano (fase 3): segue il palmo anche della mano che non prende.
        float palm[3], hvel[3] = {0, 0, 0};
        for (int i = 0; i < 3; ++i) {
            palm[i] = handPos[h][i];
            if (g_palmValid[h])
                palm[i] += g_palmLocal[h][0] * handAxes[h][0][i] + g_palmLocal[h][1] * handAxes[h][1][i] + g_palmLocal[h][2] * handAxes[h][2][i];
        }
        HandVelocity(h, 0.06, hvel);
        const float hspeed = std::sqrt(hvel[0] * hvel[0] + hvel[1] * hvel[1] + hvel[2] * hvel[2]);
        if (valid[h] && !handAllowed) {
            // Voce 6: anche l'arma nella sinistra (la destra ci arriva per prenderla).
            void* keep[3] = {g_held[0].comp, g_held[1].comp, g_wpn.comp};
            ProxyTick(h, pawn, palm, dt, keep, 3, now, hspeed);
            ProxyKnockStatics(h, palm, nullptr, hspeed);
        } else if (!valid[h]) {
            ProxyPark(h, pawn);
        }
        if (!valid[h] || !handAllowed) {
            fingers::g_curlTarget[h] = 0.0f;
            if (vr::control::g_grabLog > 0 && h == 1) {
                vr::control::g_grabLog.fetch_sub(1);
                vr::log::Line("GRABLOG: left hand not valid");
            }
            Release(h, "hand lost");
            if (h == 1) WeaponRelease("hand lost");  // voce 6
            if (h == 1 && g_gren.actor) {
                // Voce 8 (revisione): un buco breve del tracciamento (la mano
                // dietro la testa per il lancio) non la lascia ai piedi di
                // Walker: resta dov'era il palmo per kGrenLostGrace s, poi cade.
                if (g_gren.lostAt < 0.0) {
                    g_gren.lostAt = now;
                    char b[160];
                    std::snprintf(b, sizeof(b), "GRENADE: left hand lost with the grenade: it stays where the palm was for up to %.2f s",
                                  kGrenLostGrace);
                    vr::log::Line(b);
                }
                if (now - g_gren.lostAt <= kGrenLostGrace) {
                    const float still[3] = {0, 0, 0};
                    GrenadeDrive(pawn, g_gren.lastPalm, still, dt, now);
                } else {
                    GrenadeRelease(pawn, "hand lost", now);
                }
            }
            if (h == 1) CorpseRelease("hand lost");  // voce 11
            if (g_glove[h].state != kGloveIdle) GloveReset(h, "hand lost");
            vr::control::g_propZone[h] = false;
            gripWas[h] = grip;
            continue;
        }
        if (h == 1 && g_gren.actor) {
            // Voce 8: granata nemica nella sinistra (aperta: la rilancia; la miccia corre).
            if (g_gren.lostAt >= 0.0) {
                char b[160];
                std::snprintf(b, sizeof(b), "GRENADE: left hand back after %.2f s, the grenade is still in it", now - g_gren.lostAt);
                vr::log::Line(b);
                g_gren.lostAt = -1.0;
            }
            if (!grip) {
                GrenadeRelease(pawn, "grip", now);
            } else {
                // Velocita' della mano su 25 ms (non 60): un palmo che si ferma
                // non lascia la granata indietro per 60 ms.
                float hvs[3] = {0, 0, 0};
                HandVelocity(1, 0.025, hvs);
                GrenadeDrive(pawn, palm, hvs, dt, now);
            }
            vr::control::g_propZone[h] = false;
            fingers::g_curlTarget[h] = g_gren.actor ? 1.0f : 0.0f;  // pugno attorno alla granata
            void* keep[2] = {g_held[0].comp, g_held[1].comp};
            ProxyTick(h, pawn, palm, dt, keep, 2, now, hspeed);
            gripWas[h] = grip;
            continue;
        }
        if (h == 1 && g_corpse.pawn) {
            // Voce 11: cadavere nella sinistra (aperta, o la sinistra sull'arma: lo lascia).
            if (!grip) CorpseRelease("grip");
            else if (GunHandHaptics(1)) CorpseRelease("left hand on the gun");
            else CorpseDrive(pawn, palm, dt, now);
            vr::control::g_propZone[h] = false;
            fingers::g_curlTarget[h] = g_corpse.pawn ? 1.0f : 0.0f;  // pugno attorno all'arto
            void* keep[3] = {g_held[0].comp, g_held[1].comp, g_corpse.mesh};
            ProxyTick(h, pawn, palm, dt, keep, 3, now, hspeed);
            gripWas[h] = grip;
            continue;
        }
        if (h == 1 && g_wpn.actor) {
            // Voce 6: arma dei nemici nella sinistra (aperta: la lascia o la lancia).
            if (!grip) WeaponRelease("grip");
            else WeaponDrive(pawn, handPos[1], handAxes[1], handPos[0], valid[0], dt, now);
            vr::control::g_propZone[h] = false;
            fingers::g_curlTarget[h] = g_wpn.actor ? 1.0f : 0.0f;  // pugno attorno all'impugnatura
            void* keep[3] = {g_held[0].comp, g_held[1].comp, g_wpn.comp};
            ProxyTick(h, pawn, palm, dt, keep, 3, now, hspeed);
            gripWas[h] = grip;
            continue;
        }
        if (g_held[h].actor) {
            if (!grip) {
                Release(h, "grip");
            } else {
                // Corpi di Walker ricreati (accucciata, livello): di nuovo ignorati.
                uint32_t sig = WalkerSig(pawn);
                if (sig && sig != g_held[h].walkerSig) g_held[h].walkerSig = IgnoreWalker(pawn, g_held[h].comp, true);
                Drive(h, handPos[h], handAxes[h], dt);
            }
            vr::control::g_propZone[h] = false;
            // Dita sull'oggetto: PoseFingers le chiude finche' toccano
            // l'oggetto nella sua posa voluta; qui solo il peso della posa
            // della mod sopra l'animazione (prima un'unica chiusura dal
            // raggio, sommata all'animazione sull'arma: dita dentro l'oggetto).
            if (g_held[h].actor) fingers::g_curlTarget[h] = 1.0f;
            void* keep[2] = {g_held[0].comp, g_held[1].comp};
            ProxyTick(h, pawn, palm, dt, keep, 2, now, hspeed);
        } else {
            float d = 0.0f;
            // Zona di presa dal palmo (dove la presa canonica mette l'oggetto e
            // dove sta la sfera della mano): dal bersaglio IK, 13 cm indietro,
            // il palmo toccava e spingeva l'oggetto prima che entrasse nella zona.
            // Voce 5 ("solidhands full"): la mano vuota disegnata e' la sfera,
            // che il tavolo ferma: la zona parte dalla sfera (niente prese
            // attraverso il piano del tavolo), se e' entro shcap cm dal palmo.
            const float* zp = palm;
            float zs[3], zsa[3][3];
            if (h == 1 && vr::control::g_solidHands.load() >= 2 && g_proxy[h].actor && ActorUsable(g_proxy[h].actor) &&
                CompPose(g_proxy[h].comp, zs, zsa)) {
                const float dz[3] = {zs[0] - palm[0], zs[1] - palm[1], zs[2] - palm[2]};
                const float lim = (std::max)(1.0f, g_tune.shCap) * 0.01f * vr::control::g_posScale;
                if (Dot3(dz, dz) <= lim * lim) zp = zs;
            }
            const Prop* p = Nearest(zp, zone, &d);
            // Voce 6: arma a terra nella zona della sinistra (vince se e' piu'
            // vicina dell'oggetto): grip = presa dell'arma o delle sue munizioni.
            // Mai con la sinistra sull'arma, a due mani o nella ricarica, come
            // granate e cadaveri (regressione finale: la zona dava l'impulso di
            // 15 ms e toglieva LB alla mano sull'arma).
            float wd = 0.0f;
            int wi = h == 1 && vr::control::g_weaponPickup.load() && !GunHandHaptics(1) ? WeaponNearest(zp, zone, &wd) : -1;
            if (wi >= 0 && p && d < wd) wi = -1;
            if (wi >= 0) p = nullptr;
            // Voce 8: granata nemica a terra nella zona della sinistra: vince su
            // oggetti e armi (e' il pericolo), il grip la prende. Mai con la
            // sinistra sull'arma, a due mani o nella ricarica (la mano dell'arma
            // non cambia).
            float gd = 0.0f;
            const int gi = h == 1 && vr::control::g_grenadeBack.load() && !GunHandHaptics(1) ? GrenadeNearest(zp, zone, &gd) : -1;
            if (gi >= 0) {
                p = nullptr;
                wi = -1;
            }
            if (h == 1) g_grenZone = gi;
            // Voce 11: cadavere nella zona della sinistra, solo se non c'e'
            // nient'altro da prendere (oggetti, armi e granate vincono: un
            // oggetto posato su un corpo si prende). Mai con la sinistra
            // sull'arma.
            CorpsePick ck;
            if (h == 1 && vr::control::g_corpseDrag.load() && !GunHandHaptics(1) && !p && wi < 0 && gi < 0 && !g_corpses.empty())
                ck = CorpseNearest(zp, zone);
            bool inZone = p != nullptr || wi >= 0 || gi >= 0 || ck.idx >= 0;
            if (inZone && !vr::control::g_propZone[h].load())
                vr::input::PulseHand(h ? 0 : 1, 15);  // si sente il contatto prima di stringere
            // Grip stretto a vuoto: pugno (le dita si chiudono come nei giochi VR).
            fingers::g_curlTarget[h] = grip && !(h == 1 && vr::control::g_leftOnGun) ? 1.0f : 0.0f;
            // Presa: grip chiuso nella zona, o chiuso da poco (0,25 s) prima di
            // toccare l'oggetto (si stringe mentre la mano arriva).
            bool gripRose = grip && !gripWas[h];
            bool flying = g_glove[h].state >= kGloveFlying;
            if (inZone && !flying && grip && now - gripDownAt[h] < 0.25) {
                if (g_glove[h].state != kGloveIdle) GloveReset(h, nullptr);
                if (gi >= 0) {
                    // Una volta per chiusura del grip (voce 8).
                    if (g_grenGripTried != gripDownAt[h]) {
                        g_grenGripTried = gripDownAt[h];
                        GrenadeGrab(pawn, gi, palm, now);
                    }
                } else if (ck.idx >= 0) {
                    // Voce 11: una volta per chiusura del grip (un congelato si rianima qui).
                    if (g_corpseGripTried != gripDownAt[h]) {
                        g_corpseGripTried = gripDownAt[h];
                        CorpseGrab(pawn, ck, zp, now, "grip in the zone", zone);
                    }
                } else if (wi < 0) {
                    const size_t propsBefore = g_props.size();
                    Grab(h, *p, handPos[h], handAxes[h]);
                    // p punta dentro g_props: una presa non riuscita che l'ha
                    // accorciata (bombola senza portatore, voce 5) l'ha spostato
                    // su un altro oggetto; qui sotto non serve piu'.
                    if (!g_held[h].actor && g_props.size() != propsBefore) p = nullptr;
                } else if (g_wpnGripTried != gripDownAt[h]) {
                    // Una volta per chiusura del grip: munizioni prese o arma in mano.
                    g_wpnGripTried = gripDownAt[h];
                    WeaponTryGrab(pawn, wi, handPos[h], handAxes[h], "grip in the zone");
                }
            } else if (inZone && !flying) {
                if (g_glove[h].state != kGloveIdle && g_glove[h].state != kGloveLocked) GloveReset(h, nullptr);
            } else {
                GloveTick(h, pawn, handPos[h], handAxes[h], grip, gripRose, zone, now, dt);
            }
            // Vicino a un oggetto o puntandone uno coi guanti: il grip e' la presa.
            vr::control::g_propZone[h] = inZone || g_glove[h].state != kGloveIdle;
            // Suggerimenti contestuali: oggetto o arma a terra nella zona della sinistra.
            if (h == 1 && !flying) {
                if (wi >= 0) g_hintSitHands |= 1u << vr::ui::kObjHintGun;
                else if (p && gi < 0 && ck.idx < 0) g_hintSitHands |= 1u << vr::ui::kObjHintNear;
            }
            // Sfera della mano: non spinge l'oggetto tenuto, quello in volo verso
            // il guanto, ne' (avvicinandosi piano) quello che sta per prendere.
            const Prop* cand = inZone && !g_held[h].actor && hspeed < 1.0f * vr::control::g_posScale ? p : nullptr;
            // Voce 11: anche il cadavere con un corpo nella zona (sotto 1 m/s:
            // sopra e' una spinta e la sfera lo urta).
            void* corpseKeep = ck.idx >= 0 && ck.body >= 0 && hspeed < 1.0f * vr::control::g_posScale ? g_corpses[ck.idx].mesh : nullptr;
            if (corpseKeep && std::find(g_corpsePairMeshes.begin(), g_corpsePairMeshes.end(), corpseKeep) == g_corpsePairMeshes.end())
                g_corpsePairMeshes.push_back(corpseKeep);
            void* keep[5] = {g_held[0].comp, g_held[1].comp, g_glove[h].state >= kGloveLocked ? g_glove[h].comp : nullptr,
                             cand ? cand->comp : nullptr, corpseKeep};
            // Voce 5 ("solidhands full"): la sfera della mano vuota e' la mano
            // disegnata. I mobili senza corpo PhysX (tavoli, sacchi di sabbia:
            // collisione semplice spenta) non la fermerebbero: attorno a lei
            // diventano solidi come attorno a un oggetto tenuto (SupportsAround).
            if (h == 1 && vr::control::g_solidHands.load() >= 2 && g_proxy[h].actor && ActorUsable(g_proxy[h].actor))
                SupportsAround(g_proxy[h].comp, hspeed);
            // Voce 11: la mano libera spinge i cadaveri (congelati: rianimati all'urto), col peso.
            if (h == 1 && vr::control::g_corpseDrag.load() && !GunHandHaptics(1) && !g_held[h].actor && !g_corpses.empty()) {
                CorpseNearHand(pawn, palm, hspeed, now);
                CorpsePushCap(pawn, palm, now);
            }
            // Schiaffo solo con la mano libera e vuota: non la mano dell'arma
            // (la sinistra sull'astina, a due mani o nella ricarica fisica).
            ProxyTick(h, pawn, palm, dt, keep, 5, now, hspeed, !g_held[h].actor && g_glove[h].state < kGloveFlying && !GunHandHaptics(h));
            if (!g_held[h].actor && g_glove[h].state < kGloveFlying) ProxyKnockStatics(h, palm, p, hspeed);
            // Riconoscibile a vista (utente 24/10): angoli luminosi attorno
            // all'oggetto vicino o puntato, piu' marcati quando e' agganciato.
            if (!g_held[h].actor && vr::control::g_grabHighlight) {
                if (inZone && ck.idx >= 0 && !flying) {
                    g_highlight = {true, g_corpses[ck.idx].mesh, 1};  // voce 11: il cadavere che la mano puo' trascinare
                } else if (inZone && wi >= 0 && !flying) {
                    g_highlight = {true, g_weapons[wi].comp, 1};  // voce 6: arma a terra
                } else if (inZone && p && !flying) {
                    g_highlight = {true, p->comp, 1};
                } else if (g_glove[h].state == kGloveAim || g_glove[h].state == kGloveLocked) {
                    g_highlight = {true, g_glove[h].comp, g_glove[h].state == kGloveLocked ? 2 : 1};
                }
            }
            if (vr::control::g_grabLog > 0 && h == 1) {
                vr::control::g_grabLog.fetch_sub(1);
                char b[400];
                std::snprintf(b, sizeof(b),
                              "GRABLOG: L free, %u props, nearest %s %.1f cm, grip %d, hand (%.0f, %.0f, %.0f) glove %d dir (%.2f, %.2f, %.2f)%s"
                              " palm (%.1f, %.1f, %.1f) | weapon %d, corpse %d body %d at %.1f cm (%u corpses near%s)",
                              static_cast<unsigned>(g_props.size()), p ? vr::script::ObjectName(p->actor).c_str() : "-",
                              d / vr::control::g_posScale * 100.0f, grip ? 1 : 0, handPos[h][0], handPos[h][1], handPos[h][2],
                              static_cast<int>(g_glove[h].state), vr::control::g_supportFwd[0].load(), vr::control::g_supportFwd[1].load(),
                              vr::control::g_supportFwd[2].load(), vr::control::g_supportFwdValid ? "" : " (no dir)", palm[0], palm[1], palm[2],
                              wi, ck.idx, ck.body, ck.idx >= 0 ? ck.d / vr::control::g_posScale * 100.0f : -1.0f,
                              static_cast<unsigned>(g_corpses.size()), GunHandHaptics(1) ? ", left hand on the gun" : "");
                vr::log::Line(b);
            }
        }
        gripWas[h] = grip;
    }
    // Voce 11: cadaveri senza piu' coppie ignorate con le sfere: CompNxAll torna normale per loro.
    g_corpsePairMeshes.erase(std::remove_if(g_corpsePairMeshes.begin(), g_corpsePairMeshes.end(),
                                            [](void* m) {
                                                for (const HandProxy& pr : g_proxy)
                                                    for (const auto& e : pr.ignored)
                                                        if (e.first == m) return false;
                                                return true;
                                            }),
                             g_corpsePairMeshes.end());
    // Voce 8: angoli rossi sulla granata nemica a terra piu' vicina a Walker
    // (entro 8 m), piu' marcati se la sinistra la puo' prendere. Vince sugli
    // altri angoli (un solo riquadro: il pericolo prima).
    if (vr::control::g_grenadeBack.load() && vr::control::g_grabHighlight && !g_gren.actor) {
        float wl[3] = {0, 0, 0};
        SafeCopy(wl, static_cast<char*>(pawn) + kOffActorLocation, 12);
        int best = -1;
        float bd = kGrenMarkRange * vr::control::g_posScale;
        for (size_t i = 0; i < g_grenades.size(); ++i) {
            const LiveGrenade& g = g_grenades[i];
            if (!g.ground || g.shortFuse || !g.mesh) continue;  // poca miccia: niente invito a prenderla
            const float d[3] = {g.pos[0] - wl[0], g.pos[1] - wl[1], g.pos[2] - wl[2]};
            const float dl = std::sqrt(Dot3(d, d));
            if (dl < bd) {
                bd = dl;
                best = static_cast<int>(i);
            }
        }
        if (g_grenZone >= 0) best = g_grenZone;
        if (best >= 0) g_highlight = {true, g_grenades[best].mesh, best == g_grenZone ? 4 : 3};
        if (best >= 0) g_hintSitHands |= 1u << vr::ui::kObjHintGrenade;  // suggerimento: angoli rossi in vista
    }
    // Suggerimenti contestuali (vr_toast): situazioni di questo tick.
    g_hintSit = g_hintSitHands | HintSituationsHeld(pawn, now);
}

}  // namespace vr::props
