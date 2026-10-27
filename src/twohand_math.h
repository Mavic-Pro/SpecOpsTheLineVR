// Presa a due mani (WI-4b, bug 7 del tester: "2 handing weapons doesn't work correctly").
//
// Funzioni pure, senza gioco ne' visore: le usano hooks.cpp (UpdateHeadTracking,
// UpdateArms) e i test offline (tests/unit_tests.cpp).
//
// Prima due prove separate decidevano se la sinistra teneva l'arma: la mira
// (ApplyTwoHandAim: origine raw della sinistra entro 12 cm dalla linea del
// controller destro, 12-70 cm avanti) e la mano disegnata (UpdateArms: entro
// 20 cm dalla presa dell'animazione). Una mano sull'astina disegnata stava a
// 14,6 cm dalla linea della mira: disegnata sull'arma, mira e dispersione a una
// mano. Quando la mira si agganciava puntava la linea tra le origini raw (10 cm
// davanti ai palmi, 21,6 gradi fuori dalla canna per la presa del gioco) e lo
// scarto si azzerava in 0,7 s: l'arma girava da sola di ~20 gradi. Filtro da
// 80 ms (7 gradi di ritardo a 90 gradi/s), scatto di 18-40 gradi al rilascio,
// nessun calcio (la calibrazione B+Y spariva con due mani), nessun veto.
//
// Ora un solo risolutore (Solver) decide la presa per mira, mano disegnata,
// dispersione, vibrazione e veti dei gesti. Spazio del tracciamento di OpenVR
// (metri; x destra, y su, z indietro); telaio dell'arma = posa 3x4 (avanti =
// -colonna 2, destra = colonna 0, su = colonna 1, origine = punto del polso).
//   NORMAL ("holdmode normal"): l'astina e' un segmento misurato dai palmi
//   (Guard: dove sta il palmo sinistro, rispetto al destro, quando la mano
//   disegnata e' sulla presa del gioco); presa entro 8 cm col grip stretto o
//   dopo 150 ms fermi li', rilascio oltre 15 cm o lasciando il grip. Mira: la
//   canna gira attorno al palmo destro quanto basta perche' l'astina passi per
//   il palmo sinistro, con lo scarto dell'astina in un riferimento "linea delle
//   mani / gravita'" (il rollio del polso non sposta la canna). Nessuno scarto
//   che decade; filtro 1-euro sulla linea tra i palmi. Al rilascio lo scarto
//   resta e cala solo mentre la mano dell'arma si muove.
//   Review round 1 (26/10): la canna segue il palmo con peso pieno entro
//   twohandgrip, poi il peso scende a 0 a twohandrelease (round 2: misurati
//   come allontanamento della sinistra, sotto) (una mano che se ne va restituisce l'arma alla destra,
//   prima la trascinava di ~21 gradi e la lasciava li'); la correzione cambia
//   al piu' di 150 gradi/s; dopo un rilascio per distanza lo scarto torna a 0
//   nel tempo, dopo il grip lasciato (o un veto morbido) resta e cala col moto.
//   Fuori dalla presa l'astina e' nel telaio dell'arma (inclinazione attuale,
//   rotazione attuale della sinistra: e' quella disegnata); alla presa astina e
//   inclinazione si fissano. Una mano rimessa sull'arma disegnata con uno scarto
//   rimasto la riprende. Stato nello spazio del tracciamento girato coi
//   ricentramenti del runtime assorbiti (ShiftSpace).
//   Review round 2 (26/10): il peso non e' piu' la distanza dal modello con
//   UNA mano in cm al palmo (il rollio dell'avambraccio destro, il punto lungo
//   l'astina e il rumore del controller destro muovevano la canna a due mani).
//   Ora: quanto la SINISTRA si e' allontanata dalla presa (il moto del palmo,
//   tolta la parte in cui la destra la segue: DepartStep), in angoli presi
//   all'astina (twohandgrip / twohandrelease a g.fwd); il disaccordo totale fra
//   le due canne solo oltre una fascia larga (apartScale); il palmo oltre la
//   fine dell'astina. Presa solo sull'arma disegnata, e dopo un rilascio per
//   distanza col grip stretto il grip si lascia e si stringe di nuovo.
//   GUNSTOCK ("holdmode gunstock"): la mira e' sempre quella del controller
//   destro con stockangle (WI-4a). Presa = controller sinistro entro 6 cm / 25
//   gradi dalla posa sul calcio salvata da B+Y ("stockmount"); senza, la prova
//   dell'astina (solo per la presa, la mira non cambia).
//   Pistole e Uzi: la direzione non cambia; la presa e' la coppa (cuprel).
#pragma once

#include "pure_utils.h"

#include <cmath>

