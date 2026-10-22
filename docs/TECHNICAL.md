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
| `haptics_ext.cpp` | ProTubeVR (64-bit bridge or ForceTubeVR_API_x32.dll) and bHaptics Player haptics |
| `bridge/protube_bridge.cpp` | 64-bit ProTubeVR bridge (embedded in d3d9.dll) |

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

- Eye tangents (0.9.6): taken from `GetProjectionMatrix` (OpenGL style:
  NDC y = +1 is up) instead of reading `GetProjectionRaw` by name. In
  OpenVR's raw values `bottom` is the upper edge (Quest 3: up 0.966, down
  1.428); up to 0.9.5 the mod used them the other way round and drew every
  eye vertically flipped in asymmetry (55 degrees up instead of down), a
  warp that changed with head pitch ("fisheye", reported with Virtual
  Desktop; it faded when the lower FOV was cut in SteamVR). A `VR-FOV` line
  is logged when raw and matrix tangents disagree.
- Settings migration (0.9.6): the auto-updater and `install.bat` keep the
  user's `specopsvr.ini`, so new defaults never arrived. `configversion` in
  the ini: below 2 the mod rewrites only lines still at the old defaults
  (`submitpose on` -> `off`, `predict off` -> `auto`), appends
  `configversion 2` and logs `CONFIG: specopsvr.ini updated`. A UTF-8 BOM
  on the first line is ignored.
- Head prediction and pose submit (0.9.6, tested in the headset with Quest 3
  over Link): frames are submitted without a render pose (`submitpose off`;
  with it the compositor's correction showed black bands at the edges and a
  warped image when moving the head) and the view is drawn with the head
  pose predicted to photon time. `predict auto` measures that delay per
  frame: from the head sample to `Submit`, plus next vsync and
  `Prop_SecondsFromVsyncToPhotons_Float` from the runtime (moving average;
  ~53 ms on Link). Controllers are sampled without head prediction
  (predicted with the head the gun looked doubled).
- Frame-tag queue (0.9.6): more than 4 tags at a Present means Draws without
  Present (loading); an even surplus never broke the eye check, so after a
  console `open` every frame went out with the pose of ~7 ticks earlier
  (98 ms). Older pairs are dropped (`stale tags` in STATS).
- Aim marker and squad-target box (0.9.6): computed during each eye's Draw
  and copied into that eye's frame tag (same tick only); Present draws the
  values of the tag it pops. Before, Present read the latest globals, which
  the game thread could already have overwritten for the next tick: one eye
  showed the marker of another frame ("marker doubles now and then").
  STATS: `marker from tag N, from globals M` (M should be 0 in gameplay).
- Off-center projection: `FPerspectiveMatrix(HalfFOV, W, H, MinZ)`
  (`0x009EAE80`, thiscall) is hooked; when it returns to
  `ULocalPlayer::CalcSceneView` (`0x0075CAC5` / `0x0075CBBA`) during an eye's
  Draw, M[0][0], M[1][1], M[2][0], M[2][1] are replaced with the eye's exact
  frustum from `GetProjectionRaw` (Quest 3: m00 0.903, m11 0.835, m20
  +-0.243, m21 +0.193; -0.193 up to 0.9.5, see eye tangents above). The whole back buffer is the eye: no crop, full
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
- **Wall in front of the body** (0.9.6): a second trace from the Pawn's
  center along the `fp` forward offset. If a wall is closer than
  `fp + 12` units, the view (with arms and gun) moves back so the wall stays
  at least 12 units away, up to 10 units behind the center when there is
  room. The body mesh is not moved with it (moved 20 units back, the cover
  pose looked broken in the headset). In cover against a tall wall (chapter 1
  airplane hostage standoff) Walker faces the wall at ~17 units: before, the
  eyes stopped 8 units from it and the whole view was wall. Moves back at
  once, returns in ~0.2 s; `CAMLOG` prints `wall pull`.

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
  not visible). Since 0.9.6 "back" means away from where the head looks, not
  behind the body: looking over a shoulder (head turned 60+ degrees, full
  extra past 120) the body slides away from the view, before the open collar
  of the hidden head was in front of the eyes ("decapitated body").
- **Bad compose ticks** (0.9.6): while walking, in isolated ticks the game's
  AnimTree composed Walker's skeleton with the right hand ~32 units from our
  limb IK target, i.e. in the animation pose, and the gun attached to it
  jumped to the hip for one frame ("doubled gun when walking"). Hook on
  `USkeletalMeshComponent::UpdateSkelPose` (`0x006455E0`, only caller in
  gameplay `0x00D36121`): verified identical in good and bad ticks were the
  limb inputs (effector, spaces, axes, joint target, flags, stretch), control
  strengths, `bIgnoreControllers`, `LastRenderTime`, LOD, AnimTree, mesh and
  actor transforms; zeroing every other control did not change it. After each
  update the mod compares `Hand_Right` with the effector (good ticks 0-6
  units): if it is over 8 (`skelfix on|off <units>`), `SpaceBases` and
  `LocalAtoms` (126 `FBoneAtom` of 32 bytes at +0x274 / +0x280) are restored
  from the last good update, so the skeleton holds for that tick and the gun
  attached afterwards stays in the hand. Bad runs last at most ~12 ms; a hand
  far from its target for 150 ms is real (arm stretched past its reach) and
  those poses are kept. Measured walking: 0 gun jumps in 1800 frames (150-200
  before), 263 updates restored. `gunlog N` / `skellog N` print the data.
  Since 0.9.7 the left hand is checked too while the mod drives it (with the
  grip below, about one update in five put it 10-15 units off the handguard
  for one frame); each hand has its own 150 ms "really out of reach" timer.
- **Left hand on the gun** (0.9.7, `leftgrip on <cm>`): in the animation the
  helper bones `IK_Hand_R` / `IK_Hand_L` sit where the game puts the hands on
  the weapon (with `arms off` they coincide with `Hand_Right` / `Hand_Left`),
  and the mod moves the `Hand_*` bones, not the `IK_*` ones. `IK_Hand_L` in
  the frame of `IK_Hand_R` (from `SpaceBases`, previous update) is therefore
  the support grip relative to the right hand, for the weapon in hand:
  (16.2, -3.1, -5.6) for M4/AK/AA12/Barrett/Milkor, 17.8 Benelli, 21.8
  sawed-off, 9.3 P90, 6.4 RPG; ~30 for pistols and the Micro Uzi, which are
  one-handed (no grip past 26). When the left controller is within `leftgrip`
  cm (default 20, release at 1.4x) of that point, the left hand blends in
  120 ms to the grip pose composed with the wanted right-hand pose: fingers
  stay closed on the handguard whatever the controller's rotation
  (gunstocks). Before, the left hand stayed open where the controller was,
  or (`leftgrip`, off by default) went back to the game animation, which does
  not follow the gun moved by the mod. If the grip is out of the left arm's
  reach (natural arm length from `LocalAtoms`, minimum seen on both arms
  because stretching writes there, times `armstretch`), it slides along the
  gun towards the right hand, up to 14 units (fast up, 0.8 s down: walking
  sways the shoulders).
