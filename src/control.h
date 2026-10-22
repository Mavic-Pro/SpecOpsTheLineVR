#pragma once
#include <atomic>
#include <openvr.h>
#include <cstdint>
#include <string>

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
// Sperimentale: invia solo coppie di occhi presentate una dopo l'altra ("paircheck on").
extern std::atomic<bool> g_pairCheck;
// Comando alla squadra (tasto squadra tenuto): il bersaglio e' il nemico lungo
// la canna, come il mirino verde. Il gioco lo sceglie al centro dello schermo,
// cioe' dove guarda la testa (segnalazione 18/10: il cecchino sul cartellone
// non si riusciva a indicare).
extern std::atomic<bool> g_commandFollowsGun;
// Pose di testa e controller predette per l'istante in cui il frame arriva
// sul display: fino al prossimo vsync + vsync->fotoni + predictFrames frame.
extern std::atomic<float> g_predictFrames;
// "predict auto": predizione = ritardo misurato testa -> fotoni (vr_output).
extern std::atomic<bool> g_predictAuto;
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
// Aggiornamento automatico (updater.cpp): "autoupdate on|off", "updaterepo proprietario/nome".
extern std::atomic<bool> g_autoUpdate;
// "updatechannel beta": l'aggiornamento include le pre-release (predefinito: stable).
extern std::atomic<bool> g_updateBeta;
std::string UpdateRepo();
// Prima persona: Walker opaco (niente dissolvenza vicino alla camera) e testa nascosta.
extern std::atomic<bool> g_fpBody;
// Mano sinistra entro g_leftGripCm dalla presa sull'astina: agganciata
// all'arma nella posa del gioco (dita chiuse sull'arma).
extern std::atomic<bool> g_leftGrip;
extern std::atomic<float> g_leftGripCm;
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
// Torrette e mitragliatrici fisse mirate col controller ("turretaim hand|stick").
extern std::atomic<bool> g_turretAimHand;
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
extern std::atomic<int> g_commandAimMode;
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
extern std::atomic<bool> g_seated;
extern std::atomic<float> g_eyeHeightAuto;  // calcolata (unita' sopra il centro del Pawn), <0 = non ancora
// "height <metri>": altezza della testa in piedi fissa (R-B6), <=0 = misurata.
extern std::atomic<float> g_heightFixedM;
// Entrambi gli stick tenuti 3 s: l'altezza attuale della testa diventa quella
// in piedi e viene salvata nell'ini.
extern std::atomic<bool> g_heightCalibRequested;
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
extern std::atomic<bool> g_vrMenuFlipY;  // "vrmenu flipy on": asse Y del puntatore invertito (diagnostica)
// "waitbeforesubmit on|off": attesa del compositor dopo la copia, subito prima dell'invio (pacing).
extern std::atomic<bool> g_waitBeforeSubmit;
extern std::atomic<unsigned long long> g_interactSimUntil[2];  // "interactsim": zona finta fino a (ms)
extern std::atomic<int> g_hintsShown;
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
extern std::atomic<float> g_twoHandCm;  // tolleranza della sinistra dalla linea dell'arma (cm)
extern std::atomic<float> g_twoHandSmoothSec;  // filtro della direzione a due mani (s), "twohand on <cm> <s>"
extern std::atomic<float> g_synthLeftCm[3];
// Gesti (vr_input.cpp): lancio della granata con la mano sinistra (LB),
// ricarica tirando giu' il caricatore (X), copertura abbassandosi (A).
extern std::atomic<bool> g_gestureGrenade, g_gestureReload, g_gestureCover;
// Pugno in avanti con la sinistra all'altezza del petto = corpo a corpo (B).
extern std::atomic<bool> g_gestureMelee;
// Ricarica fisica (reload physical, predefinita): grip sinistro sul
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