namespace vr::twohand {

// --- Vettori (3 float) ----------------------------------------------------------
inline float Dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline void Cross3(const float a[3], const float b[3], float out[3]) {
    const float x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    out[0] = x;
    out[1] = y;
    out[2] = z;
}
inline float Norm3(const float a[3]) { return std::sqrt(Dot3(a, a)); }
inline bool Normalize3(float v[3]) {
    const float n = Norm3(v);
    if (!(n > 1e-9f)) return false;
    v[0] /= n;
    v[1] /= n;
    v[2] /= n;
    return true;
}
inline float SmoothStep01(float x) {
    x = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
    return x * x * (3.0f - 2.0f * x);
}

// Assi dell'arma di una posa: avanti = -colonna 2, destra = colonna 0, su = colonna 1.
inline void GunAxes(const float m[3][4], float f[3], float r[3], float u[3]) {
    for (int i = 0; i < 3; ++i) {
        f[i] = -m[i][2];
        r[i] = m[i][0];
        u[i] = m[i][1];
    }
}

// Riferimento dell'astina attorno alla canna f: "su" = la verticale del mondo
// ortogonale a f, "destra" = f x su. Non dipende dal rollio della mano
// dell'arma: col risolutore "ad ancora" nel telaio dell'arma 5/10/20 gradi di
// rollio del polso giravano la canna di 1,8/3,6/7,2 gradi (verifica b7,
// problema 1). Con la canna oltre ~70 gradi dall'orizzonte la verticale si
// confonde con f: si passa gradualmente al "su" dell'arma (gunUp).
inline void GuardFrame(const float f[3], const float gunUp[3], float r[3], float u[3]) {
    float ug[3] = {-f[1] * f[0], 1.0f - f[1] * f[1], -f[1] * f[2]};  // Y - (Y.f) f
    const float h = std::sqrt(1.0f - (f[1] > 1.0f ? 1.0f : (f[1] < -1.0f ? 1.0f : f[1] * f[1])));
    const float wg = SmoothStep01((h - 0.17f) / 0.17f);
    float un[3] = {gunUp[0], gunUp[1], gunUp[2]};
    const float uf = Dot3(un, f);
    for (int i = 0; i < 3; ++i) un[i] -= uf * f[i];
    const bool okG = Normalize3(ug), okU = Normalize3(un);
    for (int i = 0; i < 3; ++i) u[i] = (okG ? wg * ug[i] : 0.0f) + (okU ? (1.0f - wg) * un[i] : 0.0f);
    if (!Normalize3(u)) {
        // f verticale e "su" dell'arma lungo f: qualsiasi normale.
        const float ax[3] = {std::fabs(f[0]) < 0.9f ? 1.0f : 0.0f, 0.0f, std::fabs(f[0]) < 0.9f ? 0.0f : 1.0f};
        Cross3(ax, f, u);
        Normalize3(u);
    }
    Cross3(f, u, r);
}

// Rollio (radianti) del "su" dell'arma attorno alla canna f rispetto a
// GuardFrame: GuardFrameCant(f, gunUp, CantOf(f, gunUp)) e' il telaio
// dell'arma stesso (destra, su).
inline float CantOf(const float f[3], const float gunUp[3]) {
    float r0[3], u0[3];
    GuardFrame(f, gunUp, r0, u0);
    float un[3] = {gunUp[0], gunUp[1], gunUp[2]};
    const float uf = Dot3(un, f);
    for (int i = 0; i < 3; ++i) un[i] -= uf * f[i];
    if (!Normalize3(un)) return 0.0f;
    return std::atan2(-Dot3(un, r0), Dot3(un, u0));
}
// GuardFrame girato di "cant" attorno alla canna (review WI-4b round 1, M3):
// l'astina disegnata ruota col rollio dell'arma (controller inclinato), quella
// del modello stava sempre in verticale: a 20/30/45 gradi di inclinazione il
// palmo sull'astina disegnata era a 2,9/4,3/6,4 cm dal modello e la presa
// girava l'arma da sola di 4,7/7,0/10,3 gradi. Ora fuori dalla presa il
// modello e' nel telaio dell'arma (inclinazione attuale) e alla presa
// l'inclinazione rispetto alla verticale si fissa: il rollio del polso dopo la
// presa non sposta la canna.
inline void GuardFrameCant(const float f[3], const float gunUp[3], float cant, float r[3], float u[3]) {
    float r0[3], u0[3];
    GuardFrame(f, gunUp, r0, u0);
    const float c = std::cos(cant), s = std::sin(cant);
    for (int i = 0; i < 3; ++i) {
        u[i] = c * u0[i] - s * r0[i];
        r[i] = c * r0[i] + s * u0[i];
    }
}

// Rotazione (3x3, colonne come le pose) da un vettore di rotazione (radianti): Rodrigues.
inline void RotFromVec(const float v[3], float R[3][3]) {
    const float th = Norm3(v);
    if (!(th > 1e-9f)) {
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) R[i][j] = i == j ? 1.0f : 0.0f;
        return;
    }
    const float k[3] = {v[0] / th, v[1] / th, v[2] / th};
    const float s = std::sin(th), c = 1.0f - std::cos(th);
    const float K[3][3] = {{0, -k[2], k[1]}, {k[2], 0, -k[0]}, {-k[1], k[0], 0}};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            float kk = 0.0f;
            for (int m = 0; m < 3; ++m) kk += K[i][m] * K[m][j];
            R[i][j] = (i == j ? 1.0f : 0.0f) + s * K[i][j] + c * kk;
        }
}

// Angolo (gradi) tra le parti 3x3 di due pose.
// Robusto anche sotto 0,01 gradi (vr::pure::PoseAngleDeg, atan2): con acos il
// rumore dei float dava ~0,02 gradi a tick di "moto" con la mano ferma, e lo
// scarto rimasto calava da solo (collaudo dei test, 26/10).
inline float RotAngleDeg(const float a[3][4], const float b[3][4]) { return static_cast<float>(vr::pure::PoseAngleDeg(a, b)); }

// --- Astina ------------------------------------------------------------------------
//
// Dove sta il palmo sinistro (rispetto al palmo destro, coordinate dell'arma:
// avanti, destra, su; metri) quando la mano sinistra disegnata e' sulla presa
// del gioco. Il segmento [tMin, tMax] lungo la canna e' dove la si puo'
// tenere (fucili: da 15 cm prima a 25 cm dopo la presa disegnata, mai sotto
// 10 cm dal palmo destro); pistole e Uzi: un punto (la coppa).
struct Guard {
    bool valid = false;
    bool shortGun = false;
    float fwd = 0.0f, right = 0.0f, up = 0.0f;
    float tMin = 0.0f, tMax = 0.0f;
};
inline Guard MakeGuard(float fwd, float right, float up, bool shortGun) {
    Guard g;
    g.valid = true;
    g.shortGun = shortGun;
    g.fwd = fwd;
    g.right = right;
    g.up = up;
    if (shortGun) {
        g.tMin = g.tMax = fwd;
    } else {
        g.tMin = fwd - 0.15f < 0.10f ? 0.10f : fwd - 0.15f;
        g.tMax = fwd + 0.25f;
        if (g.tMax < g.tMin) g.tMax = g.tMin;
    }
    return g;
}

// Presa disegnata (polso sinistro rispetto al polso destro disegnato,
// coordinate dell'arma, metri) -> palmo sinistro voluto rispetto al palmo
// destro. Il polso destro disegnato sta sul palmo + (10,2 cm avanti lungo la
// canna) (telaio WI-4a ruotato attorno al palmo) + armoffset; il polso sinistro
// disegnato e' l'origine raw del controller sinistro (+ armoffset nei suoi
// assi), 10,2 cm lungo il SUO -Z dal palmo. leftToGun: rotazione del
// controller sinistro nelle coordinate dell'arma (x destra, y su, z indietro):
// gun^T * raw sinistro. Review WI-4b round 1 (M4): prima si supponeva il
// controller sinistro parallelo al destro (gunInRaw^T, sotto): con la
// sinistra ruotata di 30/45/60 gradi (normale sotto un'astina) il palmo stava
// 5,3/7,8/10,2 cm fuori dal modello con il polso disegnato sulla presa.
// gunLeft: l'arma e' nella mano sinistra (mancini). gripFix false
// ("openxrgrip off"): la posa e' gia' il palmo.
inline void PalmTargetFromDrawnGripRot(const float gripWrist[3], const float leftToGun[3][3], bool gunLeft, bool gripFix,
                                       const float armOffM[3], float out[3]) {
    using vr::pure::kGripOriginX;
    using vr::pure::kGripOriginY;
    using vr::pure::kGripOriginZ;
    // Coordinate locali dell'arma: x destra, y su, z indietro.
    const float oR[3] = {gripFix ? (gunLeft ? kGripOriginX : -kGripOriginX) : 0.0f, gripFix ? kGripOriginY : 0.0f,
                         gripFix ? kGripOriginZ : 0.0f};
    const float oL[3] = {gripFix ? (gunLeft ? -kGripOriginX : kGripOriginX) : 0.0f, gripFix ? kGripOriginY : 0.0f,
                         gripFix ? kGripOriginZ : 0.0f};
    const float mirror = gunLeft ? -1.0f : 1.0f;
    const float woR[3] = {armOffM[1] * mirror, armOffM[2], -armOffM[0]};
    // Dal polso disegnato al palmo, nel raw del controller sinistro -> locale dell'arma.
    const float rawL[3] = {-oL[0] + armOffM[1] * -mirror, -oL[1] + armOffM[2], -oL[2] - armOffM[0]};
    float leftOff[3];
    for (int i = 0; i < 3; ++i) leftOff[i] = leftToGun[i][0] * rawL[0] + leftToGun[i][1] * rawL[1] + leftToGun[i][2] * rawL[2];
    const float grip[3] = {gripWrist[1], gripWrist[2], -gripWrist[0]};
    float loc[3];
    for (int i = 0; i < 3; ++i) loc[i] = -oR[i] + woR[i] + grip[i] - leftOff[i];
    out[0] = -loc[2];
    out[1] = loc[0];
    out[2] = loc[1];
}
// Rotazione del controller sinistro nelle coordinate dell'arma (gun^T * raw sinistro).
inline void LeftToGun(const float gun[3][4], const float leftRaw[3][4], float out[3][3]) {
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 3; ++k) out[i][k] = gun[0][i] * leftRaw[0][k] + gun[1][i] * leftRaw[1][k] + gun[2][i] * leftRaw[2][k];
}
// Ipotesi del controller sinistro parallelo al destro (stessa posa raw; il raw
// sta 39,4 gradi sopra il raggio di puntamento, gunInRaw = GunRotInRaw): senza
// la posa della sinistra (mancini, sinistra persa).
inline void PalmTargetFromDrawnGrip(const float gripWrist[3], const float gunInRaw[3][4], bool gunLeft, bool gripFix,
                                    const float armOffM[3], float out[3]) {
    float m[3][3];
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 3; ++k) m[i][k] = gunInRaw[k][i];
    PalmTargetFromDrawnGripRot(gripWrist, m, gunLeft, gripFix, armOffM, out);
}

