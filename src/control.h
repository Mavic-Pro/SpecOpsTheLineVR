#pragma once
#include <atomic>
#include <cstdint>
#include <string>

// Canale di controllo a runtime: la DLL legge periodicamente
// specopsvr_cmd.txt (accanto alla dll), esegue i comandi e lo cancella.
// Serve a fare esperimenti (modalita' di tracking, screenshot, dump della
// camera) senza ricompilare ne' riavviare il gioco. Vedi README per l'elenco.
namespace vr::control {

enum TrackMode : int {
    kModeOff = 0,        // nessuna modifica alla camera
    kModeHmd = 1,        // posa del visore via OpenVR
    kModeSynthetic = 2,  // oscillazione sinusoidale finta, per test senza visore
};

extern std::atomic<int> g_mode;
extern std::atomic<bool> g_recenterRequested;
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
// Comando alla squadra (tasto squadra tenuto): il bersaglio e' il nemico lungo
// la canna, come il mirino verde. Il gioco lo sceglie al centro dello schermo,
// cioe' dove guarda la testa (segnalazione 18/10: il cecchino sul cartellone
// non si riusciva a indicare).
extern std::atomic<bool> g_commandFollowsGun;
// Pose di testa e controller predette per l'istante in cui il frame arriva
// sul display: fino al prossimo vsync + vsync->fotoni + predictFrames frame.
extern std::atomic<float> g_predictFrames;
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
// Correzione dell'inclinazione del controller (gradi, + = verso l'alto).
extern std::atomic<float> g_aimPitchOffsetDeg;
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
// Diagnostica: riga COLPO per ogni colpo del giocatore.
extern std::atomic<bool> g_fireLog;
// Diagnostica: "projlog N" registra per N secondi i proiettili sparati (PROJECTILE ...).
extern std::atomic<int> g_projLogSeconds;
// Grilletto destro premuto (letto dall'input VR): innesca la ricerca dei proiettili lanciati.
extern std::atomic<bool> g_rightTriggerHeld;
// Tasto squadra (RB) premuto nell'input passato al gioco: comando alla squadra.
extern std::atomic<bool> g_squadButtonHeld;
// Aggiornamento automatico (updater.cpp): "autoupdate on|off", "updaterepo proprietario/nome".
extern std::atomic<bool> g_autoUpdate;
std::string UpdateRepo();
// Prima persona: Walker opaco (niente dissolvenza vicino alla camera) e testa nascosta.
extern std::atomic<bool> g_fpBody;
// Mano sinistra sull'astina con la mira a due mani: la tiene il gioco (dita chiuse sull'arma).
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
extern std::atomic<float> g_armRotDeg[3];
// Allungamento massimo del braccio per raggiungere i controller (1 = nessuno).
extern std::atomic<float> g_armStretch;
extern std::atomic<float> g_armOffsetCm[3];
// Nomi dei controlli IK nell'AnimTree (destra, sinistra, rotazione destra,
// rotazione sinistra) e richieste di diagnostica (armdebug, armbones).
std::string ArmControlName(int i);
extern std::atomic<int> g_armDebugFrames;
extern std::atomic<bool> g_armBonesRequested;
// "bodysize": altezza dello scheletro di Walker (ossa piu' alta e piu' bassa) nel log.
extern std::atomic<bool> g_bodySizeRequested;
// "camlog N": per N frame, posizione di base della vista (gioco) e finale (6DOF) nel log.
extern std::atomic<int> g_camLogFrames;
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
extern std::atomic<float> g_synthLeftCm[3];
// Gesti (vr_input.cpp): lancio della granata con la mano sinistra (LB),
// ricarica tirando giu' il caricatore (X), copertura abbassandosi (A).
extern std::atomic<bool> g_gestureGrenade, g_gestureReload, g_gestureCover;
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
