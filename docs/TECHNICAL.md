# Technical notes

How SpecOpsTheLineVR works, the engine internals it relies on, and the tools
used to develop and test it. Addresses refer to the Steam build of
`SpecOpsTheLine.exe` (PE timestamp `0x50041F0C`, 12 July 2012, 32-bit, no
ASLR); `dllmain.cpp` refuses to hook any other executable.

## Architecture

The mod is a proxy `d3d9.dll` placed next to the game. It loads the real
`d3d9.dll` from the Windows folder and installs its hooks with MinHook.

| File | Role |
|---|---|
| `dllmain.cpp` | Proxy entry points, executable check, start-up |
| `real_d3d9.cpp` | Loads the system `d3d9.dll` |
| `hooks.cpp` | Device hooks, stereo, camera, first person, arms, aiming |
| `vr_session.cpp` | OpenVR session (Background until the headset is worn, then Scene) |
| `vr_output.cpp` | Frame transfer to the compositor (worker thread, D3D11), cinema overlay |
| `vr_input.cpp` | Touch controllers presented as an XInput pad, gestures |
| `script_call.cpp` | Calls into UnrealScript through `UObject::ProcessEvent` |
| `control.cpp` | `specopsvr.ini` / `specopsvr_cmd.txt` commands |
| `display_spoof.cpp` | Lets the game create a window larger than the desktop |
| `logger.cpp` | Per-session log with header, crash handler and minidumps |
| `updater.cpp` | Automatic update from the GitHub releases (channels, revocation list) |
| `haptics_ext.cpp` | ProTubeVR (ForceTubeVR_API_x32.dll) and bHaptics Player haptics |

## Rendering

- **Device**: when `gpushare` is on, `Direct3DCreate9Ex` + `CreateDeviceEx`.
  `D3DPOOL_MANAGED` textures/volumes/cubes become `DEFAULT` + `DYNAMIC`;
  vertex/index buffers become `DEFAULT` without `DYNAMIC` (with it the game
  drops to 2 fps).
- **Synchronous stereo**: `FViewport::Draw(UBOOL)` (`0x008F6A80`) is called
  twice per tick, once per eye, offsetting the camera by half the IPD.
- **Eye identification** on the render thread: the view origin in vertex
  shader constant `c5` (= -origin) is matched against a ring of origins
  recorded on the game thread (including the 6DOF head offset).
- **Field of view**: hook on `APlayerController::GetFOVAngle()` (`0x006F1EA0`),
  per-eye `VRTextureBounds` crop.
- **Full-screen view**: hook on `ULocalPlayer::CalcSceneView` (`0x0075C460`)
  to drop the 16:9 constraint.
- **Frame transfer**: the back buffer of each eye is copied (StretchRect) into
  a shared D3D9Ex surface opened by D3D11 (`OpenSharedResource`); a worker
  thread does `WaitGetPoses`, `CopyResource` and `Submit`.
- **Post effects**: motion blur and depth of field are switched off in
  `GSystemSettings` (`0x014D77B0`).
- **Near plane**: `GNearClippingPlane` (`0x0141578C`).
- **Frame tags and render pose**: before every presenting `FViewport::Draw`
  the game thread queues a tag {eye, tick, HMD pose of the tick}; the render
  thread pops it at the matching Present (render commands stay in order). The
  eye found from the view origin (c5) is only a cross-check: on disagreement
  the queue is re-aligned (STATS `frame tags disagree/resync/empty`). The HMD
  pose and the head deltas are published together by `UpdateHeadTracking`
  (render thread) and frozen once per tick for both eyes. Frames are
  submitted with their pose (`Submit_TextureWithPose`, `submitpose on`). In
  0.9.1 the pose came from the eye-origin ring, which is ambiguous while
  standing still (double images).
- **Eye ring**: entries carry a valid flag; the (0,0,0) origin of post-process
  passes is ignored; first match per frame; ring and pending eye images are
  dropped on cinema transitions.