// Distanza del palmo sinistro (d = palmo sinistro - palmo destro) dal segmento
// dell'astina per la canna f (r, u = GuardFrame). along = posizione lungo la canna.
inline float GuardDistance(const Guard& g, const float d[3], const float f[3], const float r[3], const float u[3], float* along) {
    float p[3];
    for (int i = 0; i < 3; ++i) p[i] = d[i] - g.right * r[i] - g.up * u[i];
    const float a = Dot3(p, f);
    if (along) *along = a;
    const float t = a < g.tMin ? g.tMin : (a > g.tMax ? g.tMax : a);
    for (int i = 0; i < 3; ++i) p[i] -= t * f[i];
    return Norm3(p);
}

// Canna f per cui l'astina (scarto destra/su fisso nel riferimento
// GuardFrameCant con l'inclinazione "cant") passa per il palmo sinistro:
// d = t f + right r(f) + up u(f), t > 5 cm.
// Punto fisso da f0 (la canna con una mano): lo scarto e' ortogonale a f, la
// mappa contrae di ~|scarto|/|d| (0,25 a 35 cm) e 12 passi bastano.
inline bool SolveForward(const Guard& g, const float d[3], const float f0[3], const float gunUp[3], float cant, float f[3]) {
    for (int i = 0; i < 3; ++i) f[i] = f0[i];
    for (int it = 0; it < 12; ++it) {
        float r[3], u[3];
        GuardFrameCant(f, gunUp, cant, r, u);
        float x[3];
        for (int i = 0; i < 3; ++i) x[i] = d[i] - g.right * r[i] - g.up * u[i];
        if (!(Norm3(x) > 0.05f) || !Normalize3(x)) return false;
        for (int i = 0; i < 3; ++i) f[i] = x[i];
    }
    float r[3], u[3];
    GuardFrameCant(f, gunUp, cant, r, u);
    float res[3];
    float t = 0.0f;
    for (int i = 0; i < 3; ++i) res[i] = d[i] - g.right * r[i] - g.up * u[i];
    t = Dot3(res, f);
    for (int i = 0; i < 3; ++i) res[i] -= t * f[i];
    return t > 0.05f && Norm3(res) < 1e-3f;
}

// --- Veti (chi usa il grip sinistro prima della presa a due mani) -------------------
// Tabella delle precedenze del grip sinistro (docs/TECHNICAL.md, WI-4b):
//   oggetto tenuto / grip in una zona di presa (props, guanti) > ricarica
//   (caricatore fuori, in mano, presa del caricatore, otturatore) > granata
//   (grip stretto lontano dall'arma: LB) > presa a due mani > presa del caricatore.
// I veti "duri" (postazione fissa, scena, nessuna arma in mano) tolgono anche
// lo scarto rimasto, subito: la torretta legge la mira (g_aimDelta*).
enum Veto : int {
    kVetoOff = 1,         // "twohand off" (sviluppo)
    kVetoStationary = 2,  // torretta / minigun: altra mira (WI-4c)
    kVetoScene = 4,       // cinematica, scena bloccata
    kVetoNoGun = 8,       // nessuna arma in mano (cambio, animazione), braccia non guidate
    kVetoProp = 16,       // oggetto nella mano sinistra
    kVetoPropGrip = 32,   // grip stretto in una zona di presa (oggetti, guanti)
    kVetoReload = 64,     // ricarica fisica in corso
    kVetoGrenade = 128,   // grip della granata (LB)
    kVetoNoGrip = 256,    // "leftgrip off" (sviluppo)
};
constexpr int kHardVetoes = kVetoStationary | kVetoScene | kVetoNoGun;
inline const char* VetoName(int v) {
    if (v & kVetoStationary) return "stationary weapon";
    if (v & kVetoScene) return "cinematic or locked scene";
    if (v & kVetoNoGun) return "no gun in hand";
    if (v & kVetoProp) return "object in the left hand";
    if (v & kVetoPropGrip) return "grip on an object";
    if (v & kVetoReload) return "reload";
    if (v & kVetoGrenade) return "grenade";
    if (v & kVetoOff) return "twohand off";
    if (v & kVetoNoGrip) return "leftgrip off";
    return "";
}

// --- Risolutore -----------------------------------------------------------------------
struct Params {
    float engageM = 0.08f;    // presa entro (palmo sinistro dall'astina)
    float releaseM = 0.15f;   // rilascio oltre
    float dwellS = 0.15f;     // presa senza grip: ferma li' per
    float filterMinHz = 1.0f; // filtro 1-euro sulla linea tra i palmi: taglio da fermi
    float filterBeta = 9.0f;  // Hz in piu' per m/s
    float filterDHz = 1.0f;   // taglio della derivata
    float stockM = 0.06f;     // calcio: controller sinistro entro (dalla posa salvata)
    float stockDeg = 25.0f;
    float blendMinS = 0.15f;  // passaggio da una a due mani: almeno
    float blendDegPerS = 100.0f;  // ... e non piu' veloce (picco 1,5x: 150 gradi/s, 2,1 gradi a tick a 72 Hz)
    float leftoverPerDeg = 0.5f;  // scarto rimasto al rilascio: gradi tolti per grado di moto della mano dell'arma
    float maxDegPerS = 150.0f;    // la correzione cambia al piu' di tanto (2,1 gradi a tick a 72 Hz)
    // Review round 2: le due canne (con una mano, filtrata, e a due mani) in
    // disaccordo: peso pieno fino a apartScale x l'angolo di twohandgrip
    // all'astina, 0 a apartScale x quello di twohandrelease (25,4 / 45,8 gradi
    // coi predefiniti e l'astina a 35,5 cm). Un rollio dell'avambraccio di +-30
    // gradi sposta la canna con una mano di ~19 gradi: resta dentro.
    float apartScale = 2.0f;
    // Filtro 1-euro della canna con una mano (rad/s): beta piu' basso di
    // quello della linea dei palmi, il rumore di rotazione del controller
    // destro (0,2 gradi) alzava il taglio a ~3 Hz.
    float dirBeta = 2.0f;
};

