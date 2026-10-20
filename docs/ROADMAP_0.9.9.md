# 0.9.9 / 1.0 work list (audit of 20/10, user: "tutte le funzionalita' full VR e i bug")

Status: [ ] todo, [x] done (committed), [~] done but needs headset check.

## A. Input / controls
- [~] A1 GateCombo drops B/Y presses of ~105-135 ms (gesture B/Y = 120 ms): pulse on release when never passed; gestures OR-ed after the gate
- [~] A2 Executions: align body/Pawn yaw to nearest downed enemy (or head) on B rising edge; camera pitch down; widen execution/melee boxes per Pawn
- [~] A3 (melee independent of grenade done; downward strike TODO) Downward strike melee gesture near a downed enemy; melee gesture independent of grenade gesture
- [~] A4 Left-handed: menu/pause read from the physical left hand
- [~] A5 Pause menu: skip stick/LT/turn rules while g_gamePaused
- [~] A6 D-pad: ignore right stick X after D-pad use until recentred
- [~] A7 Vive trackpad: zero the stick while a zone click is down
- [~] A8 Dashboard: reset menu tap state while dashboard/no data; resync g_vrDashboard from IsDashboardVisible
- [~] A9 Legacy input fallback: retry actions periodically
- [~] A10 WMR: Y (left secondary) binding
- [~] A11 Duck-to-cover A suppressed during physical reload
- [~] A12 Reload: re-send X at insertion if ammo did not change; reset mag state on weapon change/death; ignore grabs from the two-hand pistol grip
- [~] A13 Turn-with-head off on turrets; PC pitch reset after leaving a turret
- [~] A14 Cover peek "up" measured from standing reference
- [~] A15 Grenade direction averaged around peak velocity
- [~] A16 Suppress melee/grenade gestures while the left hand is at the support grip
- [~] A17 Calibration failure feedback (double pulse)
- [~] A18 (on/off normalised, gesture/dynres/gpushare fixed; numeric warnings TODO) ini: on/off/1/0 normalisation, warnings for bad values, dynres max, gpushare parsing, gesture melee documented

## B. Robustness / session
- [~] B1 KeepSubclass re-entrancy guard / only replace the known game proc
- [~] B2 Window protections only while HmdPresent (minimize/focus after taking the headset off)
- [~] B3 Cinema overlay re-created after SteamVR restart (generation counter also in Switch)
- [~] B4 Worker quiesce without timeout before releasing surfaces/textures
- [~] B5 ConfigureReprojection retried until the HMD tracking system is known
- [~] B6 SteamVR death without Quit event detected
- [~] B7 g_present reset on Quit; back-off on failed Scene init
- [~] B8 PropertyChanged filtered to projection-relevant properties
- [~] B9 ProTube bridge: pipe before InitPistol; wide-char paths
- [~] B10 bHaptics socket close order
- [~] B11 ini written via temp file + replace; migration on normalised lines
- [~] B12 Crash handler: only fatal codes, dump from helper thread
- [~] B13 Orderly shutdown on WM_CLOSE (exit crash)
- [~] B14 Display spoof wanted size updated on Reset (setres)

## C. Rendering
- [~] C1 Scope circle/vignette centred on tangent (0,0) per eye
- [~] C2 Scope: eye recognition with eyeK=0 (skip RecordEyeOrigin)
- [ ] C3 Scope state decided before the two Draws, in the frame tag
- [~] C4 Aim marker cleared on cinema frames
- [-] C5 (rejected: the ring tick is diagnostic only, a similar check added stutter on 18/10) Worker: pair ticks must match
- [~] C6 Full-screen overlays scaled with dynamic resolution
- [~] C7 System-memory surfaces only when needed (32-bit address space)
- [~] C8 Output not disabled when crop size is still 0
- [~] C9 offcenter change marks projection dirty
- [ ] C10 Film grain off; head-locked material effects reviewed; DoF off without losing bloom
- [ ] C11 Right-eye view state leak

## D. Install / update
- [~] D1 Updater never installs a revoked tag
- [~] D2 rollback prefers .old
- [~] D3 uninstall checks the specopsvr marker
- [~] D4 Unicode paths (manifest UTF-8, ini, command file)
- [~] D5 collect_logs: copy first, add SteamVR logs and input folder
- [~] D6 Installer: accept Binaries\Win32 / exe path, non-Steam (GOG) path

## E. Full-VR features
- [~] E1 (honestreload; chambering TODO) Honest reload: no fire with the magazine out; optional chambering
- [x] E2 (checked 20/10: the FMOD listener already follows the rendered view/head, not the gun; hook kept as "audiolog" diagnostic) Head-relative audio (FMOD listener)
- [ ] E3 Wrist / weapon HUD (ammo, grenades, objective), game HUD widgets hidden
- [ ] E4 Body holsters: primary shoulder, secondary hip, grenades chest, magazines belt
- [ ] E5 Physical grenades: grab, hold, throw strength = distance
- [ ] E6 World interaction: ammo/weapon pickup by hand, doors
- [ ] E7 Body cover + blind fire over cover
- [ ] E8 Squad orders by pointing with the left hand
- [ ] E9 Comfort in scripted camera/vehicle sequences
- [ ] E10 Moral choices / QTE by gestures (ending: gun to the temple)
- [~] E11 Directional bHaptics damage
- [ ] E12 Laser pointer in cinema-screen menus
- [~] E13 (movedir hand, seated -> game crouch view; one-handed TODO) Seated / one-handed presets, movedir hand
- [ ] E14 Hidden-area mesh (performance)
