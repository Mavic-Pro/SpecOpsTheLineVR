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

## Review fixes (after 0.9.9, in progress)

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
- **Covered by existing code:** R-U4 (world-locked squad marker: the orange
  target box is already projected per eye from `g_squadTargetLoc` with the
  eye's own projection, and M19 showed no `Canvas.Project` traffic to fix);
  R-R1 (identity guards: `StillAlive`/`ObjClass` checks on every captured
  pointer, `LivePlayerController()`, presence pruning of the projectile list
  - the generation counter was not needed once every use re-validates).
- **Headset notices (R-U1):** `src/vr_toast.cpp` draws short messages with
  GDI into a head-locked SteamVR overlay (`specopsvr.status`, 22 cm below
  the gaze at 1 m, 3.5 s each, queue of 4, identical text once per 10 s,
  recreated on a new SteamVR connection): update installed, recentered,
  standing height saved, gunstock calibrated, VR output paused, ProTube /
  bHaptics disconnected, SteamVR reconnected. `toasts off` disables them.
- **Interaction census (physical interaction plan, stage 0 / R-T4):**
  `interactlog on` lists every `YTrigger_*Interaction` actor of the level
  every 2 s (`INTERACT:` lines: class, name, position, distance from
  Walker, `GetCurrentIconPosition`, `IsEnabledForTacView`), the table the
  grab-to-use stages need.
- **Magazine visual (R-W7 step 1):** `weaponbones` lists the bones of the
  weapon mesh in hand (M4/AA12/Desert Eagle/SCAR/M9A1 have `Magazine`,
  `Slide`, `Muzzle`...; the Milkor has none); while the physical reload has
  the magazine out or in hand, `HideBoneByName("Magazine")` hides it on the
  weapon (`MAGAZINE:` lines), restored on insert or weapon change.
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
- `armdebug`, `armbones`, `firelog on`, `inputlog N`, `gesturelog N`,
  `dumpcam`, `shot <name>`, `vischeck on` (enemies in front of the view but
  not drawn, every 0.5 s), `gesturesim melee|throw|reload|reloadlost|duck|weapon`
  - diagnostics in the log.

## Test tools (`tools/`)

- `gamectl.py` - launch/close the game, send commands, screenshots.
- `levelsweep.py` - loads every campaign level and checks the VR state.
- `deepsweep.py` - jumps through every checkpoint of every level.
- `loadouttest.py` / `weapontest.py` - fire the weapons and measure the shot
  direction against the barrel.
- `ue3.py` - engine object browser.
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