- **Measured**: rendered projection 108.0 x 110.2 degrees for a requested 108
  (Quest 3); Walker's skeleton is 94 units tall, i.e. ~52 units/m for a 1.8 m
  man, so `posscale 50` is right.
- **First-person eye**: Walker's location + `fpforward` along the yaw of the
  Pawn (not the camera rotation, which at copy time could still hold the
  gun direction set by the mod: in cover the eyes jumped ~19 units per frame).
- IPD from `GetEyeToHeadTransform`.
- **Eye height** changes of the game (crouching in cover, popping up: ~20
  units in one frame) are smoothed on the view only (`eyesmooth`).
- **Cinema screen**: entered only after 0.6 s of cinematic mode (0.15 s when
  the camera leaves Walker), so short scripted moments stay in VR.

## Per-eye projection and view state (0.9.4)

- Off-center projection: `FPerspectiveMatrix(HalfFOV, W, H, MinZ)`
  (`0x009EAE80`, thiscall) is hooked; when it returns to
  `ULocalPlayer::CalcSceneView` (`0x0075CAC5` / `0x0075CBBA`) during an eye's
  Draw, M[0][0], M[1][1], M[2][0], M[2][1] are replaced with the eye's exact
  frustum from `GetProjectionRaw` (Quest 3: m00 0.903, m11 0.835, m20
  +-0.243, m21 -0.193). The whole back buffer is the eye: no crop, full
  texture bounds; before, 1932x2087 of 2400x2500 pixels were used. The mode
  travels in the frame tag, so crop/bounds, the aim marker, the HUD canvas
  and the vignette always match how the image was drawn. Not used while a
  sniper scope zooms (the game's mask is centred). Verified: the off-center
  image equals the scaled crop of the symmetric one (mean difference 5/255,
  moving squadmate only).
- Per-eye view state: `ULocalPlayer.ViewState` (+0x88) holds occlusion
  history and LOD fades. With one state for two views per tick, the left
  eye's occlusion results decided what the right eye drew. The right eye now
  gets its own state from `AllocateViewState` (`0x00C4F370`, `new
  FSceneViewState`, 0x2F0 bytes), swapped in only for its Draw, re-created if
  the engine re-creates its own. Frame-to-frame draw-call jitter per eye
  dropped 2-8x in a still scene (`EYEDRAWS` line).
- Canted displays (Pimax and similar): `GetEyeToHeadTransform` also carries
  a rotation of each eye, and `GetProjectionRaw` tangents are in that rotated
  eye frame. With the off-center projection the eye's view rotation is the
  head rotation composed with the eye's (in the `GetPlayerViewPoint` hook, so
  the aim marker follows), and the HUD is moved to the head-forward direction
  inside each eye. Logged as `VR-FOV: left eye canted N degrees`. Headsets
  with parallel eyes (Quest, Index, Vive, Pico...) are unaffected.
  `eyecant <deg>` simulates it for testing: at 10 degrees the left eye image
  moves 105 px right on a 1200 px shot (expected ~95).
- GPU timing (0.9.4): D3D9 timestamp queries from the first draw after a
  Present to the Present, read back a few frames later without waiting;
  `GPU:` line = both eyes per tick, max, headset frame time, scene percent and
  the game-thread Draw to render-thread Present latency.
- Dynamic resolution: `ScreenPercentage` (UE3 `scale set`) draws the scene in
  a centred sub-rectangle and, in this build, does not upscale it
  (`UpscaleScreenPercentage` has no effect); the mod crops that rectangle and
  the compositor scales it. The percentage travels in the frame tag; `dynres
  test` (alternating 100/70% every 0.5 s, checking the scene viewport of every
  frame) found 0 wrong crops in 1200 frames. Controller: every 0.7 s,
  `pct * sqrt(0.85 * frame / gpu)`, -10/+5 steps of 5, between `dynres min`
  and 100, only in stereo gameplay (100% on the cinema screen). `dynres budget
  <ms>` fakes the headset frame for tests (3 ms: down to 65%; 12 ms: back to
  100%). Changing it costs at most one ~20 ms frame.