struct Input {
    float dt = 0.0f;            // secondi dall'ultimo passo
    float gun[3][4] = {};       // telaio dell'arma con una mano (WI-4a), origine = punto del polso
    float palmR[3] = {};        // palmo della mano dell'arma
    bool leftValid = false;
    float palmL[3] = {};        // palmo della mano di supporto
    bool grip = false;          // grip sinistro stretto
    int veto = 0;               // Veto
    bool gunstock = false;      // "holdmode gunstock"
    bool mountValid = false;    // posa sul calcio salvata (B+Y)
    float mountPos[3] = {};     // controller sinistro nel raw destro (metri)
    float mountRot[3][4] = {};
    float rightRaw[3][4] = {};  // pose raw (solo col calcio)
    float leftRaw[3][4] = {};
    // Astina di adesso (rotazione attuale della sinistra) nelle coordinate
    // dell'arma DISEGNATA (Solver::DrawnRot: telaio con una mano girato dello
    // scarto rimasto); nella presa conta quella fissata alla presa.
    Guard guard;
};

struct Output {
    bool engaged = false;
    bool aimChanged = false;   // gun diverso dal telaio con una mano
    float gun[3][4] = {};      // telaio finale
    float corrDeg = 0.0f;      // correzione applicata (gradi)
    float targetDeg = 0.0f;    // correzione voluta dalle due mani (gradi, prima del peso)
    float weight = 0.0f;       // peso della mira a due mani (allontanamento x disaccordo x fine dell'astina; 0 senza la mira a due mani)
    float dist = 1e9f;         // palmo sinistro dall'astina del telaio con una mano (m; diagnostica)
    float nearDist = 1e9f;     // palmo sinistro dall'astina dell'arma DISEGNATA (m): prova di presa, "vicino all'arma"
    float along = 0.0f;        // lungo la canna (m)
    float distOut = 1e9f;      // palmo sinistro dall'astina del telaio finale (m; ~0 con la presa a due mani in NORMAL)
    float drawShift[3] = {};   // sinistra disegnata: dalla presa disegnata al punto del modello (avanti, destra, su del telaio finale; m)
    float mountCm = -1.0f, mountDeg = -1.0f;  // dalla posa sul calcio
    int event = 0;             // 1 presa, 2 rilascio
    const char* why = "";
    const char* how = "";      // modo della presa: "rifle", "short weapon", "stock", "stock (no mount saved)"
    bool byGrip = false;
    bool fading = false;       // rilascio per distanza: lo scarto torna a 0 nel tempo
    // Review round 2 (nella presa con la mira a due mani):
    float departDeg = 0.0f;    // allontanamento della sinistra dalla presa (gradi): solo il moto del palmo, non il polso destro
    float turnDeg = 0.0f;      // la parte della mano dell'arma (gradi; non conta per il peso)
    float apartDeg = 0.0f;     // canna con una mano (filtrata) contro quella a due mani (gradi)
    float endCm = 0.0f;        // palmo oltre la fine dell'astina (cm)
    const char* limit = "";    // il peso che limita: "departure", "hands apart", "barrel end"
};

class Solver {
public:
    Params p;
    bool engaged = false, byGrip = false, gripBlock = false;
    float dwell = 0.0f;
    float c[3] = {0.0f, 0.0f, 0.0f};       // correzione applicata: vettore di rotazione nel telaio dell'arma (rad)
    float cStart[3] = {0.0f, 0.0f, 0.0f};
    float lastTarget[3] = {0.0f, 0.0f, 0.0f};
    float w = 1.0f, blendT = 0.15f;
    float prevRot[3][4] = {};
    bool havePrev = false;
    bool aimWasOn = false;
    vr::pure::OneEuro fd[3];
    const char* how = "";
    // Review WI-4b round 1:
    Guard held;                // astina fissata alla presa (M4: poi le rotazioni del polso sinistro non entrano)
    float cant = 0.0f;         // inclinazione dell'arma rispetto alla verticale, fissata alla presa (M3)
    bool fadeOut = false;      // rilascio per distanza: lo scarto torna a 0 nel tempo (H1)
    float fadeFrom[3] = {0.0f, 0.0f, 0.0f};
    float fadeW = 0.0f, fadeT = 0.15f;
    float weight = 1.0f;       // peso dell'ultimo passo
    // Review WI-4b round 2: allontanamento attribuito alle due mani.
    vr::pure::OneEuro f1f[3];  // canna con una mano filtrata (disaccordo, attribuzione)
    bool haveDep = false;
    float depPrev1[3] = {0.0f, 0.0f, 0.0f}, depPrev2[3] = {0.0f, 0.0f, 0.0f};  // ultime canne (spazio del tracciamento)
    float dep[2] = {0.0f, 0.0f};   // parte del palmo sinistro (rad; destra, su attorno alla canna a due mani)
    float turn[2] = {0.0f, 0.0f};  // parte della mano dell'arma (rad)
    float wDep = 1.0f, wApart = 1.0f, wEnd = 1.0f;
    float wApplied = 1.0f;     // peso applicato: cambia al piu' di maxDegPerS di canna (i pesi possono cadere in pochi tick)
    float pend[2] = {0.0f, 0.0f};  // allontanamento non ancora applicato alla canna (rad): segue al piu' a maxDegPerS
    float apart = 0.0f, endM = 0.0f, alongTwo = 0.0f;
    bool needRegrip = false;   // rilascio per distanza col grip stretto: per riprendere si lascia e si stringe di nuovo
    float leftGoal[3] = {0.0f, 0.0f, 0.0f};  // scarto rimasto voluto (cala col moto); c lo segue al piu' a maxDegPerS

    void Reset() {
        engaged = byGrip = gripBlock = false;
        dwell = 0.0f;
        for (int i = 0; i < 3; ++i) c[i] = cStart[i] = lastTarget[i] = fadeFrom[i] = 0.0f;
        w = 1.0f;
        havePrev = aimWasOn = false;
        for (auto& f : fd) f.Reset();
        held = Guard();
        cant = 0.0f;
        fadeOut = false;
        fadeW = 0.0f;
        weight = 1.0f;
        for (auto& f : f1f) f.Reset();
        ResetDeparture();
        needRegrip = false;
        leftGoal[0] = leftGoal[1] = leftGoal[2] = 0.0f;
    }
    void ResetDeparture() {
        haveDep = false;
        dep[0] = dep[1] = turn[0] = turn[1] = 0.0f;
        wDep = wApart = wEnd = 1.0f;
        wApplied = 1.0f;
        pend[0] = pend[1] = 0.0f;
        apart = endM = 0.0f;
    }