- **Body under the head** (0.9.7): the camera is `fp` 15 units in front of
  Walker's centre and ~10 above his eyes; with `bodyback 6` his shoulders
  were ~18 units behind the eyes and a hand 45 cm in front of the head was
  10 units out of reach (the left one 20+). Default `bodyback -10 10`: the
  body moves forward along its own axis, the extra (looking down, head turned)
  away from the gaze. Checked looking down to -85 degrees and turning the head
  80-130 degrees: chest, legs and a shoulder, no open collar. Settings
  version 3 migrates `bodyback 6 10`.
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
- Spread (21/10, Ghidra): the cone is applied by the caller `FUN_0093f510`
  **before** `CalcWeaponFire`, by `FUN_00f60300` rotating `End - Start`; the
  angle is `lerp(min, max, YArsenalManager.m_accuracy +0x81C)` per weapon
  (another pair of values while aiming). The nominal direction is the
  PlayerController rotation (measured over 15 shots: 0.02 degrees mean
  deviation; `YGamePawn.m_aimAtPosition` +0x140C is the same ray's end). The
  hook measures the deviation between the nominal ray (`m_aimAtPosition -
  Start`) and the received ray, and applies it around the barrel axis, scaled
  by `spread` (0..2, on = 1, off = 0) and by stance (two hands 0.6, ADS 0.5,
  one-handed long gun 1.5). Pass-through: the caller loops with the same End
  and a Start advanced past penetrable actors; a call with the same End within
  100 ms keeps the advanced Start and the corrected End (restarting from the
  muzzle made it hit the same actor and stop). Zeroing `m_accuracy` inside the
  hook (0.9.x) was a no-op: too late and only the minimum of the lerp.
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
- Controllers are predicted `handpredict` ms ahead (default 0, `auto` = like the head); head roll and hand
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
- ProTubeVR: the only public 32-bit `ForceTubeVR_API_x32.dll` (2020) predates
  the ProVolver, so the mod normally uses a 64-bit bridge (`bridge/`,
  `specopsvr_protube.exe`). It is built as an x64 ExternalProject, embedded in
  `d3d9.dll` as an RCDATA resource (so the single-file auto-update carries it)
  and extracted next to the game when missing or different. The bridge loads
  `ForceTubeVR_API_x64.dll` from the ProTubeVR Companion App (found via the
  Steam library folders; a copy next to the exe wins), calls `InitPistol`
  (first device = channel pistol1 = gun hand, second = pistol2 = support hand)
  and reads 8-byte messages `{op, power, power2, channel, float seconds}`
  (op 1 kick, 2 rumble, 3 shot) from `\\.\pipe\SpecOpsVR_ProTube_<pid>`. It
  exits with the game process. A `ForceTubeVR_API_x32.dll` placed next to the
  game is still used directly when present. C exports: `KickChannel(uint8
  power, channel)`, `RumbleChannel(uint8, float seconds, channel)` (channel
  0 = all). `protubetest` fires a test kick. Kick on every player shot
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
- Physical reload (`reload physical`, 0.9.6): a three-state machine
  (magazine in / out / held) on the left hand. The magwell is a point
  `magwell` cm forward along the gun axis and below the right controller,
  using the same `aimpitch` rotation as aiming. Grip within 12 cm of it and
  pull past 18 cm = out; grip pressed with the hand 35+ cm below the head and within
  45 cm of it horizontally (or the grip kept held from the eject down to
  55+ cm below the head) = new magazine held; held within 7 cm of the
  magwell = X (the game's reload). Releasing the grip drops the held
  magazine; 12 s without finishing or the X button resets. While the left
  hand is at the magwell or the grip belongs to the reload, the grip is not
  sent to the game as LB (grenade).

## Review fixes (0.9.10)

Work list from the multi-agent review in `docs/REVIEW_FULLVR.md` (item ids
H/M/L/R-*), each item verified in isolation (A/B tests in the scratchpad,
`tools/regress.py` PASS before every commit).

- **Scenes and camera:** gun pose saved/restored around cinematics (H1),
  special moves locked to the game with the neck following the animation
  where it should (vault, cover, rope); head catch-up and stick turns keep
  the pivot compensation consistent (H3); cinema/VR hysteresis, instant black
  on scene settle, `cinemaalign` re-reference after a cutscene (R-B5).
- **Ballistics (H2):** the game applies its cone *before* `CalcWeaponFire`
  (caller `FUN_0093f510` -> `FUN_00f60300`, accuracy at arsenal+0x81C) around
  the PlayerController rotation; the hook remaps the recovered deviation
  around the barrel, scaled by `spread` and stance (two-hand 0.6, ADS 0.5,
  one-handed long gun 1.5), and keeps the advanced start of pass-through
  shots. Game recoil values are saved per weapon and restored when the arms
  are released (L11).
- **Rendering:** aim marker and target box sized in degrees from the eye
  projection (R-C2); scope mask taken from the frame tag of the eye being
  presented (L4); worker thread holds references on the eye textures (M7);
  VR output retried with back-off after a failure (L22); fence timeouts and
  `Canvas.Project` calls per tick counted in the log (L2, M19: hook on
  `UCanvas::execProject` @0x00A15D90; measured 0.0 calls per tick in
  gameplay with the HUD on, so the canvas remap has nothing to double-shift
  and no projection fix-up is needed); vignette from forced motion only
  (M20).
- **Input and gestures:** melee intent independent from Y, B gesture delayed
  one poll, reload state frozen on pose loss (M14), honest reload only in
  gameplay, D-pad modifier with release suppression, turns blocked in locked
  scenes, neutral pad on XInput errors (M16), turret stick pitch and
  time-based turret gain (L17, L18), grenade direction kept while the grip
  is held and refreshed at release (L21), support-hand `aimpitch` as a
  rotation about the controller's right axis (L20), haptics by physical
  hand (L19), lateral gun offsets mirrored for left-handed play (M17).
- **Arms:** `KeepGoodBones` state reset on acquire/release (L9), IK controls
  retried every second on a mesh where they were missing (L10), squad
  pointing traces only candidates near the ray (L13), object-scan time per
  tick in the `PROFILE` line (L12). Two-hand grip tolerance `twohand on <cm>`
  (12 cm, was 8; a tester's left hand sat 9-10 cm under the barrel and the
  grip never engaged) with a log line saying how far the hand is.
- **Room-scale (L8, and a bigger find):** `UObject::ProcessEvent`
  (0x005AAF20) silently ignores any UFunction whose `iNative` (UFunction
  +0x98) is non-zero, i.e. every `native(N)` function dispatched by opcode:
  `Move`, `MoveSmooth`, `SetLocation`, `SetPhysics`... `CallFunction`
  "succeeded" on them without doing anything, so the room-scale Pawn
  catch-up (R-B4, with `Move` and then `MoveSmooth`) never moved Walker;
  the view only kept the head offset. The Pawn is now moved by calling the
  C++ `AActor::moveSmooth` (0x0088FB70, `__thiscall(actor, const FVector*)`
  -> UBOOL, the function `execMoveSmooth` calls after clearing
  `bJustTeleported` at Actor+0xA4 bit 0x400) directly under SEH.
  `CallFunction` and `ucall` now log "is an opcode native (iNative N)" and
  refuse such functions instead of pretending. L8 itself: the game's POV
  comes from the tick, before the Draw in which the Pawn moves, so after a
  step the view base lagged the Pawn by one frame; the step made in the
  tick is added to the base of that tick. `roomstep <units> [raw]` makes a
  diagnostic step (`raw` = without the correction) and `camlog` shows
  base-pawn per tick.
- **View state (L5):** when the engine recreates `LocalPlayer.ViewState`
  (two owner changes per regress session: menu and level load) the mod's
  right-eye state is destroyed first through vtable slot 0
  (`FSceneViewStateInterface::Destroy`, as `FUN_00c60790` does for the
  engine's own), instead of leaking it with its occlusion queries.
- **Vive wands (L28):** explicit `dpadleft`/`dpadright` actions (right
  trackpad east/west zones = D-pad) and `sticky` dpad zones, so sliding the
  thumb from the centre click no longer presses A/B.
- **Grenade (R-W6):** the throw speed is the hand's mean speed over the
  60 ms sample; the redirected grenade keeps the game's speed scaled by
  `clamp(hand / 4 m/s, 0.4, 1.3)` (`GRENADE:` line shows the factor).
  `gesturesim throw` now moves the hand at 4 m/s (the 3 m/s of the old
  simulation was below the 3.2 m/s threshold raised on 18/10).
- **Gun-hand melee (R-I3):** the gesture sample carries the gun hand's
  velocity; a jab along the barrel or a downward strike over 3.5 m/s with
  the gun below the eyes presses B (`gesturesim jab|strike`).
- **Manual action (R-W3):** `manualaction on` adds a `Mag::Chamber` state
  after the magazine is inserted: the honest-reload gate keeps the trigger
  off until the left hand grips within 15 cm of the receiver (10 cm ahead
  of the gun hand, 3 cm up) and pulls back 8 cm along the barrel. Off by
  default (the game's weapons are all self-loading); `gesturesim reload`
  racks the action when the option is on.
- **Cinema on the GPU path (R-C5):** the virtual screen frame now goes
  through the same shared surface as the eyes (StretchRect into the shared
  surface, `CopySubresourceRegion` into the overlay texture, which is now
  B8G8R8A8 with `VROverlayFlags_IgnoreTextureAlpha`) instead of a ~5.7 MB
  CPU readback per tick; the letterbox rows are read from a staging copy
  every 15 frames (copied one frame, mapped the next). The CPU path remains
  the fallback when shared surfaces are unavailable. *Needs a headset (or
  null-driver) check of menus and Bink movies.*
- **Stereo cutscene screen (R-C6, experimental, `cinema3d on`):** in-engine
  cutscenes (not Bink movies) are drawn twice with the game camera moved
  +-0.4 half-IPD along its own right vector (no head tracking), the two
  frames go through the eye slots with their frame-tag eye and the output
  worker composes them side by side in a double-width overlay texture with
  `VROverlayFlags_SideBySide_Parallel` (`CINEMA: stereo screen` line).
  *Needs a headset comfort check.*
- **Hidden-area mesh (R-C7, experimental, `ham on`):** `IDirect3DDevice9::
  Clear` (vtable 43) is hooked; after a full Z-buffer clear of an eye's
  scene viewport (at least half the back buffer) the OpenVR hidden-area
  mesh of that eye, rasterized once into a 64x64 grid and reduced to 48
  horizontal strips (two rectangles each, one cell of margin), is written
  at depth 0 with a rect `Clear`, so the depth test rejects those pixels
  before shading. Off by default until measured in the headset (`HAM:`
  lines show the mesh size; GPU time in the `PROFILE` line).
- **Covered by existing code:** R-U4 (world-locked squad marker: the orange
  target box is already projected per eye from `g_squadTargetLoc` with the
  eye's own projection, and M19 showed no `Canvas.Project` traffic to fix);
  R-R1 (identity guards: `StillAlive`/`ObjClass` checks on every captured
  pointer, `LivePlayerController()`, presence pruning of the projectile list
  - the generation counter was not needed once every use re-validates);
  R-B1 (eye-pivot helper: `compensatePivot(dUnits, fwd, headRotates)` is
  the single correction used by the stick turn, the head catch-up and the
  cover/turret paths, see H3/M1).
- **Headset notices (R-U1):** `src/vr_toast.cpp` draws short messages with
  GDI into a head-locked SteamVR overlay (`specopsvr.status`, 22 cm below
  the gaze at 1 m, 3.5 s each, queue of 4, identical text once per 10 s,
  recreated on a new SteamVR connection): update installed, recentered,
  standing height saved, gunstock calibrated, VR output paused, ProTube /
  bHaptics disconnected, SteamVR reconnected. `toasts off` disables them.
- **Interaction census and affordance (physical interaction plan, stages
  0-1 / R-T4):** every 2 s the `YTrigger_*Interaction` actors of the level
  (Zipline, FastRope, Turret, Effects, DP, GoldPoint, VP, InteractionHold,
  InteractionSequence; class defaults skipped) are cached with their
  position and a grab point (`GetRopeStart(out vector)` for ziplines,
  `GetEntryPoint(out vector, out rotator)` for turrets, read through
  ProcessEvent; `GetCurrentIconPosition`/`IsEnabledForTacView` throw inside
  the game on trigger instances and are not used). `interactlog on` prints
  them (`INTERACT:` lines). Stage 1: the hands' IK world targets are
  published (`g_handWorld`); a hand within 25 cm of a grab point gets a
  40 ms haptic tick every 300 ms (120 ms under 10 cm) and the entry is
  logged. Stage 2 (`grabuse on`): a grip closing while the hand is in the
  zone presses A (the game's use button) for 150 ms and that grip is not
  a grenade/squad press; `interactsim right|left [ms]` fakes the zone for
  the test. *Needs a headset check at a rope/zipline/turret.*
- **Hip holster (R-W5):** the game has two weapon slots that Y alternates,
  so a second grab zone at the hip (right grip 55-100 cm below the head, to
  the side, not in front, not during a reload) does the same as the
  shoulder grab (`gesturesim holster`). Direct slot selection would need a
  script function that was not found; cycling is what the game offers.
- **Magazine visual (R-W7 step 1):** `weaponbones` lists the bones of the
  weapon mesh in hand (M4/AA12/Desert Eagle/SCAR/M9A1 have `Magazine`,
  `Slide`, `Muzzle`...; the Milkor has none); while the physical reload has
  the magazine out or in hand, `HideBoneByName("Magazine")` hides it on the
  weapon (`MAGAZINE:` lines), restored on insert or weapon change.
- **Cabin box (R-B2):** on a mount (Pawn not on firm ground: helicopter,
  truck, turret) the horizontal head offset is clamped to 25 units
  (`ClampCabinOffset`, both the real and the synthetic head path), so
  leaning cannot put the view through the cabin; on foot there is no limit
  (room-scale). Falling already counts as on foot (M2).
- **Body facing from the hands (R-B3, experimental, `bodyfacing hands`):**
  the yaw that drives the body turn is the circular mean of head (0.5),
  the perpendicular to the left-right controller line nearest the head
  (0.3) and the gun hand (0.2), with a 20 degree dead zone instead of
  `turnwithhead`; default stays `head`. *Needs a headset check.*
- **Recoil data (R-W4, first step):** the game's 24 recoil bytes saved per
  weapon (L11) are logged as six floats (`RECOIL:` line) to derive the
  visual kick per class from them.
- **Controller hints (R-U2):** `HintsTick` queues seven short hints (aim,
  triggers, buttons, grips, hand reload, D-pad/recenter, gunstock) one every
  7 s after 15 s of 3D gameplay, in the first three play sessions
  (`hints auto`, counted in `hintsshown`; `hints on|off`). They use the
  R-U1 overlay, so they also appear in the log as `TOAST:` lines.
- **Standing height (R-B6):** `height <metres>` fixes the standing head
  height (the automatic one keeps the highest head position seen); both
  sticks held 3 s (recenter fires at 1 s) take the current head height as
  the standing one and save it with `SaveIniValue`.
- **Recenter (R-C3):** 150 ms of instant black on recenter, like the
  cutscene settle. **Submit watchdog (R-R3):** a compositor `Submit` over
  100 ms is logged (at most every 5 s). **Special moves (R-T3):**
  `SPECIALMOVE` lines carry the `YE_SpecialMove` names.
- **Robustness:** engine crash filter `FUN_00827770` hooked so a game crash
  writes the mod's CRASH line and minidump (R-R2); script calls that throw
  are disabled by name (L25); `openvr_api.dll` delay-loaded with failure
  stubs and the minimized window restored, so a missing dll leaves the game
  playable (M6, H4); asynchronous logger (queue + writer thread, ERROR/CRASH
  written synchronously, log order verified, R-R5).
- **Updates (R-R4):** boot sentinel - a freshly installed dll leaves
  `specopsvr_update.pending`; `specopsvr_boot.count` grows at every start
  and is cleared on a clean exit or after 90 s of life past the ini; at the
  third start after two early deaths `d3d9.dll.prev` is put back, the dll
  kept as `d3d9.dll.bad` and the tag written to `specopsvr_skip.txt` (never
  reinstalled while it exists). Companion files: `package.py` writes
  `companions.json` (name, size, SHA-256 of `openvr_api.dll`, scripts and
  README) and prints the `gh release create` line with every asset; the
  updater downloads the ones missing or different (never `specopsvr.ini`).
  `reset_settings.bat` restores the shipped ini keeping a dated copy.
- **Found in the headset (21/10 evening, Virtual Desktop):** the output
  worker and the Scene switch were never exercised by the headless gate
  (OpenVR stays Background there), so three regressions surfaced only live:
  the toast tick touched overlays without the session lock while the session
  thread did `VR_Shutdown`/`VR_Init` (Scene never came up; now the tick takes
  a try-shared lock and skips during a switch); the GPU cinema path never
  marked the frame ready (`pk.cinema` missing in `DeliverGpuSlot`: SteamVR
  stayed on "Next up"); the L3 "same slot tick" constraint never matched and
  killed the same-tick fast path (reverted, mismatches counted instead).
  Also: the 12 cm two-hand tolerance engaged with the support hand *under*
  the barrel (no gunstock) and the hand looked glued below the gun - the
  tolerance is now 12 cm sideways and 8 cm vertically, with an 80 ms filter
  on the hand-to-hand line (`twohand on <cm> <s>`); the compositor wait now
  sits after the copy, right before `Submit` (`waitbeforesubmit on`); a
  half-rate toast (submits/s < 0.6 x refresh) and a Virtual Desktop notice
  (driver `oculus_virtualdesktop` from `LastKnown/ActualHMDDriver`) tell
  the player to disable SSW, which no PC-side setting can do.
- **Aiming after the first headset round (21/10):** the stance factors of
  the cone are configurable (`spread on <scale> <ads> <twohand> <onehand>`,
  defaults 1 / 0.1 / 0.5 / 1.5): with the sights at the eye the shot is
  practically a laser, so the weapon's own sights are usable; the green
  marker is **off by default** (configversion 6 migrates `marker on` to
  `off`; players who want it turn it back on) and a new **`laser on`** draws
  a red beam from 25 units ahead of the barrel to the point of impact
  (start point carried per eye in the frame tag like the marker, drawn with
  Clear rects, hidden while aiming down the sights).
- **In-headset settings menu (`src/vr_menu.cpp`, asked 21/10):** a 1.5 m
  SteamVR overlay (`specopsvr.menu`) placed in front of the player, mouse
  input from the controller laser (`VROverlayInputMethod_Mouse`,
  `MakeOverlaysInteractiveIfVisible`, mouse scale = bitmap size, y flipped),
  drawn with GDI into a 1100x820 RGBA bitmap (`SetOverlayRaw`). Tabs:
  **Basic** (17 curated settings with one-line explanations: hand, height,
  turning, vignette, HUD, body, reload, two-hand, marker, laser, spread,
  room-scale, seated, hints, this menu, update channel, auto-update),
  **Advanced** (every key of the player's ini with the comment block above
  it as help; on/off toggles, numbers with -/+ steps, choices cycle) and
  **Controls** (the full control list). Every change runs `Execute` at once
  and `SaveIniValue` into the ini. Opens by itself 2 s after the first
  virtual screen (game main menu) when `vrmenu on`; PLAY closes it; `vrmenu
  show|hide` from commands; while open the game pad is neutral. A Virtual
  Desktop banner (driver detected) tells what to disable (SSW, 72 Hz,
  Ethernet). Ticks on the session thread, same as the Scene switch (no
  overlay calls outside the lock). *Needs the headset for the pointer.*
- **Headset checklist for this batch** (everything above passed the
  headless gate; these need eyes and hands): toasts and the first-session
  hints (`TOAST:` lines); menus and Bink movies on the GPU cinema path
  (`gpucopy`); `cinema3d on` comfort in an in-engine cutscene; `ham on`
  (no visible corners, GPU time in `PROFILE`); `bodyfacing hands`;
  `manualaction on`; gun jab/strike and the hip holster; grenade speed
  factor (`GRENADE:` line); magazine bone during a hand reload; rope /
  zipline / turret: haptic tick near the grab point and `grabuse`; both
  sticks 3 s for the standing height; `submitpose on` (R-C1); left-handed
  play (`hand left`: lateral offsets mirrored, haptics swapped).
- **R-U3 wrist HUD (21/10):** `src/vr_wrist.cpp`, overlay `specopsvr.wrist`
  attached to the support-hand controller (11 cm towards the forearm, facing
  the controller's +Y, 9 cm wide, `wristhud on|off [m]`): magazine / reserve
  (`g_weaponAmmo`), weapon short name, health (`Pawn.Health` +0x304).
  Data published once per tick by `PublishWristData` (only in first person,
  gameplay, Walker alive, no menu); redrawn (GDI) only when a value changes.
  The game's own ammo widget is not hidden (the `DrawTile` hook filtered by
  the calling script function was judged too fragile; `hud` scale remains).
- **R-U5 laser on the game menus (21/10):** `CinemaLaserTick` in the output
  worker: the gun controller's pose (standing space, stored by the HMD tick
  in `g_gunCtrlPose`) gives a ray that `ComputeOverlayIntersection` tests
  against the cinema overlay; the UV is mapped through the shown crop rows
  to the game window's client area and the Windows cursor is moved there
  (the UIScene menus follow the cursor and draw their own); trigger edges
  send left clicks with `SendInput`. No SteamVR input focus on the overlay,
  so the controller buttons keep reaching the game. `cinemalaser flipy on`
  and `cinemalaser uv full` are the two UV conventions to try if the cursor
  lands in the wrong place (`LASER:` lines log the first hits). **Outcome
  (21/10, headset + headless):** the intersection and the mapping work, but
  the game recentres the Windows cursor every frame even on the title screen
  (`SetCursorPos` and relative `SendInput` moves both end at the screen
  centre, `cursor NOT moved` in the log) and its UIScene menus are pad /
  keyboard driven: no highlight follows the mouse. So the laser cannot drive
  the game's menus without a deeper hook (the engine's mouse capture / a
  `GetCursorPos` IAT hook plus synthesized UI navigation), which is left
  out: `cinemalaser off` by default, code kept. The menus are navigated with
  the stick and A/B as before. The in-VR quick menu part of R-U5 is the
  settings menu (`vrmenu`), whose overlay now releases SteamVR input focus
  when it closes (otherwise A did not reach the game until Meta was pressed).
- **Interaction stages 3-4 (21/10):** stage 3 was already covered: B of the
  punch gesture held back one poll (M3), candidates within 60 degrees of the
  gaze with `IsBeingExecutedOrRevived`, body turned only by the needed angle,
  impact haptic on the execution special moves; the rifle-butt jab exists as
  the opt-in gun melee. Stage 4: `commandaim support` targets the squad
  order along the support hand's pointing direction (`g_supportFwd`, from
  the left controller's rotation) while the squad button is held; pick-ups
  go through the interaction scan (`Pickup` classes) and `grabuse`.
- **Not done, with the reason:** R-I2 extra controller profiles (Pico,
  Focus 3, Pimax) need the SteamVR `controller_type` strings from a log of
  those headsets; the downward-strike gesture (A3) is skipped after today's
  false positives with the gun melee (the forward punch already triggers
  executions); QTE / moral-choice gestures (E9/E10) and the per-move policy
  table need per-scene playtests and R-T3 (special-move names), both left
  for a later milestone; hiding the game's ammo widget (part of R-U3) see above.
- **Game menus on the virtual screen (21/10), real cause:** the game saves
  `ScreenPercentage` in its settings, so after a session where the dynamic
  resolution had lowered it the game *started* at 65% while the mod assumed
  100%: the UIScene menu is laid out on the full viewport but the scene is
  drawn in a 65% box, and the crop of the virtual screen shows that box
  ("menu cut on every side"). Reproduced without a headset (`shot` of the
  back buffer: box 65% at boot, 100% after `scale set ScreenPercentage
  100`, 90% after `90`). `UpdateDynamicResolution` now forces 100% once at
  start; the virtual screen keeps 100%. The notes below record what was
  tried while the cause was unclear (the `ui169` clamp made it worse: cut
  on every side; `UpdateAutoResolution` on the virtual screen stays, it is
  harmless). The UIScene menus of
  the game (`UUIScreenObject::GetViewportSize` → the scene's
  `CurrentViewportSize`, fed by `UGameUISceneClient::GetViewportSize` at
  0x00F539E0) lay themselves out by the *height* of the back buffer, so with
  a per-eye buffer (2112x2304) the main menu is 4096 px wide and cut at the
  right; the 16:9 crop of the virtual screen then cuts it top and bottom
  too. Things tried: a 16:9 sub-viewport in `CalcSceneView` (restricts only
  the 3D, the UI stays full-window); a canvas scale in `AdjustCanvasMatrix`
  (the menus are not drawn through it); clamping the height returned by the
  scene client (`ui169`, hook on 0x00F539E0: fires, but the scene keeps its
  layout); hooking `UUIScreenObject::GetViewportSize` at 0x00574390 makes
  the game exit silently 3 s after start (no crash filter, no dump, no
  Windows event) — do not hook that function. What works: a 16:9 back
  buffer (`setres 2112x1188w`): the menu is complete, as on a monitor.
  Done automatically with `menu169 on` (after 1.5 s of virtual screen, back
  to the per-eye size when the screen ends), but the campaign load then
  fail-fasted inside the Steam overlay (0xc0000409, CRT abort in
  gameoverlayrenderer.dll), so it is off by default. Also fixed: the
  first-frame layout (menu in a 65% box) came from the window being resized
  after the game laid out the viewport; `UpdateAutoResolution` now runs on
  the virtual screen too, so the `setres` happens at once. A `dumpcam`-style
  `shot`/`burst` of the back buffer (`specopsvr_shots/*.bmp`) is the way to
  see what the game draws; the mirror window shows the same back buffer.
- **Seated play (21/10):** with `seated on` the vertical reference is the
  head height at the recenter, never the standing maximum nor the fixed
  `height`; the Scene start (headset put on) requests a silent recenter so
  the reference is taken with the headset worn. Before, with `height 1.55`
  and the head seated at 1.2 m, the view sat 35 cm under Walker's eyes and
  the body followed it into the floor ("deformed dwarf").
- **Left-handed body (21-22/10): arm swap.** With `hand left` (no separate
  option: the arm swap follows the dominant hand) the gun side (side 0) is
  Walker's LEFT arm and the
  support side his right: `HandBone/WeaponBone/IkHelperBone/ShoulderBone(side)`
  return the swapped names, the IK control names are taken with `i ^ 1`
  (`IK_LeftHand`, `IKRot_LeftHand` for side 0) and the rig is re-acquired
  when the dominant hand changes. The weapon mesh component, which the game
  attaches to the pawn mesh at bone `Weapon_Right` (`Attachments` TArray at
  USkeletalMeshComponent+0x348, 52-byte `FAttachment` entries: Component,
  BoneName, RelativeLocation/Rotation/Scale + 4), is re-attached by
  rewriting the entry's BoneName to `Hand_Left` (`UpdateGunAttachment`;
  `Weapon_Left` is not rigid with the hand). After the re-attach the gun hand
  rests on the animation for 20 frames and the hand/weapon relation is
  measured once at the last frame, then frozen: measuring it while the hand
  is driven made the relation rotate ~10 degrees per measurement (spiral).
  The support grip (`leftgrip`) is off in this mode (the animation's grip
  is for the other hand). Headless check (`armswaptest.py`, synthetic
  poses): both hands 0.0 units from their targets, pistol drawn in the left
  hand (`shots/swap_left.png`). Still to judge in a headset by a left-handed
  player: finger poses (the left hand uses the animation's support-hand
  fingers), the attach offset on the hand, holster/reload gestures.
  Mirroring the mesh (a `mirrorbody` experiment, code removed) is a dead end: the
  engine's `CalcComponentToFrameMatrix` (0x00583610) and bone quaternions
  do not survive a reflection (IK controls simply stop acting; Ghidra, vtable
  slot 84 = `USkelControlLimb::CalculateNewBoneTransforms` 0x00CE6EA0).
- **Audio on the PC speakers (22/10):** the game's FMOD Ex 4.32 opens the
  Windows default playback device at start (WASAPI, output type 8, on an
  explicit endpoint) and never follows it; Virtual Desktop makes its own
  device the default only when the stream starts, so a game launched before
  that played on the PC ("Realtek Digital Output" in the log at start, no
  audio in the headset). Neither the per-app routing of Windows
  (`AudioPolicyConfig`, tried first) nor a default-device change moves such
  a stream (headless test with `IPolicyConfig::SetDefaultEndpoint`: the
  game's session stayed on the start device). FMOD can reopen its output
  after init (`System::setDriver`, Ghidra on fmodex.dll: stop, close,
  re-enumerate, init on the driver, driver 0 = the current default) but
  refuses the new device when the mix format differs from the one at init
  (result 0x3e: Realtek 7.1 = 8 channels, Virtual Desktop / Oculus = 2).
  `src/audio_diag.cpp` therefore hooks the process-wide `IAudioClient`
  vtable (`GetMixFormat`, `IsFormatSupported`, `Initialize`): while FMOD
  runs `System::init` (hooked export) or `setDriver` the client reports a
  fixed 48 kHz float stereo format and the stream is opened with
  `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | SRC_DEFAULT_QUALITY`, so Windows
  converts to any device and every reopen succeeds. An
  `IMMNotificationClient` (own MTA thread) turns a default-device change
  into a reopen request executed on the game thread inside the FMOD
  listener hook (`EventSystem::getSystemObject` + `setDriver(0)`). Headless
  check: the game's active audio session follows Realtek -> HDMI -> Realtek
  -> Oculus headphones. Diagnostics: `audioroute where` (device with the
  game's active session, from `IAudioSessionManager2`), `audioroute reopen
  [driver]`. The game therefore always mixes in stereo (what the headset
  plays).
- **Two-handed aim (22/10):** with a pistol the hands are 3-6 cm apart, the
  controller-to-controller line is not a barrel (1 cm of shake = 15
  degrees): the grip only poses the support hand and lowers the spread, the
  aim stays with the gun hand. With a rifle the engage offset (gun direction
  minus hand line, remembered so the gun does not jump) decays to zero in
  0.7 s: the grip engages exactly on the 12 cm boundary, so the remembered
  offset was 17-40 degrees and the gun pointed that far from the hand line
  once the hand settled on the handguard ("aim shifted").
- **Executions (22/10):** `YPlayerController.CheckExecute` (script) compares
  the camera pitch with `m_executionMaxCamPitchAngle` (+0xB90, -1000 =
  -5.5 degrees) and below it calls `StartMeleeAttack`: in VR the downed enemy
  is looked at from above (-28 degrees in the log, ten punches, no
  execution). The assist now writes -16384 (-90 degrees) every tick.
- **Squad orders with the support hand (22/10):** the game validates the
  target in the virtual at vtable +0x550 (0x008389D0): the candidate (+0xEA0)
  becomes `m_currentSelectedTacticalMarkable` (+0xE98) only after two async
  visibility traces; a different candidate resets the counters. The game
  calls it every frame with its own centre-of-view candidate and the mod with
  its pick: two different ones never completed ("not taken by the game"),
  which only agreed along the barrel. The function is hooked and, while the
  mod has a pick, calls with another markable are ignored. The "not taken"
  log line now prints the validation fields. Those fields also showed that
  the mod's own call through `ProcessEvent` never ran (candidate +0xEA0
  stayed "none": the function is an opcode native, which this game's
  `ProcessEvent` ignores, so the "selected" results along the barrel were
  the game validating its own identical candidate). The mod now calls the
  implementation directly through the hook's original pointer; `uprops
  func:Class.Function` prints a function's `iNative` to spot such cases.
- **Pistol support grip (22/10):** no two-handed pistol animation to measure
  (IK_Hand_L 30+ units away), so the support hand takes a synthetic pose next
  to the gun hand: `cuprel` units in the gun-hand bone frame (X forward, Y
  down, Z right; default 0.5 2.5 -4.5 = left of and under the grip, chosen on headless screenshots) with the hand
  axes Y and Z negated (palms facing each other, as two controllers held side
  by side); engages within `leftgrip` cm like the rifle grip. Before, the
  left hand went through the pistol.
- **Options retired from the ini and the menu (22/10):** the player asked
  for no switches for things that must simply work (two-handed grip engages
  by proximity) and none for dead experiments; see "Hidden settings" under
  Developer commands. Code defaults now equal the old ini defaults
  (`submitpose off`, `near 3`).
- **Deploy (22/10):** `gamectl.py deploy` is the install command; the copy
  fails on the locked dll while the game runs, so `regress.py` now kills the
  game, deploys and fails if the installed dll differs from the built one
  (two commits had passed the gate with a stale dll).
- **Two-handed grip with pistols (22/10):** the grip needed the support
  hand 12-70 cm ahead of the gun hand along the barrel, which fits a rifle's
  handguard but never a pistol (the support hand wraps the gun hand, the
  controllers sit side by side). For pistols and the Uzi (`WeaponInfo::pistol`,
  `g_weaponShort`) the window is -6..30 cm.
- **Automatic recenters (21/10):** Virtual Desktop sends
  `ChaperoneUniverseHasChanged` / zero-pose-reset events in bursts; they are
  now debounced to one per 10 s and silent (no toast, no blink;
  `g_recenterSilent`): the "Recentered" toast popping up in front of the
  virtual screen every few seconds was the "half-3D nauseating flag".
- **Tests:** `src/pure_utils.h` holds the pure functions of the updater and
  the ini parser (versions, JSON, revocation, companions, line normalization,
  migrations) covered by `tests/unit_tests.cpp` (R-T2); `regress.py` runs
  them first and gained the `gestures` (simulated physical reload) and
  `cinema` (SetCinematicMode on/off) phases (R-T1). `gesturesim` now feeds
  one fake sample every two ticks (~0.7 s per gesture) instead of all 31
  in one tick, so time-dependent states (magazine out, rack) really happen.

## Audit fixes and first full-VR features (0.9.9)

Audit of 20/10 (work list in `docs/ROADMAP_0.9.9.md`) and a tester report
(Quest 3 over Virtual Desktop).

- **Squad orders.** The chapter 1 billboard sniper is a `YCheapShooter`
  (not in the Pawn list; health float at +0x264). The mod selects the target
  along the barrel and calls `YPlayerController.SetAsCurrentSelectedMarkable`
  (PC+0xE98). The target now stays selected until the button is released
  while it is alive (drifting the gun off it let the game's head-centre pick
  take over and no order was issued). When nothing is along the barrel the
  view direction is searched too: the game's red triangle sits at the centre
  of the view, so players aim it with the head. Every press/release is
  logged (`SQUAD: button pressed/released`).
- **Mounted guns aimed by hand** (helicopter minigun): the game's crosshair
  (`YHUD.m_hideCrosshair`, +0x480 bit 0x40) is hidden like with hand-held
  weapons; it stayed at the centre of the view next to the green marker.
- **Input:** B/Y presses of ~105-135 ms passed the combo gate never (melee,
  executions, punch gesture); gestures are OR-ed after the gate. Execution
  assist (`execassist`): on B Walker turns to the nearest downed enemy within
  2 m. Left-handed pause/Back from the physical left controller; dashboard
  state resynced every second; D-pad no longer turns; turrets without
  turn-with-head and with the pitch reset on exit; honest reload
  (`honestreload`: no firing with the magazine out); grenade/melee gestures
  separated and off with the left hand at the gun; cover peek from a standing
  entry.
- **Rendering:** scope circle and vignette centred on each eye's forward
  direction (the circles were ~15 degrees apart), eyes kept apart while scoped
  (eye recognition), no marker on cinema frames, full-screen Canvas effects
  scaled with dynamic resolution, readback surfaces only when needed.
- **Robustness:** window procedure chaining only over the game's own, window
  protections only while the headset is worn, cinema overlay recreated after
  a SteamVR restart, SteamVR death without Quit detected, orderly stop on
  WM_CLOSE, crash handler only for fatal codes, ini written through a temp
  file.
- **Install/update:** withdrawn releases never installed, pre-releases rank
  below the final version, `rollback.bat` prefers `d3d9.dll.old`,
  `uninstall.bat` only removes the mod's dll, `collect_logs.bat` adds SteamVR
  logs and bindings, Unicode-safe game folder.
- **Features:** `movedir hand`, seated play uses the game's crouch view,
  directional bHaptics damage from `Pawn.LastHitBy`.

## Virtual Desktop and haptics fixes (0.9.8)

Found on 20/10 by playing through Virtual Desktop (Streamer 1.34.22, Quest 3).
VD presents the headset to SteamVR as an `oculus` device (HMD shim), so the
mod sees the same tracking system name as over Link.

- **Window procedure never active.** Shortly after CreateDevice the game
  installs its own window procedure (0x00F59DF0) over ours and does not call
  ours: `keepfocus`, the WM_GETMINMAXINFO unclamp and everything else in
  `UnclampedWndProc` stopped after ~10 messages, in every version.
  `KeepSubclass()` (called from EndScene) puts ours back in front, chaining the
  game's procedure (which does not call ours: no loop). `wndinfo` logs the
  current procedure and the message count.
- **Crash in binkw32.dll when the window was minimized** (VD/SteamVR closing
  the dashboard): the GetClientRect spoof skipped iconic windows, UE3 shrank
  the viewport to 0x0 (SetWindowPos 6x40, 0x00BE07DA) and the menu's Bink video
  wrote past its buffer ("Fatal error"). While the session is Scene the spoof
  now also covers iconic windows and a minimize is undone at once
  (WM_SYSCOMMAND SC_MINIMIZE swallowed, SIZE_MINIMIZED -> SW_SHOWNOACTIVATE).
- **Headset state after standby.** With VD the HMD activity level stays
  Idle/Standby after the headset is put back on, so the mod stopped sending
  images and blocked the controls. The HMD pose moving (> 2 mm or ~0.2 degrees
  between two 500 ms samples) now counts as worn whatever the activity level.
- **Start pressed in the main menu.** SteamVR opens its dashboard by itself
  when the game becomes the scene app; the pause-on-dashboard Start is now sent
  only during 3D gameplay (stereo drawn in the last 1.5 s).
- **Pending eye images dropped every frame outside gameplay** (0.9.7
  regression): the "no stereo for 1.5 s -> virtual screen" fallback was decided
  after the back-from-cinema check, so every frame looked like a return from
  the cinema screen. It is now decided before.
- **ProTubeVR bridge** (see Haptics): ProVolver support through the Companion
  App's 64-bit dll.

## VR audit fixes (0.9.7)

On 19/10 five agents audited the mod (rendering, comfort, weapons, input/UX, an
automated playtest). What changed, by area:

**View and comfort**
- The body is the PlayerController rotation (`+0x60/+0x64`), not the game
  camera: camera shakes, CameraAnims and weapon recoil no longer rotate the
  view. Body pitch is 0 unless `stickpitch on`; game roll is dropped.
  Measured: a 3 s M4 burst kept the view at 0 degrees (10 degrees climb
  before). `YWeapon.m_recoilDistortionRange[3]` (+0x7E0) is zeroed on the held
  weapon while the arms are driven; a visual kick (2.5 degrees, x2.2 for heavy
  weapons, 15 ms up, 60 ms decay) moves only the drawn gun.
- Head and controller poses are sampled once per tick on the game thread at
  the start of `FViewport::Draw` (`headsample draw`, default), not 2-3 times per
  tick in `BeginScene` on the render thread. PlayerController writes
  (turn with head, turret) are now on the game thread; `AddControllerYaw` also
  adds the delta to this tick's body yaw.
- The per-eye offset is removed before eye-height smoothing and head
  collision and added back after: both eyes keep their own height under head
  roll, and collision no longer shrinks the IPD. Collision `t` recovers in
  0.1 s (stops at once). Wall pull grows at most 150 units/s.
- Smooth turn is done by the mod (`turnspeed`, default 150 deg/s, quadratic
  after a 0.15 dead zone) and counted as own yaw; stick and snap turns pivot
  around the head (`posRef` moved so the eyes stay put). Near cover every
  non-own yaw change above 0.5 degrees per tick is absorbed (was 7 degrees).
- Left stick moves where you look (`movedir head`) outside cover.
- `crouchview real`: the game's crouch (capsule 45 -> 25, pawn 20 units down)
  is removed from the view and the mesh is raised by the same amount (less
  when looking down past 30 degrees); a physical crouch lowers the mesh.
  `height auto`: eye height = standing HMD height x posscale - 45 (25..55);
  `seated on` uses `fp`.
- Room-scale: past 15 cm the Pawn is moved with `Actor.Move` (collisions) and
  `posRef` shifts by the distance actually moved.
- Standing-height reference: spikes over 10 cm per sample ignored, decays
  1 cm/s within 15 cm of standing. Coverpeek uses only the sideways (body
  right) component. Turn-with-head catch-up limited to 180 deg/s.
- `bodyback -10 26` (body under the head, settings version 3-4); `Neck` is
  hidden with `head` (no neck stump looking down while crouched).
- Compositor fades (`fades`): fade from black on cinema switches and view
  jumps over 80 units; black while the camera leaves Walker before the cinema
  screen; 0.12 s blink on snap turns (`snapfade`). Comfort vignette updated
  once per pair and time-based, also driven by forced motion (vehicles,
  non-own rotations).
- Scripted 3D scenes (`bCinematicMode` with the camera on Walker): arms follow
  the game animation; the view goes down with Walker's head bone (only
  downwards, 0.3 s smoothing).
- Head yaw from poses near +-90 degrees pitch blends in the up vector (stable
  when looking straight down).

**Rendering and output**
- A new left image while a pair is still pending drops the whole old pair
  (no left/right from different ticks). The worker waits for the D3D11 copy
  (event query) before returning shared slots, and keeps `g_workerBusy` until
  after `Submit` (eye textures could be freed during Submit).
- Presents without stereo Draws for 200 ms outside cinema go to the cinema
  screen (no flat image in both eyes during blocking loads).
- HUD: per-draw tag (`PeekRenderingTag`) for off-center and screen percent;
  the HUD is scaled by the dynamic-resolution percentage. Full-screen Canvas
  draws (over 85% of the back buffer) use the game's matrix. Default
  `hud 0.5 100` (settings version 5).
- Cinema: crop held for 30 frames before changing, readback capped at 1600 px
  wide, screen re-placed on recenter, dark grey background.
- Scope: the zoomed view looks along the barrel (`g_muzzleAimYaw/Pitch`), is
  mono and is masked to a circle (0.42 of the half height).
- OpenVR events: dashboard / input focus -> one Start press and no input or
  gestures; universe or zero-pose change -> recenter; IPD / HMD property change
  -> projection recomputed; Quit -> `AcknowledgeQuit_Exiting`, session closed
  and retried every 5 s (also when SteamVR is missing at start).
- VS constant diagnostics only with `vsdiag on`; log flushed at most every
  250 ms (errors at once).

**Weapons and hands**
- Left hand grip: relation fixed per weapon (accepted after 0.3 s stable,
  then only small changes), blend out from the last relation, slerp. Without a
  left controller the left hand holds the gun (or goes back to the game
  animation with no gun). `KeepGoodBones` compares each hand with the
  reachable point (target clamped to shoulder + arm reach), so reaching past
  the arm no longer freezes the skeleton for 150 ms.
- Shots and marker start at the muzzle bone (`YWeapon.m_muzzleBoneName`
  +0x2E8 on the weapon mesh); if the shoulder -> muzzle line is blocked the
  shot starts at the obstacle and the drawn gun moves back along the barrel
  (up to 20 units).
- Hand/weapon relation: a very different measurement must be confirmed by the
  next one (no 90-degree wrist twist during weapon switches).
- Two-handed aim keeps the offset between the right-hand direction and the
  hands line captured when the grip engages (no jump, gunstock calibration
  preserved). `aimpitch` is a rotation about the controller's right axis.
- `marker off` only hides the marker. Turret marker uses body yaw only (body
  pitch is 0). Milkor: no straight-line marker. RPG: LT held with the trigger.
- Physical reload sends X at the eject; magwell offset per class (pistols in
  the grip, P90 on top, shotguns further forward). Traces include pawns.

**Input, UX, robustness**
- The left grip's meaning (grenade or gun/reload) is decided at the press.
  Gestures are off in cinema, locked scenes, pause and with the dashboard;
  duck-to-cover only out of cover with the stick still; the weapon-swap
  gesture needs the hand behind the face plane and to the side.
- B+Y and the two stick clicks are held back 120 ms (short taps pass as
  100 ms pulses), so combos no longer leak. Left menu held 0.6 s = Back.
- Action state read with QPC. Bindings for Vive (trackpad dpad), Cosmos, HP
  Reverb G2, Index grip as `grab`; legacy fallback after 5 s with no active
  action (headset worn, no dashboard). Neutral connected pad when the VR
  session is up but the controllers give no data. `hand left` swaps the
  controllers.
- Settings: values lower-cased, decimal commas, true/false/yes/no, trailing
  comments; each migration step applies only to inis older than its version;
  the shipped ini is embedded (`default_ini.h`) and its values are applied for
  keys missing from the user's ini. Updater checks the release asset size and
  SHA-256 (`digest`); rollback also restores `d3d9.dll.old`; install keeps a
  foreign `d3d9.dll` as `d3d9.dll.other`; `package.py <tag>` checks
  `version.h`. Threads wait for the ini instead of 15 s. Window deactivation
  is ignored while the headset is in use (`keepfocus`). Crash filter chained
  to the game's, vectored handler for always-fatal codes. bHaptics socket
  read. Squad scan back-off fixed.

**Found while testing 0.9.7 in the headset (19/10)**
- Squad orders on the billboard sniper were refused: the game checks the
  target's visibility from its camera, which `height auto` had lowered. The
  game camera keeps `fp`; only the view adds the height difference.
- Crash at death: the new body/room-scale code moved the Pawn, mesh and bones
  of the ragdoll. Everything that moves Walker stops when `Health <= 0`.
- Doubled world at checkpoint saves: Presents without stereo Draws go to the
  cinema screen only after 1.5 s (short stalls skip the frame).
- Doubled world after videos and scripted scenes: the eye recognition from the
  view-origin register (c5) had been switched off with the `vsdiag`
  diagnostics; it re-aligns the frame tags (7 re-alignments measured after the
  prologue's cinema switches). Leftover mono tags are skipped and the queue is
  cleared on cinema switches. Both eyes of a tick use the same head base and
  body rotation.
- Helicopter: on a mount (turret, driven vehicle, or a Base actor that moved
  in the last second; the old "Base == WorldInfo" test failed on most
  StaticMeshActor and Terrain floors) the view is rigid to the Pawn at the
  filtered offset of Walker's head (Spine3 -> Neck + 12 units, ~1 s filter;
  bone positions lag the moving helicopter), no `fp` forward offset, no body
  following, no room-scale, no eye-height smoothing, head at most 6 units
  above Walker's (the cabin does not block traces). Fades only on real jumps
  outside scripted scenes; black hold only at death.

Not changed (need the headset or more work): `submitpose` A/B now that the
projection is fixed; the root cause of the bad compose ticks (still ~5-9% of
updates restored in combat); world-anchored Canvas icons follow the HUD
scaling; elbow pole target fixed in actor space; marker drawn over the gun.

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
- `uprops <object> [<part of name>]` - properties of the object's class
  (and superclasses) with type, offset and current value, by reflection
  (`uprops pc markable`, `uprops pawn execut`): the fastest way to name an
  offset seen in Ghidra or to read a game threshold.
- Hidden settings (not in the ini nor in the menu, 22/10: things that must
  simply work or experiments): `twohand on|off [cm] [s]`, `leftgrip on|off [cm]`,
  `cuprel x y z` (pistol support-hand pose), `coversnap`, `cinemaalign`,
  `keepfocus`, `execassist`, `muzzleaim`, `aimassist`, `gamecrosshair`,
  `arms`, `gun`, `6dof`, `bodyfollow`, `bodyfacing`, `bodyback`, `eyesmooth`,
  `fp`, `near`, `posscale`, `viewport`, `offcenter`, `eyestate`, `submitpose`,
  `handpredict`, `mode`, `menu169`, `cinema3d`, `ham`, `cinemalaser`.
  `configversion 7` removes their lines (and comments) from older inis.
- `armdebug`, `armbones`, `firelog on`, `inputlog N`, `gesturelog N`,
  `dumpcam`, `shot <name>`, `vischeck on` (enemies in front of the view but
  not drawn, every 0.5 s), `gesturesim melee|throw|reload|reloadlost|duck|weapon`,
  `audioroute where|reopen [driver]` (device of the game's audio session /
  reopen FMOD's output) - diagnostics in the log.

## Test tools (`tools/`)

- `gamectl.py` - launch/close the game, send commands, screenshots.
- `mirrorrec.py start|stop` - records what the headset shows and plays (the
  mod's mirror window, `mirrorwin on`: borderless 16:9 window, 1920x1080 or
  what fits the screen, fed by an additional swap chain with the central
  16:9 crop of the right eye; captured by ffmpeg, the default playback
  device by `wasapiloop.exe`, muxed into an mp4 at stop). Not
  OculusMirror: over Link it takes the VR focus from SteamVR (22/10: input
  focus captured, controllers dead, then the Link session dropped).
- `levelsweep.py` - loads every campaign level and checks the VR state.
- `deepsweep.py` - jumps through every checkpoint of every level.
- `loadouttest.py` / `weapontest.py` - fire the weapons and measure the shot
  direction against the barrel.
- `ue3.py` - engine object browser.
- `ghq.py` - decompiles functions of the persistent Ghidra project (needs
  `JAVA_HOME`); `FindImm.java` (same scripts folder) lists the functions that
  use an immediate, e.g. a field offset (`0xE98` -> the squad target
  validation).
- Script packages (`SRGame/ScriptFinalRelease/*.u`) are plain UE3 packages:
  the name table gives property and function names (`m_executionMaxCamPitchAngle`,
  `CheckExecute`) and the objects a function references can be listed from
  its bytecode without a decompiler (session scripts `unames.py`, `urefs.py`).
- `hmdwindow.py` - captures the SteamVR null driver window for headset-free
  tests.
- `package.py` - builds the release zip, `companions.json` and prints the
  release command with every asset the updater may fetch.
- `regress.py` - release gate: unit tests, then stand / sweep / move /
  checkpoint / fire / gestures / cinema phases on the game in synthetic-pose
  mode (kills the game; never run two game tests at once).
- `tests/unit_tests.cpp` - offline tests of `src/pure_utils.h`
  (`cmake --build build --config Release --target unit_tests`, then
  `build\Release\unit_tests.exe`).
- `native_names.py` - extracts the UE3 native registration table (4054
  `AClass::execName` functions, records {name pointer, function pointer} in
  `.data`) as `0xaddress<TAB>function<TAB>name` lines (Ghidra/REA seed format).
- `ghq.py` - queries a persistent Ghidra project of the exe: decompiled code and
  callers by address or name pattern in ~6 s. Setup once (Ghidra 12.1.x, JDK
  21): `analyzeHeadless <dir> SpecOps -import SpecOpsTheLine.exe` (~6 min),
  then `-process SpecOpsTheLine.exe -noanalysis -postScript ApplyNames.java
  natives.tsv` (the native functions are only referenced from the table, so
  Ghidra does not find 2293 of them by itself).

Loading levels writes checkpoints over the Steam Cloud saves: the test tools
back them up and restore them.