- GPU A/B (scale set, still scene, each setting A/B/A/B): AmbientOcclusion
  -15%, FilteredDistortion -17%, Distortion -9%, ScreenPercentage 70 -25%;
  Bloom, LensFlares, FogVolumes, DynamicShadows, whole-scene dominant shadows,
  MaxShadowResolution, ShadowFilterQualityBias, MaxAnisotropy, DetailMode all
  under 5%. `vrpreset` turns off the first two. `OneFrameThreadLag` off
  (Draw->Present 4.1 -> 1.9 ms at 300 fps, ~14 -> ~3 ms in the headset) was
  in the preset at first, but in the headset it serialises the game and
  render threads: 48-53 fps instead of 75-86 in a chapter 1 fight. It stays
  as in the game (`threadlag off` to try it). `scale dump` output is not returned to the
  console; `ConsoleCommand` output is now logged as `CONSOLE>`.
- Same-tick submit: after the right eye's Present the render thread waits up
  to a quarter of the headset frame for both GPU copies and hands this tick's
  pair to the output thread (before: always the previous tick's pair). Only
  with GPU time under half the frame; both eyes or neither. Forced Scene test:
  1198 of 1201 pairs in the same tick, no repeated eyes, tags agree.
- `resolution auto`: `GetRecommendedRenderTargetSize` (per eye, includes the
  SteamVR resolution slider) applied with `setres WxHw` when it differs by more
  than 4%, max 4096 (display_spoof desktop). Quest 3 here: 2528x2776; the
  device reset went through and the game kept running.
- Meta ASW (Quest over Link), first headset test of 0.9.4: 36 fps locked, GPU
  6 ms of 13.9, compositor wait 17-20 ms per frame; `OculusDebugToolCLI -f`
  with `server: asw.Off` -> 70-72 fps at once. `linkasw off` (default) does
  this when the first Scene session starts on an `oculus` tracking system,
  and a hidden PowerShell (`Wait-Process` on the game) sets `asw.Auto` back
  when the game exits.
- SteamVR Motion Smoothing: per-application override
  `motionSmoothingOverride = 2` in the game's app section
  (`GetApplicationKeyByProcessId`, normally `steam.app.50300`) through
  `IVRSettings`. Values from the SteamVR dashboard enum: 0 use global,
  1 force on, 2 force off, 3 force always-on. Virtual Desktop's SSW has no API: detected from the
  tracking system / model name and logged as a hint. Headset model and
  tracking system are logged once (`VR: headset ...`).
- Squad orders: `YCheapShooter` actors (simplified long-range shooters, e.g.
  the billboard sniper of chapter 1, `m_health` +0x264) are not in the Pawn
  list; they are found in GObjects (every 2 s while the squad button is held)
  and accepted within 4 degrees without the visibility trace. The chosen
  target is kept while within 1.5x the tolerance and only replaced by one at
  least 40% better centred (switching every frame between two enemies made
  the game reject the selection).
- Not done, with reasons: texture pool size (in the encrypted engine ini, no
  sign of streaming problems); LOD biases (GPU at ~4 ms of 13.9); rendering
  both eyes as two views of one scene (UE3 splitscreen style): CPU tick is
  3-4 ms for both eyes at 250+ fps, so the gain does not justify the risk, and
  it would lose the per-eye view states.
- `HITCHES` (every 300 stereo ticks): ticks over 20 ms and the longest one;
  gaps over 1 s (cinema screen, loading) are no longer counted as ticks.

## Camera and head tracking

- The game copies the camera point of view with `memcpy`; the mod replaces
  the IAT entry (`0x01109438`) and edits rotation/location on the copy into
  the camera cache.
- **Aim vs view**: with hand aiming the camera used by the game logic points
  where the gun points, while `APlayerController::GetPlayerViewPoint`
  (`0x00AB83F0`), called by `CalcSceneView`, rebuilds the view as body + head.
- **6DOF**: head position relative to the recenter reference (standing
  height = highest head position seen), rotated into the body frame.
- **Head collision**: `UWorld::SingleLineCheck` (`0x009BDE30`) on `GWorld`
  (`[0x014DF734]`). The trace starts at the Pawn's center at eye height,
  not at the eye point (which is `fp` forward units ahead): with Walker
  pressed against a wall in scripted scenes the eye point is inside the wall,
  and a trace starting there kept the view stuck inside it (0.9.5).

## Engine objects (UE3)

- `GObjects` = `0x014D41EC` (TArray of `UObject*`), `GNames` = `0x014C1EC8`
  (entry text at +0x10).
- `UObject`: +0x04 index, +0x28 outer, +0x2C name, +0x34 class.
  `UField::Next` +0x40; `UStruct`: super +0x3C, children +0x4C, script
  bytecode TArray +0x54. `UProperty`: array dim +0x44, flags +0x4C
  (`CPF_Parm` 0x80, `CPF_OutParm` 0x100, `CPF_ReturnParm` 0x400), offset
  +0x64. `UFunction`: flags +0x94, parms size +0xA6, native pointer +0xB0.
- `UObject::ProcessEvent` is vtable slot 66 (`0x005AAF20`).
- `tools/ue3.py` reads all of this from outside the process
  (`props <Class>` prints property offsets).

Useful offsets (`ue3.py props`):

| Object | Field | Offset |
|---|---|---|
| Actor | Location / Rotation / DrawScale / WorldInfo | +0x54 / +0x60 / +0x6C / +0xEC |
| Pawn | Mesh | +0x3A8 |
| YGamePawn | current weapon / arsenal | +0x1594 / +0x1590 |
| YGamePawn | camera invisibility cylinders | +0x1C08 |
| YWeapon | weapon mesh component | +0x2E0 |
| PrimitiveComponent | LocalToWorld / Translation / Rotation | +0x70 / +0x1C0 / +0x1CC |
| PlayerController | bCinematicMode (bit 0x8000) / CheatManager | +0x39C / +0x47C |

## First-person body and arms

- Walker's AnimTree already has hand IK controls: `IK_RightHand` /
  `IK_LeftHand` (`SkelControlLimb`, two-bone IK) and `IKRot_RightHand` /
  `IKRot_LeftHand` (`SkelControlSingleBone`). Every frame the mod writes the
  controller poses into them in actor space and forces strength 1.
- The gun is attached to bone `Weapon_Right`, a child of `Hand_Right`. The
  hand is oriented so that the weapon bone matches the controller frame; the
  hand/weapon relation is measured from the skeleton (`GetBoneQuaternion`).
- The orientation is measured against the actual weapon mesh in the hand
  (re-measured on every weapon change and every ~30 frames while the mesh is
  in the hand): some weapons (RPG) are attached with their own rotation.
- `IKBone_RightHand` / `IKBone_LeftHand` (IK on the helper bones, used by the
  game e.g. for the RPG on the shoulder) are evaluated after our controls and
  pull the hands back to the animation: they are kept at strength 0 while
  the arms follow the controllers.
- Walker's arm is ~24 units (shoulder to wrist): the limb IK may stretch up
  to 25% (`bAllowStretching`, `StretchLimits` = 1.0..`armstretch`) so the
  hands reach the controllers with the arms extended.
- `YGamePawn` fades Walker when the camera is inside his "camera invisibility
  cylinders"; in first person they are set to zero, the `head` bone is hidden
  (`HideBoneByName`) and the mesh is moved back (`bodyback`, plus up to 10
  more units when looking down past 30 degrees so the inside of the collar is
  not visible).
- Directly loading a mission (`open TheLine_P?mission=SP_Gorge01_P`) leaves
  the Gorge01 intro stuck on a transition movie (`YSeqAct_FullscreenMoviePlayer`
  `03a_transition`) also **without** the mod: an artefact of the direct load,
  not a mod bug.
- Every cached engine pointer is validated (still in `GObjects` at its index,
  same class) before being written: objects die on level changes.

## Shooting

- `YWeapon::CalcWeaponFire(FVector Start, FVector End, FImpactInfo*)`
  (`0x004A3D50`, called by `execCalcWeaponFire` `0x00C27130`) receives every
  instant-hit trace. For the player's weapon the mod replaces the trace with
  the ray of the visible barrel (weapon mesh `LocalToWorld` X axis, from the
  point of that axis closest to the game's own start), the same ray that
  places the green marker. The game's start point is 25-30 units off the
  barrel axis and its direction follows the body: without this, shots went
  to the left of the marker and sometimes stopped on obstacles the barrel
  does not "see".
- Spread: `YArsenalManager.m_accuracy` (+0x81C, 0 = perfect, rises while
  firing) is applied inside `CalcWeaponFire`. Kept by default (`spread on`);
  `spread off` zeroes it for the duration of the player's shot.
- Projectiles (`YWeapon_Launcher`: RPG, Milkor) do not use `CalcWeaponFire`:
  the game launches them along the body direction. While the trigger is held
  (or the magazine drops) the mod looks for new projectiles of the player
  (`Instigator` = Walker, or `Owner` = Walker / the weapon: the RPG rocket is
  the warhead loaded in the tube, owned by the weapon) and rotates their
  velocity from the body frame to the barrel frame, so rockets fly along the
  barrel and Milkor grenades keep their lob relative to the barrel.
- Squad orders: the game selects the order target natively among tactical
  markables nearest to the screen centre
  (`m_currentSelectedTacticalMarkableBestScreenDist`), i.e. where the head
  looks. While the squad button (RB) is held the mod picks the live,
  visible enemy nearest to the eye-to-marker ray and passes it to
  `YPlayerController.SetAsCurrentSelectedMarkable` (screen distance 0), before
  and after the frame is drawn.
- Cover: body yaw changes made by the game (not by the stick or the mod, more
  than 7 degrees in a frame) are moved into the yaw reference, so the view
  does not spin (`coversnap`). Since 0.9.4 only within 1.5 s of cover or a
  special move (`YGamePawn.m_specialMove` +0x491: 19 = entering cover, the
  turn happens there while `m_coverFaceState` is still 0; 20 = leaving), with
  a 0.4 s window for a turn that comes just before the special move. Measured
  on 18/10: entering cover turns only the Pawn (+0x64), the PlayerController
  yaw does not change; every compensation in the logs until then was a
  checkpoint jump, a mode switch or a scripted turn, which left Walker's
  body turned away from the view (stick forward walked into walls).
  In cover, raising the head 12 cm or leaning 18 cm from where it was when
  entering cover holds LT: Walker pops out to aim (`coverpeek`).
- Turrets (`YWeapon_Stationary`): the PlayerController is steered towards the
  right controller (35% of the error per frame, yaw and pitch); the part of
  the yaw actually performed is moved into the yaw reference and body pitch
  is left out of the view, so the view does not follow the turret
  (`turretaim hand`).
- Aim down the sights: from tracked poses (head offset vs. the right
  controller ray, not the drawn gun, whose pose changes with the game's aim
  mode and made the state flicker): eye within 8 units of the ray and 4-45
  units behind the controller hides the marker and holds LT (`ads`).
- Two-handed aim blends in and out over 100 ms; while the left hand holds the
  barrel its grip does not throw a grenade.
- Haptics: the game's motor speed is remembered and pulsed every frame on the
  gun hand (and the left hand when two-handed); gestures confirm with 70 ms.
- Controllers are predicted `handpredict` ms (25) ahead; head roll and hand
  roll use atan2(right.y, up.y).
- Scenes with bCinematicMode but the camera still on Walker stay in 3D unless
  `YPlayerController.IsPlayingFullscreenMovie` (`cinemawalker`).
- Turning: past `turnwithhead` degrees of head yaw the excess moves to the
  PlayerController rotation and to the yaw reference (view unchanged).
- Aim assist (`YWeapon` friction / snap-on flags at +0x260,
  `YPlayerController.m_aimSnapOnActiveByUser` +0x734) is disabled while
  aiming by hand.
- Verification (`firelog on` + `tools/weapontest.py` + `tools/shotreport.py`):
  the impact point of every shot is compared with the marker as an angle at
  the muzzle. Measured with spread off, median error 0.03-0.3 degrees, max
  under 0.15 degrees for: AA12, AK47, Barrett M99, Benelli M1014, Desert
  Eagle, FAMAS, HK417, M4, M9, Micro Uzi, P90, Python, SAW (Adams), SCAR,
  Sawed-off shotgun, Steyr Elite (+ Lugo's), TAR21, UMP45. Launchers: RPG
  rocket and Milkor grenades (also the truck Milkor) leave along the barrel
  within 0.1 degrees horizontally (the Milkor keeps its vertical lob; the
  game's small sideways convergence towards the body crosshair is removed).
  The RPG only fires while aiming (LT held).
- Not obtainable by the player (no weapon mesh in any campaign level, or
  `m_weaponMeshComp` empty in the class defaults so `GiveWeapon` returns
  None): AK74, AKS74u, FNP45, Glock18, MP7, MSG90, M110, Mossberg 590A1,
  OTs-14 Groza, SIG P220, TDI Vector and the `_Silencer`, `_Scope`,
  `_HPScope`, `_LaserSight`, `_Commando` variants. Cut content.
- With the barrel inside a cover the trace starts inside it and the bullets
  hit the cover at the muzzle (`SHOT ... on StaticMeshActor`, impact at the
  start): the marker shows the same point.

## Haptics

- Controller rumble: the game's motor speed (XInputSetState) is remembered and
  pulsed every frame (legacy pulses are ~4 ms) on the gun hand, and on the
  left hand while two-handed; gestures confirm with 70 ms.
- ProTubeVR: `ForceTubeVR_API_x32.dll` next to the game, C exports
  `InitRifle`, `KickChannel(uint8 power, channel)`, `RumbleChannel(uint8,
  float seconds, channel)` (channel 0 = all). Kick on every player shot
  (`CalcWeaponFire` hook, launcher projectiles), strength by weapon class,
  one kick per shotgun blast.
- bHaptics: WebSocket `ws://127.0.0.1:15881/v2/feedbacks` of the bHaptics
  Player, `{"Submit":[{"Type":"frame","Key":...,"Frame":{"position":
  "ForearmR"|"ForearmL"|"Vest","dotPoints":[{"index":i,"intensity":0-100}],
  "durationMillis":ms}}]}`. Shots on the forearms, damage (Walker's health
  drop) on the whole vest, gestures on the left forearm. Reconnects every
  10 s.

## Gunstock calibration

B + Y held for 1 s (buttons not passed to the game while both are held):
the stock is aimed at a distant target looked at in the middle of the view, so
the head direction is the real aiming direction; `aimpitch` = head pitch
minus controller pitch, applied at once and written to `specopsvr.ini`
(results beyond +-45 degrees are rejected). 0.9.3 used the eye-to-controller
line, wrong on a stock where the hand sits ~25 cm below the eye (-67 degrees).

## Diagnostics added in 0.9.3

- `STATS` every 300 ticks: presents per eye, repeated eyes, frame tag
  agreement, cinema switches, largest per-tick view jump, main-view FOV.
- `VISCHECK`: enemies within 2500 units and 35 degrees of the view that are
  not drawn for more than 0.3 s (investigation of enemies disappearing when
  leaning out of cover).
- `camlog N`, `bodysize`, `SHOT ... on <actor>`, `GRENADE`, `COVER`, `ADS`.
- `tools/regress.py`: release gate (eye pairing, view stability while the gun
  sweeps, checkpoint/cutscene transition, shot accuracy, stalls).

## Input

- Controllers are returned to the game as an Xbox pad from hooked
  `XInputGetState` / `XInputSetState` (IAT `0x0110974C` / `0x01109748`). The
  game's actions come from `YGamepadInputScheme`.
- Since 0.9.4 they are read through SteamVR Input (`vrinput actions`). The
  legacy API maps every controller onto the old Vive layout: on Index
  controllers `A` arrives as grip and there is no `X`/`A` at all
  (`legacy_bindings_index_controller.json`), so cover and reload were missing.
  The dll writes `specopsvr_inputctions.json` and default bindings for
  `oculus_touch`, `knuckles`, `vive_controller` and `holographic_controller`
  next to the game (so they also arrive with the auto-updater), calls
  `SetActionManifestPath` once per OpenVR session (Scene only), and turns the
  actions back into the Touch legacy state the rest of the code reads (axis 0
  stick, axis 1 trigger, Grip, A = primary, ApplicationMenu = secondary,
  Touchpad = stick click, System = left menu). Haptics go through
  `TriggerHapticVibrationAction`. SteamVR registers the app as
  `steam.app.50300`, so players can rebind it in the SteamVR binding UI.
  `vrinput legacy` restores the old path.
- Controller pose: aiming and the gun are tuned on the Touch raw pose. Other
  controllers' raw poses differ (Vive: ~38 degrees), so the pose is converted
  with the `tip` component of the SteamVR render models:
  `raw * tip(controller) * inverse(tip(Touch))` (`controllerpose auto`).
  Touch controllers (`oculus_*` models) are left untouched; the correction
  is logged once per controller (`INPUT: right controller knuckles ...`).
- Gestures are detected from controller and headset poses and velocities and
  injected as short button presses.

## Developer commands

Write commands to `specopsvr_cmd.txt` next to the game (or use
`tools/gamectl.py cmd ...`):

- `ucall <object> <Function> [args]` - call an UnrealScript function
  (`pc`, `pawn`, `cheat`, `world`, `0xADDRESS`, `Class[#k]`).
- `cheats` - creates the CheatManager (`YGameInfo.AllowCheats` returns false in
  the shipping build: its bytecode is patched for a moment), then
  `ucall cheat God`, `ucall cheat AllAmmo`, ...
- `ce <event>` - fires a Kismet console event through
  `PlayerController.ServerCauseEvent` (`YPlayerController.CauseEvent` is empty
  in the shipping build). Levels contain the developers' checkpoint events
  (`cp01`, `CP03`, ...).
- `console <command>` - `PlayerController.ConsoleCommand`; missions load with
  `open TheLine_P?mission=SP_Gorge01_P?game=SRAssets.YGameInfo_SP_Content`.
- `armdebug`, `armbones`, `firelog on`, `inputlog N`, `gesturelog N`,
  `dumpcam`, `shot <name>` - diagnostics in the log.

## Test tools (`tools/`)

- `gamectl.py` - launch/close the game, send commands, screenshots.
- `levelsweep.py` - loads every campaign level and checks the VR state.
- `deepsweep.py` - jumps through every checkpoint of every level.
- `loadouttest.py` / `weapontest.py` - fire the weapons and measure the shot
  direction against the barrel.
- `ue3.py` - engine object browser.
- `hmdwindow.py` - captures the SteamVR null driver window for headset-free
  tests.
- `package.py` - builds the release zip.

Loading levels writes checkpoints over the Steam Cloud saves: the test tools
back them up and restore them.