    // Ricentramento del runtime assorbito (WI-2, hooks.cpp): lo spazio del
    // tracciamento gira di "yaw" attorno alla verticale (vr::pure::SpaceShift,
    // x' = c x - s z, z' = s x + c z) e le pose saltano con lui. Lo stato tenuto
    // in quello spazio (filtri della linea tra i palmi e della canna con una
    // mano, ultima rotazione della mano dell'arma, ultime canne
    // dell'allontanamento) gira con loro; la correzione e' nel telaio dell'arma
    // e resta. Review round 1 (H2): prima il filtro inseguiva il salto (40
    // gradi: canna 19,3/8,5/3,8 gradi nei primi tick a 72 Hz) e lo scarto
    // rimasto calava di 0,5 x 40 gradi in un tick (il salto contato come moto).
    void ShiftSpace(float yaw) {
        const float cy = std::cos(yaw), sy = std::sin(yaw);
        auto turnFilter = [&](vr::pure::OneEuro* f) {
            if (!(f[0].init && f[2].init)) return;
            const float x = f[0].x, z = f[2].x, dx = f[0].dx, dz = f[2].dx;
            f[0].x = cy * x - sy * z;
            f[2].x = sy * x + cy * z;
            f[0].dx = cy * dx - sy * dz;
            f[2].dx = sy * dx + cy * dz;
        };
        turnFilter(fd);
        turnFilter(f1f);
        if (havePrev) {
            for (int j = 0; j < 3; ++j) {
                const float x = prevRot[0][j], z = prevRot[2][j];
                prevRot[0][j] = cy * x - sy * z;
                prevRot[2][j] = sy * x + cy * z;
            }
        }
        for (float* v : {depPrev1, depPrev2}) {
            const float x = v[0], z = v[2];
            v[0] = cy * x - sy * z;
            v[2] = sy * x + cy * z;
        }
    }

