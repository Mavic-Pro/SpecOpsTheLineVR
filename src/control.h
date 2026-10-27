#pragma once
#include <atomic>
#include <openvr.h>
#include <cstdint>
#include <string>

namespace vr::pure {
struct GunFrameParams;  // pure_utils.h (telaio dell'arma, WI-4a)
}

// Canale di controllo a runtime: la DLL legge periodicamente
// specopsvr_cmd.txt (accanto alla dll), esegue i comandi e lo cancella.
// Serve a fare esperimenti (modalita' di tracking, screenshot, dump della
// camera) senza ricompilare ne' riavviare il gioco. Vedi README per l'elenco.
namespace vr::control {

// Cartella del gioco (Binaries\Win32), sicura per le API ANSI: nome corto 8.3
// se il percorso ha caratteri fuori dalla code page.
std::string GameDirectory();

enum TrackMode : int {
    kModeOff = 0,        // nessuna modifica alla camera
    kModeHmd = 1,        // posa del visore via OpenVR
    kModeSynthetic = 2,  // oscillazione sinusoidale finta, per test senza visore
};

extern std::atomic<int> g_mode;
extern std::atomic<bool> g_recenterRequested;
extern std::atomic<bool> g_recenterSilent;
// Eventi di ricentramento del runtime (zero pose / area di gioco, OpenXR
// reference space change): contati qui, li conferma o scarta la testa (WI-2).
extern std::atomic<uint32_t> g_runtimeRecenterEvents;
extern std::atomic<int> g_headTraceMode;  // "headtrace" (collaudi): passate della trace della testa
extern std::atomic<bool> g_fpPivotFix;  // "fppivot" (collaudi): correzione del perno degli occhi
extern std::atomic<bool> g_recenterHandsFix;  // "recenterhands" (collaudi): controller nel tick tenuto di un salto
extern std::atomic<bool> g_uiHookGameViewport;
extern std::atomic<bool> g_uiHookUiScene;
extern std::atomic<bool> g_ui169;
extern std::atomic<bool> g_menu169;  // menu del gioco: back buffer 16:9 sullo schermo virtuale  // menu del gioco disposti in 16:9 sullo schermo virtuale  // diagnostica: aggancia anche UGameViewportClient::GetViewportSize  // ricentramento automatico (eventi): niente avviso ne' nero
extern std::atomic<bool> g_dumpCameraRequested;
extern std::atomic<float> g_synthYawAmpDeg;
extern std::atomic<float> g_synthPitchAmpDeg;
extern std::atomic<float> g_synthPeriodSec;
extern std::atomic<int> g_yawSign;
extern std::atomic<int> g_pitchSign;
extern std::atomic<bool> g_disablePostFx;  // motion blur + profondita' di campo
extern std::atomic<bool> g_vrOutput;       // invio dell'immagine al visore
extern std::atomic<bool> g_vrOutputForce;  // invia anche con il visore non indossato (test)
extern std::atomic<int> g_mirrorRequest;
// Aggiornamento della finestra sul monitor in stereo: 1 frame ogni N (il
// Present della finestra 2400x2500 costa ~5 ms al thread di rendering).
extern std::atomic<int> g_monitorEvery;
// Occhio destro in una finestra specchio sempre in primo piano ("mirrorwin on").
extern std::atomic<bool> g_mirrorWin;
// Schermo virtuale: fuori dal gameplay (cinematiche, menu, caricamenti) la
// scena del gioco, mono e in 16:9, viene mostrata su un grande schermo fisso
// nella stanza invece che attorno alla testa. g_cinemaActive = stato del tick
// corrente (thread di gioco -> rendering/uscita).
extern std::atomic<bool> g_cinemaEnabled;
extern std::atomic<bool> g_cinemaActive;
extern std::atomic<bool> g_cinemaForce;  // diagnostica: schermo virtuale anche in gameplay
extern std::atomic<float> g_cinemaWidthM, g_cinemaDistM;
extern std::atomic<int> g_outputTest;
// Scala dell'immagine per occhio inviata al visore (1 = piena risoluzione):
// meno pixel da leggere dalla GPU, piu' fps, meno nitidezza.
extern std::atomic<float> g_outputScale;
// Ogni frame inviato a SteamVR con la posa della testa usata per disegnarlo
// (Submit_TextureWithPose): la riproiezione del visore corregge dalla posa
// giusta. Senza, l'immagine "nuotava" muovendo la testa (segnalazione 18/10).
extern std::atomic<bool> g_submitWithPose;
// OpenXR con "submitpose on": gradi in piu' per lato del campo visivo disegnato
// e dichiarato ("xroverscan <gradi>", 1,5; WI-1, review: chiave di taratura).
extern std::atomic<float> g_xrOverscanDeg;
// Sperimentale: invia solo coppie di occhi presentate una dopo l'altra ("paircheck on").
extern std::atomic<bool> g_pairCheck;
// Pose di testa e controller predette per l'istante in cui il frame arriva
// sul display: fino al prossimo vsync + vsync->fotoni + predictFrames frame.
extern std::atomic<float> g_predictFrames;
// "predict auto": predizione = ritardo misurato testa -> fotoni (vr_output).
extern std::atomic<bool> g_predictAuto;
// OpenVR (SteamVR, Steam Link): un frame in piu' fino ai fotoni del frame aperto
// da WaitGetPoses quando l'attesa torna nella seconda meta' del frame
// ("predictframe auto|on|off", WI-1). auto = spento finche' la riga POSEAGE
// (posa di disegno contro quella del frame data dal runtime) non conferma per
// due finestre di fila che manca un frame (g_predictFrameAutoOn).
extern std::atomic<int> g_predictFrameMode;  // 0 off, 1 on, 2 auto
extern std::atomic<bool> g_predictFrameAutoOn;
// Predizione della testa al massimo questi ms ("predictmax <ms>", 150; WI-1).
extern std::atomic<float> g_predictMaxMs;
// Predizione dei soli controller in millisecondi ("handpredict"; 0 = nessuna).
extern std::atomic<float> g_handPredictMs;
// Con il device D3D9Ex (file specopsvr_gpushare): copia GPU->GPU degli occhi
// tramite superfici condivise invece della lettura via CPU.
extern std::atomic<bool> g_gpuCopy;  // diagnostica: 1 = salta la copia, 2 = salta copia e WaitGetPoses, 3 = solo senza WaitGetPoses
// Cattura dell'immagine mostrata dal compositor (texture specchio dei due
// occhi): percorso del BMP da scrivere, vuoto = nessuna richiesta.
bool TakeMirrorShotRequest(std::string* outPath);    // 1 = mostra, 0 = nascondi la finestra specchio di SteamVR, -1 = nulla
// Stereo ad alternanza di frame: meta' distanza interpupillare in unita'
// Unreal (0 = mono). Il thread di rendering pubblica in g_presentEye l'occhio
// del frame che sta per essere presentato (-1 = sconosciuto/mono).
extern std::atomic<float> g_stereoHalfIpd;
extern std::atomic<int> g_presentEye;
// true = stereo sincrono (la scena viene disegnata due volte per tick, un
// occhio per volta); false = alternanza di frame (AFR, sfarfalla).
extern std::atomic<bool> g_syncStereo;
// FOV orizzontale forzato nel gioco (gradi), 0 = quello del gioco. Con
// g_fovAuto lo calcola vr_output dai frustum degli occhi del visore.
extern std::atomic<float> g_fovOverride;
extern std::atomic<bool> g_fovAuto;
// GetTickCount64 dell'ultimo Draw stereo (doppio): serve a distinguere un
// frame stereo con occhio non riconosciuto da un frame senza stereo (menu).
extern std::atomic<uint64_t> g_lastStereoDrawMs;
// Prima persona (sperimentale): camera = posizione del Pawn + altezza occhi
// (+ spostamento in avanti lungo lo yaw). Unita' Unreal.
extern std::atomic<bool> g_firstPerson;
extern std::atomic<float> g_fpEyeHeight;
extern std::atomic<float> g_fpForward;
// Piano di clipping vicino in prima persona (unita' Unreal; gioco = 10):
// nasconde la testa di Walker, che mirando si sporge davanti alla camera.
extern std::atomic<float> g_fpNearPlane;
// true = la testa ruota solo la vista renderizzata e i colpi seguono l'arma
// (mouse); false = la testa ruota anche la mira del gioco (comportamento
// iniziale, colpi verso il centro della vista).
extern std::atomic<bool> g_aimFollowsBody;
// true = i colpi seguono il controller destro ("aim hand"); la vista resta
// corpo + testa. Richiede g_aimFollowsBody = true.
extern std::atomic<bool> g_aimHand;
// Telaio dell'arma (WI-4a, bug 6): presa ("holdmode": 0 normal = controller
// nudi, 1 gunstock = calcio), angolo della canna sul raggio di puntamento del
// controller (gradi, + = in su: "gunangle" con normal, "stockangle" con
// gunstock) e "gunyaw" (gradi, + = verso l'esterno). Sostituiscono aimpitch
// (angolo sul raw -Z dei Touch, 39,4 gradi sopra il raggio).
extern std::atomic<int> g_holdMode;
extern std::atomic<float> g_gunAngleDeg, g_stockAngleDeg, g_gunYawDeg;
// "openxrgrip" (per il palmo e il raggio quando OpenXR non lo misura).
extern std::atomic<bool> g_openXrGripFix;
extern std::atomic<float> g_openXrGripPitch;
// Angolo della presa attiva (gunangle o stockangle).
float HoldAngleDeg();
// Parametri del telaio per la mano FISICA (0 destra, 1 sinistra): raggio di
// puntamento nel raw (misurato dal backend OpenXR, altrimenti la costante dei
// Touch), angolo della presa (gunAngle false: 0, il raggio puro, per la mano
// di supporto), gunyaw col segno della mano. source: da dove viene il raggio.
vr::pure::GunFrameParams GunFrameFor(int physicalHand, bool gunAngle, const char** source = nullptr);
// Orientamento finto del controller per i test (gradi, relativi a corpo).
extern std::atomic<float> g_synthAimYawDeg;
extern std::atomic<float> g_synthAimPitchDeg;
// Indicatore di mira (aim hand) e protezione della testa dai muri.
extern std::atomic<bool> g_aimMarker;
extern std::atomic<bool> g_headCollision;
// Stick destro in VR: rotazione a scatti (gradi, 0 = continua, predefinito)
// e beccheggio dallo stick (predefinito no: lo sguardo su/giu' e' la testa).
extern std::atomic<float> g_snapTurnDeg;
extern std::atomic<bool> g_stickPitch;
// PlayerController corrente (camera - 0x77C), per la rotazione a scatti.
extern std::atomic<void*> g_playerController;
// Vignettatura di comfort: 0 = spenta (predefinito), 1 = massima. Intensita'
// corrente del movimento (0..1) aggiornata dall'input.
extern std::atomic<float> g_vignette;
extern std::atomic<float> g_motionAmount;
// HUD (Canvas 2D): scala verso il centro (1 = originale) e distanza
// apparente in unita' Unreal (0 = disparita' nulla, all'infinito).
extern std::atomic<float> g_hudScale;
extern std::atomic<float> g_hudDepth;
// 6DOF: spostamento della testa (posizione) e rollio applicati alla vista.
// g_posScale = unita' Unreal per metro (gioco ~2 cm/unita' -> 50).
extern std::atomic<bool> g_sixDof;
extern std::atomic<float> g_posScale;
extern std::atomic<int> g_rollSign;
// Spostamento finto della testa per i test (cm, avanti/destra/su), con lo
// stesso periodo di "synth".
extern std::atomic<float> g_synthPosCm[3];
// Arma nella mano: il modello dell'arma segue il controller destro (solo con
// aim hand). Offset dell'impugnatura nel riferimento del controller (cm:
// avanti, destra, su) e posizione della mano finta in modalita' synth (cm,
// rispetto alla testa al ricentramento: avanti, destra, su).
extern std::atomic<bool> g_gunInHand;
extern std::atomic<float> g_gunOffsetCm[3];
// Offset dell'impugnatura per singola classe d'arma (cm), sommato a
// g_gunOffsetCm. Restituisce false se la classe non ha un offset proprio.
bool GunOffsetForClass(const std::string& cls, float out[3]);
// Zoom dei cannocchiali: con un'arma con mirino telescopico, quando il gioco
// zooma, la mod lascia passare il FOV del gioco invece di forzare quello del visore.
extern std::atomic<bool> g_scopeZoom;
// Braccia di Walker guidate dai controller (IK del suo AnimTree): le mani
// stanno dove stanno i controller e l'arma, attaccata alla mano destra, le
// segue. armrot = rotazione della mano rispetto al controller (gradi: pitch,
// yaw, roll, per la destra; la sinistra e' speculare), armoffset = posizione
// del polso rispetto al controller (cm: avanti, destra, su).
extern std::atomic<bool> g_arms;
// Mira dalla canna: il gioco mira verso il punto in cui punta la canna del fucile visibile.
extern std::atomic<bool> g_muzzleAim;
// Aiuto alla mira del pad (friction, aim snap-on) spento mirando col controller.
extern std::atomic<bool> g_noAimAssist;
// Dispersione dei colpi del gioco (off = i colpi vanno esattamente dove punta la canna).
extern std::atomic<bool> g_spread;
// Scala del cono di dispersione del gioco riportato sulla canna: 0 = colpi
// esattamente lungo la canna, 1 = come il gioco ("spread on|off|<0..2>").
extern std::atomic<float> g_spreadScale;
// Diagnostica: riga COLPO per ogni colpo del giocatore.
extern std::atomic<bool> g_fireLog;
// Diagnostica: "projlog N" registra per N secondi i proiettili sparati (PROJECTILE ...).
extern std::atomic<int> g_projLogSeconds;
// Grilletto destro premuto (letto dall'input VR): innesca la ricerca dei proiettili lanciati.
extern std::atomic<bool> g_rightTriggerHeld;
// Arma corrente del giocatore (puntatore, 0 se nessuna) e munizioni lette a
// ogni tick (m_currentAmmoCount +0x764, m_currentMagAmmo +0x778): servono
// alla ricarica fisica per capire se il gioco ha davvero ricaricato.
extern std::atomic<uint32_t> g_weaponPtr;
// Ultima pressione di B (tasto o gesto del pugno), GetTickCount64: l'aiuto
// alle esecuzioni gira il corpo verso il nemico a terra prima che il gioco
// veda il tasto (vedi ExecutionAssist in hooks.cpp).
extern std::atomic<uint64_t> g_meleeIntentMs;
// Gioco in chiusura (WM_CLOSE / WM_DESTROY della finestra): la mod smette di
// toccare camera, PlayerController e finestra e di inviare al visore. Il
// gioco si chiudeva a volte con un crash nel suo codice (PlayerController
// senza Pawn, 20/10) mentre gli hook continuavano a scrivere.
extern std::atomic<bool> g_exiting;
// "execassist on|off" (predefinito on).
extern std::atomic<bool> g_execAssist;
// "honestreload on|off": col caricatore fuori (ricarica fisica) non si spara.
extern std::atomic<bool> g_honestReload;
// "audiolog N": N righe di diagnostica dell'ascoltatore FMOD.
extern std::atomic<int> g_audioLog;
extern std::atomic<int32_t> g_weaponAmmo[2];
// Tasto squadra (RB) premuto nell'input passato al gioco: comando alla squadra.
extern std::atomic<bool> g_squadButtonHeld;
// Aggiornamento automatico (updater.cpp, sempre attivo): "updaterepo proprietario/nome".
// "updatechannel beta": l'aggiornamento include le pre-release (predefinito: stable).
extern std::atomic<bool> g_updateBeta;
std::string UpdateRepo();
// Prima persona: Walker opaco (niente dissolvenza vicino alla camera) e testa nascosta.
// WI-3: la gestione del corpo vale sempre; "fpbody full|arms" sceglie solo cosa
// si disegna (arms = busto e gambe nascosti nel solo rendering). g_fpBody = full.
extern std::atomic<bool> g_fpBody;
extern std::atomic<int> g_fpBodyMode;  // 0 full, 1 arms
// "neckmodel <avanti cm> <su cm>": occhi rispetto al perno del collo del giocatore (ancora del corpo, WI-3).
extern std::atomic<float> g_neckModelCm[2];
extern std::atomic<int> g_wi3TestMask;  // collaudi A/B del WI-3 (wi3test)
// WI-3 fix 2 (review: valori da regolare nel visore, Avanzate):
// "necksize <0..1>": scala del collo di Walker nel solo rendering con la testa nascosta (1 = collo e sciarpa
// interi, testa chiusa sulla cima del collo; 0 = come HideBone: collo chiuso nel suo perno, sciarpa a fili);
// "neckring <unita'>": raggio dell'anello del collo tenuto fuori dalla vista guardando in basso (giocatori alti);
// "reachslack <unita'>": margine della portata del braccio dell'arma prima che il corpo avanzi;
// "seatedbelow <m>": soglia di "seated auto"; "seatedstand <m> <s>": seduti e poi in piedi, salita e durata
// prima di rifare la misura (anche la durata del ritorno a sedere).
extern std::atomic<float> g_neckSize;
extern std::atomic<float> g_neckRingR;
extern std::atomic<float> g_reachSlack;
extern std::atomic<float> g_seatedBelowM;
extern std::atomic<float> g_seatedStandRiseM;
extern std::atomic<float> g_seatedStandHoldS;
// Follow-up della regressione finale (27/10): "bodyalign <s>": con lo yaw della testa fermo per tanto il corpo
// torna sotto la testa (dentro la zona morta di turnwithhead), 0 = spento; "bodyalignspeed <gradi/s>": velocita'
// massima di quella rotazione (vr::pure::BodyFollow). La vista non cambia.
extern std::atomic<float> g_bodyAlignHoldS;
extern std::atomic<float> g_bodyAlignDegS;
// "camownyaw on|off" (solo collaudi prima/dopo, follow-up 27/10; prima "meshyaw", ancora accettato): vista con i
// controller non tracciati (camera del gioco) corretta delle rotazioni del corpo fatte dalla mod dopo la camera nello
// stesso tick.
extern std::atomic<bool> g_camOwnYaw;
// Review 2 del follow-up (27/10), solo collaudi prima/dopo: "armslead on|off" = le mani convertite nello spazio del
// Pawn con lo yaw attuale + il passo scritto da UpdatePawnFacing in questo Draw (l'arma disegnata non sta piu' un
// passo avanti mentre il corpo gira); "bodyalignwall on|off" = il corpo non torna sotto la testa con un muro o
// un riparo basso davanti agli occhi (arretramento attivo o previsto lungo lo yaw della testa).
extern std::atomic<bool> g_armsLead;
extern std::atomic<bool> g_bodyAlignWallStop;
// Mano sinistra agganciata all'arma nella posa del gioco (dita chiuse
// sull'arma) quando la tiene la presa a due mani (WI-4b: stessa prova della
// mira; prima un raggio suo, "leftgrip <cm>", ora ignorato). "leftgrip off" la spegne.
extern std::atomic<bool> g_leftGrip;
extern std::atomic<float> g_bodyBack;
// Spostamento aggiuntivo del corpo guardando in basso (oltre 30 gradi).
extern std::atomic<float> g_bodyBackLook;
// Il corpo segue gli spostamenti orizzontali della testa (6DOF).
extern std::atomic<bool> g_bodyFollowsHead;
// Girandosi fisicamente oltre questi gradi il corpo di Walker segue la testa
// (la vista non cambia): lo stick "avanti" torna verso dove si guarda.
// 0 = mai (solo stick). Segnalazione 18/10: "still had to use the right stick".
extern std::atomic<float> g_turnWithHeadDeg;
// Yaw (unita' Rotator) aggiunto dalla mod al PlayerController (rotazione a
// scatti, corpo che segue la testa): escluso dalle rotazioni "del gioco".
extern std::atomic<int32_t> g_ownYawUnits;
// Stick destro inclinato (rotazione dal giocatore): niente compensazione.
extern std::atomic<bool> g_stickTurning;
// Copertura: rotazione del corpo decisa dal gioco (ingresso nel riparo)
// compensata nella vista ("coversnap on|off").
extern std::atomic<bool> g_coverSnapCompensate;
// Copertura: sporgersi col corpo (alzarsi o spostarsi di lato) = LT, Walker
// esce dal riparo per mirare ("coverpeek on|off"). g_coverPeek: stato attuale.
extern std::atomic<bool> g_coverPeekEnabled;
extern std::atomic<bool> g_coverPeek;
// Mirando in copertura la vista segue Walker che si sporge oltre lo spigolo
// ("coverlean on|off").
extern std::atomic<bool> g_coverLeanEnabled;
// Torrette e mitragliatrici fisse mirate col controller ("turretaim hand|stick").
extern std::atomic<bool> g_turretAimHand;
// Presa sulla torretta (WI-4c, Basic "Turret grip"): "turretgrip auto|normal|gunstock"
// (0 auto = segue holdmode, 1 normal = due mani sulle maniglie come un manubrio,
// una mano basta; 2 gunstock = mira lungo il calcio come col fucile).
extern std::atomic<int> g_turretGrip;
// "turretlimits on|off" (Avanzate): la mod non spinge la torretta oltre i limiti del supporto.
extern std::atomic<bool> g_turretLimits;
// Taratura (Avanzate, review WI-4c round 1; prima fissi nel codice). Manubrio:
// turretbarmin / turretbarmax = distanza dei palmi (cm) per agganciarlo (tenuto
// fino a 5 cm sotto e 10 sopra); turretbaralign = gradi fra l'avanti della linea
// dei palmi e l'avanti dei controller (tenuto fino a +15); turretbarslope =
// inclinazione massima della linea (tenuto fino a +10); turretbarblend = secondi
// del passaggio una mano <-> manubrio; turretbarsmooth / turretbarbeta = filtro
// 1-euro (Hz, beta). Limiti (gradi nel telaio del supporto): [0] torrette a terra
// (turretgroundright/left/up/down), [1] minigun dell'elicottero
// (turretflightright/left/up/down, misurati); indice 0 destra, 1 sinistra, 2 su, 3 giu'.
extern std::atomic<float> g_turretBarMinCm, g_turretBarMaxCm, g_turretBarAlignDeg, g_turretBarSlopeDeg;
// turretpivot N (cm, 10-80): torrette a terra tenute per le maniglie con le due
// mani, perno N cm davanti alle mani (mani a sinistra -> canna a destra, mani
// giu' -> canna su; piu' corto = piu' angolo per cm). Non la minigun dell'elicottero.
extern std::atomic<float> g_turretPivotCm;
extern std::atomic<float> g_turretBarBlendSec, g_turretBarSmoothHz, g_turretBarBeta;
extern std::atomic<float> g_turretLimitDeg[2][4];
// Presa attiva in questo tick su una torretta mirata col controller (-1 nessuna
// torretta, 0 NORMAL, 1 GUNSTOCK). La scrive hooks; HoldAngleDeg la usa.
extern std::atomic<int> g_turretHoldNow;
// Torretta con presa NORMAL: i grip sono le maniglie (niente RB / LB al gioco).
extern std::atomic<bool> g_turretGripsAreHandles;
// Presa effettiva (0 normal, 1 gunstock): quella della torretta in torretta, se no holdmode.
int EffectiveHoldMode();
// Mira con le tacche: occhio sulla linea della canna -> niente mirino verde e
// LT (mira del gioco). "ads on|off"; g_adsActive = stato attuale.
extern std::atomic<bool> g_adsEnabled;
extern std::atomic<bool> g_adsActive;
// Mano sinistra sulla canna (mira a due mani): il grip sinistro non lancia
// la granata (LB) mentre si tiene l'arma.
extern std::atomic<bool> g_twoHandEngaged;
extern std::atomic<float> g_cupRel[3];
extern std::atomic<int> g_squadValidateLog;  // "squadpick": frame di log delle chiamate alla convalida del bersaglio
std::string TakeSquadPickRequest();  // "squadpick <parte della classe>": una richiesta, svuotata alla lettura  // pistole: mano di supporto rispetto all'osso della mano dell'arma (X avanti, Y giu', Z sinistra, unita')
// Mano sinistra agganciata all'arma (presa sull'astina, hooks UpdateArms):
// come g_twoHandEngaged, il grip sinistro non e' una granata.
extern std::atomic<bool> g_leftOnGun;
// Posa di testa e controller letta una volta per tick sul thread di gioco
// (inizio del Draw) invece che a ogni BeginScene ("headsample draw|render").
extern std::atomic<bool> g_headSampleOnDraw;
// Rotazione continua con lo stick destro fatta dalla mod (gradi/s; 0 = del gioco).
extern std::atomic<float> g_turnSpeedDeg;
// Direzione dello stick sinistro: "movedir head" = verso dove si guarda
// (fuori dalla copertura), "body" = verso il corpo. g_headYawUnits = testa
// rispetto al corpo (unita' Rotator), scritto da hooks.
extern std::atomic<bool> g_moveWithHead;
// "movedir hand" e yaw del controller sinistro rispetto al corpo (unita' UE,
// stesso riferimento di g_headYawUnits).
extern std::atomic<bool> g_moveDirHand;
extern std::atomic<int32_t> g_leftYawUnits;
extern std::atomic<bool> g_leftYawValid;
// Eventi di SteamVR (vr_session): dashboard aperta, richiesta di pausa del
// gioco (Start una volta, vr_input), proiezione degli occhi da ricalcolare.
extern std::atomic<bool> g_vrDashboard;
// Diagnostica delle costanti dei vertex shader ("vsdiag on").
extern std::atomic<bool> g_vsDiag;
// Dissolvenze del compositor su tagli, teletrasporti, morte ("fades on|off")
// e battito di ciglia sulla rotazione a scatti ("snapfade on|off").
extern std::atomic<bool> g_fades;
// Mancini ("hand left"): i ruoli dei controller si scambiano (il sinistro
// regge l'arma e ha grilletto/tasti della mano dell'arma).
extern std::atomic<bool> g_leftHanded;
// Laser sullo schermo virtuale (R-U5): il controller dell'arma muove il cursore
// del gioco sui menu, il grilletto clicca. flipy/full: convenzioni UV da provare.
extern std::atomic<bool> g_cinemaLaser, g_cinemaLaserFlipY, g_cinemaLaserFullUv;
// Posa del controller dell'arma (spazio standing di OpenVR, corretta), per il laser.
extern std::atomic<float> g_gunCtrlPose[12];
extern std::atomic<bool> g_gunCtrlPoseValid;
// HUD al polso (R-U3).
extern std::atomic<bool> g_wristHud;
extern std::atomic<float> g_wristHudWidthM;
// Ordini alla squadra: 0 testa (gioco), 1 canna, 2 mano di supporto che punta.
extern std::atomic<float> g_supportFwd[3];  // direzione della mano di supporto nel mondo
extern std::atomic<bool> g_supportFwdValid;
inline vr::ETrackedControllerRole GunRole() {
    return g_leftHanded ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand;
}
inline vr::ETrackedControllerRole SupportRole() {
    return g_leftHanded ? vr::TrackedControllerRole_RightHand : vr::TrackedControllerRole_LeftHand;
}
// Mano logica (0 appoggio/sinistra, 1 arma/destra) -> mano fisica delle azioni.
inline int PhysicalHand(int logical) { return g_leftHanded ? 1 - logical : logical; }
// Ricentramento richiesto: anche lo schermo virtuale si rimette davanti.
extern std::atomic<bool> g_cinemaRecenter;
// Movimento della vista non fatto dal giocatore (veicoli, rotazioni del gioco),
// 0..1, per la vignettatura (hooks).
extern std::atomic<float> g_forcedMotion;
// Altezza degli occhi: "height auto" = dalla testa vera in piedi al ricentramento
// (pavimento di SteamVR), "seated on" = fissa (fp) per chi gioca seduto.
extern std::atomic<bool> g_heightAuto;
// Seduti effettivo (WI-3): "seated on", o "seated auto" con la seduta riconosciuta
// (vr::pure::SeatedDetector all'inizio della scena e ai ricentramenti voluti).
extern std::atomic<bool> g_seated;
extern std::atomic<int> g_seatedMode;  // 0 off, 1 on, 2 auto
extern std::atomic<bool> g_seatedDetected;
void UpdateSeatedEffective();
extern std::atomic<float> g_eyeHeightAuto;  // calcolata (unita' sopra il centro del Pawn), <0 = non ancora
// "height <metri>": altezza della testa in piedi fissa (R-B6), <=0 = misurata.
extern std::atomic<float> g_heightFixedM;
// Entrambi gli stick tenuti 3 s: l'altezza attuale della testa diventa quella
// in piedi e viene salvata nell'ini.
extern std::atomic<bool> g_heightCalibRequested;
// Nuova cattura della seduta ("seated auto" scelto dal menu, WI-3).
extern std::atomic<bool> g_seatedCaptureRequested;
// Room-scale: il Pawn segue gli spostamenti fisici oltre 15 cm ("roomscale on|off").
extern std::atomic<bool> g_roomScale;
// Rotazioni del corpo fatte con lo stick destro (continua o a scatti), unita'
// Rotator, consumate da hooks per ruotare attorno alla testa.
extern std::atomic<int32_t> g_stickYawUnits;
// Col visore in uso la perdita del focus della finestra non mette in pausa ("keepfocus").
extern std::atomic<bool> g_keepFocus;
// specopsvr.ini letto (i thread di aggiornamento e aptica lo aspettano).
extern std::atomic<bool> g_configLoaded;
// Diagnostica dei nemici davanti alla vista ma non disegnati ("vischeck on", predefinito off).
extern std::atomic<bool> g_visCheck;
// "roomstep <unita'>": passo room-scale diagnostico lungo l'avanti del corpo (L8).
extern std::atomic<float> g_roomStepRequest;
extern std::atomic<bool> g_roomStepRaw;  // "roomstep N raw": senza correzione della base
// Ricentramento di direzione e posizione sotto il nero alla fine dello schermo virtuale.
extern std::atomic<bool> g_cinemaAlign;
// Avvisi brevi nel visore ("toasts on|off", predefinito on; R-U1).
extern std::atomic<bool> g_toasts;
// Suggerimenti sui controller: "hints off|on|auto" (0/1/2) e sessioni gia' fatte ("hintsshown N").
extern std::atomic<int> g_hints;
// "bodyfacing head|hands": il corpo segue la testa oltre turnwithhead (head), oppure
// il busto stimato da testa (0,5), linea tra le mani (0,3) e mano dell'arma (0,2)
// con zona morta di 20 gradi (hands, R-B3).
extern std::atomic<bool> g_bodyFacingHands;
// "interactlog on": censimento delle interazioni fisiche del livello nel log (fase 0, R-T4).
extern std::atomic<bool> g_interactLog;
// Fase 1/2: mano (0 destra, 1 sinistra) dentro la zona di presa di un'interazione
// (scritto dal thread di gioco) e "grabuse on|off": grip chiuso nella zona = A.
extern std::atomic<bool> g_interactZone[2];
extern std::atomic<bool> g_grabUse;
// Oggetti di scena (props.cpp, 24/10): "grabprops on|off"; mano (0 destra, 1
// sinistra) vicina a un oggetto da prendere / che tiene un oggetto (scritti dal
// thread di gioco: il grip li' non e' granata ne' ordine); grip letto dai
// controller (thread di input) e grip finto per i collaudi ("grabsim left|right
// on|off"); "throwboost <x>" (lancio = velocita' della mano x), "grabzone <cm>",
// "grabright on|off" (anche la mano dell'arma), "grablog N" (N tick di misure).
extern std::atomic<bool> g_grabProps;
extern std::atomic<bool> g_gravityGloves;
extern std::atomic<bool> g_grabHighlight;  // "grabhighlight on|off": angoli luminosi sull'oggetto puntato  // "gravitygloves on|off": punta, stringi, scatta (come Half-Life: Alyx)
extern std::atomic<bool> g_propZone[2];
extern std::atomic<bool> g_propHeld[2];
// GetTickCount64 del rilascio dell'ultimo oggetto per mano (props.cpp Release):
// i gesti della sinistra (granata, pugno) aspettano 0,5 s dopo un lancio.
extern std::atomic<unsigned long long> g_propReleasedMs[2];
extern std::atomic<bool> g_gripRaw[2];
extern std::atomic<bool> g_gripSim[2];
extern std::atomic<float> g_throwBoost;
// "throwmode peak|classic" (menu Basic, predefinito peak): lancio dal picco
// della velocita' della mano negli ultimi ~110 ms, con la rotazione del polso,
// la leva (baricentro lontano dalla mano) e un limite di energia (props.cpp,
// voce 4 del piano; "throwenergy <J>" nell'ini = griptune energy); classic =
// la media di 60 ms di prima. 1 = peak, 0 = classic.
extern std::atomic<int> g_throwMode;
// Ora (ProfMs) dell'ultima lettura delle pose (inizio di UpdateHeadTracking, hooks.cpp): la storia delle
// mani la usa per le velocita' del lancio (props.cpp, voce 4). Il tick di props arriva piu' tardi nello
// stesso Draw, dopo lavoro variabile: con la sua ora i campioni sbagliavano del 5-10%, una volta di 1,6 volte.
// Scritta solo dalla lettura sul thread di gioco (ViewportDrawThunk); con "headsample render" (2-3 letture
// per tick sul thread di rendering) 0: la storia usa l'ora del tick (revisione 26/10).
extern std::atomic<double> g_poseStampMs;
extern std::atomic<float> g_grabZoneCm;
// "masskg <kg>": kg per unita' di GetBodyMass (40: bottiglia 1,7 kg, radio 8,
// barile di carburante 33); i limiti degli oggetti pesanti in props.cpp sono in kg.
extern std::atomic<float> g_massKgPerUnit;
// "objectweight off|light|full" (menu Basic, predefinito light): peso degli
// oggetti nella mano libera (props.cpp, voce 2 del piano): forza e coppia
// massime della mano, abbassamento e piega della presa; light = forza e coppia
// x2, abbassamento e piega a meta'; off = la guida di prima. 0/1/2. Taratura:
// "weightforce|weighttorque|weightsag|weightdroop|weightturn" (ini) = griptune force|torque|klin|krot|alphamin;
// "weightbreak <ms>" (ini) = griptune breaktime (distacco degli oggetti incastrati, 0 = spento);
// "weightwedge <s>" (ini) = griptune wedgetime (incastrato senza via d'uscita, il palmo fuori portata: la
// mano lo lascia; 1 s il predefinito, 0 = mai); "weightregrip <s>" (ini) = griptune regriptime (bloccato
// con la mano ferma che lo tocca: la presa si riprende dove si trova; 0 = mai: strappi anche a mano
// ferma, e il rilascio solo da weightwedge).
extern std::atomic<int> g_objectWeight;
// "analoggrip on|off" (menu Basic, predefinito off; voce 6 del piano): con il
// peso (objectweight light/full) la forza e la coppia della mano sull'oggetto
// tenuto seguono quanto si stringe il grip, dal 40% appena oltre la soglia
// della presa (0,6) al 100% a fondo; con la presa lenta l'oggetto resta piu'
// indietro, pende un po' di piu' (mai oltre la piega massima) e torna piano
// all'angolo della mano. Mai un rifiuto: con la presa lenta l'accelerazione non
// scende sotto il pavimento della presa stretta (WeightOf, props.cpp). Il
// lancio usa la presa piu' stretta degli ultimi analoggripthrow ms. Taratura:
// "analoggripmin|analoggripturn|analoggripdroop|analoggriphold|analoggripease|
// analoggriprise|analoggripthrow|analoggripfloor|analoggriploose|analoggripfirm"
// (ini; fuori scala riportati dentro) = griptune gripmin|gripturn|gripdroop|
// griphold|gripease|griprise|gripthrow|gripfloor|griplo|griphi.
// g_gripAnalog: valore analogico del grip (0 destra, 1 sinistra come
// g_gripRaw; thread di input, vr_input.cpp GripAxis = vr::pure::GripAxisOf).
// "synthgrip <0..1> [rumore] [dopo ms] [rampa ms] | off" (collaudi): valore
// finto per entrambe le mani, con un rumore uniforme +-rumore a ogni lettura
// (grip che trema); con "dopo ms" vale fra tanto, con "rampa ms" ci arriva in
// linea retta in tanti ms (g_synthGripNext/NextRamp/At in ms di
// QueryPerformanceCounter, g_synthGripGen = numero del comando; props.cpp
// SynthGripTick, a ogni tick).
extern std::atomic<bool> g_analogGrip;
extern std::atomic<float> g_gripAnalog[2];
extern std::atomic<float> g_synthGrip, g_synthGripNoise;
extern std::atomic<float> g_synthGripNext, g_synthGripNextNoise, g_synthGripNextRamp;
extern std::atomic<unsigned long long> g_synthGripAt;
extern std::atomic<int> g_synthGripGen;
// "impacthaptics off|low|full" (menu Basic, predefinito full): vibrazione
// degli urti (props.cpp, voce 3 del piano): l'oggetto tenuto che urta, la mano
// vuota che schiaffeggia qualcosa, la presa e l'oggetto urtato vibrano in
// proporzione all'impulso (ampiezza, durata, frequenza: 90 Hz sopra i 5 kg,
// 225 sotto); low = ampiezza a meta'; off = niente urti, presa e oggetto
// urtato con gli impulsi fissi di prima. 0/1/2. Taratura: griptune hapdv,
// hapj, haprefr, haprearm, hapwindow, hapspherekg, hapspeed. "hapticlog N": le prossime N
// uscite della vibrazione nel log (vr_input.cpp; senza visore i valori calcolati).
extern std::atomic<int> g_impactHaptics;
extern std::atomic<int> g_hapticLog;
// "solidhands off|held|full" (menu Basic, predefinito held dal 26/10,
// decisione dell'orchestratore; configversion 10 cambia "solidhands off"):
// la mano libera disegnata segue la fisica (props.cpp RenderHandPose, voce 5
// del piano). held = sull'oggetto tenuto, solo con objectweight acceso
// (resta indietro, si abbassa e si piega con lui; premuto su un tavolo si
// ferma li') e sul punto afferrato di portiere e cartelli; full = anche la
// mano vuota sulla sua sfera, che si ferma sugli oggetti con un corpo fisico
// (provato: il coperchio della cassa del fucile; muri e pavimento del
// livello 1 non la fermano). La mano dell'arma e la mira non cambiano mai.
// 0/1/2. Taratura (ini, menu Advanced): solidhandsdead,
// solidhandsturn, solidhandscap, solidhandscapturn, solidhandsin,
// solidhandsout = griptune shdead|shturn|shcap|shcapturn|shin|shout.
// "solidlog N": i prossimi N frame della mano disegnata nel log ("SOLID:").
extern std::atomic<int> g_solidHands;
extern std::atomic<int> g_solidLog;
// Distanza (cm) tra la mano di supporto disegnata e il suo controller
// (hooks.cpp, solidhands): l'HUD al polso, agganciato al controller vero,
// sfuma oltre 5 cm (vr_wrist.cpp). 0 con solidhands off, senza braccia e
// fuori dal gioco (ReleaseArms, UpdateArms).
extern std::atomic<float> g_supportHandSepCm;
extern std::atomic<bool> g_grabRightHand;
extern std::atomic<int> g_grabLog;
// Oggetti come armi (props.cpp, pipeline dei colpi, 24/10): "propcombat on|off"
// = gli oggetti lanciati o tenuti in mano che colpiscono nemici, vetri,
// finestre di sabbia e oggetti distruttibili producono un colpo (riga
// "PROPS: hit" nel log; gli effetti li aggiungono le voci successive del
// piano). "hitlog N": le prossime N prove di contatto nel log (candidati e
// risultato della trace del motore), diagnostica.
extern std::atomic<bool> g_propCombat;
// Lancio che ferisce (props.cpp ThrowDamage, voce 1 del piano, 26/10):
// "propthrowdamage on|off" (predefinito on, menu Basic). Un oggetto lanciato
// che colpisce un nemico (evento della pipeline, serve propcombat): leggero =
// poco danno da mischia (non uccide mai), pesante o in testa = la reazione
// della mischia di Walker (barcolla o cade). Squadra e civili mai.
extern std::atomic<bool> g_propThrowDamage;
// Mischia con oggetti tenuti (props.cpp HeldMelee, voce 4 del piano, 26/10):
// "propmelee on|off" (predefinito on, menu Basic). Un oggetto tenuto in mano
// che colpisce un nemico oltre 2,5 m/s (evento della pipeline, serve
// propcombat): la reazione della mischia di Walker (barcolla o cade, da
// 3 m/s o in testa) o un colpo leggero, danno da mischia che non uccide mai,
// testa x2; al piu' un colpo ogni 0,6 s per nemico, mai durante la mischia di
// Walker. Bottiglie e bicchieri si rompono sul colpo. Squadra e civili mai.
extern std::atomic<bool> g_propMelee;
// Vetri, finestre di sabbia e casse (props.cpp WorldDamage, voce 2 del piano,
// 26/10): "propglass on|off" (predefinito on, menu Basic). Un oggetto lanciato
// o tenuto in mano che colpisce un vetro o una cassa distruttibile
// (YDestructibleObject: anche le finestre che trattengono la sabbia), una mesh
// fratturabile o una YSandBomb gli fa danno con TakeDamage del gioco, in base
// all'energia: la bottiglia incrina, la radio lanciata sfonda (e la valanga
// la fa partire il Kismet del livello). Serve propcombat.
extern std::atomic<bool> g_propGlass;
// Scudi con oggetti tenuti (props.cpp OnOtherShot / ShieldTick, voce 3 del
// piano, 26/10): "propshield on|off" (predefinito on, menu Basic). Un oggetto
// tenuto in mano ferma i colpi istantanei dei nemici (lo fa gia' la trace del
// gioco; la mod riscrive sull'oggetto anche il colpo che prende Walker dopo
// averne attraversato la scatola): spinta e vibrazione a ogni colpo, e dopo
// alcuni colpi (piatti e libri 2, valigie e radio 6, metallo e sacchi di
// sabbia 15) vola via dalla mano. off = i colpi attraversano gli oggetti
// tenuti (la trace del gioco rifatta con gli oggetti tenuti fuori). Non serve
// propcombat.
extern std::atomic<bool> g_propShield;
// Bombole esplosive da lanciare (props.cpp CarrierCreate / CarrierDamage, voce
// 5 del piano, 26/10): "propexplosive on|off" (predefinito on, menu Basic).
// Le YExplosiveActor (bombole di propano, di sub) non diventano fisiche
// (bWorldGeometry, bMovable spento): alla presa o col guanto le porta un
// KActorSpawnable con la stessa mesh e l'originale si nasconde. Un colpo
// (di Walker o di un nemico) o una caduta da circa 3 m (urto oltre 12 m/s) le fa esplodere dove sono,
// con l'esplosione e il danno del gioco. off = non si prendono (prima erano
// evidenziate ma non si muovevano).
extern std::atomic<bool> g_propExplosive;
// Armi dei nemici raccolte con la mano (props.cpp WeaponGrab / WeaponHandover,
// voce 6 del piano, 27/10): "weaponpickup on|off" (predefinito on, menu Basic).
// La sinistra prende un'arma lasciata a terra (fisica, guidata come gli
// oggetti); portata alla mano destra (entro 15 cm) la destra la impugna
// (YPickup_Weapon.DoPickup: l'arma in mano cade a terra se ce ne sono gia'
// due). Un'arma come una che Walker ha gia': toccarla (o passarla alla destra)
// ne prende le munizioni. off = le armi a terra restano al tasto A (grabuse).
extern std::atomic<bool> g_weaponPickup;
// Disarmo (props.cpp Disarm / DisarmTick, voce 10 del piano, 27/10): "disarm
// on|off" (predefinito on, menu Basic). Un colpo forte sull'arma di un nemico
// (oggetto tenuto, oggetto lanciato, mano vuota) lo fa barcollare e, piu'
// forte, gliela fa cadere (YArsenalManager.ServerDropWeapon, la strada della
// morte del gioco): l'arma diventa una pickup a terra (voce 6). L'IA
// disarmata non spara; dopo 5 s riprende la sua arma se e' ancora entro 3 m.
// Serve propcombat (la pipeline dei colpi).
extern std::atomic<bool> g_disarm;
// Cadaveri spinti e trascinati (props.cpp CorpseGrab / CorpseDrive, voce 11
// del piano, 27/10): "corpsedrag on|off" (predefinito on, menu Basic). Il grip
// della sinistra vicino a un arto di un nemico morto lo afferra e lo trascina
// con la fisica del ragdoll (AddImpulse sul corpo preso, forza della mano con
// il peso di objectweight, che cresce se il corpo non viene: pesante, mai
// immobile); la mano libera lo spinge. Mai i pawn vivi (Health > 0), mai chi
// sta ancora morendo (morte "a terra", animazione di morte, meno di 1 s dalla
// morte). I cadaveri congelati dal gioco tornano fisici con
// YGamePawn.ConditionalReanimateRagdoll. Serve grabprops.
extern std::atomic<bool> g_corpseDrag;
// GetTickCount64 fino a cui la mano libera (per mano, come g_propHeld) va
// sull'arma di un nemico (props.cpp DisarmHands: palmo entro ~30 cm dal box
// dell'arma, o diretto li' nei prossimi 0,2 s): i gesti della sinistra (pugno
// = B, granata = LB, vr_input.cpp) cedono al disarmo (revisione, 27/10).
extern std::atomic<unsigned long long> g_disarmNearMs[2];
// Finta granata (props.cpp FakeGrenadeLanded, voce 7 del piano, 27/10):
// "fakegrenade on|off" (predefinito on, menu Basic). Un oggetto piccolo
// (raggio sotto 15 unita': lattine, bottiglie, sassi) lanciato o lasciato dal
// guanto che atterra entro 3 m da un nemico: l'IA riceve l'avviso di una
// granata a frammentazione (YAIEventPropagator.SendEvent_GrenadeImpact, come
// la granata vera) e il pawn piu' vicino grida "granata!"
// (GrenadeImpactImminent). Un avviso ogni 10 s per gruppo. Non serve
// propcombat.
extern std::atomic<bool> g_fakeGrenade;
// Granate nemiche rilanciate (props.cpp GrenadeTick / GrenadeGrab /
// GrenadeRelease, voce 8 del piano, 27/10): "grenadeback on|off" (predefinito
// on, menu Basic). Una granata dell'IA (YProj_Physical, non Milkor, RPG,
// Mortar) ferma a terra entro 8 m ha angoli rossi; il grip della sinistra
// nella sua zona la prende (gravita' e collisione col mondo spente, guidata
// sul palmo con Actor.Velocity) e aprendo la mano parte con la velocita' della
// mano per throwboost (al piu' 26 m/s). La miccia continua a correre: in mano
// esplode e uccide Walker. Mai con la sinistra sull'arma. Rilanciata e' di
// Walker (Instigator, InitCauser): le sue uccisioni contano per lui. Serve
// grabprops.
extern std::atomic<bool> g_grenadeBack;
// Rumore per distrarre (props.cpp PropNoise, voce 9 del piano, 27/10):
// "propnoise on|off" (predefinito off, menu Basic). Un oggetto lanciato,
// lasciato, urtato dalla mano o dal guanto che sbatte (cambio di velocita'
// oltre 3 m/s da oltre 3 m/s), o una bottiglia rotta in mano, manda all'IA
// l'evento d'udito del corpo a corpo del gioco
// (YAIEventPropagator.SendHearEvent_Melee) nel punto dell'urto, con il
// raggio scalato dall'energia (3-15 m). Al piu' un evento al secondo.
extern std::atomic<bool> g_propNoise;
// Sonda di prova del rumore (props.cpp noiseprobe, revisione della voce 9):
// "noiseprobe on|off" (solo console, per i test; niente menu ne' ini). Alla
// prima accensione un hook su FUN_00E428A0 conta i bot che registrano davvero
// lo stimolo d'udito di ogni rumore (riga "PROPS: noise probe:" 1,5 s dopo).
extern std::atomic<bool> g_noiseProbe;
extern std::atomic<int> g_hitLog;
// Dita (props.cpp PoseFingers): "fingercurl <0..1>" forza la chiusura (-1 =
// automatica), "fingeraxis <dita> [pollice]": asse di flessione nello spazio
// dell'osso, 1/2/3 = X/Y/Z, negativo = verso opposto.
extern std::atomic<float> g_fingerCurlForce;
extern std::atomic<int> g_fingerAxis, g_thumbAxis;
extern std::atomic<int> g_graspDump;  // "graspdump N": geometria della presa nel log per N aggiornamenti
// Collaudi: mano sinistra finta in moto ("synthleftvel") e rilascio a tempo ("grabsim left off <ms>").
extern std::atomic<float> g_synthLeftVelCm[3];
extern std::atomic<float> g_synthLeftRotDeg[3];  // "synthleftrot <pitch> <yaw> <roll>": mano sinistra finta ruotata
extern std::atomic<unsigned long long> g_synthLeftVelStart, g_synthLeftVelUntil;
// "synthleftrotvel <pitch> <yaw> <roll> <ms>": la mano sinistra finta ruota (gradi/s);
// "synthsmooth on|off": moto finto col contatore ad alta risoluzione (collaudi del peso).
extern std::atomic<float> g_synthLeftRotVelDeg[3];
extern std::atomic<unsigned long long> g_synthLeftRotVelStart, g_synthLeftRotVelUntil;
extern std::atomic<bool> g_synthSmooth;
// "synthleftramp <f> <r> <u> <v1> <ms1> <v2> <ms2> [release]" (collaudi del lancio, voce 4): la mano
// sinistra finta va lungo (f, r, u) con la velocita' che sale da 0 a v1 cm/s in ms1, poi va a v2 cm/s in
// ms2 (contatore ad alta risoluzione); "release" apre il grip finto alla fine e la mano va avanti a v2 per 50 ms.
extern std::atomic<float> g_synthRamp[7];  // direzione unitaria (3), v1 cm/s, ms1, v2 cm/s, ms2
extern std::atomic<unsigned> g_synthRampSeq;  // cresce a ogni comando
// "synthleftjump f r u ms": salto della mano sinistra finta di (f, r, u) cm fra ms (collaudi; 0 = nessuno in attesa).
extern std::atomic<float> g_synthJumpCm[3];
extern std::atomic<unsigned long long> g_synthJumpAt;
// "synthleftlost <ms>": la mano sinistra finta non e' tracciata per <ms> (collaudo della mano persa con la granata, voce 8).
extern std::atomic<unsigned long long> g_synthLeftLostUntil;
extern std::atomic<bool> g_synthRampRelease;
// "synthcomp on|off": rotazioni del corpo compensate con un riferimento finto della testa (pose finte, collaudi).
extern std::atomic<bool> g_synthComp;
// "fpscap <hz>": tick di gioco al piu' a <hz> (collaudi senza visore; 0 = libero).
extern std::atomic<float> g_fpsCap;
// --- Collaudi senza visore dei bug del tester (25/10, WI-0; docs/TECHNICAL.md) ---
// "synthctrl [left] <yaw> <pitch> <roll>" / "synthctrl [left] off": con le pose
// finte la mano dell'arma (0) o la sinistra (1) e' una matrice raw di
// controller con questi angoli (gradi, rispetto al corpo) e passa dallo
// stesso calcolo della mira del visore (telaio dell'arma compreso, WI-4a).
extern std::atomic<bool> g_synthCtrlOn[2];
extern std::atomic<float> g_synthCtrlDeg[2][3];  // yaw, pitch, roll
// "synthheight <m>": altezza finta della testa sul pavimento (0 = off).
extern std::atomic<float> g_synthHeightM;
// "simevent zeropose": ricentramento del runtime simulato (1, consumato da vr_session.cpp).
extern std::atomic<int> g_simEvent;
// "viewlog N" / "bodylog N [ossa]": righe VIEW / BODY per N tick; "twohandinfo": una riga TWOHAND.
extern std::atomic<int> g_viewLogTicks, g_bodyLogTicks;
extern std::atomic<bool> g_twoHandInfoRequest;
std::string BodyLogBones();
extern std::atomic<unsigned long long> g_gripSimOffAt[2];
// Collaudo "breakdrop <ms>|off": la presa finta si apre <ms> dopo che un
// distacco (props.cpp, BreakawayStep) ha liberato l'oggetto; -1 = spento.
extern std::atomic<int> g_breakDropMs;
// "ham on|off": profondita' 0 nelle zone fuori lente all'azzeramento dello Z-buffer (R-C7).
extern std::atomic<bool> g_ham;
// "cinema3d on|off": schermo virtuale in stereo (due disegni a 0,4 IPD, overlay fianco a fianco; R-C6).
extern std::atomic<bool> g_cinema3d;
// Fattori del cono per postura: tacche all'occhio, due mani, arma lunga a una mano ("spread on <scala> <ads> <due> <una>").
extern std::atomic<float> g_spreadAds, g_spreadTwoHand, g_spreadOneHand;
// "laser on|off": raggio visibile dalla canna al punto d'impatto (aiuto alla mira alternativo al mirino).
extern std::atomic<bool> g_laser;
// "gesture gunmelee on|off": colpo di calcio / dall'alto con la mano dell'arma (predefinito off).
extern std::atomic<bool> g_gestureGunMelee;
// Menu nel visore: "vrmenu on|off" (apertura automatica all'avvio), aperto adesso, Virtual Desktop rilevato.
extern std::atomic<int> g_vrMenu;
extern std::atomic<bool> g_vrMenuOpen;
extern std::atomic<bool> g_vdDetected;
// Eseguibile diverso da quello Steam analizzato (copia non Steam): avviso nel visore.
extern std::atomic<bool> g_nonSteamExe;
extern std::atomic<bool> g_vrMenuFlipY;  // "vrmenu flipy on": asse Y del puntatore invertito (diagnostica)
// "waitbeforesubmit on|off": attesa del compositor dopo la copia, subito prima dell'invio (pacing).
extern std::atomic<bool> g_waitBeforeSubmit;
extern std::atomic<unsigned long long> g_interactSimUntil[2];  // "interactsim": zona finta fino a (ms)
extern std::atomic<int> g_hintsShown;
// Suggerimenti contestuali sugli oggetti gia' visti in questo profilo ("objecthints N" nell'ini, bit
// 1 << vr::ui::ObjHint; li scrive vr_toast con "hints auto"; "objecthints 0" li fa rivedere).
extern std::atomic<unsigned> g_objectHintsShown;
// Ini mai caricato (eseguibile non supportato, avvio lento): legge dall'ini
// solo autoupdate, updatechannel e updaterepo. Thread dell'aggiornamento.
void ReadUpdateSettingsFromIni();
// Attende la lettura dell'ini (al massimo maxMs) e poi altri extraMs.
void WaitForConfig(unsigned maxMs, unsigned extraMs);
// Arma in mano che spara solo mirando (RPG): LT tenuto col grilletto (vr_input).
extern std::atomic<bool> g_weaponNeedsAim;
// "handpredict auto": controller predetti come la testa (fino ai fotoni).
extern std::atomic<bool> g_handPredictAuto;
// Altezza della vista con Walker accucciato: "real" = testa vera sul
// pavimento, "game" = abbassata dal gioco (crouchview real|game).
extern std::atomic<bool> g_crouchViewReal;
// WI-3: "crouchview auto|real|game" (0, 1, 2) e "crouchtime <s>" (rampa della vista
// con l'accucciata del tasto; 0 = salto sotto un battito di ciglia).
extern std::atomic<int> g_crouchViewMode;
extern std::atomic<float> g_crouchTime;
// Accucciarsi davvero (30 cm sotto l'altezza in piedi) mette Walker nella
// postura accucciata del gioco, con le sue animazioni ("physicalcrouch on|off").
extern std::atomic<bool> g_physicalCrouch;
// "hittest <z>": linea come un colpo istantaneo verso Walker a <z> unita' sopra
// il centro del Pawn, esito nel log (-1000 = nessuna richiesta).
extern std::atomic<float> g_hitTestZ;
extern std::atomic<bool> g_snapFade;
extern std::atomic<bool> g_vrPauseRequest;
extern std::atomic<bool> g_vrProjectionDirty;
extern std::atomic<int32_t> g_headYawUnits;
// Stato del gioco per i gesti (hooks): scena bloccata (bCinematicMode),
// Walker in copertura, gioco in pausa (WorldInfo.Pauser).
extern std::atomic<bool> g_gameLocked;
extern std::atomic<bool> g_inCover;
extern std::atomic<bool> g_gamePaused;
// Scene "nel motore" con la camera di Walker e il giocatore bloccato
// (bCinematicMode): in 3D ("cinemawalker 3d") o sullo schermo virtuale ("screen").
extern std::atomic<bool> g_cinemaWalker3D;
// Calibrazione del calcio (gunstock): B + Y tenuti 1 s con il calcio in
// posizione di mira. La richiesta la esegue UpdateHeadTracking.
extern std::atomic<bool> g_stockCalibRequest;
// Migrazione (WI-4a, review round 1): un vecchio "aimpitch X" calibrato e'
// diventato "holdmode gunstock" + stockangle. hooks.cpp lo dice con un avviso,
// una volta, dopo qualche secondo di gioco (prima solo una riga nel log).
extern std::atomic<bool> g_holdMigrationNotice;
// 0.10.0: specopsvr.ini di una versione precedente rimesso ai predefiniti
// (LoadConfigFile, vr::pure::kResetConfigVersion): avviso nel visore dopo 10 s di gioco.
extern std::atomic<bool> g_settingsResetNotice;
// Feedback aptico esterno: ProTubeVR (ponte a 64 bit o ForceTubeVR_API_x32.dll)
// e bHaptics Player. g_protubeSwap ("protube swap") scambia i due ProVolver.
extern std::atomic<bool> g_protube, g_protubeSwap, g_bhaptics;
// Mirino del gioco (al centro della vista, cioe' legato alla testa): nascosto
// mirando con la mano, il mirino verde sulla canna lo sostituisce
// ("gamecrosshair on|off", predefinito off).
extern std::atomic<bool> g_gameCrosshair;
// Input dei controller: azioni di SteamVR Input (binding per Index, Touch,
// Vive, WMR, rimappabili in SteamVR) o API legacy ("vrinput actions|legacy").
extern std::atomic<bool> g_actionInput;
// Stato della vista (occlusione, LOD) separato per l'occhio destro
// ("eyestate on|off").
extern std::atomic<bool> g_perEyeViewState;
// Proiezione decentrata per occhio: tutto il back buffer e' l'occhio, niente
// ritaglio ("offcenter on|off").
extern std::atomic<bool> g_offCenter;
extern std::atomic<bool> g_projFlip;  // alto/basso degli occhi scambiati (prova)
// Prova dei display inclinati senza Pimax ("eyecant <gradi>", 0 = visore reale).
extern std::atomic<float> g_simCantDeg;
// Risoluzione dinamica: con la GPU vicina al limite del frame del visore la
// scena si disegna piu' piccola (ScreenPercentage di UE3) e il mod la riallarga
// ("dynres on|off", "dynres min <percento>").
extern std::atomic<bool> g_dynRes;
extern std::atomic<int> g_dynResMin;
// Soglie del controllore (WI-1, vr::pure::DynResController), rapporti col
// frame del visore: giu' sopra "dynresdown" (1,0) o sopra "dynresslow" (0,92)
// per 2 s, su dopo 5 s sotto "dynresup" (0,65); almeno "dynresdwell" s per
// livello (4); livello fallito bloccato "dynresbackoff" s (20), il doppio a
// ogni nuovo fallimento fino a 16 volte.
extern std::atomic<float> g_dynResDown, g_dynResSlow, g_dynResUp, g_dynResDwellSec, g_dynResBackoffSec;
// Prova: frame del visore finto in ms ("dynres budget <ms>", 0 = dal visore).
extern std::atomic<float> g_dynResBudgetMs;
// Prova: "dynres test" alterna 100/70% per 10 s e controlla che ogni immagine
// abbia il ritaglio giusto (viewport della scena = percentuale del frame tag).
extern std::atomic<int> g_dynResTestFrames;
// Preset grafico VR: spegne gli effetti a schermo che costano e cambiano tra
// i due occhi (SSAO, distorsione filtrata) ("vrpreset on|off").
extern std::atomic<bool> g_vrPreset;
// Invio delle immagini nello stesso tick quando la GPU ha margine
// ("lowlatency on|off").
extern std::atomic<bool> g_lowLatency;
// Risoluzione del gioco = quella consigliata da SteamVR per un occhio (che
// segue il cursore della risoluzione di SteamVR), "resolution auto"; oppure
// quella della finestra (ResX/ResY), "resolution window".
extern std::atomic<bool> g_autoResolution;
// Misura del tempo GPU con query di timestamp ("gputimer on|off").
extern std::atomic<bool> g_gpuTimer;
// Quest via Link: l'ASW di Meta, una volta acceso, blocca il gioco a meta'
// frequenza (36 fps a 72 Hz) anche con margine. "linkasw off" (predefinito):
// spento all'avvio della sessione VR e rimesso su Auto all'uscita del gioco;
// "linkasw keep": non si tocca.
extern std::atomic<bool> g_linkAswOff;
// Motion Smoothing di SteamVR (Index, Vive, Pimax, WMR) spento solo per
// questo gioco ("motionsmoothing off|keep").
extern std::atomic<bool> g_motionSmoothingOff;
// OneFrameThreadLag di UE3 ("threadlag on|off", predefinito on come nel gioco).
extern std::atomic<bool> g_threadLag;
// Pose dei controller riportate al riferimento dei Touch tramite il punto
// "tip" del modello SteamVR, cosi' la mira e' uguale con ogni controller
// ("controllerpose auto|raw").
extern std::atomic<bool> g_poseCorrect;
// Scrive "chiave valore" in specopsvr.ini (sostituisce la riga o la aggiunge).
void SaveIniValue(const std::string& key, const std::string& value);
// deploy/specopsvr.ini incorporato nella dll (predefiniti distribuiti): il
// menu VR ci prende le voci che l'ini dell'utente non ha ancora.
const char* DefaultIniText();
// Esegue una riga di comando come dall'ini / specopsvr_cmd.txt (menu VR).
void ExecuteLine(const std::string& raw);
extern std::atomic<float> g_armRotDeg[3];
// Allungamento massimo del braccio per raggiungere i controller (1 = nessuno).
extern std::atomic<float> g_armStretch;
extern std::atomic<float> g_armOffsetCm[3];
// Nomi dei controlli IK nell'AnimTree (destra, sinistra, rotazione destra,
// rotazione sinistra) e richieste di diagnostica (armdebug, armbones).
std::string ArmControlName(int i);
extern std::atomic<int> g_armDebugFrames;
extern std::atomic<bool> g_armBonesRequested;
// "weaponbones": ossa del modello dell'arma in mano nel log (R-W7).
extern std::atomic<bool> g_weaponBonesRequested;
// "bodysize": altezza dello scheletro di Walker (ossa piu' alta e piu' bassa) nel log.
extern std::atomic<bool> g_bodySizeRequested;
// "camlog N": per N frame, posizione di base della vista (gioco) e finale (6DOF) nel log.
extern std::atomic<int> g_camLogFrames;
extern std::atomic<int> g_gunLogFrames;
extern std::atomic<int> g_skelLogCalls;
// Ossa dell'ultimo aggiornamento buono rimesse quando la mano destra resta
// lontana dal bersaglio IK ("skelfix on|off [unita']").
extern std::atomic<bool> g_armSkelFix;
extern std::atomic<float> g_armSkelFixDist;  // diagnostica: aggiornamenti dello scheletro di Walker  // diagnostica: canna dell'arma rispetto agli occhi per N tick
// Costante di tempo (s) con cui l'altezza della vista segue quella del gioco:
// accovacciarsi in copertura o sporgersi abbassa/alza Walker di ~40 cm in un
// frame (18/10, "when taking cover my screen is shaking"). 0 = immediato.
extern std::atomic<float> g_eyeHeightSmooth;
// Distanza tra gli occhi letta dal visore (GetEyeToHeadTransform) invece del
// valore fisso di "stereo"; "ipd auto|fixed".
extern std::atomic<bool> g_ipdFromHmd;
extern std::atomic<float> g_synthHandCm[3];
// Mira a due mani: con la mano sinistra davanti alla destra (impugnatura da
// fucile) la mira va dalla mano destra alla sinistra. synthleft = mano
// sinistra finta per i test (cm, come synthhand; 0 0 0 = assente).
extern std::atomic<bool> g_twoHand;
// Presa a due mani (WI-4b, twohand_math.h). twohandgrip: il palmo sinistro
// entro tanti cm dall'astina la prende (col grip subito, senza dopo
// twohanddwell s fermo li'); twohandrelease: oltre, la lascia. twohandsmooth:
// taglio (Hz) del filtro 1-euro sulla linea tra i palmi da fermi.
// twohandguard: spostamento (cm avanti, destra, su) dell'astina misurata dalla
// presa disegnata (Avanzate: twohandguardfwd / twohandguardright /
// twohandguardup, una chiave per numero: il menu passa solo il primo numero di
// una riga; "twohandguard f r u" resta come comando). "twohand on|off [cm] [Hz]":
// comando di sviluppo (presa, filtro).
extern std::atomic<float> g_twoHandCm;
extern std::atomic<float> g_twoHandReleaseCm;
extern std::atomic<float> g_twoHandDwellSec;
extern std::atomic<float> g_twoHandFilterHz;
extern std::atomic<float> g_twoHandGuardCm[3];
// GUNSTOCK (WI-4b): posa del controller sinistro sul calcio nel raw destro,
// salvata da B + Y ("stockmount x y z yaw pitch roll", cm e gradi; "none" =
// nessuna); la presa a due mani col calcio vale entro stocktol (cm) e
// stocktoldeg (gradi; "stocktol <cm> [gradi]" resta come comando).
extern std::atomic<float> g_stockTolCm, g_stockTolDeg;
bool StockMount(float pos[3], float rot[3][4]);
// Grip sinistro gia' usato da un gesto (vr_input.cpp): 1 = ricarica fisica
// (caricatore fuori / in mano, presa del caricatore, otturatore), 2 = granata
// (LB). La presa a due mani non parte (precedenze del grip sinistro, WI-4b).
extern std::atomic<int> g_leftHandBusy;
// Palmo sinistro sull'arma per la presa a due mani (hooks.cpp TwoHandStep):
// entro twohandrelease dall'astina (o da quella disegnata con lo scarto
// rimasto), o vicino alla posa sul calcio. vr_input: il grip alla pressione
// non e' una granata (review WI-4b round 1: una sola geometria).
extern std::atomic<bool> g_leftNearBarrel;
// "twohandshift 0|1" (collaudi prima/dopo, review WI-4b round 1): lo stato
// della presa a due mani e del manubrio gira con un ricentramento del runtime assorbito.
extern std::atomic<bool> g_twoHandShiftFix;
extern std::atomic<float> g_synthLeftCm[3];
// Gesti (vr_input.cpp): lancio della granata con la mano sinistra (LB),
// ricarica tirando giu' il caricatore (X), copertura abbassandosi (A).
extern std::atomic<bool> g_gestureGrenade, g_gestureReload, g_gestureCover;
// Pugno in avanti con la sinistra all'altezza del petto = corpo a corpo (B).
extern std::atomic<bool> g_gestureMelee;
// Ricarica fisica (reload physical; dal 22/10 predefinita "simple"): grip sinistro sul
// caricatore e tirarlo via, grip alla cintura per prenderne uno nuovo,
// portarlo sotto l'arma per inserirlo (X). "simple" = tocca e tira giu'.
extern std::atomic<bool> g_reloadPhysical;
// "manualaction on": dopo la ricarica fisica l'arma va armata (gesto al castello) prima di sparare (R-W3).
extern std::atomic<bool> g_manualAction;
// Caricatore rispetto al controller destro: cm in avanti lungo la canna e sotto.
extern std::atomic<float> g_magwellCm[2];
// Spostamento del caricatore per il tipo d'arma in mano (cm, sommato a
// magwell): pistole nell'impugnatura, P90 sopra la canna (hooks).
extern std::atomic<float> g_magwellClassCm[2];
// Ultimo lancio col gesto: direzione della mano (spazio di SteamVR, unitaria)
// e istante (GetTickCount64): la granata lanciata parte in quella direzione.
extern std::atomic<float> g_throwDir[3];
extern std::atomic<float> g_throwSpeed;  // velocita' media della mano nel lancio (m/s), 0 = ignota
extern std::atomic<unsigned long long> g_throwStamp;
// Arma presa dalla spalla: grip destro con la mano sopra la spalla = Y (arma seguente).
extern std::atomic<bool> g_gestureWeapon;
// Diagnostica: righe INPUTLOG (stato grezzo dei Touch) ogni 250 ms, per N righe.
extern std::atomic<int> g_inputLogFrames;
// Diagnostica: righe GESTILOG (grandezze usate dai gesti) ogni 250 ms, per N righe.
extern std::atomic<int> g_gestureLogFrames;
// Diagnostica: simula un gesto ("throw", "reload", "duck") con pose finte.
bool TakeGestureSimRequest(std::string* out);
// true = la vista occupa tutta la finestra (niente letterbox 16:9 forzato
// dallo script del gioco tramite LocalPlayer.Origin/Size).
extern std::atomic<bool> g_fullViewport;


// Da chiamare dal thread di rendering (EndScene): legge ed esegue i comandi
// al massimo una volta ogni ~0.5s.
void Poll();

// Se e' stato chiesto uno screenshot, restituisce true e il percorso completo
// del file da scrivere (consumando la richiesta).
bool TakeScreenshotRequest(std::string* outPath);

}  // namespace vr::control