    // Rotazione dell'arma DISEGNATA: telaio con una mano girato della
    // correzione attuale (scarto rimasto fuori dalla presa). hooks.cpp ci
    // costruisce l'astina con la rotazione attuale della sinistra (review
    // round 2: prima col telaio con una mano, 1,3-2,4 cm fuori con uno scarto).
    void DrawnRot(const float gun[3][4], float out[3][4]) const {
        float R[3][3];
        RotFromVec(c, R);
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) out[i][j] = gun[i][0] * R[0][j] + gun[i][1] * R[1][j] + gun[i][2] * R[2][j];
            out[i][3] = gun[i][3];
        }
    }

    // Allontanamento (review WI-4b round 2, problema alto). Il round 1 pesava
    // la mira con la distanza del palmo dal modello dell'astina con UNA mano,
    // in cm dove sta il palmo: il rollio dell'avambraccio destro sposta quel
    // modello (12,7 gradi di canna per 20 di rollio), di piu' lontano lungo la
    // canna, e il rumore del controller destro entrava nella canna a due mani
    // (rollio +-20 a 60 cm: 10,6 gradi; scorrere la mano da 22 a 60 cm con la
    // linea 12 gradi fuori: 8,3 gradi). Il disaccordo fra le due canne (T =
    // canna a due mani - canna con una mano) cambia per due motivi che si
    // vedono separati: il palmo sinistro si muove (la canna a due mani gira:
    // "dep") o la mano dell'arma ruota (la canna con una mano gira: "turn").
    // Mirando con due mani girano insieme e si annullano; una mano che se ne va
    // muove solo "dep"; il rollio del polso solo "turn". Una parte annulla
    // l'altra quando vanno in versi opposti (la destra raggiunge la sinistra,
    // o il contrario, anche con un tick di ritardo), quindi |dep| <= |T - T0|:
    // il rumore non si accumula. Il peso guarda |dep| (angoli presi
    // all'astina: twohandgrip / twohandrelease a g.fwd), il disaccordo totale
    // (fascia larga, apartScale) e il palmo oltre la fine dell'astina.
    // Coordinate: destra / su attorno alla canna a due mani (riferimento
    // dell'astina, gravita'); spazio del tracciamento per le ultime canne.
    void DepartStep(const float f1s[3], const float f2[3], const float gunUp[3]) {
        float gr[3], gu[3];
        GuardFrame(f2, gunUp, gr, gu);
        if (haveDep) {
            float b[3], a[3];
            for (int i = 0; i < 3; ++i) {
                b[i] = f2[i] - depPrev2[i];
                a[i] = depPrev1[i] - f1s[i];
            }
            dep[0] += Dot3(b, gr);
            dep[1] += Dot3(b, gu);
            turn[0] += Dot3(a, gr);
            turn[1] += Dot3(a, gu);
            const float n = std::sqrt(dep[0] * dep[0] + dep[1] * dep[1]);
            if (n > 1e-9f) {
                const float ux = dep[0] / n, uy = dep[1] / n;
                const float q = turn[0] * ux + turn[1] * uy;
                if (q < 0.0f) {
                    const float k = n < -q ? n : -q;
                    dep[0] -= k * ux;
                    dep[1] -= k * uy;
                    turn[0] += k * ux;
                    turn[1] += k * uy;
                }
            }
        } else {
            dep[0] = dep[1] = turn[0] = turn[1] = 0.0f;
            haveDep = true;
        }
        for (int i = 0; i < 3; ++i) {
            depPrev1[i] = f1s[i];
            depPrev2[i] = f2[i];
        }
    }
    float DepartDeg() const { return std::sqrt(dep[0] * dep[0] + dep[1] * dep[1]) * 57.2957795f; }
    float TurnDeg() const { return std::sqrt(turn[0] * turn[0] + turn[1] * turn[1]) * 57.2957795f; }

    Output Step(const Input& in) {
        Output o;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 4; ++j) o.gun[i][j] = in.gun[i][j];
        const float dt = in.dt > 0.0f ? (in.dt > 0.2f ? 0.2f : in.dt) : 0.0f;
        float f1[3], r1[3], u1[3];
        GunAxes(in.gun, f1, r1, u1);
        // Moto della mano dell'arma (gradi) dall'ultimo passo: consuma lo scarto rimasto.
        const float motionDeg = havePrev ? RotAngleDeg(prevRot, in.gun) : 0.0f;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 4; ++j) prevRot[i][j] = in.gun[i][j];
        havePrev = true;
        // Linea tra i palmi: grezza per la prova di presa, filtrata per la mira.
        float d[3] = {in.palmL[0] - in.palmR[0], in.palmL[1] - in.palmR[1], in.palmL[2] - in.palmR[2]};
        float df[3] = {d[0], d[1], d[2]};
        if (in.leftValid) {
            for (int k = 0; k < 3; ++k) {
                fd[k].minCutoff = p.filterMinHz;
                fd[k].beta = p.filterBeta;
                fd[k].dCutoff = p.filterDHz;
                df[k] = fd[k].Step(d[k], dt);
            }
        } else {
            for (auto& f : fd) f.Reset();
        }
        // Canna con una mano filtrata (stesso 1-euro): disaccordo e
        // attribuzione dell'allontanamento non seguono il rumore di rotazione
        // del controller destro (review round 2: 0,2 gradi di rumore davano
        // 0,28-0,46 gradi rms alla canna a due mani).
        float f1s[3];
        for (int k = 0; k < 3; ++k) {
            f1f[k].minCutoff = p.filterMinHz;
            f1f[k].beta = p.dirBeta;
            f1f[k].dCutoff = p.filterDHz;
            f1s[k] = f1f[k].Step(f1[k], dt);
        }
        if (!Normalize3(f1s))
            for (int k = 0; k < 3; ++k) f1s[k] = f1[k];
        // Arma DISEGNATA: telaio con una mano girato della correzione (scarto
        // rimasto fuori dalla presa). Review round 2 (medio): la presa si prova
        // solo su quella, che e' quella che si vede; prima anche sul modello con
        // una mano, e il rilascio solo su quello: con uno scarto i due modelli
        // distano ~lungo x scarto e la mano rimessa sull'arma a 59 cm con 16-20
        // gradi rimasti prendeva e lasciava a ogni tick.
        float fD[3], rD[3], uD[3];
        if (Norm3(c) > 1e-6f) {
            float gd[3][4];
            DrawnRot(in.gun, gd);
            GunAxes(gd, fD, rD, uD);
        } else {
            for (int i = 0; i < 3; ++i) {
                fD[i] = f1[i];
                rD[i] = r1[i];
                uD[i] = u1[i];
            }
        }
        // Astina: nella presa quella fissata alla presa, con l'inclinazione
        // fissata (M3, M4); fuori dalla presa quella di adesso nel telaio
        // dell'arma (inclinazione attuale).
        const Guard& g = engaged ? held : in.guard;
        if (in.leftValid && g.valid) {
            float gr[3], gu[3];
            if (engaged) {
                GuardFrameCant(f1, u1, cant, gr, gu);
            } else {
                for (int i = 0; i < 3; ++i) {
                    gr[i] = r1[i];
                    gu[i] = u1[i];
                }
            }
            o.dist = GuardDistance(g, d, f1, gr, gu, &o.along);  // modello con una mano: solo diagnostica
            if (engaged) {
                GuardFrameCant(fD, u1, cant, gr, gu);
                o.nearDist = GuardDistance(g, d, fD, gr, gu, nullptr);
            } else {
                o.nearDist = GuardDistance(g, d, fD, rD, uD, &o.along);
            }
        }
        // Calcio: posa del controller sinistro nel raw destro.
        const bool stockTest = in.gunstock && in.mountValid;
        if (stockTest && in.leftValid) {
            float dp[3] = {in.leftRaw[0][3] - in.rightRaw[0][3], in.leftRaw[1][3] - in.rightRaw[1][3], in.leftRaw[2][3] - in.rightRaw[2][3]};
            float rel[3], relRot[3][4] = {};
            for (int i = 0; i < 3; ++i) rel[i] = in.rightRaw[0][i] * dp[0] + in.rightRaw[1][i] * dp[1] + in.rightRaw[2][i] * dp[2];
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    relRot[i][j] = in.rightRaw[0][i] * in.leftRaw[0][j] + in.rightRaw[1][i] * in.leftRaw[1][j] + in.rightRaw[2][i] * in.leftRaw[2][j];
            float e[3] = {rel[0] - in.mountPos[0], rel[1] - in.mountPos[1], rel[2] - in.mountPos[2]};
            o.mountCm = Norm3(e) * 100.0f;
            o.mountDeg = RotAngleDeg(relRot, in.mountRot);
        }
        const char* nowHow = stockTest ? "stock" : (in.guard.shortGun ? "short weapon" : (in.gunstock ? "stock (no mount saved)" : "rifle"));
        auto engage = [&](bool grip, const char* why) {
            engaged = true;
            byGrip = grip;
            dwell = 0.0f;
            how = nowHow;
            o.event = 1;
            o.why = why;
            held = in.guard;
            cant = CantOf(fD, uD);  // = telaio dell'arma disegnata: la distanza appena misurata vale anche nella presa
            fadeOut = false;
            ResetDeparture();
            needRegrip = false;
            // Linea filtrata ripartita dal palmo di adesso: una mano arrivata
            // di slancio (grip alla prima prova) trascinava il filtro da 20-25
            // cm prima, e il suo assestarsi contava come allontanamento
            // (rilascio dopo 2-5 tick nel collaudo dei test, round 2).
            for (int k = 0; k < 3; ++k) {
                fd[k].Reset();
                df[k] = fd[k].Step(d[k], dt);
            }
        };
        auto release = [&](const char* why) {
            engaged = false;
            byGrip = false;
            dwell = 0.0f;
            o.event = 2;
            o.why = why;
            ResetDeparture();
        };
        // --- allontanamento nella presa con la mira a due mani (review round 2) ---
        const bool aimHold = !in.gunstock && g.valid && !g.shortGun && in.leftValid;
        float f2[3] = {f1[0], f1[1], f1[2]};
        bool solved = false;
        const float gf = g.fwd > 0.10f ? g.fwd : 0.10f;
        const float aIn = std::atan2(p.engageM, gf), aOut = std::atan2(p.releaseM > p.engageM + 1e-3f ? p.releaseM : p.engageM + 1e-3f, gf);
        auto departure = [&]() {
            const float dep0[2] = {dep[0], dep[1]};
            const bool had = haveDep;
            DepartStep(f1s, f2, u1);
            // La canna segue l'allontanamento (moto del palmo non seguito dalla
            // destra) al piu' a maxDegPerS: la parte in ritardo resta in
            // "pend". Mirare con due mani (le due parti si annullano) e il
            // rollio del polso destro non cambiano dep: passano subito.
            // Round 1 limitava la correzione intera, anche la compensazione
            // del polso destro.
            if (had) {
                pend[0] += dep[0] - dep0[0];
                pend[1] += dep[1] - dep0[1];
                const float n = std::sqrt(pend[0] * pend[0] + pend[1] * pend[1]), mx = p.maxDegPerS * 0.01745329252f * dt;
                const float k = n > mx ? (n - mx) / n : 0.0f;
                pend[0] *= k;
                pend[1] *= k;
            }
            const float dRad = std::sqrt(dep[0] * dep[0] + dep[1] * dep[1]);
            wDep = 1.0f - SmoothStep01((dRad - aIn) / (aOut - aIn));
            float x[3];
            Cross3(f1s, f2, x);
            apart = std::atan2(Norm3(x), Dot3(f1s, f2));
            const float s = p.apartScale > 1.0f ? p.apartScale : 1.0f;
            wApart = 1.0f - SmoothStep01((apart - s * aIn) / (s * (aOut - aIn)));
            // Palmo lungo la canna a due mani (linea filtrata): oltre la fine
            // dell'astina da twohandgrip a twohandrelease il peso va a 0.
            float gr2[3], gu2[3], pp[3];
            GuardFrameCant(f2, u1, cant, gr2, gu2);
            for (int i = 0; i < 3; ++i) pp[i] = df[i] - g.right * gr2[i] - g.up * gu2[i];
            alongTwo = Dot3(pp, f2);
            endM = alongTwo < g.tMin ? g.tMin - alongTwo : (alongTwo > g.tMax ? alongTwo - g.tMax : 0.0f);
            const float span = p.releaseM - p.engageM > 1e-3f ? p.releaseM - p.engageM : 1e-3f;
            wEnd = 1.0f - SmoothStep01((endM - p.engageM) / span);
        };
        if (engaged && aimHold && !in.veto) {
            solved = SolveForward(g, df, f1, u1, cant, f2);
            if (solved) departure();  // altrimenti (mani troppo vicine) restano i pesi di prima
        }
        const float wNow = wDep * wApart * wEnd;
        const char* lim = wDep <= wApart && wDep <= wEnd ? "departure" : (wApart <= wEnd ? "hands apart" : "barrel end");
        bool distRelease = false;
        // --- presa / rilascio ---
        if (!in.leftValid) {
            if (engaged) release("left controller lost");
            dwell = 0.0f;
            gripBlock = false;
            needRegrip = false;
        } else if (in.veto) {
            if (engaged) release(VetoName(in.veto));
            dwell = 0.0f;
        } else if (stockTest) {
            const bool inside = o.mountCm >= 0.0f && o.mountCm < p.stockM * 100.0f && o.mountDeg < p.stockDeg;
            const bool outside = o.mountCm > p.stockM * 150.0f || o.mountDeg > p.stockDeg * 1.5f;
            if (!engaged && inside) engage(false, "left controller on the stock");
            else if (engaged && outside) release("left controller off the stock");
        } else if (!in.guard.valid) {
            if (engaged) release("no grip on this weapon");
            dwell = 0.0f;
        } else {
            if (needRegrip && !in.grip) needRegrip = false;
            if (!engaged) {
                const bool inside = o.nearDist < p.engageM;
                if (!inside || needRegrip) {
                    dwell = 0.0f;
                    if (o.nearDist > p.engageM + 0.02f) gripBlock = false;
                } else if (in.grip) {
                    engage(true, "grip on the barrel");
                } else {
                    dwell += dt;
                    if (dwell >= p.dwellS && !gripBlock) engage(false, "hand held on the barrel");
                }
            } else if (aimHold ? wNow <= 1e-4f : o.nearDist > p.releaseM) {
                // Mira a due mani: il peso e' arrivato a 0 (mano andata via,
                // mani troppo in disaccordo, palmo oltre la fine dell'astina).
                // Le altre prese (pistola, calcio senza posa): distanza.
                const char* why = !aimHold ? "left hand off the barrel"
                                           : (lim[0] == 'd' ? "left hand off the barrel"
                                                            : (lim[0] == 'h' ? "gun hand turned away from the left hand"
                                                                             : "left hand past the end of the barrel"));
                release(why);
                distRelease = true;
                if (in.grip) needRegrip = true;
            } else if (byGrip && !in.grip) {
                release("grip released");
                gripBlock = true;
            } else if (!byGrip && in.grip) {
                byGrip = true;  // ora la tiene il grip: lasciarlo la lascia
            }
        }
        if (engaged && how != nowHow) how = nowHow;  // cambio di presa (holdmode) con la mano sull'arma
        // --- mira ---
        const bool aimOn = engaged && aimHold;
        if (!aimOn) ResetDeparture();
        float target[3] = {0.0f, 0.0f, 0.0f};
        if (aimOn) {
            if (!solved) {
                // Tick della presa (o mani vicine): canna e allontanamento da qui.
                solved = SolveForward(g, df, f1, u1, cant, f2);
                if (solved && !haveDep) departure();
            }
            if (solved) {
                // Canna voluta: quella a due mani, o col peso verso la canna con
                // una mano FILTRATA (slerp): il rumore del controller destro non
                // entra nemmeno nella fascia. Il peso applicato cambia al piu'
                // di maxDegPerS di canna: una mano che esce di slancio (1,2 m/s)
                // attraversa la fascia in 4 tick e la canna tornava alla destra
                // a 3 gradi a tick. Il limite e' sul PESO, non sulla correzione:
                // la correzione deve compensare subito la rotazione della mano
                // dell'arma (un rollio di 20 gradi in un tick, col limite sulla
                // correzione, spostava la canna fino a 16,8 gradi per 0,1 s).
                weight = wDep * wApart * wEnd;
                {
                    float x[3];
                    Cross3(f1s, f2, x);
                    const float th = std::atan2(Norm3(x), Dot3(f1s, f2));
                    const float mx = p.maxDegPerS * 0.01745329252f * dt;
                    const float lim = th > 1e-4f ? mx / th : 1.0f;
                    const float dw = weight - wApplied;
                    wApplied += dw > lim ? lim : (dw < -lim ? -lim : dw);
                }
                float fA[3] = {f2[0], f2[1], f2[2]};
                if (pend[0] != 0.0f || pend[1] != 0.0f) {
                    float gr[3], gu[3];
                    GuardFrame(f2, u1, gr, gu);
                    for (int i = 0; i < 3; ++i) fA[i] = f2[i] - pend[0] * gr[i] - pend[1] * gu[i];
                    Normalize3(fA);
                }
                float ft[3] = {fA[0], fA[1], fA[2]};
                if (wApplied < 0.9999f) {
                    float x[3];
                    Cross3(f1s, fA, x);
                    const float sn = Norm3(x), th = std::atan2(sn, Dot3(f1s, fA));
                    if (sn > 1e-6f) {
                        const float ka = std::sin((1.0f - wApplied) * th) / std::sin(th), kb = std::sin(wApplied * th) / std::sin(th);
                        for (int i = 0; i < 3; ++i) ft[i] = ka * f1s[i] + kb * fA[i];
                    } else {
                        for (int i = 0; i < 3; ++i) ft[i] = f1s[i];
                    }
                    Normalize3(ft);
                }
                float ax[3];
                Cross3(f1, f2, ax);
                o.targetDeg = std::atan2(Norm3(ax), Dot3(f1, f2)) * 57.2957795f;
                Cross3(f1, ft, ax);
                const float s = Norm3(ax), cs = Dot3(f1, ft);
                const float th = std::atan2(s, cs);
                if (s > 1e-7f) {
                    for (int i = 0; i < 3; ++i) ax[i] *= th / s;  // vettore di rotazione nel mondo
                    for (int i = 0; i < 3; ++i) target[i] = in.gun[0][i] * ax[0] + in.gun[1][i] * ax[1] + in.gun[2][i] * ax[2];
                }
                for (int i = 0; i < 3; ++i) lastTarget[i] = target[i];
                o.along = alongTwo;
            } else {
                for (int i = 0; i < 3; ++i) target[i] = lastTarget[i];  // mani troppo vicine: si tiene l'ultima
            }
            o.weight = wApplied;
            o.departDeg = DepartDeg();
            o.turnDeg = TurnDeg();
            o.apartDeg = apart * 57.2957795f;
            o.endCm = endM * 100.0f;
            o.limit = lim;
        }
        // Passaggio verso le due mani (presa, o cambio di presa con la mano
        // sull'arma): dallo scarto attuale (0 o quello rimasto) al bersaglio,
        // smoothstep su max(0,15 s, angolo / 100 gradi/s).
        if (aimOn && !aimWasOn) {
            for (int i = 0; i < 3; ++i) cStart[i] = c[i];
            w = 0.0f;
            blendT = -1.0f;
            fadeOut = false;
        }
        aimWasOn = aimOn;
        float want[3] = {c[0], c[1], c[2]};
        bool limit = true, leftoverPath = false;
        if (aimOn) {
            limit = false;  // nella presa: limite sul peso (sopra) e passaggio dolce alla presa (qui)
            if (blendT < 0.0f) {
                float dv[3] = {target[0] - cStart[0], target[1] - cStart[1], target[2] - cStart[2]};
                const float ang = Norm3(dv) * 57.2957795f;
                blendT = ang / p.blendDegPerS;
                if (blendT < p.blendMinS) blendT = p.blendMinS;
            }
            if (w < 1.0f) w += blendT > 0.0f ? dt / blendT : 1.0f;
            const float s = SmoothStep01(w);
            for (int i = 0; i < 3; ++i) want[i] = cStart[i] + (target[i] - cStart[i]) * s;
        } else if (in.veto & kHardVetoes) {
            for (float& v : want) v = 0.0f;
            limit = false;
            fadeOut = false;
        } else {
            if (distRelease) {
                // Mano andata via dall'astina: lo scarto (pochi gradi, il peso
                // l'ha gia' quasi tolto) torna a 0 nel tempo, non col moto.
                const float n = Norm3(c);
                fadeOut = n > 1e-6f;
                for (int i = 0; i < 3; ++i) fadeFrom[i] = c[i];
                fadeW = 0.0f;
                fadeT = n * 57.2957795f / p.blendDegPerS;
                if (fadeT < p.blendMinS) fadeT = p.blendMinS;
            }
            if (fadeOut) {
                fadeW += fadeT > 0.0f ? dt / fadeT : 1.0f;
                const float s = fadeW >= 1.0f ? 1.0f : SmoothStep01(fadeW);
                for (int i = 0; i < 3; ++i) want[i] = fadeFrom[i] * (1.0f - s);
                if (fadeW >= 1.0f) fadeOut = false;
            } else {
                // Scarto rimasto (grip lasciato, veto morbido, calcio, pistola): cala solo col moto della mano dell'arma.
                // Review round 2: il calo va nell'obiettivo (leftGoal) e c lo segue al piu' a maxDegPerS. Prima
                // il limite tagliava il calo di un tick: un colpo di polso di 20 gradi in un tick toglieva 2,1
                // gradi invece di 10, e il resto andava perso (in gioco 15,7 -> 7,2 gradi dopo 120 gradi di moto).
                const float n = Norm3(leftGoal);
                if (n > 0.0f) {
                    const float cut = p.leftoverPerDeg * motionDeg * 0.01745329252f;
                    const float k = n > cut ? (n - cut) / n : 0.0f;
                    for (int i = 0; i < 3; ++i) leftGoal[i] *= k;
                }
                for (int i = 0; i < 3; ++i) want[i] = leftGoal[i];
                leftoverPath = true;
            }
            for (int i = 0; i < 3; ++i) lastTarget[i] = 0.0f;
        }
        if (limit) {
            // Fuori dalla presa (scarto che cala col moto, ritorno nel tempo):
            // al piu' maxDegPerS, nessun passo oltre ~2 gradi a tick a 72 Hz.
            float dv[3] = {want[0] - c[0], want[1] - c[1], want[2] - c[2]};
            const float n = Norm3(dv), mx = p.maxDegPerS * 0.01745329252f * dt;
            if (n > mx) {
                const float k = n > 0.0f ? mx / n : 0.0f;
                for (float& v : dv) v *= k;
            }
            for (int i = 0; i < 3; ++i) c[i] += dv[i];
        } else {
            for (int i = 0; i < 3; ++i) c[i] = want[i];
        }
        if (!leftoverPath)
            for (int i = 0; i < 3; ++i) leftGoal[i] = c[i];  // fuori dallo scarto rimasto l'obiettivo e' la correzione stessa
        o.engaged = engaged;
        o.byGrip = byGrip;
        o.fading = fadeOut;
        o.how = engaged ? how : nowHow;
        o.corrDeg = Norm3(c) * 57.2957795f;
        if (o.corrDeg > 1e-5f) {
            // Telaio finale: rotazione = gun * Exp(c), attorno al palmo destro.
            float R[3][3];
            RotFromVec(c, R);
            float wl[3];  // polso nel telaio dell'arma
            const float wp[3] = {in.gun[0][3] - in.palmR[0], in.gun[1][3] - in.palmR[1], in.gun[2][3] - in.palmR[2]};
            for (int i = 0; i < 3; ++i) wl[i] = in.gun[0][i] * wp[0] + in.gun[1][i] * wp[1] + in.gun[2][i] * wp[2];
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) o.gun[i][j] = in.gun[i][0] * R[0][j] + in.gun[i][1] * R[1][j] + in.gun[i][2] * R[2][j];
            for (int i = 0; i < 3; ++i) o.gun[i][3] = in.palmR[i] + o.gun[i][0] * wl[0] + o.gun[i][1] * wl[1] + o.gun[i][2] * wl[2];
            o.aimChanged = true;
        }
        const Guard& gOut = engaged ? held : in.guard;
        if (in.leftValid && gOut.valid) {
            float f2o[3], r2[3], u2[3], gr2[3], gu2[3];
            GunAxes(o.gun, f2o, r2, u2);
            if (engaged) {
                GuardFrameCant(f2o, u1, cant, gr2, gu2);
            } else {
                for (int i = 0; i < 3; ++i) {
                    gr2[i] = r2[i];
                    gu2[i] = u2[i];
                }
            }
            o.distOut = GuardDistance(gOut, d, f2o, gr2, gu2, nullptr);
            if (aimOn) {
                // Sinistra disegnata sul modello (M3): la presa disegnata gira
                // col rollio dell'arma, il modello no (inclinazione fissata).
                float sh[3];
                for (int i = 0; i < 3; ++i) sh[i] = gOut.right * (gr2[i] - r2[i]) + gOut.up * (gu2[i] - u2[i]);
                o.drawShift[0] = Dot3(sh, f2o);
                o.drawShift[1] = Dot3(sh, r2);
                o.drawShift[2] = Dot3(sh, u2);
            }
        }
        return o;
    }
};

// Posa sul calcio (B+Y con "holdmode gunstock"): controller sinistro nel raw
// destro, testo dell'ini "stockmount x y z yaw pitch roll" (cm, gradi; angoli
// esatti come ExactEuler).
inline void StockMountFromPoses(const float rightRaw[3][4], const float leftRaw[3][4], float out6[6]) {
    float dp[3] = {leftRaw[0][3] - rightRaw[0][3], leftRaw[1][3] - rightRaw[1][3], leftRaw[2][3] - rightRaw[2][3]};
    float rel[3][4] = {};
    for (int i = 0; i < 3; ++i) {
        rel[i][3] = rightRaw[0][i] * dp[0] + rightRaw[1][i] * dp[1] + rightRaw[2][i] * dp[2];
        for (int j = 0; j < 3; ++j)
            rel[i][j] = rightRaw[0][i] * leftRaw[0][j] + rightRaw[1][i] * leftRaw[1][j] + rightRaw[2][i] * leftRaw[2][j];
    }
    const vr::pure::Euler e = vr::pure::ExactEuler(rel);
    out6[0] = rel[0][3] * 100.0f;
    out6[1] = rel[1][3] * 100.0f;
    out6[2] = rel[2][3] * 100.0f;
    out6[3] = e.yaw * 57.2957795f;
    out6[4] = e.pitch * 57.2957795f;
    out6[5] = e.roll * 57.2957795f;
}
inline void StockMountToPose(const float v6[6], float pos[3], float rot[3][4]) {
    vr::pure::PoseFromEuler(v6[4] * 0.01745329252f, v6[3] * 0.01745329252f, v6[5] * 0.01745329252f, rot);
    for (int i = 0; i < 3; ++i) pos[i] = v6[i] * 0.01f;
}

}  // namespace vr::twohand
