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
| `vr_session.cpp` | VR session (OpenVR: Background until the headset is worn, then Scene) |
| `vr_runtime.cpp` | Backend selection (`runtime steamvr|openxr|auto`): the interface accessors the rest of the code uses |
| `vr_openxr.cpp` | OpenXR backend: the OpenVR interfaces the mod uses, reimplemented on the OpenXR runtime (`xr_shim_ifaces.h` is generated) |
| `vr_output.cpp` | Frame transfer to the compositor (worker thread, D3D11), cinema overlay |
| `vr_input.cpp` | Touch controllers presented as an XInput pad, gestures |
| `script_call.cpp` | Calls into UnrealScript through `UObject::ProcessEvent` |
| `control.cpp` | `specopsvr.ini` / `specopsvr_cmd.txt` commands |
| `display_spoof.cpp` | Lets the game create a window larger than the desktop |
| `logger.cpp` | Per-session log with header, crash handler and minidumps |
| `updater.cpp` | Automatic update from the GitHub releases (channels, revocation list) |
| `haptics_ext.cpp` | ProTubeVR (64-bit bridge or ForceTubeVR_API_x32.dll) and bHaptics Player haptics |
| `props.cpp` / `props.h` | Interactive world: grabbing, weight, throws, gravity gloves, hand spheres (solid hands), the hit pipeline and gameplay items 1-11 |
| `vr_fake.cpp` / `vr_fake.h` | Fake VR runtime for tests without a headset (`runtime fake`; never runs `vr_openxr.cpp`) |
| `vr_menu.cpp` | In-headset settings menu (Basic, Advanced from the ini comments, Controls) |
| `vr_toast.cpp` / `vr_wrist.cpp` | Toasts and hints in the headset; wrist HUD |
| `ui_text.h` | In-headset texts (hints, toasts), measured by the unit tests |
| `pure_utils.h` / `twohand_math.h` | Pure maths and logic shared with `tests/unit_tests.cpp` |
| `bridge/protube_bridge.cpp` | 64-bit ProTubeVR bridge (embedded in d3d9.dll) |

## Game addresses and non-Steam copies (28/10)

The mod was reverse engineered on the Steam `SpecOpsTheLine.exe` (TimeDateStamp
0x50041F0C, no ASLR). Every fixed address it uses (functions, hook points in
the middle of functions, return addresses, globals such as GObjects/GNames/
GWorld, vtables, import-table slots) is now resolved at start by
`src/game_addrs.cpp`, first thing in `DllMain`:

1. if the expected bytes are at the Steam address, that address is used
   (Steam exe, or a copy whose code is identical: same build with the DRM
   wrapper removed);
2. otherwise the signature is searched in `.text`, and is used only if it
   matches exactly once;
3. otherwise the value stays 0: the code that needs it skips its feature
   (MinHook refuses a null target, direct calls and IAT patches check for 0)
   instead of hooking a random place.

Import-table slots are found by DLL + function name/ordinal. The old
`kXxxAddr` constants are now `const uintptr_t&` references into
`vr::addr::g[]`, so the code that uses them did not change.

Signatures (`src/game_sigs.inc`) are generated, never written by hand:
`python tools/gensigs.py`. Bytes that change between builds are wildcards:
every dword in the `.reloc` table and the rel32 operand of call/jmp/jcc. The
window can start a few instructions after the address (prologues are shared by
many functions) and grows until it is unique, with at least 12 fixed bytes.
To add an address, add it to the lists in `gensigs.py` and run it again.
`python tools/gensigs.py --check <exe>` prints which signatures resolve in
another executable, offline.

The log says `ADDR: n/49 game addresses resolved (... at the Steam layout, ...
by signature)` plus one line per address found by signature or missing. With a
different exe the headset toast says how many addresses are missing.

Testing on the Steam copy: the file `specopsvr_forcesigscan` in the game folder
skips step 1, so everything is resolved by searching; `tools/regress.py` must
pass both with and without it.

Limits: a build compiled differently can also move struct fields (Pawn,
PlayerController offsets...); those offsets are not covered by signatures.
Lookups by class/function/property name through GObjects/GNames work on any
build once those two globals are found.

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
- Settings reset (0.10.0, `vr::pure::kResetConfigVersion` = 12): an ini with
  `configversion` below 12, or without the line, is copied to
  `specopsvr.ini.bak-<yyyyMMdd_HHmmss>` (the name `reset_settings.bat` uses)
  and replaced by the shipped ini (`kDefaultIni`); log `CONFIG: specopsvr.ini
  of configversion N reset to the defaults of this version; your old settings
  are kept in ...`. After 10 s of live play a toast says so
  (`kToastSettingsReset`, log `CONFIG: settings reset notice shown.`). If the
  copy fails the ini is migrated as before (`CONFIG: could not save a copy`).
  The shipped `deploy/specopsvr.ini` carries `configversion 12`, so a fresh
  install or `reset_settings.bat` is not reset again. Unit test
  `TestConfigReset`.
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
- Dynamic resolution: `ScreenPercentage` (UE3 `scale set`) renders the 3D scene
  smaller. **In gameplay the game upscales it back to the whole back buffer.**
  Since 27/10 evening the mod therefore never crops or rescales by the
  percentage: the `kGeometryPercent` constant in hooks.cpp (100) is the
  percentage for the eye crop (vr_output.cpp), the aim marker, the HUD canvas,
  the highlight and squad-target boxes, the laser, the scope bars and the
  vignette. Every frame tag carries it (`tag.screenPercent`), and a Present
  without a tag uses it too, so such a Present right after a change is sent
  instead of dropped (WI-1's 250 ms rule in "Tester VR fixes (25/10)" no
  longer fires). See "Headset session fixes (27/10 evening)".
  - **History of the wrong assumption:**
    - Up to 27/10 the mod assumed a centred, un-upscaled sub-rectangle and
      cropped it.
    - The "box" really seen on 21/10 was the boot menu (UI scenes are not
      upscaled).
    - `dynres test` measured only `DrawIndexedPrimitive` viewports, so it
      missed the game's full-screen upscale pass.
  - **Consequence (user, VDXR at 120 Hz, 27/10):** at 120 Hz the 8.3 ms frame
    makes dynres step between 85 and 100% all the time. Each step zoomed the
    image around the buffer centre, which differs per eye with the off-centre
    projection, so the two eyes showed different images and two aim markers.
  - **Proof:** back-buffer captures in the headset at 75% and at 100% show the
    same framing.
  - **What is unchanged:** the real `ScreenPercentage` is still in
    `g_screenPercent`. Each GPU query records it (the controller uses only
    samples of the current level), the `GPU:` and `DYNRES` lines show it, and
    `dynres test` (flips 70 <-> 100% every 0.5 s for 1200 frames) still
    compares the largest `DrawIndexedPrimitive` viewport with it.
  - **Controller:** since WI-1 (25/10) `vr::pure::DynResController`: p90 of the
    last 2 s, dwell times and a back-off for failed levels (see "Tester VR
    fixes (25/10)", WI-1). A step down is still `pct * sqrt(0.85 * frame /
    p90)`, 5 to 10 points, rounded to a multiple of 5 towards the current
    level; a step up is +5; the range is `dynres min` (default 65, 40-100) to
    100. Active (`dynres on`, the default) only in stereo gameplay with the
    headset in Scene or with `dynres budget` set (100% on the cinema screen).
    From 0.9.4 until WI-1 the same formula ran every 0.7 s.
  - `dynres budget <ms>` fakes the headset frame for tests (0.9.4
    measurement: 3 ms -> down to 65%; 12 ms -> back to 100%). A change costs at
    most one ~20 ms frame (0.9.4 measurement; WI-1 logs the length of the tick
    with the change and of the next one in `DYNRES`).
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
  than 4%; the longer side at most 4032 with the aspect kept
  (`vr::pure::FitRecommendedSize`: display_spoof's 4096 desktop minus the
  window borders). Quest 3 here: 2528x2776; the
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
- Since WI-2 (25/10): nearest-hit view traces without pawns for the pull,
  only the penetrating part of the head offset clamped, a stable eye-height
  reference, runtime recenters absorbed: see "Tester VR fixes (25/10)", WI-2.

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
  120 ms (since WI-4b: when the two-handed grip holds the gun, the same flag as
  the aim; `leftgrip <cm>` is ignored; see "WI-4b: two-handed grip") to the grip pose composed with the wanted right-hand pose: fingers
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
  version 3 migrates `bodyback 6 10`. **Replaced in WI-3 (26/10)** by the neck anchor (see
  "WI-3: body, seated play, crouch"): `bodyback` is now only a fine-tune (0).
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
  by `spread` (0..2, on = 1, off = 0) and by stance: sights at the eye (ADS)
  0.1, two hands 0.5, a long gun in one hand 1.5 (not pistols); adjustable as
  `spread on <scale> <ads> <two hands> <one hand>`. Pass-through: the caller loops with the same End
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
  While aiming in cover (`m_aimingReplicated`, +0x5F0 bit 0x800) the view,
  and with it hands and gun, follows the sideways part (relative to the Pawn,
  6-unit dead zone, at most 45) of the Head bone offset from the mesh origin:
  around a corner Walker's head moves ~30 units, while the view stayed on the
  player's head behind the wall (hostage scene, ch. 1, 23/10: the player had
  to walk out of cover in the room). The offset is added in ApplyFirstPerson
  with an 80 ms time constant (`coverlean`).
- `YGamePawn::RaiseWeapon` (0x0052F2B0): with the body a step from a wall in
  front the script calls it with YRWS_Raised (2) and m_weaponRaised (+0x499)
  makes MayAim (0x0065B7C0) and UpdateMayFireWeaponInternal (0x0068A820)
  refuse aim and fire (zoomWithoutFire): the trigger reached the game and no
  shot left. With the barrel aim the mod turns YRWS_Raised into None for the
  player (YRWS_NoAim, scripted no-aim zones, is kept); MuzzleStart already
  handles a barrel inside the wall.
- Turrets (`YWeapon_Stationary`): the PlayerController is steered towards the
  turret aim (the gun hand; with *Turret grip* normal the ground-turret pivot
  or, on the helicopter minigun, the handlebars) with a time-based gain
  (30 ms time constant, at most 0.5 per tick, about 0.35 per tick at 90 Hz;
  WI-1), yaw and pitch; the part of
  the yaw actually performed is moved into the yaw reference and body pitch
  is left out of the view, so the view does not follow the turret
  (`turretaim hand`).
  Since WI-4c (26/10) the turret follows the grip mode (`turretgrip`: NORMAL,
  both hands on the handles, or GUNSTOCK, the stock), never past its mount's
  limits; see "WI-4c: turret grip modes". Since 27/10 evening NORMAL works as
  handlebars only on the helicopter minigun; a ground turret pivots like a
  real mounted gun (`turretpivot`): see "Headset session fixes (27/10
  evening)".
- Aim down the sights: from tracked poses (head offset vs. the right
  controller ray, not the drawn gun, whose pose changes with the game's aim
  mode and made the state flicker): eye within 8 units of the ray and 4-45
  units behind the controller hides the marker and holds LT (`ads`).
- Two-handed aim: see "WI-4b: two-handed grip" (one solver for aim, drawn left
  hand, spread, haptics and gesture vetoes; the 100 ms blend of 0.9.x is gone).
  While the left hand holds the barrel its grip does not throw a grenade.
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
- Known problem (found 25/10, open): the same pattern `on KActorSpawnable`, a
  few units from the barrel axis, is the mod's own hand sphere (props.cpp,
  ProxyCreate). The shoulder -> muzzle trace of `MuzzleStart` and the shot
  trace do not skip `props::IsHeldActor`, so at some wrist poses the bullet
  starts on the right palm sphere and stops there, and the marker sits on the
  hand. See "Baselines on the WI-0 build" for the measurement; the fix is a
  merge item with the physics work.

## Haptics

- Controller rumble: the game's motor speed (XInputSetState) is remembered and
  pulsed every frame (legacy pulses are ~4 ms) on the gun hand, and on the
  left hand while two-handed; gestures confirm with 70 ms.
- Impact pulses (Boneworks plan item 3, `impacthaptics`, see "Interactive
  world"): `vr::input::PulseHand(hand, ms, amp, freqHz)` keeps one pulse per
  hand (the strongest pending one wins; end time on the
  QueryPerformanceCounter clock, because `GetTickCount64` moves in ~15.6 ms
  steps). `UpdateHaptics` mixes it per frame with the old output through
  `vr::pure::HapticMix`: without an impact pulse the output is bit-for-bit
  the old one (game motor on the gun hand = 600 + motor/20 us, fixed pulses
  3500 us, 150 Hz, 20 ms per frame; unit test over all 65536 motor values);
  an impact pulse wins when its amplitude is not lower, and is sent with its
  own frequency and the time it has left (at most 20 ms per frame, so the
  repeated calls end it on time).
  - The gun hand does not change (user rule; item 3 review, 26/10): impact
    pulses are never sent to the right hand unless it holds an object
    (`grabright`), nor to the left hand while it is on the gun
    (`g_leftOnGun`), two-handing (`g_twoHandEngaged`) or busy with the
    physical reload (`vr::input::LeftHandOnReload`, the gesture code's
    "left hand busy": on the magazine, holding the new one, racking). There
    the old fixed pulses stay (knock 20 ms, grab 50 ms, at 3500 us) and
    the empty-hand slap is off (`props.cpp GunHandHaptics`).
  - SteamVR Input and the OpenXR backend: `TriggerHapticVibrationAction(...,
    seconds, freq, amp)`; the OpenXR `InputShim` passes the three values to
    `xrApplyHapticFeedback` (`XrHapticVibration`).
  - Legacy API (`vrinput legacy`): `TriggerHapticPulse(us)` with us = amp x
    3900; SteamVR on Quest reads it as amplitude, and the OpenXR
    `SystemShim` turns it into amp = us/3900 for 20 ms; frequency and exact
    duration are lost on that path.
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
the head direction is the real aiming direction. 0.9.3 used the eye-to-controller
line, wrong on a stock where the hand sits ~25 cm below the eye (-67 degrees).

Since WI-4a (26/10, see "WI-4a: gun frame" below) B + Y calibrates the angle of the
current hold: `gunangle` with `holdmode normal` (gun at the eye, looking along the
barrel), `stockangle` with `holdmode gunstock`. The angle is the elevation of the
head's line of sight in the controller's pointing frame,
`atan2(head . up, head . forward)` (`vr::pure::SightLineInAim`), so it is right with
the controller rolled; the old `aimpitch` = head pitch minus controller pitch was
applied as a rotation about the controller's X axis and was right only at roll 0
(unit test: up to 24.9 degrees off at roll 60). Accepted with the palm 15-60 cm from
the eyes and the line of sight within 30 degrees of the gun's plane; the result is
rounded to 0.5 degrees, limited to the key's range (gunangle +-45, stockangle
-60..90) and saved. The sideways component is logged (`STOCK:` line) but not applied
(`gunyaw` stays a manual Advanced key). Test command: `stockcal` (same as B + Y).

WI-4a review (round 1): in the normal hold the measurement is circular if the player
lines up the DRAWN gun, which already sits at the current `gunangle` above the
pointing ray: the sight line then is that angle and B + Y returns it (no harm, no
correction). The stock case works because the cheek weld fixes the controller to the
eye. So B + Y is no longer offered for the normal hold (Basic help, Controls page,
README, rejection toast): bare-controller players use Basic > Gun angle. The
command still works (it is right if the player points the controller at the target
like a real pistol without looking at the drawn gun); headset check.

WI-4b: with `holdmode gunstock` B + Y also saves where the left controller sits on the
stock (`stockmount x y z yaw pitch roll`: the left raw pose in the right raw frame, cm
and degrees), when the palms are 10-90 cm apart (`STOCK: left controller on the stock
saved ...`). The gunstock two-handed grip uses it (see "WI-4b: two-handed grip").

## Diagnostics added in 0.9.3

- `STATS` every 300 ticks: presents per eye, repeated eyes, frame tag
  agreement, cinema switches, largest per-tick view jump, main-view FOV.
- `VISCHECK`: enemies within 2500 units and 35 degrees of the view that are
  not drawn for more than 0.3 s (investigation of enemies disappearing when
  leaning out of cover).
- `camlog N`, `bodysize`, `SHOT ... on <actor>`, `GRENADE`, `COVER`, `ADS`,
  `CROUCH`.
- `hittest <z>`: a zero-extent line (like an instant-hit bullet) from 2 m in
  front of Walker towards him, `<z>` units above the Pawn centre; `HITTEST`
  logs what it hits.
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
  The dll writes `specopsvr_input\actions.json` and default bindings for
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
  in the same gun frame as aiming (WI-4a: from the gun's wrist point, turned about the palm). Grip within 12 cm of it and
  pull past 18 cm = out; grip pressed with the hand 35+ cm below the head and within
  45 cm of it horizontally (or the grip kept held from the eject down to
  55+ cm below the head) = new magazine held; held within 7 cm of the
  magwell = X (the game's reload). Releasing the grip drops the held
  magazine; 12 s without finishing or the X button resets. While the left
  hand is at the magwell or the grip belongs to the reload, the grip is not
  sent to the game as LB (grenade).

## Interactive world (props.cpp, from 24/10)

Plan, one stage at a time (each finished and verified before the next; all
six done on 24/10 without a headset, the headset check is still to do):
1. grab, hold and throw the physics objects already in the game;
2. small static props (bottles, boxes, cans, debris, also inside
   `StaticMeshCollectionActor`) replaced at runtime by physics copies;
3. solid hands that push and knock objects over without grabbing;
4. breaking (bottles, glass) on shots, throws and hard impacts;
5. constrained objects (car doors, signs on poles, doors) moved by the hand
   through their hinge;
6. polish: two-handed objects, performance, pass over every level.

**Stage 1 (done 24/10, needs a headset check):**
- Candidates: refreshed every ~0.5 s (and right after `spawnprop`) the `KActor`
  subclasses within 12 m with `Physics == PHYS_RigidBody` (Actor +0x94 = 10),
  visible, with a `BodyInstance` (PrimitiveComponent +0x170) and a bounding
  sphere under 2.5 m. Actors referenced by an `RB_ConstraintActor`
  (`ConstraintActor1/2` +0x1F8/+0x1FC) are marked as constrained (stage 5).
- Zone: the palm (13 cm in front of the IK target, where the canonical grip
  puts the object and where the hand body sits) within `grabzone` cm (8) of
  the object's world box; a 15 ms tick on entering it. Until stage 3 it was
  the IK target, so the palm reached the object first. In the zone and while holding, the grip is not
  LB/RB nor a gesture. The left hand's grenade and punch gestures are off
  while it holds an object and for 0.5 s after its release (`g_propHeld[1]`,
  `g_propReleasedMs[1]` set by `Release`; item 4 review, 26/10): before, an
  overhead swing or throw of a held bottle pressed LB, because gesture buttons
  are ORed into the pad after the pad mask.
- Grab: grip closed in the zone, or closed up to 0.25 s before the hand
  arrives. Left hand only by default (`grabright on` for the gun hand).
- Canonical grip (`CanonicalGrip`, 24/10, after the tester's "not a correct
  grip"): the object always ends in the same pose relative to the hand,
  whatever way it was lying. Grip frame measured every frame from the four
  knuckles (`MeasurePalm`: o = their mean, a = pinky->index, f = towards the
  knuckles, p = palm side), not from a bent finger bone (that tilted the palm
  normal 35 degrees). Mesh axes from the scaled `UStaticMesh` bounds (+0x94):
  longest -> a, thinnest of the other two -> p (a flat face on the palm). If
  the mesh origin sits at one end (bottles, cans) the other end (the neck)
  goes towards the thumb. Among the allowed sign/swap candidates with the
  mesh's handedness, the one with the least visible turn. Classes: power
  (cross half-extents <= 7 / 8 cm: object axis 0.4 cm + 0.6 x half
  thickness proximal of the knuckles, hand 30% of the length from the base),
  pinch (thin plates up to 30 cm), anything larger keeps the palm contact.
  The object turns into the pose in 0.10-0.20 s by slerp around the point
  that ends in the hand (the bottle's origin is its base: turning around it
  swept the hand). spirit_bottle_01: centre f -2.6 p 5.5 a 6.6 cm from the
  knuckles, measured in game within 0.08 cm and 0.1 degrees of the target.
- Fingers (`PoseFingers`, after `UpdateSkelPose`, on the SpaceBases): a
  contact solver with absolute joint angles from a straight finger (the curl
  used to be added on top of the rifle pose of the left hand: fingers inside
  the bottle). Each finger closes with fist proportions until its first
  segment touches the object (rounded box from the mesh bounds, roundness
  from the bounds sphere), then each later joint closes on its own until it
  touches (limits 90/110/80). Thumb: swung out of the object around its base,
  then its joints rest on the object or over the index and middle fingers.
  Measured on the bottle: clearance 0.00 cm, tips within 0.07 cm. Targets
  smoothed in ~0.08 s, weight in 0.12 s; empty grip = fist; left hand on the
  gun = untouched. 0.06 ms per frame.
- Hold: every game tick (`WorldInfo.TimeSeconds` changes; Draw runs once per
  eye) right after the hands of that frame are computed:
  `SetRBLinearVelocity` = hand velocity + the grip point's speed from the
  wrist's angular velocity + 0.5 x error / dt (max 30 m/s, 30 x 15 / kg
  above 15 kg but never under 5 m/s, relative to Walker: see "Boneworks
  plan, item 1" below; before it the mass was compared in engine units and
  the limit never applied);
  `SetRBAngularVelocity` = wrist angular velocity + 0.5 x
  axis-angle(error) / dt (max 55 rad/s); `WakeRigidBody`. Measured: settles
  in 18-87 ms after a 60-180 degree wrist turn, 1.8 cm behind a hand moving
  at 1.5 m/s. The object collides with the world. Stuck more than 60 cm away
  for 0.3 s: it slips out of the hand (with `objectweight` on: 80 cm above
  30 kg, and only while the object is not coming closer; item 2 below).
  `griptune <palm|gap|finger|thumb|height|back|round|gain|relgrace|minspeed|
  pawnframe|force|torque|klin|krot|sagcap|droopcap|brake|liftmin|pawnacc|
  overshoot|overtime|alphamin|fingerreach|framevel> <value>` changes the grip
  measures, the drive limits and the weight at runtime.
- PhysX limits: every body spins at most 7 rad/s by default
  (NX_MAX_ANGULAR_VELOCITY): a held or glove-pulled object gets 60 rad/s via
  `NxActor::setMaxAngularVelocity` (BodyInstance +0x170 -> BodyData +0x60 ->
  vtable +0xF0; +0xF4 reads the old value, logged as 7.0). A 140 degree turn
  went from 0.35 s to 0.06 s.
- Walker's own PhysX bodies: `Pawn.PhysicsPushBody` (+0x404), the 16 bodies
  of his PhysicsAsset (Mesh +0x3A8 -> PhysicsAssetInstance +0x25C -> Bodies
  +0x44), the two spring capsules of `YPawnSpringyRBCollider` and above all
  the kinematic capsule of `YPawnKinematicRBCollider` (Walker's 15 x 45 unit
  cylinder for gameplay physics, a raw NxActor at +0x200 with no userData;
  found by bisecting the scene's actors). In VR the head and chest are inside
  that capsule: an object brought near the face was knocked back every step
  (PhysX turned a -38 rad/s command into +7..+15). While held or pulled by
  the glove each pair is set to NX_IGNORE_PAIR with
  `NxScene::setActorPairFlags` (scene vtable +0xA4, checked with
  `getActorPairFlags` +0xA8; scene from `NxActor::getScene` +0x4; the
  scene's actor list is `NpScene` +0x308, used to find the spring capsules by
  userData -> RB_BodyInstance -> OwnerComponent -> Owner). Re-applied if the
  bodies are rebuilt; restored on release once the object is outside the
  capsule plus `RBPushRadius`. `bBlockActors` off and `bIgnoreRigidBodyPawns`
  on as before.
- Throw: on release the linear velocity = hand velocity averaged over 60 ms
  x `throwboost` (1.3), at most 1.5 x the object's `maxSpeed`. Measured: hand
  2.4 m/s -> wanted 2.92, object 3 ticks later 2.78 m/s (gravity/damping).
  The hand's sphere lets the released object out before it becomes solid
  again (release grace, item 1 below).
- Gravity gloves (`gravitygloves`, Half-Life: Alyx style, left hand): point
  the controller (or the palm) at an object up to 8 m away (16 degree cone,
  kept up to 26 degrees once chosen, light tick on change), close the grip to
  lock it, flick the hand towards the catch point faster than 1 m/s or
  upwards faster than 1.2 m/s: the object flies to
  the palm on a ballistic arc recomputed every tick. Caught if the grip is
  still closed, otherwise it floats in front of the hand for 0.6 s and a new
  grip press catches it. Since 27/10 evening the gloves also pull dropped
  guns (item 6's `YPickup_WeaponSpawnable`): see "Headset session fixes
  (27/10 evening)".
- Highlight (`grabhighlight`): cyan corner brackets around the object the
  hand can grab or the glove points at, gold and thicker once locked.
- Commands: `grabprops`, `gravitygloves`, `grabhighlight`, `throwboost`,
  `propcombat` and `hitlog N` (hit pipeline, see below), `propthrowdamage`
  (item 1), `propglass` (item 2; test: `glasshit 0x<actor> <kg> <m/s>`) and
  `propshield` (item 3; tests: `enemyfire 0x<pawn> [shots] [x y z]`,
  `shieldtrace on|off`), `propmelee` (item 4), `propexplosive` (item 5;
  test: `explhit <damage>`), `weaponpickup` (item 6; tests: `weapontest`,
  `weapongrip x y z p y r`), `grenadeback` (item 8; `grablog N` adds
  `GRENLOG:` lines while a grenade is held), `corpsedrag` (item 11; tests:
  `corpsetest` grabs the nearest corpse body without the zone, `corpsetest
  bones` logs the corpse's bodies, bones and positions; `grablog N` adds
  `GRABLOG: L corpse` lines while dragging),
  `grabzone`, `grabright`, `grablog N` (error, rotation, velocities, hand and
  object pose per tick), `graspdump 1` (finger joints, contact clearances,
  grip frame, object pose against the target), `griptune`,
  `spawnprop <StaticMesh> [cm]` (KActorSpawnable in front of the left hand),
  tests: `grabsim left|right on|off [ms]`, `synthleftvel f r u ms`,
  `synthleftrot p y r`, `fingercurl <0..1|-1>`, `fingeraxis <f> [t]`,
  `grabtest <part of a mesh name>` (left hand takes the nearest such object,
  no zone). `masskg <kg>` (ini only, item 1 below). `objectweight
  off|light|full` (Basic menu "Object weight") and `weightforce`,
  `weighttorque`, `weightturn`, `weightsag`, `weightdroop` (ini), item 2
  below; `weightbreak <ms>`, `weightwedge <s>` and `weightregrip <s>` (ini,
  wedged objects, item 2 follow-ups below);
  tests: `breakdrop <ms>|off` (the fake grip opens <ms> after a breakaway
  frees the object), `synthleftrotvel p y r ms` (the fake left hand turns,
  degrees/s), `synthsmooth on|off` (fake hand motion on the high-resolution
  clock), `synthcomp on|off` (fake poses: every body turn compensated by the
  head reference, like cover and turret aim) and `fpscap <hz>` (game ticks at
  most at <hz>, for 90/45 Hz tests without a headset). `impacthaptics
  off|low|full` (Basic menu "Impact haptics") and `griptune hapdv|hapj|
  haprefr|haprearm|hapwindow|hapspherekg|hapspeed|hapslapkg|hapapproach`,
  also as ini keys (Advanced menu) `hapticdv`, `hapticfull`,
  `hapticrefractory`, `hapticrearm`, `hapticwindow`, `hapticapproach`,
  `haptichandkg`, `hapticslapkg`, `hapticslapspeed`, item 3 below;
  `hapticlog N` (the next N controller vibration frames in the log as
  `HAPTIC out:`, with the values sent; without a headset the values that
  would be sent). `throwmode peak|classic` (Basic menu "Throw style"),
  `throwenergy <J>`, `throwplace`, `throwfull`, `throwglitch` (ini) and
  `griptune peakwin|peakdt|peakjump|energy|spinmax|spinjump|lever|peakmin|
  peakfull|peakout`, item 4 below; test: `synthleftramp f r u v1 ms1
  v2 ms2 [release]` (the fake left hand speeds up along (f, r, u) from 0 to
  v1 cm/s in ms1, then to v2 cm/s in ms2; `release` opens the fake grip at
  the end while the hand goes on at v2 for 50 ms). `solidhands
  off|held|full` (Basic menu "Solid hands", default held), `solidhandsdead`,
  `solidhandsturn`, `solidhandscap`, `solidhandscapturn`, `solidhandsin`,
  `solidhandsout` (ini) and `griptune shdead|shturn|shcap|shcapturn|shin|
  shout|shtele|shwall|shguard`, item 5 below; `solidlog N` (the next N
  frames of the drawn free hand as `SOLID:` lines, with the bone check).
  `analoggrip on|off` (Basic menu "Analog grip"), `analoggripmin`,
  `analoggriploose`, `analoggripfirm`, `analoggripturn`, `analoggripdroop`,
  `analoggriphold`, `analoggripease`, `analoggriprise`, `analoggripthrow`,
  `analoggripfloor` (ini) and `griptune gripmin|griplo|griphi|gripturn|
  gripdroop|griphold|gripease|griprise|gripthrow|gripfloor`, item 6 below;
  test: `synthgrip <0..1> [noise] [after ms] [ramp ms] | off` (fake analog
  grip value).

**Stage 2 (done 24/10, needs a headset check): static props become physical.**
- The game already has it: `KActorFromStatic.MakeDynamic(StaticMeshComponent)`
  (static native, iNative 0: callable through ProcessEvent on
  `Default__KActorFromStatic`) moves the same component (mesh, materials,
  lighting) onto a new `KActorFromStatic` with `PHYS_RigidBody`; when it falls
  asleep the game puts it back as static where it stopped (`OnSleepRBPhysics`
  -> `MakeStatic`). Spec Ops uses it for the bottles you can shoot:
  `StaticMesh.bCanBecomeDynamic` (+0xE8), `Health for KActorFromStatic`
  (+0x100, 30 for bottles) and `KActorForStaticClass` (+0x104, e.g.
  `YKAFST_GlassMedium`, glass particles and sound when it breaks).
  `StaticMeshComponent.bNeverBecomeDynamic` (+0x23C) is set on 493 of 505
  small components in level 1: only the 12 bottles are allowed by the game.
- MakeDynamic (decompiled at 0x005761b0) needs the component's `BodyInstance`
  (+0x170), and most small props have none (`BlockRigidBody` off): the mod
  calls `SetBlockRigidBody(true)` then `PrimitiveComponent.InitRBPhys`
  (static body, owner not rigid body) first. Done at the moment of the grab
  or the glove lock, never before: the world stays as the level designers
  left it until a hand touches it.
- Candidates (scan): `StaticMeshComponent` (exact class) whose owner has
  `bWorldGeometry` (Actor +0x9C bit 0x100: `StaticMeshActor`,
  `StaticMeshCollectionActor`), visible (`HiddenGame` +0x134 bit 0x4), with
  simple collision (`StaticMesh.BodySetup` +0x90), bounding sphere 2-55 units,
  within 8.5 m, mesh name not excluded. The exclusion list was reviewed by
  hand on the 422 small meshes of the 18 levels (sweep 24/10): vehicle parts,
  lights and lamps, ducts, trims, columns, panels, glass panes and shards,
  signs, portholes, tables, shelves, cabinets, racks, carts, fixed machines,
  plants and rocks, heaps and clusters fused into one mesh, scattered debris,
  corpses, cinematic props. Meshes with `bCanBecomeDynamic` always pass.
  130 of 422 remain: bottles, cans, bags, suitcases, boxes and crates,
  radios, books, plates, glasses, chairs, barrels, sandbags, cushions, toys,
  tools. Objects are identified by their component (a collection holds many).
- Surfaces without a body: an object made physical fell through the
  furniture it stood on (the level 1 radio dropped 72 cm). The scan also
  lists the static meshes near Walker with simple collision and no body, up
  to 4 m radius and not visual-only (edges, decals, sand, sky, backdrops,
  glass, ...): larger meshes often have a crude simple collision left off on
  purpose, and made solid they put invisible walls around Walker (a held
  bottle slipped out of the hand). Around a held, pulled, just-converted or
  just-released object (4 s) they are made solid the same way (16 at the
  conversion, then at most 6 per tick). Measured: the radio lifted 12 cm and
  released lands back on its rifle case (+2.4 cm).
- Settling: level props are often sunk into each other or into the ground
  and PhysX ejected them when they became physical (a barrel jumped); for
  0.5 s after the conversion the speed stays under 1 m/s (2.5 x the hand
  speed when knocked by a hand) and the spin under 10 rad/s, read and capped
  through `NxActor::getLinearVelocity/getAngularVelocity` (+0xE8/+0xEC).
  An object locked by the glove stays still until the flick.
- Deferred canonical grip: if the hand appeared a moment before the grab
  (knuckles not measured yet) the canonical pose is applied on the first
  tick with the measure, instead of the palm grip.
- Scan cost: one pass over GObjects (~190,000 objects) took ~5 ms in one
  tick every 0.5 s (a lost frame at 90 Hz, stage 1 too); now 1/45 of the
  objects per tick (~0.13 ms), full pass every ~0.5 s, lists swapped at the
  end of a pass. `PROPS: scan` in the log every 30 s.

**Stage 3 (done 24/10, needs a headset check): solid hands.**
- Each hand has an invisible PhysX body: a `KActorSpawnable` with the sphere
  of `CH_PHYS_ASSETS.PawnCollisionTriggerMesh` (radius 50 units) at scale
  0.05 (`SetScale` before `SetStaticMesh`, so the body is created small),
  hidden (`SetHidden`), on the palm. Driven like a held object (hand velocity
  + half the error per tick, max 30 m/s), mass 0.1 through
  `NxActor::setMass` (+0xB4; `getMass` +0xB8 read 0.009), no gravity and no
  rotation (`raiseBodyFlag` +0x164, `NX_BF_DISABLE_GRAVITY |
  NX_BF_FROZEN_ROT`). Units of `GetBodyMass`: bottle 0.04, radio 0.2, fuel
  barrel 0.83 (measured on level 1, item 1 below), so 0.1 is ~4 kg (hand and
  forearm): a bottle flies, a radio slides, a barrel barely moves. With 1.5
  the hand bulldozed barrels.
- Walls stop the sphere; more than 15 cm from the palm for 0.15 s it is put
  back on the palm (`SetRBPosition`). Hidden menus, cinema and disabled grabs
  park it 100 m under Walker.
- The sphere takes part in no trace: its component's `BlockZeroExtent` and
  `BlockNonZeroExtent` bits are off (since 25/10, "Hand spheres must not
  block the gun" below), so shots, the aim marker and the view go through
  it; `BlockRigidBody` stays on.
- Pairs set to `NX_IGNORE_PAIR` with the sphere: Walker's bodies, the held
  objects of both hands, the object flying to the glove and, while the hand
  moves under 1 m/s, the object in the grab zone (otherwise the hand pushed it
  away before the grip). A pair goes back to normal only once the object is
  6 units from the palm, or at once above 1 m/s (a slap). The object the hand
  has just released is the exception (item 1 below).
- Static props hit by the palm sphere at 0.3-8 m/s become physical
  (`MakePropDynamic`) and the sphere pushes them: a bottle knocked off a crate
  falls (measured 1.3-2.6 m). Only small ones (bounding sphere up to 30
  units): a barrel on a dune slope rolled down 10 m. Above 8 m/s it is a
  tracking jump, not a hand. The sphere itself moves at most 10 m/s for the
  same reason (with 30 m/s a jump of the fake test hand threw a bottle 3.6 m).

**Stage 4 (done 24/10, needs a headset check): breaking.**
- The game already breaks its own bottles when shot: a native contact /
  hit path (e.g. 0x00adda50 for pawn bumps) checks the C++
  `CanBecomeDynamic` (StaticMeshComponent vtable +0x2A0: mesh
  `bCanBecomeDynamic`, no `bNeverBecomeDynamic`, `BlockRigidBody` on,
  rigid body not disabled), calls MakeDynamic and applies an impulse or
  damage. The KActorFromStatic gets the mesh health (+0x2F0) and template
  (+0x2F4); `TakeDamage` beyond the health destroys it with the template's
  effect: `YKAFST_GlassMedium` (particles `FX_vaporizing_glassMedium_M`,
  sound `sfx_VaporBottle`) for the bottles (health 30, liquor bottles 1),
  `YKAFST_Bottle` for plastic containers, `YKAFST_CorpseHead` for skulls.
- Impacts (mod): released, thrown, knocked or dropped objects are followed
  for 4 s; a velocity change above 5 m/s within 50 ms, starting from above
  3 m/s (thrown at a wall, dropped from about 1 m or more; per tick was not
  enough at 250 Hz, PhysX spreads a landing over several steps), breaks a
  breakable one with `TakeDamage(health + 1, YDamageType_Melee)`. Not during
  the 0.5 s settling after a conversion. `PROPS: X did not break (largest
  change Y m/s)` is logged for breakables that survived, to tune the
  threshold from playtest logs.
- Smashed in the hand: hand at 3-8 m/s and, after the PhysX step, the object
  moving at less than 40% of the velocity given the tick before, for two
  ticks in a row (read through `NxActor::getLinearVelocity`; the UE3 actor
  velocity comes one tick late and every sharp acceleration looked like a
  hit). The hand lets go and the object breaks.
- Shots: `Hooked_CalcWeaponFire` passes Walker's impacts to
  `props::OnShot` (FImpactInfo: HitActor +0, HitLocation +4, HitInfo +0x34,
  HitComponent +0x4C, checked: the component belongs to the actor). Static
  props that the game would not make dynamic (bounding sphere up to 40
  units) become physical and get `AddImpulse` at the hit point with a
  velocity change of 6 m/s for light ones (less above mass 0.1); breakable
  ones break. The level 1 radio moved 56 cm when shot.
- Broken components are removed at once from every list of the mod (props,
  ignored pairs of the hand spheres): the game destroys them.
- Measured: bottle dropped from 1.3 m broke at 6.2 m/s; dropped bottles
  broke at 5.4, 7.1 and 10.2 m/s; a bottle swung down onto the wall in the
  hand broke as smashed.

**Stage 5 (done 24/10, needs a headset check): hinged objects.**
- Level 1 has 9 car doors (`KActor` hinged to the world by `RB_HingeActor`,
  swing limit 15-30 degrees with stiffness 350, `bDisableCollision`) and the
  stop sign (`KActor` on the `sign_stop_pole` `StaticMeshActor`, free twist
  about the pole). Setup: `RB_ConstraintActor.ConstraintSetup` +0x200
  (`RB_ConstraintSetup`: Pos1 +0x54, PriAxis1 +0x60 in PhysX units, limits
  +0xDC..), flags +0x208.
- Driving the whole body to the hand did nothing (the hinge cancelled it:
  the sign stayed still with the hand 38 cm away). Now the grab keeps the
  point under the palm (in the object's axes, no canonical pose) and every
  tick `AddImpulse` at that point = mass x 0.5 x (wanted point velocity -
  measured point velocity); wanted = hand velocity + the error recovered in
  50 ms, capped at 10 m/s; the point velocity is measured from its
  displacement between ticks (with the centre of mass guessed at the centre
  of the bounds the estimate was wrong for the sign, which turns about the
  pole, and the drive oscillated between 0.6 and 3.9 rad/s). PhysX turns the
  impulse into the motion the hinge allows. More than 25 cm from the palm for
  0.25 s (end of travel, hand pulled away): the hand lets go. No throw on
  release; the fingers close on the object's real pose; the gloves skip
  constrained objects.
- Measured: a door pulled 40 cm turns 40-62 degrees (the soft limit gives a
  little), following the hand at ~0.4 m/s with ~9 cm lag; the sign moved
  15 cm sideways turns 38-44 degrees.

**Stage 6 (done 24/10, needs a headset check): polish.**
- Cost: `PROPS: tick X ms on average, Y ms at most` every 30 s. Level 1:
  0.15-0.18 ms per tick on average, under 1.3 ms at most while grabbing,
  converting, pulling with the glove, breaking and shooting. The engine
  objects the first grab needs (`Default__KActorFromStatic`, the
  `KActorFromStatic`, `YDamageType_Melee` and `KActorSpawnable` classes, the
  hand-sphere mesh) are picked up by name index during the incremental scan:
  looking for them through all of GObjects cost a ~5 ms spike at the first
  grab of every level. 8 surfaces made solid at the conversion (16 before),
  then 6 per tick. `spawnprop` (tests only) still costs ~15 ms.
- Level pass (all 18 levels, novr, `grabtest any` from the start point):
  every level loads and plays without a crash; 0.07-0.16 ms per tick on
  average; a real prop grabbed in 10 levels (in the other 8 there is no
  prop within 8 m of the start point).
- Haptics: 20 ms pulse when the hand knocks a prop over, 70 ms when an
  object breaks in the hand.
- Two-handed objects: left out. In Spec Ops the right hand always holds the
  gun, so a second hand on an object is only possible with `grabright on`
  and the gun would stay in that hand.
- Test command `grabtest any` takes the nearest grabbable object.

**Shared hit pipeline (gameplay plan, item 0, 24/10; no effects yet).**
Objects become hits on enemies and on world targets. Items 1 (thrown
objects hurt), 2 (glass and sand windows) and 4 (melee with held objects)
consume the events; for now the only output is the log.
- Who is followed: every tick, the objects in `g_falling` (released, thrown,
  knocked over, shot, broken loose: 4 s) and the held ones (not the hinged
  ones). `propcombat on|off` (default on, Basic menu "Objects as weapons");
  off in cinematics and while the game is locked.
- **Origin of a moving object** (review fix, 24/10). Every entry of
  `g_falling` now records where it came from (`FallingAdd`), and `PropHit`
  carries it as `source`, next to `kind` and `hand`:
  - `kThrown`: released by the hand at 1.5 m/s or more (`Release` with the
    grip).
  - `kDropped`: released more slowly, or for another reason (slipped,
    disabled).
  - `kGlove`: locked, flown or hovered by a glove, then let go.
  - `kShot`: made physical and pushed by Walker's bullet (`OnShot`).
  - `kKnocked`: knocked by a hand sphere (`ProxyKnockStatics`).
  - `kOther`: became physical some other way (a grab that failed).
  - `kHeld`: in the hand.

  `hand` is the hand that holds it (`kHeld`) or that threw, dropped, gloved
  or knocked it; it is -1 for `kShot` and `kOther`. Before this fix, every
  object in `g_falling` counted as thrown. A radio sent flying by a bullet
  would therefore have been "thrown" into an enemy, and item 1 would have
  hurt the enemy a second time. When the same component has two entries
  (made physical at the grab, then released), the newest origin is copied
  onto both. A new origin while the object is in flight also starts a new
  throw (hit memo).
- Targets:
  - **Enemies are decided by the game, not by the class name** (review fix,
    24/10). Before, any pawn whose class name did not contain Adams, Lugo,
    Walker, Civil or Refugee counted as an enemy. That included
    `YPawn_Riggs`, `YPawn_Guide`, `YPawn_FakeGould`, `MatineePawn`,
    `YPawn_Coop`, the vehicles and `YGamePawn_Menu`. Now, in order:
    1. `YGamePawn.m_squadMemberType` (+0x4BC, enum `YE_SquadMember`: Adams 0,
       Lugo 1, Walker 2, Daniels 3, Riggs 4, Gould 5, Fari 6, None 7). A named
       character is never a target. Read in memory: dummy 7, Lugo 1, Adams 0,
       Walker 2; summoned Riggs 4, FakeGould 5.
    2. If the pawn has a team: `Pawn.GetTeamNum` (non-opcode native, called
       through ProcessEvent; 255 = no team) indexes `YTeamInfo.m_standing[7]`
       (+0x23C, enum `YE_Standing`: Neutral 0, Friendly 1, Hostile 2) of
       Walker's team (`YGamePawn.GetYTeam`). Only Hostile is an enemy.
       - This is the table the game uses in `YGamePawn::IsHostile` (vtable
         +0x698 = 0x00CF0170, which calls FUN_0086ff90 through +0x818). In
         single player, that function considers everyone except the pawn
         itself hostile, so it is useless here.
       - Level 1 has 7 `YTeamInfo` (`TeamIndex` +0x20C). Walker, Lugo and
         Adams share team 0, whose `m_standing` is 1 2 1 0 1 2 2: teams 1, 5
         and 6 are hostile, 0, 2 and 4 friendly, 3 neutral. The squad's
         `GetTeamNum` returns 0. The summoned dummy returns 255 (no
         controller), and so does one given an `AIController` (no PRI).
    3. Without a team (no controller or no PRI: the summoned dummy, scene
       pawns): an enemy only if the class derives from `YPawn_Enemy` and its
       name contains none of Adams, Lugo, Walker, Riggs, Gould, Guide, Civil
       or Refugee (`YPawn_LugoCanDie` derives from `YPawn_Enemy`).

    The decision is made per pawn and redone every 2 s (two ProcessEvent
    calls at most). One log line per (class, team, standing, outcome):
    `PROPS: pawn <class>: squad S, team T, standing N (<why>) -> enemy|not a
    target`. The 88 Pawn classes loaded in level 1 split into `YPawn_Enemy`
    subclasses (marauders, elite, heavy, medium, `LugoCanDie`) and the rest:
    `YPawn_StealthRanger` (Walker, Adams, Lugo), `YPawn_Guide` (Riggs,
    Gould, FakeLugo), `YPawn_Civilian`, `YPawn_Multiplayer`, `MatineePawn`,
    `Vehicle`, `Scout` and `YGamePawn_Menu`.
  - Enemies come from `WorldInfo.PawnList` (+0x4F4, `NextPawn` +0x218):
    alive (`Health` +0x304 > 0), within 15 m, not hidden. Cylinder from
    `Pawn.CylinderComponent` +0x3AC (`CollisionHeight` +0x200,
    `CollisionRadius` +0x204).
  - **World targets** come from the incremental `Scan` (no GObjects pass of
    its own). A target is within 15 m of Walker, measured from the box, not
    the centre. The old 20 m cap on the radius is gone, so big targets now
    count. Each target keeps the box of its `CollisionComponent`.
    - `FracturedStaticMeshActor` is any fracturable mesh, not only glass:
      kind `kTgtFractured` (was `kTgtGlass`). It includes its subclasses
      `PhysXDestructibleActor`, `YFracturedSMActorMovable` and
      `FracturedSMActorSpawnable`.
    - `FracturedStaticMeshPart` (also a subclass) is excluded: these are the
      loose chunks of a broken mesh. Level 1 keeps a pool of 51 of them
      9.4 km away.
    - `YSandBomb` (a `StaticMeshActor`) and `YDestructibleObject` with its
      subclasses (an `Actor`, not a `KActor`).
    - Class default objects and archetypes are skipped by `ObjectFlags` +0x8
      (`RF_ClassDefaultObject` 0x200, `RF_ArchetypeObject` 0x400; defaults
      read 0x104200). Their template `CollisionComponent` is not null, as the
      old comment assumed.
    - The component must be attached (+0x50 bit 0). In a streaming level
      that is loaded but not visible, components are detached with zero
      bounds. That is why the 4 `YDestructibleObject` of level 1 showed zero
      bounds from CP04: they are in `SP_Desert01_03_K`, loaded but hidden
      (`LevelStreaming` flags +0x60 = 0x408). Setting bShouldBeLoaded and
      bShouldBeVisible (0x180) from Python made it visible with no scripted
      event (`f0r/showlevel.py`). After that, `PROPS: scan` lists them:
      `2 hit targets (...; 2 glass)` near the bus, `4 hit targets` on the
      coach.
    - `glass`: the target's mesh name contains "glass" (cached per mesh; the
      bus panes `m01a_busGlass_*`). At hit time the event's `glass` also
      comes from the trace's `FCheckResult.PhysMaterial` (+0x2C) or
      `Material` (+0x28). The coach panes `m01a_166_166_single` have no
      "glass" in their name, but the zero-extent trace returns
      `GlassWindow01_PHYSMAT`. Their `RB_BodySetup.PhysMaterial` (+0x90) is
      null, so the material comes from the mesh's material.
- Motion segment per object and tick:
  - In flight: from the previous centre, along the previous tick's PhysX
    velocity (`NxActor` +0xE8). `Actor.Velocity` +0x140 is a tick late.
    The segment is as long as the travel (or the real displacement, if
    longer) plus the object's half-size along the motion. The object
    bounces off the enemy's kinematic capsule (with or without
    `bIgnoreRigidBodyPawns`, see below), so its centre never gets inside.
    - **Approaching and not already touching** (review fix): an enemy is a
      candidate only if the point moves towards the nearest point of its
      axis. The point must also not already be in contact (within the radius
      plus the object's box projected towards the axis, and the half height
      plus the box's vertical half-size, 1 unit of margin).
      - Before, an object resting against the dummy, or pushed by an enemy
        walking into it during its 4 s in `g_falling`, started its trace
        touching the capsule.
      - The look-ahead (half-size along the motion minus the trace box, plus
        2 units) already covers the contact from the tick before, so a
        throw is not lost.
    - **Ends of long objects** (review fix): objects whose major axis is
      over 16 cm and 1.5 times the second (`LongAxisEnds`, the same test as
      the held tip) also sweep their two ends. Each end moves at its own
      velocity: the centre's plus the PhysX angular velocity of the tick
      before times the arm. The trace box is at most 4 units. A bottle
      thrown sideways with its centre 22-25 units from the axis (outside the
      centre's trace) now hits with an end.
  - In the hand: from the real previous position, along the velocity the
    hand gives that point. That is the hand velocity over 30 ms plus the
    wrist's angular velocity (from `Drive`) times the lever arm. It runs for
    the centre, and for the tip of long objects.
    - PhysX velocity is useless here. A radio pressed against an enemy shook,
      PhysX reported 24 m/s with the hand still, and the result was a fake
      hit every 0.5 s.
    - A sweep needs at least 1.5 m/s at the point.
    - **Tracking jumps** (review fix): a sweep is skipped only when the hand
      moved faster than 25 m/s over at least 11 ms in the last 50 ms
      (`HandJumped`). Before, any hand over 10 m/s (30 ms average) was
      dropped, so the hardest real swings would never hit (item 4).
      Consecutive ticks are not enough: without a headset the game runs
      ~225 ticks/s and the fake hand updates less often, so a 12 m/s swing
      looks like 48 m/s between two ticks.
    - **Wrist velocity on a re-grab** (review fix): `Drive` resets the
      wrist's angular velocity at every grab (`Held.wristInit`), and
      `Release` zeroes `g_heldHandW`. Before, re-grabbing the same object
      with the same hand measured the rotation since the previous hold over
      a single tick. At 225 ticks/s an 80-degree turn would read ~160 rad/s
      (1.4 rad in one 4.4 ms tick, halved by the smoothing), a fake swing of
      several m/s.
- Hit test:
  - First a cheap check: segment-to-axis distance against each enemy
    cylinder, and box overlap against the world targets.
  - Only if there is a candidate, the engine trace runs:
    `UWorld::SingleLineCheck` 0x009BDE30 (the one `TraceLine` uses).
    - Source = the object, so it ignores itself.
    - Box extent = the object's smallest half-size, at most 8 units.
    - Flags 0x2097 (Pawns, Movers, Level, Others, LevelGeometry, Terrain;
      no StopAtAnyHit, so the nearest hit wins).
    - Walker, the hand spheres and the held objects are skipped (the trace
      restarts beyond them).
    - A wall in between means no hit.
    - Since item 4 (26/10): actors that block nothing (bBlockActors off, such
      as weapons on the ground) are traced through (`TracePassThrough`); held
      objects also reach an enemy's arms (`kHeldArmReach`, zero-extent
      trace), and a slow first contact no longer uses up the swing
      (`HitMemo.speed`). See "Melee with held objects" below.
  - With an extent, the trace hits the enemy's `CylinderComponent`
    (BlockNonZeroExtent only).
  - **Near a world target, a zero-extent trace too** (review fix), when the
    box trace found no target. Some targets block only zero-extent traces:
    the coach panes of level 1 have `PrimitiveComponent` +0x14C = 0xC3002D
    (BlockZeroExtent, no BlockNonZeroExtent) and a PhysX body. The second
    trace's target counts only if it is not beyond whatever stopped the box
    trace.
  - The target's kind and `glass` come from the `Scan` list (or from the
    class, if the target is not in it yet).
- Bone:
  - Two zero-extent traces hit the `SkeletalMeshComponent` through its
    PhysicsAsset (BlockZeroExtent): first along the motion, then
    horizontally towards the enemy's axis at the impact height.
  - Their bone counts only if the body hit is within the object's half-size
    plus 12 units of the contact. A radio dropped on the head reached
    `Spine3` 22 units lower, behind the head.
  - Otherwise, the PhysicsAsset body nearest to the contact
    (`PhysicsAsset` +0x258 -> `BodySetup` TArray +0x40 ->
    `RB_BodySetup.BoneName` +0x84; positions from
    `SkeletalMeshComponent.GetBoneLocation`, non-opcode native).
    `FindClosestBone` counts every bone and gave `Camera_Bone_Left` and
    `FACE_LOBR` on the dummy.
- **FCheckResult in this build:** Actor +4, Location +8 (centre of the
  trace box at impact), Normal +0x14, Time +0x20, Item +0x24 (PhysicsAsset
  body index), Material +0x28, PhysMaterial +0x2C, Component +0x30,
  **BoneName +0x38**, Level +0x40, LevelIndex +0x44. In stock UE3 BoneName is
  at +0x34, but here +0x34 stays 0.
  - Verified on the dummy: Item 2, `BodySetup[2].BoneName` = `Spine3` = the
    name at +0x38.
  - PhysMaterial verified on the coach glass (`GlassWindow01_PHYSMAT`,
    zero-extent trace; Material was null there).
  - `SingleLineCheck` copies 0x4C bytes (Ghidra).
  - The `hittest` diagnostic in hooks.cpp still reads +0x34, so it never
    shows a bone.
- Event `PropHit`:
  - the object (actor, component), mass (`GetBodyMass`);
  - the velocity of the hitting point before the impact, and its speed;
  - the target (actor, component, kind: pawn / fractured mesh / sand window
    / destructible) and `glass`;
  - contact point and normal, direction of motion, bone (FName, and which
    of the three methods found it);
  - kind (`kFlying` or `kHeld`), `source` (origin, above), hand, and `tip`
    (held: the far tip; in flight: one of the two ends).

  Events are handled after every object has been tested and before this
  tick's impact breakage (Source does the same and keeps damage outside the
  physics step): `OnPropHit`. Log line:
  `PROPS: hit <class> bone <b> (<trace|side trace|closest body>) with <mesh>
  (<source>[, left|right hand][, tip|end]) at X m/s, mass M, at (x, y, z)`.
  For a world target, `<class> (<fractured mesh|sand window|destructible>[,
  glass])` replaces the bone.
- Limits:
  - One hit per throw per target. A held swing also gets one hit per
    target; the swing ends when its fastest point drops under 1 m/s. That
    fixed two hits 0.5 s apart from one swing, when the object was stopped
    by the enemy while the hand went on. Since the item-4 review (26/10)
    this speed is measured relative to Walker (point velocity minus
    `FrameVelocity`): while he walks with the hand still the swing ends, and
    the next real swing is a new one. The sweeps themselves still use the
    world speed (the bump happens: `PROPS: hit` and the generic pulse).
  - On top of that, at most one hit every 0.5 s per (object, target) pair.
- `bIgnoreRigidBodyPawns` fix, as the plan asks:
  - `Release` and the glove now restore the actor flags (+0x9C bit 0x200
    and `bBlockActors`) as soon as the object's box is out of Walker's
    capsule (5-unit margin, no `RBPushRadius`, because the push body is
    pair-ignored too). Log: `GRAB: <object> solid for pawns again 0.00 s
    after release`.
  - Only the `NX_IGNORE_PAIR` with Walker's bodies (`IgnoreWalker`) lasts
    the 0.5 s.
  - **Finding:** in this exe the flag is read only by FUN_008ba520, a
    `Pawn::IgnoreBlockingBy`-like virtual. It means "a Pawn in
    PHYS_RigidBody ignores this actor" (scan of `.text` for every bit-9
    test on +0x9C). So it never made thrown objects pass through living
    enemies. Their `YPawnKinematicRBCollider` capsule (an NxActor with no
    userData) blocks either way.
  - A/B on the dummy, with the flag forced on for 0.5 s from Python
    (`hitdrop2.py` with `OLDFLAG=1`):
    - a radio dropped on the head rests on the head (z 738, head top 731)
      with the fix and without it;
    - dropped 22 units off-axis, it slides down the side both ways.
  - The earlier "radio sank to the chest" case at the hostage scene was an
    off-axis drop sliding down the side.
- Measured on level 1 CP04 (novr, god mode, `Summon
  SRGame.YPawn_MarauderAssault`). The dummy is blue because its mesh
  package is not loaded, but it has the cylinder, the kinematic and springy
  colliders and the `CH_ENEMY_SK_PHYS` PhysicsAsset (16 bodies).
  - Throw at the chest from 3.5 m:
    `hit YPawn_MarauderAssault bone Spine3 (trace) ... (thrown) at 6.6 m/s,
    mass 0.128`, contact 19 units above the pawn centre. The radio bounced:
    x velocity went from +6.3 to -1.9 m/s at 27 units from the axis (the
    capsule's 15 plus the radio's half-size). It then fell 1.2 m from the
    enemy.
  - Radio dropped from 40 units above the head: `bone head (closest body)
    at 4.7 m/s` in one run, `Spine3 (trace)` in another, and the radio rests
    on the head. It rests on the top of the invisible capsule (z 738, top
    731). The dummy's visible head is 10-20 cm lower (it stands in an aiming
    crouch), so in the screenshot the radio floats above it. Item 1 should
    keep this in mind: thrown objects bounce off the capsule, not the mesh.
  - Held radio swung into the chest at 3 m/s: one hit per swing
    (`Spine1`, 2.8-3.4 m/s). With the radio pressed against the enemy and
    the hand still for 2.5 s: none. Moving at 1 m/s or 0.5 m/s: none.
  - Summoned `YPawn_Lugo` (friendly): the radio rests on his head and there
    is no hit and no candidate.
  - After the review fixes (same setup):
    - Throw: `hit YPawn_MarauderAssault bone Spine3 (trace) with
      radio_unit_04 (thrown, left hand) at 6.1 m/s`.
    - Drop on the head: `... (dropped, left hand) at 4.1 m/s`, one hit line
      in 8.5 s while the radio settles on the capsule top.
    - Held swings at 3 m/s: one hit per swing (`held, left hand`, 2.9 and
      3.1 m/s), none pressed, none slow.
    - Held swing at 12 m/s (`f0r/fastswing.py`): one hit at 11.7 m/s. The
      old 10 m/s cap would have skipped it.
    - Re-grab after a release and an 80-degree hand turn (`f0r/regrab.py`):
      the wrist reads 0.0 rad/s on the first 6 `Drive` ticks, with no hit.
    - The dummy written forward at 1.5 m/s into a radio resting 52 units
      away (`f0r/walkinto.py`): the radio was pushed 90 units, with no
      candidate and no hit.
    - Bottle (`bottle_round02`) thrown with its centre 22-25 units from the
      axis (`f0r/throwat.py`): `hit ... with bottle_round02 (thrown, left
      hand, end) at 5.1 m/s`.
    - Hostility (`f0r/sides.py`, `f0r/teamtest.py`): summoned
      `MarauderRifle` with an `AIController` is `no team, enemy class ->
      enemy`. `CivilianMale` and `Guide` are `no team, not an enemy class`.
      `Riggs` (squad 4), `FakeGould` (5) and `Lugo` (1) are `squad member`.
      The dummy given a spawned `YPlayerReplicationInfo` with team 1, 2, 3:
      `GetTeamNum` returns 1, 2, 3, giving `standing 2 -> enemy`,
      `standing 1 -> not a target` and `standing 0 -> not a target`.
    - Coach glass (`SP_Desert01_03_K` made visible, Walker on the coach
      side): `hit YDestructibleObject (destructible, glass) with
      radio_unit_04 (thrown, left hand) at 9.5 m/s`, found by the
      zero-extent trace (`GlassWindow01_PHYSMAT`).
    - Bus glass (`YDestructibleObjectDelayed`, `m01a_busGlass_*`): listed as
      targets (2 glass), but 3 throws were stopped by the scripted scene's
      `InterpActor`s. The sand `ground_edge_01` blocks the box trace, and
      `dubai_bus_wrecked` blocks the zero-extent one. No hit.
- Cost (`PROPS: hits ...` every 30 s, next to `PROPS: tick`): 10 dummies
  within 6 m, 12 throws in 20 s, up to 4 objects followed.
  - 0.0022 ms per tick with objects in motion; 0.0011 ms per tick on average.
  - 0.08 ms at most: a tick with a hit, its bone traces and the log line.
  - 103 traces in 30 s.
  - `PROPS: tick` stays at 0.155-0.165 ms without `spawnprop`.
  - No string building per tick: class information is cached per class and
    the hostility per pawn (2 s), and names are built only for the log line
    of a hit or a `hitlog` line.
  - After the review (1 dummy, throws and held swings):
    - 0.0037-0.0047 ms per tick with objects in motion; 0.0005-0.0008 ms
      per tick on average.
    - 0.04 ms at most in a window with 2 held hits and 106 traces.
    - 0.40 ms at most in the window with the first hostility decision of the
      session and a `closest body` bone (16 `GetBoneLocation` calls).
    - An earlier build looked up the class names (`YGamePawn`,
      `YPawn_Enemy`, `YTeamInfo`) in GNames on the first decision and
      peaked at 0.95 ms. They are now looked up at the start of a `Scan`
      pass, with the other target names.
    - `PROPS: tick` 0.148-0.163 ms on average, 0.42 ms at most in a window
      without `spawnprop`. Its 8.6-18.7 ms peaks are the test-only full
      rescan after `spawnprop`.
- Not verified:
  - `FracturedStaticMeshActor` and `YSandBomb`: level 1 has neither (only
    class defaults and the part pool). Whether their traces and materials
    behave like the coach glass is left to item 2, on a map that has them.
  - The bus glass of level 1 (see above): a hit there needs the scene state
    in which the sand and the bus are out of the way.
  - Real AI enemies with a team (the hostile `m_standing` path was exercised
    only with a PRI spawned onto the dummy), and real AI in combat.
  - A hit with the `kShot`, `kKnocked` or `kGlove` origin. They go through
    the same `FallingAdd` as thrown and dropped.
    - The only static prop near CP04 that `OnShot` handles is the level 1
      radio, and it sits beyond a pawn-blocking volume. `f0r/shotinto.py`
      put the dummy behind it, 35 units away, and shot it:
      `PROPS: shot moved radio_unit_04`, then `HITLOG: radio_unit_04 flying
      (shot) at 1.0 m/s, candidate YPawn_MarauderAssault 26 units from its
      axis -> trace hit YBlockingVolume (not a target)`.
    - So the origin reaches the pipeline, and the wall in between correctly
      gives no hit.
    - Knock and glove were not run against an enemy: there is no small
      static prop near the dummy.
  - The headset.
- Regressions (`suite.sh`): grasp, drive, static grab, glove, knocking the
  bottle over, breaking on a drop and on a smash, the shot, and the door
  (55-59 degrees) all behave as before.
  - After the review: the same, with the door at 53.1 degrees and the
    bottle knocked 21 cm.
  - The shot test printed "moved 0.0 cm". Its first aiming shot had already
    hit the radio (`PROPS: shot moved radio_unit_04`), and the test measures
    from just before the last shot.
  - The fuel barrel did not move, but the pre-feature dll (rebuilt from the
    snapshot) does the same.
  - The stop sign turned 5 degrees in the suite. It is flaky with both dlls:
    38.6 or 4.7 / 4.0 degrees with the pre-feature dll, and 38.6, 12.9, 5.6
    or 4.7 degrees with the new one, depending on the run and on what the
    tests before it did.
- Diagnostics: `hitlog N` logs the next N contact tests: origin,
  candidate, distance from the axis, what the trace hit, and the bone
  traces. Near a world target it also logs each trace: actor, mesh,
  material, phys material, and the distance along the motion. `grablog`
  lines now end with the wrist's angular velocity.
- Test setup: the dummy above, plus the scratch scripts `hitthrow.py`,
  `hitdrop2.py`, `heldswing.py`, `multithrow.py`, `summonline.py` and
  `tpnear.py`. The review added `f0r/throwat.py` (throw with aim
  correction, at a pawn or at a point), `f0r/dropon.py`, `f0r/fastswing.py`,
  `f0r/regrab.py`, `f0r/walkinto.py`, `f0r/sides.py`, `f0r/teamtest.py`,
  `f0r/showlevel.py` and `f0r/lookat.py`.

**Boneworks plan, item 1: mass units and the release snag (24/10).**
- **Mass units.** `GetBodyMass` (through `GetRootBodyInstance`) returns the
  PhysX mass in engine units, not kg (`URB_BodyInstance_execGetBodyMass` @
  0x00A126E0 -> 0x00C34C60 = `NxActor::getMass`, vtable +0xB8). `Grab`
  compared it with 15 kg, so the heavy-object speed limit never applied:
  every object, barrels included, could be driven at 30 m/s.
  - Now `MassKg(held)` = units x `masskg`. The default is 40; it is an ini
    key, range 5-500.
  - `maxSpeed` (`HeldMaxSpeed`) = 30 m/s x min(1, 15 / kg), but never under
    `griptune minspeed` (5 m/s). The release cap (1.5 x `maxSpeed`) follows
    it. Both caps apply relative to Walker (see "Speed cap in Walker's
    frame" below).
  - `held.mass` stays in engine units, because `ConstrainedDrive`'s
    `AddImpulse` needs them.
- **Floor and warning (review).** Without a floor the cap fell to 2.25 m/s at
  200 kg and to ~1.1 m/s for the 0.83-unit barrel at `masskg 500` (the
  Advanced menu allows it). The hand then outran the object at normal
  speed and the 60 cm / 0.3 s slip rule dropped it: a refusal, which the
  user's rule forbids. Now the cap never goes under 5 m/s, and a grab over
  60 kg logs `GRAB: WARNING heavy object: 415 kg (over 60; masskg 500), max
  speed 5.0 m/s (floor 5.0)`. The heaviest grabbable objects seen in the
  level pass weigh ~45 kg at 40, so in normal play the floor never acts.
  - Test, `p1m/heavylift.py` with a spawned fuel barrel at `masskg 500`
    (415 kg): held still 0.1 cm; lifted at 0.5 m/s at most 0.8 cm; moved at
    3 m/s 4.2 cm on average (the 49.4 cm peak is the fake hand's 50 cm
    restart jump, the same as at 33 kg); settled 0.1 cm.
  - The same test with `griptune minspeed 0.5` (the old formula: 1.1 m/s):
    46.8 cm on average and 72.8 cm at most while the hand moved at 3 m/s,
    still 26.2 cm behind after the hand stopped.
- **Fallback.** When `GetBodyMass` cannot be read, the mass is 0.2 units
  (8 kg). It used to be 5.0 units, which would have made `ConstrainedDrive`
  push a door with 25 times the impulse. On level 1 the read never failed:
  the car door reads 0.843 units and the stop sign 0.082.
- **The two 0.200 readings are real masses (review).** The radio and the
  sandbag converted from statics both read exactly 0.200, the fallback's
  value. `MassReread` now reads `GetBodyMass` again 5 and 30 `Drive` ticks
  after the grab and logs it with the component's scale (the rows of
  `LocalToWorld`); if it differs by more than 5 % it updates the mass and
  `maxSpeed`. Measured with `p1m/reread.py` and `p1m/spawnscale.py`
  (`spawnprop <mesh> <cm> [scale | sx sy sz]` sets `Scale3D` before the mesh,
  so the body is created scaled):

  | Object | Scale (component `Scale3D`) | At the grab | 5 / 30 ticks later | Spawned at that scale |
  |---|---|---|---|---|
  | `radio_unit_04`, level | 1.25 | 0.200 | 0.200 / 0.200 | 0.200 (0.128 at 1) |
  | `sandbag_single_02`, level | 1 x 0.8 x 1.519 | 0.200 | 0.200 / 0.200 | 0.200 (0.181 at 1) |
  | `bottle_quad01`, level | 1 | 0.042 | 0.042 / 0.042 | 0.042 |
  | `427_fuel_barrel_03`, level | 1 | 0.831 | 0.831 / 0.831 | 0.831 |

  - The scale sits on the component (`Scale3D` +0x1DC); `DrawScale` (+0x6C)
    and `DrawScale3D` (+0x70) of the `StaticMeshCollectionActor` and of the
    `KActorFromStatic` are 1. `MakeDynamic` keeps the component, and its
    scale.
  - The engine's mass grows about with the square of the scale, not the
    cube: radio x1.25 -> x1.5625 (0.128 -> 0.200); x2 -> 0.528 (x4.1);
    sandbag x2 -> 0.744 (x4.1); bottle x2 -> 0.151; barrel x0.5 -> 0.196
    (x0.24); radio 1 x 1 x 2 -> 0.204. The two 0.200 values are a
    coincidence of two scales.
  - `SetScale` on a body that already exists changes the bounds (radius 33
    -> 68 cm) but not the mass (0.128): only a body created at that scale
    has the scaled mass.
- **Calibration.** The masses below were read on real props of level 1 at
  CP04 with `p1m/calib.py`. The script writes Walker's position next to each
  cluster and sends `grabtest <mesh>` without the grip, so the object is
  grabbed and dropped on the next tick. The bus seat was spawned with
  `spawnprop` at scale 1.

  | Mesh | Units | kg at 40 | Believable kg |
  |---|---|---|---|
  | `bottle_quad01`, `bottle_round02` | 0.042, 0.041 | 1.7, 1.6 | ~1.4 |
  | `radio_unit_04` (level, `Scale3D` 1.25) | 0.200 | 8.0 | 5-10 |
  | `radio_unit_04` (spawned, scale 1) | 0.128 | 5.1 | 5-10 |
  | `sandbag_single_02` (level, 1 x 0.8 x 1.519) | 0.200 | 8.0 | 15-20 |
  | `sandbag_single_02` (spawned, scale 1) | 0.181 | 7.2 | 15-20 |
  | `busdepot_travelbag` | 0.400 | 16.0 | ~12 |
  | `suitcase_02_open` | 0.419 | 16.8 | ~14 |
  | `busdepot_sleeping_bag` | 0.335 | 13.4 | 2-3 |
  | `dubai_bus_chair` (spawned) | 0.57 | 22.8 | ~15 |
  | `427_fuel_barrel_03` | 0.831 | 33.2 | 20-60 (empty to part full) |

  - The engine gives every mesh the same density, so no single factor fits
    every object. The sleeping bag, for example, weighs about as much as a
    suitcase, and the sandbag is light.
  - Leaving out the sleeping bag, the geometric mean of the believable kg per
    unit in the table is ~37; 40 is the round value just above it. The plan's
    50 made the bottles 2.1 kg and the suitcases 21 kg.
  - The plan expected bottle 0.02, radio 0.2 and barrel 1.2 units (1, 10 and
    60 kg at 50). On level 1 the bottles read 0.04 and the barrel 0.83.
  - The level pass found barrels of 1.0-1.1 units in other levels, so the
    heaviest grabbable objects weigh ~40-45 kg at 40.
  - At 40, `maxSpeed` is 30 m/s up to 15 kg. Above that it is 28.1 m/s for
    the travel bag, 26.8 for the suitcase, 19.7 for the bus seat and 13.5 for
    the fuel barrel.
  - Every grab logs the mass, e.g. `GRAB: left hand took KActorFromStatic
    KActorFromStatic (427_fuel_barrel_03, mass 33.2 kg = 0.831 units, max
    speed 13.5 m/s, radius 81 cm)`. `(not read)` marks the fallback.
- **Still liftable.** A spawned fuel barrel (33.2 kg), measured with
  `p1m/heavylift.py`:
  - held still: error 0.1 cm;
  - lifted at 0.5 m/s: 0.8 cm at most;
  - moved at 3 m/s: 0.1-3.6 cm, the same jitter the fake novr hand gives
    every object.

  The 13.5 m/s cap was reached only once, on a 50 cm jump of the fake hand
  (`synthleftvel` restarts from the base pose); the barrel caught up in
  25 ms. A `grabtest` from 6 m away now ends with the barrel slipping out of
  the hand: at 13.5 m/s it needs 0.45 s to arrive, and the slip rule waits
  0.3 s. Only the test command does this; in play the grab zone is 8 cm.
- **Speed cap in Walker's frame (review).** The hand positions are in the
  world, so Walker's own motion used to count against the cap: running with
  a heavy object and swinging the hand could exceed it, and a throw while
  running was clipped more for heavy objects.
  - `CapScale(v, pawnVel, cap)` returns the largest k in 0..1 with
    |k v - pawnVel| <= cap, but never less than the old world cap (cap / |v|),
    and it only scales v: it never adds velocity off v's direction. `Drive`
    and `Release` use it. `griptune pawnframe 0` gives the old world-frame
    cap, for comparison.
  - `PawnVelocity` is Walker's `Actor.Velocity` (+0x140), at most 20 m/s.
    Not the change of his location: running on level 1's dunes, the
    location jumps up and down at the terrain steps (up to 14-15 m/s
    vertically for a few ticks), while the camera, and the hands with it,
    smooth them out. `Actor.Velocity` stays smooth, except for one tick at
    the start of the run (over 20 m/s, then 3.8). With "cap v - pawnVel,
    then add pawnVel back" that tick would have pushed a heavy object
    sideways; with `CapScale` it can only allow the full command for one
    tick.
  - The `Drive` GRABLOG line now ends with `pawn v (...) moved (...)` (the
    velocity used and the one from the location, over 30 ms) and `| capped`.
  - Test, `p1m/sprint.py`: spawned fuel barrel in the fake left hand,
    `pad fake 0 1 ...` forward for 3 s (Walker at 3.7-3.8 m/s; holding A
    gave the same speed, so no faster sprint was available in novr), the
    hand swinging back and forth at 3 m/s; `p1m/sprintan.py` counts the
    ticks where Walker moves faster than 1 m/s.

    | Mass, frame | Error mean / max while running | Ticks over 20 cm | Capped ticks |
    |---|---|---|---|
    | 415 kg (`masskg 500`, 5 m/s), world (old) | 14.2 / 59.3 cm | 226 of 777 | 360 |
    | 415 kg, Walker | 1.0 / 5.2 cm | 0 of 782 | 94 |
    | 33 kg (13.5 m/s), world (old) | 0.9 / 6.1 cm | 0 of 741 | 3 |
    | 33 kg, Walker | 1.0 / 8.5 cm | 0 of 769 | 0 |

    No slip and no release in any run. At the real masses the cap hardly
    ever binds; the 415 kg runs show what the frame changes. Cover slides,
    mantles and scripted moves were not tested.
- **Release snag (confirmed, fixed).** While an object is held, its pair with
  the hand spheres is ignored (`ProxyPair`). On release the pair came back
  at once above 1 m/s (the slap rule). The sphere (5 cm radius, on the palm)
  sits inside an object held in the power grip, so PhysX pushed the object
  out on its first step and the throw went off course.
  - **Fix:** `Release` stores the component in `g_relGrace[sphere][hand]`
    for both spheres, and `ProxyTick` keeps the pair ignored whatever the
    hand speed. For each sphere the grace ends when the object's box is more
    than the sphere radius + 6 units (17 cm) from that sphere's palm, or
    after `griptune relgrace` seconds (0.3). After that the usual rules
    apply. The second sphere is the gun hand's (or the other free hand's):
    a left-hand throw that starts next to the two-handed grip is not
    knocked by it either.
  - **Review fixes:** expired records are dropped at the top of `ProxyTick`
    (before, a record lingered until the object left the sphere's ignored
    list, and fired seconds later with lines like `3.039 s after release` or
    `8698.8 cm from the palm`). `relgrace 0` now sets no record at all:
    before, the pair still stayed ignored in the release tick, because
    `Release` runs later in the tick than the `now` that `ProxyTick` gets.
  - **Grace log:** `GRAB: left hand sphere ends the release grace X s after
    release, object Y cm from the palm (out of reach|timeout|no bounds,
    released by the left hand)`, only when the grace kept a pair ignored
    that the slap rule would have made solid; the first 20 times or under
    `grablog`.
  - **Throw log:** the throw check logs the PhysX velocity 3 ticks after
    release and again at 0.1 s. Each line gives the angle from the release
    velocity, both with the gravity of the elapsed time added back and
    without it, and how far the object moved: `GRAB: thrown object flies at
    4.95 m/s (wanted 5.00), 0.0 deg off the throw (2.0 before gravity) after
    12 ms, moved 6 cm (dz 1)`. Below 0.5 m/s (a drop) there is no angle.
  - **Test:** `p1m/throwdir.py <mesh> <relgrace>`.
    - Each trial spawns the object and grabs it with `grabtest` in the fake
      left hand, moves the hand with `synthleftvel` at 3 m/s for 330 ms, and
      releases at 250 ms.
    - There are 10 directions: forward, forward-up, sideways both ways,
      diagonals, up, and outward-left.
    - The head turns 36 degrees per trial, so thrown objects do not lie on
      the next path.
    - The fake hand reached 2.2-6.0 m/s at release.

    | Object, `relgrace` | Within 5 deg at 3 ticks | Max | Within 5 deg at 0.1 s | Max | Speed / wanted |
    |---|---|---|---|---|---|
    | bottle (1.6 kg), 0 (old code path) | 3/10 | 20.3 deg | 3/10 | 41.7 deg | 0.62-2.78 |
    | bottle, 0.3 | 10/10 | 0.1 deg | 10/10 | 0.5 deg | 0.97-1.00 |
    | radio (5.1 kg), 0.3 | 10/10 | 0.1 deg | 10/10 | 0.4 deg | 0.95-1.00 |

    - Before the review fixes (one tick of grace left at 0) the bottle at 0
      gave 5-6/10 and 54 degrees at most; one throw flew at 1.1 m/s after
      0.1 s instead of 5.1.
    - With 0.3 the grace ended by distance after 0.10-0.22 s for the bottle
      (17-19 cm from the palm). In an earlier run one bottle thrown straight
      up at 2.8 m/s reached the 0.30 s timeout 12 cm from the palm.
    - The second sphere's grace was not exercised: in these tests the gun
      hand never moved faster than 1 m/s next to a released object.
- **Advanced menu for existing installs (review).** `masskg` showed up in the
  Advanced menu only on a fresh install: `install.bat` and the updater keep
  the user's `specopsvr.ini`, and `ApplyDefaultsMissingFrom` applies missing
  defaults without writing them. The game's own test ini (23/10) had no
  `masskg`, `grabprops` or `grabzone` line. `LoadIni` now appends
  `vr::pure::DefaultLinesMissingFrom(userLines, DefaultIniText())`: each
  default key the user's ini lacks (by `ConfigKey`), with the comments just
  above it, shown with its default value; changing it writes the line
  (`SaveIniValue` appends it). Basic items whose key is missing now show the
  default too. Covered by `TestDefaultLinesMissing` in `tests/unit_tests.cpp`;
  the overlay itself cannot open without a headset, so it was not seen in
  the menu.
- **Unchanged:**
  - the gun hand and aiming (`HandTarget(0)`, barrel, muzzle, ADS,
    `CalcWeaponFire`): not touched;
  - the drive gains and `ConstrainedDrive`;
  - the slap rule for every other object;
  - drops: below 1 m/s the old rule already kept the pair ignored within 6
    units.
- **Cost:**
  - one `ReadBounds` per ignored pair with a grace, while it lasts; two
    `GetBodyMass` calls per grab; one `Actor.Velocity` read per held object
    and tick, and one 12-byte copy of Walker's location per tick (only the
    GRABLOG line reads it);
  - `PROPS: tick` 0.156-0.172 ms on average in the grab suite (before:
    0.149-0.170).
- **Hit pipeline after the change** (fresh relaunch, dummy at CP04, same
  scripts as item 0):
  - `hitthrow.py`: the held radio touched the dummy's head during the
    wind-up (`hit YPawn_MarauderAssault bone head (closest body) ... (held,
    left hand) at 2.0 m/s`); released at 5.7 m/s, it passed 7 cm from the
    axis at z 739, above the capsule top (731), so there was no thrown hit.
    The snapshot dll does exactly the same today (held head hit at 2.0 m/s,
    then z 738 and no thrown hit), so this is the test's geometry in this
    session, not this item.
  - `hitdrop2.py` (40 units above the head): `bone Spine3 (trace) ...
    (dropped, left hand) at 4.1 m/s`. The radio rests at z 738.
  - `heldswing.py` at 3 m/s: one hit per fast swing (`Spine1 (trace)`,
    2.8 m/s), none pressed, none slow.
  - `p1m/regrab2.py`: the same actor is re-grabbed, the wrist reads 0.0
    rad/s on the first 6 `Drive` ticks, and there is no hit.
- **Grab suite** (`suite.sh`, run twice with the final code: `p1f`, `p1g`):
  - **Same as before:** grasp (power grip, centre f -2.6 p 5.4 a 6.6 cm,
    turned 48 degrees), static grab (`bottle_quad01`, 1.7 kg), glove (flies
    1.8 m in 0.41 s, caught), bottle knocked 56 and 58 cm, barrel not
    knocked, bottle broken on the smash, stop sign 5.4 and 5.3 degrees
    (flaky as documented).
  - **Drive:** settle times 0.065/0.029/0.066/0.087/0.034/0.042 s and
    0.064/0.022/0.065/0.081/0.044/0.042 s, sweep error 2.1 and 1.9 cm;
    before, 0.063-0.080 s and 2.0 cm.
  - **Drop:** broke at 5.9 m/s in `p1g`, and at 6.2 m/s in a separate run
    from a fresh session. In `p1f` it did not break (largest change 2.8 m/s):
    that grab ended with the bottle tilted (the canonical grip turned it 85
    degrees from the grab pose; upright in the other runs), and it came to
    rest intact at the crate's edge (`p1f_dr/after.png`). The drop never
    reaches the speed cap or the pawn frame (Walker standing, 0.4 m/s).
  - **Shot:** the radio moved 133 cm in `p1f`. In `p1g` the log shows `PROPS:
    shot moved radio_unit_04` twice, while the script followed another
    radio instance and printed 0 cm.
  - **Door:** turned 60.1 and 48.9 degrees, no slip-off.
- **Door, controlled A/B (review).** The earlier door evidence compared
  single pulls from whatever angle the previous pull left. `p1m/doorreset.py`
  saves the door's pose in a fresh session (heading 9.4 degrees) and before
  each pull stops it, puts it back with `SetRBRotation` / `SetRBPosition`
  and stops it again (one `ucall` per command poll: the dll keeps only one
  pending, and the rest of a batch was lost). The door was at 9.4-11.5
  degrees at every grab. 5 pulls per arm, 40 cm/s for 1 s:

  | dll | Turned (deg) | Mean | Slip-offs |
  |---|---|---|---|
  | snapshot (before item 1) | 50.4, 58.6, 43.6, 58.6, 57.1 | 53.7 | 0/5 |
  | this item, `relgrace 0.3` | 42.1, 54.4, 58.7, 51.3, 37.4 | 48.8 | 0/5 |
  | this item, `relgrace 0` | 55.0, 41.9, 54.9, 37.5, 43.7 | 46.6 | 0/5 |

  - The spread inside each arm (37-59 degrees) is larger than the gap
    between the means; `ConstrainedDrive` reads neither `maxSpeed` nor the
    grace, and the door's mass (0.843) is read the same way.
  - No slip-off in 15 pulls from a fixed start: the earlier suite's 32.3
    degrees and slip-off came from the door's starting state.

**Boneworks plan, item 2: weight (25/10; review fixes the same day).**
Heavy objects in the free hand now lag, sag, turn slowly and tilt when held
off-centre. User rule: with one free hand (the right one always holds the
gun) nothing grabbable may become impossible to lift or turn, so weight is lag
and slowness, never a refusal. The gun hand and aiming are not touched
(`HandTarget(0)`, `GunBarrel`, `MuzzleStart`, ADS, `CalcWeaponFire`).
- **Option.** `objectweight off|light|full` (Basic menu "Object weight",
  default `light`). The mode is fixed at the grab, so changing it with an
  object in the hand cannot make the object jump. `off` runs the old `Drive`
  code path unchanged (no sag, no droop, no frame jumps, old smash check).
  Hinged objects (`ConstrainedDrive`) always keep the old path.
- **Tuning.** Values are for `full`. `light` doubles the force, the torque,
  the lift floor and the turn floor, and halves sag and droop.

  | ini / `griptune` | Default | Meaning |
  |---|---|---|
  | `weightforce` / `force` | 200 N | The hand's strongest pull, gravity included |
  | `weighttorque` / `torque` | 12.5 N m | The hand's strongest twist |
  | `weightturn` / `alphamin` | 3 rad/s2 | Angular acceleration floor, for any mass and size |
  | `weightsag` / `klin` | 2000 N/m | Sag = kg x 9.81 / klin |
  | `weightdroop` / `krot` | 60 N m/rad | Droop = gravity torque / krot |
  | `sagcap`, `droopcap` | 6 cm, 20 degrees | Caps |
  | `liftmin` | 3 m/s2 | The upward acceleration net of gravity never drops below this, for any mass |
  | `brake` | 0.7 | Share of the real deceleration the safe stop counts on (margin for the one-tick pose delay) |
  | `overshoot`, `overtime` | 2 cm (x 2.5 = degrees), 15 ms | Overshoot allowed when the hand stops dead |
  | `pawnacc` | 60 m/s2 | Walker's acceleration given to the object for free |
  | `framevel` | 1 | Walker's velocity from the hand frame's centre (0 = `Actor.Velocity`, as before) |
  | `fingerreach` | 3 cm (x 5 = degrees) | How far the fingers follow the real object away from the target pose |

  - **History of the force value.** The plan proposed 1200 N. With item 1's
    masses (radio 8 kg, rifle case 52 kg) 1200 N made the radio settle in
    30 ms at `full`, with no felt delay, so the first version used 400 N
    (`light` 800 N). The review found that the default (`light`) then missed
    the plan's numbers (radio 26-38 ms instead of 60-120; barrel turning at
    12-18 rad/s2 instead of at most 8): only `full` met them. Force and
    torque are now halved, so `light` behaves like the old `full`.
  - With 200 N, anything over ~11 kg (`light`: ~19 kg) reaches the lift floor
    and rises at 3 m/s2 net (`light`: 6).
- **Heaviest grabbable prop in level 1.** `rifle_case_01`, the instance next to
  the level radio: 52.0 kg (1.299 units at `masskg 40`). Its mesh bounds are a
  1.38 x 0.86 x 1.0 m box. The two other instances have a bounding sphere of
  57 units, over the 55-unit limit, so they cannot be grabbed. A spawned copy
  at scale 0.9 has the same mass (65.2 kg at scale 1). The fuel barrel is
  33.2 kg; one larger-scale instance beside the bottle crate is 49.1 kg.
- **Control law (`Drive`, weight path), per tick:**
  1. **Target.** `HeldTarget` applies sag and droop (`WeightPose`), scaled by
     the arrival blend so the grab does not jump.
     - Sag: the target moves down by kg x 9.81 / klin.
     - Droop: the target turns about the grip point by |torque| / krot, with
       torque = (centre of the bounds - grip) x m g. The grip point is the
       canonical anchor, or the palm for palm grips.
  2. **Wanted velocity.** Hand velocity + wrist spin + gain x error / dt, as
     in the old path.
  3. **Walker's velocity (`FrameVelocity`).** The velocity of the hand frame's
     centre (`SetHandFrame`) over the last 30 ms, at most 20 m/s;
     `Actor.Velocity` only as a fallback.
     - `Actor.Velocity` has one-tick spikes (20 m/s at the start of a run,
       then 3.8) and leaves out moving bases. In the safe stop below, which
       works relative to Walker, a spike asked for the object at ~19 m/s, and
       the force budget pushed it that way for a tick. With the velocity
       forced to 20 m/s for 0.6 s from outside (`p2r/velspike.py`) the radio
       flew ahead and slipped out of the hand (error 250 cm). With the frame
       centre: error at most 2.6 cm, no slip.
     - The centre is what the hands really follow: terrain steps are already
       smoothed by the camera.
     - Its history is carried by frame jumps like the hand history, so a jump
       is not read as a velocity.
  4. **Safe stop (`SafeStopCap`).** Relative to Walker, the wanted speed is
     capped so the object can still brake within the remaining error plus
     the allowance: |v| <= max(sqrt(2 aB (e + 2 cm)), aB T + sqrt((aB T)^2 +
     2 aB e)), with T = 15 ms and aB = brake x the deceleration available in
     the direction of motion (`BrakeAccel`: gravity helps stop a rise and
     hinders stopping a descent). If the hand stops dead, the object stops
     too.
     - Rotation uses the same cap, with the box inertia about the spin axis
       and the angular floor.
     - The cap acts on the whole wanted velocity (hand motion included), in
       every direction. Two weaker versions were tried first: braking only
       the error term let the rifle case overshoot 20-57 cm, and capping only
       the component towards the target let it fly sideways past.
     - Also tried: subtracting one tick of travel (v dt) for the pose delay.
       At 90 Hz the barrel's overshoot at `full` did not change (6-7 cm), so
       it was dropped. The overshoot comes from the force shared with the
       vertical correction (see "Measured").
  5. **Base velocity.** The PhysX velocity read before this tick's command
     (`NxVelocities`). `SetRBLinearVelocity` takes effect at once, so this
     read is the result of the last step.
     - Energy added by contacts is cut off: the base is at most the expected
       velocity (last command + one step of gravity) + 0.5 m/s, and the
       angular base at most the last command + 1 rad/s.
     - For 0.2 s after a frame jump, a measured velocity faster than the
       expected one is ignored (energy from the new penetration); a slower
       one is used. Before, the measured velocity was ignored entirely, so an
       object blocked after a carry saw its command grow to the safe-stop
       limit and left at that speed once free.
  6. **Force budget.** The change is at most aMax x dt, with
     aMax = max(force / kg, g + liftmin) (game gravity, 15 m/s2).
     - Gravity is served first, the rest goes to motion. When the budget was
       spent on the whole vector, the case pushed sideways sank at 0.8 m/s.
     - Walker's change of velocity (step 3) is free, capped at `pawnacc`.
     - Then item 1's `CapScale` cap, relative to the same Walker velocity.
  7. **Torque budget.** The angular change is limited by
     |I dw / dt| <= torque, with I the diagonal box inertia
     kg (e_j^2 + e_k^2) / 3 on the mesh axes (`WeightSetup`, from
     `MeshLocalBounds` and the component scale).
     - **Floor:** the change is never cut below `alphamin` x dt (3 rad/s2,
       `light` 6). Torque / inertia falls with mass and with the square of
       the size: the 52 kg case got 1.6 rad/s2 at `full`, and with
       `masskg 500` fractions of a rad/s2, so turning it was in effect
       refused.
     - Then the 55 rad/s cap.
  8. **Slip rule.** 80 cm above 30 kg (60 below). The 0.3 s timer runs only
     while the object is not moving towards the target at 0.25 m/s or more.
     Since the wedged-objects follow-up below, the window restarts when
     either the error has shrunk by 0.25 m/s on average since the window
     started, or the measured velocity towards the target, averaged over the
     window, is 0.25 m/s or more. A bottle vibrating against the crate (0.6 m/s
     "towards" its target every tick, error fixed at 131 cm) is let go by the
     position-based wedge rule (`WedgeStep`) instead.
  9. **Stored command.** `lastCmdV` / `lastCmdW` store the command after all
     the limits, for the smash check, the hit pipeline and the next tick.
- **Smash check (weight on).** The first tick compares the measured velocity
  with the command from before the impact (`lastCmdV`). The second compares
  it with the same command (`smashRef`): after the impact the command
  restarts from the measured velocity plus one tick of force.
  - With weight on, the command restarts from the measured velocity, so a
    command over 3 m/s means the object really moved that fast; a blocked
    object never gets there. So the hand only has to be in a swing: over
    1.5 m/s on the first tick (`off`: 3), and under 8 m/s (not a tracking
    jump) on the second.
  - The second tick also counts as stopped below 40% of the faster of the
    reference and the hand.
  - Why: the fake hand of the tests jumps between 4.9 and 2.4 m/s every 4
    ticks (`GetTickCount64` steps), and the gate fell on the low value. One
    traced miss (`p2r/smash2_light_3`): the bottle hit the crate at 3.2 m/s
    on a 2.4 m/s hand sample and was never checked. Another
    (`p2r/smf_2.log`): the first tick stopped (along -1.0 m/s against a
    5.2 m/s reference), and the second fell on a 2.4 m/s hand sample and was
    not checked.
  - `off` keeps the old check.
- **Release (weight on).** The throw starts from the object's own velocity
  (the last command) plus what the hand's force can add in the 60 ms of the
  hand-velocity average (aMax x 0.06 s). Before, a 52 kg case that followed
  the hand slowly jumped to the hand's speed when let go. Bottle and radio
  keep their throws: `light` gives a 14 and 3 m/s margin over their own
  velocity.
- **Held hits (weight on).** The hit pipeline's point velocity is the
  object's last command (`lastCmdV` + `lastCmdW` x arm), not the hand's. A
  heavy object lagging behind the hand hit at the hand's speed. The command
  is clean like the hand: pressed against an enemy, the base is the measured
  velocity and the command stays low.
- **Fingers (weight on).** `HeldShape` moves the target pose towards the real
  object by at most `fingerreach` (3 cm, 15 degrees). A small lag then shows
  as the object slipping a little in the grip. Beyond that the hand keeps a
  grip shape instead of closing into a fist on air. Until item 5 moves the
  drawn hand, a heavy object trails the hand while moving (case `light`:
  ~10 cm sideways, ~19 cm on a lift; `full` up to 37 cm) while the fingers
  stay on the controller. README says so.
- **View traces ignore held objects and hand spheres (`TraceLineView`,
  hooks.cpp; `props::IsHeldActor`).** The wall pull and the head collision
  trace past an object held in either hand and past both hand spheres. Each
  retry starts where the trace leaves the object's bounding sphere (at most
  30 units further per retry, 3 retries), so a wall behind still counts.
  - Found in the tests:
    - the rifle case held in front of the face pulled the view back, and the
      hands with it. The weighted case followed with a lag, and view and case
      oscillated by ~9 cm (`still` error std 23.5 mm, force limit every tick).
      After the fix: 0.0 mm;
    - a bottle held 35 cm in front and 15 cm below the eyes, on the low trace
      (10 units under the eye), pulled the view back 25 units (50 cm, `camlog`:
      `wall pull 25.0`). A first fix that retried 1 unit past the hit hit the
      same bottle 4 times and still counted it as a wall. With the
      bounding-sphere skip: `wall pull 0.0`.
    - When the hand moved away, the view came back forward in 0.2 s and
      moved the hand target with it. This was the "bimodal" sideways and
      lift settle times of the first round (~25 or ~100 ms, `off`
      included), wrongly put down to the body turning.
  - In the headset this was a general bug: holding something, or the left
    hand, in front of the chest pulled the view back.
  - Aim, muzzle and shot traces are unchanged (`TraceLine`).
  - `camlog N` now also logs `CAMLOG: view trace hit <actor> (<class>) at
    t=...` (ignored or not).
- **Frame jumps (`CheckFrameJump`, `FrameJump`).**
  - **Hand frame.** `UpdateArms` reports `SetHandFrame(center, bodyYaw,
    g_bodyYaw, refYaw, originOff)`:
    - `refYaw` is the yaw of the head reference (`g_hmdYawReference` x
      `g_yawSign`, in rotator units) used for this tick's hand offsets;
    - `originOff` is the offset of the tracking origin (from `posRef`) in
      the body frame.
    - Both are taken in `UpdateHeadTracking` together with the hand offsets
      (`g_trackRefYaw`, `g_trackOriginOff`); with fake poses they are 0.
  - **Effective frame.** A point fixed in the room maps to o + Rz(yaw -
    refYaw) T(x), with o the world position of the tracking origin.
    - Cover snaps, turret aim and the execution assist turn the body and the
      reference together: the effective yaw does not change, the hands stay
      still in the world, and it is not a jump.
    - The first version used the body yaw alone, so every game turn over 10
      degrees in a tick moved the held object, the spheres and the hand
      history about the head while the hands stayed put.
    - A recenter (reference and `posRef` change) moves the hands in the
      world and is carried.
  - **Trigger.** Before the hand history is updated, at a point next to the
    hand that holds something:
    - the effective yaw changes by more than 10 degrees; or
    - the point moves more than max(10 cm, Walker's speed x dt) beyond what
      Walker's motion explains.
    - "Explains" means the closest of: nothing, `Actor.Velocity` x dt,
      Walker's measured motion over 30 ms (`g_pawnHist`), or the frame
      centre's recent velocity x dt.
    - A frame older than 0.1 s is not compared (arms off, cinematics).
  - **What moves.** The transform is J(x) = o1 - e + Rz(dYaw) (x - o0), where
    e is the explained part: PhysX already moved the object by the commanded
    velocity, which contains Walker's. The first version moved everything by
    the full centre move, which counted Walker's motion twice (6.6 cm at
    6 m/s and 90 Hz), and the hand history lost Walker's velocity for 30 ms.
    J is applied to:
    - the held object (weight on): `SetRBPosition` / `SetRBRotation`, with
      the velocities, `lastCmdV`/`lastCmdW`/`smashRef` turned. A mirrored
      mesh (negative determinant) takes the actor's `Rotation` + the yaw
      change, because the normalised rows of a mirrored matrix gave a
      rotator 180 degrees off;
    - the hit pipeline's state of the held object (`tr->c`, tip, ends,
      velocities): before, the held-hit sweep started at the old position
      and swept the jump (up to ~46 cm);
    - both hand spheres and the hand history;
    - the wrist state (`g_wristPrevHa`, `g_wristW`, `g_heldHandW`, `g_prevTa`);
    - the frame centre's history (shifted by the centre's unexplained move).
  - **Walls (`WorldBlocked`).** Before carrying the object, a world-only
    trace (BSP, level meshes, terrain, movers) runs from the new hand
    position to the object's new centre. If it is blocked, the object is not
    carried: it stays driven, and the wall stops it, as before item 2.
    - Level meshes with a bounding sphere under 40 units (80 cm), such as
      bottles and boxes on a crate, are not walls: the trace goes past them.
    - Before that rule, a static bottle between the hand and the object
      cancelled the carry, and the object, driven along the arc, knocked the
      bottle off (`p2r/snapknock2.py`, first `light` run).
  - **Log.** `GRAB: frame jump (hands turned X deg, axes Y deg, moved Z cm
    beyond Walker's motion, E cm explained by it): N held object(s) and M
    hand sphere(s) carried, B blocked by a wall, pose residual R cm` (the
    first 20, or under `grablog`). Under `grablog`, one `GRABFRAME:` line
    per tick gives the centre, origin, yaws and the explained and
    unexplained moves (not `GRABLOG:`, so the older parsers such as
    `drivetest.py` skip it); the smash check logs `GRABSMASH:` lines.
  - **Scope.** Only with `objectweight` on. With `off`, the old behaviour is
    kept.
- **Measured** (novr, level 1 CP04, `p2w/weighttest.py`).
  - **Setup.**
    - Each run spawns the object in the fake left hand (`synthsmooth on`).
    - Phases: holding still 2 s; 1.5 m/s sideways for 0.6 s and back (3
      repetitions each); 1.5 m/s up for 0.4 s and down (2 repetitions); a
      3 rad/s wrist turn for 1 s and back.
    - Settle time: from the first tick where the hand's own 30 ms velocity
      passes 0.1 m/s, to the first entry into the steady band
      (±max(0.25 cm, 15%)) that lasts 30 ms. The band is relative to the
      steady lag, so a heavy object with a large steady lag can "settle"
      early; lag and overshoot are the better measures there.
    - **90 Hz** runs use the new test command `fpscap 90`: the props tick
      waits so that a game tick lasts at least 1/90 s (11 ms; 45 Hz: 22 ms).
      The wait is left out of `PROPS: tick`. UE3's `set GameEngine
      bSmoothFrameRate True` + `MaxSmoothedFrameRate 90` worked once and
      then no longer took effect after being reset to `False`.
  - **225 ticks/s** (4 ms; bottle `p2w/f6_bottle`, others `p2w/g4_*`):

  | Object | Mode | Settle side / back / up / down (ms) | Peak lag side / up (cm) | Overshoot (cm) | Back at the hand after stop (s) | Net lift (m/s2) | Wrist turn: alpha (rad/s2), lag, over (deg) | Still std (mm) |
  |---|---|---|---|---|---|---|---|---|
  | bottle | off | 24-40 / 25-30 / 28-69 / 28-29 | 0.7-0.8 / 0.8-1.0 | 0.8 | 0.02-0.04 | - | 190.0, 1, 0.7 | 0.53 |
  | bottle | light | 28-30 / 29-58 / 29 / 25-29 | 0.8-1.0 / 0.8-1.0 | 0.7-0.8 | 0.02-0.04 | 104.2 | 189.5, 1, 0.7 | 0.52 |
  | bottle | full | 28-29 / 30-59 / 32-33 / 28-78 | 1.1-1.4 / 1.4 | 1.0-1.4 | 0.02-0.04 | 73.5-78.1 | 188.5, 1, 0.7 | 0.47 |
  | radio | off | 28-29 / 25-28 / 27-28 / 24-25 | 0.8-1.0 / 1.0 | 0.8-1.0 | 0.03-0.04 | - | 185.0, 1, 0.8 | 0.53 |
  | radio | light | 90-94 / 91-96 / 129-136 / 49-50 | 2.7 / 3.6 | 1.4-1.7 | 0.05-0.09 | 32.3-33.2 | 82.4, 4, 3.8 | 0.37 |
  | radio | full | 71-75 / 67-273 / - / 298-299 | 6.4 / 11.8-12.0 | 1.2-1.6 | 0.08-0.22 | 9.7-10.2 | 43.3, 7, 3.3 | 0.41 |
  | barrel | off | 22-23 / 27-29 / 19-23 / 30-33 | 0.6-0.8 / 0.6-0.8 | 0.6-0.8 | 0.02-0.39 | - | 176.2, 1, 0.7 | 0.22 |
  | barrel | light | 296-300 / 175-293 / 248-251 / 294-298 | 10.6-10.8 / 18.8-19.0 | 1.0-1.8 | 0.22-0.50 | 6.3-6.5 | 6.8, 50, 3.5 | 0.27 |
  | barrel | full | 197-258 / 226-290 / 298 / 298-300 | 14.0-16.3 / 33.3-34.1 | 0.8-4.7 | 0.28-0.89 | 2.9 | 3.5, 89, 3.5 | 0.43 |
  | case | off | 26 / 26-27 / 26 / 24-25 | 0.7 / 0.8 | 0.6-1.0 | 0.02-0.28 | - | 175.0, 1, 0.6 | 0.50 |
  | case | light | 217-339 / 294-298 / 242-250 / 298-300 | 10.1 / 19.6-20.0 | 0.6-1.6 | 0.22-0.58 | 5.8-6.0 | 6.5, 50, 3.4 | 0.26 |
  | case | full | 192-393 / 186-292 / 298-300 / 298-299 | 11.9-16.6 / 34.7-35.3 | 1.0-4.4 | 0.31-0.64 | 2.8-3.2 | 4.6, 89, 3.5 | 0.53 |

  - **90 Hz** (`fpscap 90`, 11 ms; `p2w/g90_*`):

  | Object | Mode | Settle side / back / up / down (ms) | Peak lag side / up (cm) | Overshoot (cm) | Back at the hand after stop (s) | Net lift (m/s2) | Wrist turn: alpha (rad/s2), lag, over (deg) | Still std (mm) |
  |---|---|---|---|---|---|---|---|---|
  | bottle | off | 44-45 / 44-45 / 55 / 55 | 1.8-2.0 / 2.2-2.4 | 1.6-2.4 | 0.04-0.08 | - | 160.9, 2, 1.9 | 0.11 |
  | bottle | light | 44-45 / 44-45 / 55-56 / 56 | 2.0 / 2.4 | 1.6-2.2 | 0.04-0.08 | 101.0-114.6 | 169.1, 2, 1.9 | 0.11 |
  | radio | off | 44-45 / 44-45 / 55-56 / 56-67 | 1.8 / 2.2 | 1.4-2.4 | 0.04-0.08 | - | 168.2, 2, 1.9 | 0.12 |
  | radio | light | 100 / 100-111 / 122 / 89-111 | 3.3 / 4.6 | 2.0-2.6 | 0.06-0.11 | 28.0 | 80.0, 4, 3.1 | 0.00 |
  | radio | full | 67-78 / 111-122 / - / 288-289 | 6.2-6.4 / 12.0-12.8 | 1.4-2.8 | 0.13-0.24 | 10.3 | 41.4, 7, 3.5 | 0.11 |
  | barrel | light | 88-278 / 88-100 / 244-245 / 300 | 8.9-9.2 / 19.8-20.0 | 0.8-3.0 | 0.18-0.36 | 5.4-6.0 | 6.4, 51, 3.2 | 0.00 |
  | barrel | full | 311-322 / 211-223 / 300 / 289-300 | 14.6-15.0 / 36.4 | 0.4-7.4 | 0.36-0.58 | 2.8 | 3.2, 89, 2.8 | 0.13 |
  | case | light | 289-300 / 88-99 / 233-245 / 289 | 8.5 / 18.6-20.0 | 0.8-3.0 | 0.22-0.35 | 5.2-5.7 | 6.4, 51, 3.2 | 0.11 |
  | case | full | 278-311 / 189-222 / 300 / 300 | 13.7 / 34.8-36.6 | 0.4-5.8 | 0.42-0.57 | 2.7 | 3.2, 90, 3.3 | 0.14 |

  - **Bottle within 10% of `off`.** At 90 Hz all four moves are the same
    (44-45 / 44-45 / 55-56 / 56 ms against 44-45 / 44-45 / 55 / 55). At
    225 ticks/s the peak lag is the same (0.8-1.0 cm). The settle ranges
    overlap: one of three repetitions per move sometimes reads 58-78 ms
    in every mode, `off` included (lift 28-69).
    - The first round's sideways and lift moves were "bimodal" (~25 or
      ~100 ms); that was the wall pull above, not the weight.
  - **Radio, `light` (the default).**
    - Sideways 90-94 ms and back 91-96 ms (90 Hz: 100 and 100-111): inside
      the plan's 60-120 ms.
    - Lift 129-136 (90 Hz: 122) and down 49-50 (90 Hz: 89-111): gravity
      hinders one way and helps the other inside the same budget.
    - Wrist turn: 80-82 rad/s2, 4 degrees of lag.
  - **Barrel and case, `light`.**
    - Lag 10.1-10.8 cm sideways and 18.8-20.0 cm on the lift (90 Hz: 8.5-9.2
      and 18.6-20.0), inside the plan's 10-25 cm except the sideways move at
      90 Hz.
    - Net lift 5.2-6.5 m/s2. Wrist turn 6.4-6.8 rad/s2, under the plan's 8
      (the floor is 6), with ~50 degrees of lag.
    - Every heavy object came back to the hand after the hand stopped
      (0.18-0.58 s).
  - **`full`.** Lift lag 33-37 cm, wrist turn 3.2-4.6 rad/s2 with ~89
    degrees of lag, net lift 2.7-3.2 m/s2. At 90 Hz the barrel and case
    overshoot up to 7.4 and 5.8 cm (see follow-ups).
  - **Holding still** (2 s, no limit cycle): error std at most 0.53 mm at
    225 ticks/s and 0.14 mm at 90 Hz, for every object and mode.
  - **Sag** (`p2w/sagtest.py`, unchanged code): logged 0.49 / 2.46 / 4.92 cm
    for 1, 5 and 10 kg at `full` (plan: 0.5 / 2.5 / 4.9).
  - **Smash in the hand** (`p2r/smashloop.sh`: `throwtest.py ... smash`, one
    crate bottle per run, relaunch every 3 runs):
    - `light` broke in the hand in 6 of 9 runs: all 6 runs where a free
      bottle was in the hand. In 1 run the bottle was wedged among the
      crate's bottles before the swing (34.8 cm from the hand) and slipped;
      in 2 runs the harness put no bottle in the hand.
    - `off`: 6 of 6. In 2 of them the bottle also started 11-15 cm off,
      wedged, and the old 30 m/s command yanked it free.
    - Before the gate change: 4 of 9 (`p2r/smash_light_*`) and 2 of 5
      (`p2r/smash2_light_*`).
- **Frame-jump tests.**
  - **Snap turn** (`p2w/snaptest.py`, radio, `snapturn 30`, `light`): two
    jumps carried, the 30-degree turn (18.8 cm at the hand) and one tick
    later the eye's swing about the pawn (15.5 cm). Afterwards: error at
    most 0.2 cm, commanded speed at most 0.16 m/s, hand speed 0, no slip.
  - **Compensated turn** (`p2r/snapcomp.py`, `synthcomp on`, 45-degree snap,
    the turn compensated like cover and turret aim):
    - no rotation jump: the hands did not turn in the world;
    - only the 23 cm eye swing was carried, the one real hand move;
    - error at most 0.3 cm, hand speed 0, wrist at most 0.2 rad/s.
    - The old rule (body yaw alone) would have turned object, spheres and
      history by 45 degrees about the head.
  - **Forced `Actor.Velocity`** (`p2r/velspike.py`, 20 m/s written 196 000
    times in 0.6 s, Walker standing, 90 Hz):
    - 55 ticks read 20 m/s;
    - new rule: 0 jumps (worst unexplained move 1.9 cm);
    - the old rule (|centre move - v dt| over 10 cm) flags all 55;
    - held radio: error at most 2.6 cm, no slip.
  - **Run start** (`p2r/runjump.py`, A sprint for 2.5 s from a fixed start,
    radio in hand):
    - 90 Hz and 45 Hz: 0 jumps; worst unexplained 2.4 / 4.5 cm; error at
      most 4.2 / 6.2 cm.
    - In these runs `Actor.Velocity` had no spike (at most 3.92 m/s), so the
      old rule stayed quiet too. The one spike in the logs (20 m/s, item 1's
      `p1m/sprint_pf1_m40.txt`) is the case `velspike.py` reproduces.
    - Some runs from other start points had 5-17 cm errors: the radio
      brushed something on the path (measured velocity dropping while the
      command did not), not a jump.
  - **Snap turn into a bottle** (`p2r/snapknock2.py`, 45-degree snap, radio
    at scale 0.6, hand 60 cm forward and 30 cm left):
    - Setup: the held radio's path is measured with `off` away from the
      crate. Walker is then moved so that the path point farthest from the
      carried poses (start, after the turn, after the eye swing; 23 cm from
      the nearest) lies on the static `bottle_round02` on the crate, with
      the hand height corrected.
    - `off`: the bottle is knocked 182 cm (2 of 2; radio within 9.8-9.9 cm
      of its centre).
    - `light`: the bottle does not move, 0.0 cm (2 of 2; the radio is
      carried and never comes closer than its start, 23 cm).
    - With the hand 35 cm forward, the path stays within 11-14 cm of the
      carried poses: `off` knocked the bottle 182 cm (5 of 5), and `light`
      moved it 9-64 cm. There the carried radio itself ended up touching
      it, and in one run the static bottle counted as a wall and cancelled
      the carry (fixed: small meshes are not walls).
- **Cost.**
  - Per held object and tick: one `NxVelocities` read plus arithmetic. Per
    tick: the frame check (no trace unless a jump carries an object).
  - `PROPS: tick` 0.166-0.173 ms on average, 0.34-0.39 ms at most, in 30 s
    windows holding a radio with `light` and the hand moving every 2 s
    (`p2w/perfhold.py`). The one 19 ms peak is the test-only `spawnprop`
    rescan.
- **Hit pipeline after the change** (fresh relaunch, summoned dummy, default
  `light`):
  - `hitthrow.py`: the held radio touched the dummy's head during the
    wind-up (`hit ... bone head (closest body) ... (held, left hand) at
    1.5 m/s`; it now reports the radio's own speed). The throw (5.8 m/s)
    then left off the head, the geometry noted in items 1 and 2.
  - `heldswing.py` at 3 m/s: one hit per fast swing (`Spine1 (trace)`, 3.3
    and 2.9 m/s), none pressed, none slow.
  - `hitdrop2.py` (40 units above the head): `bone Spine3 (trace) ...
    (dropped, left hand) at 1.8 m/s`.
  - `p1m/regrab2.py`: the same actor re-grabbed; the wrist reads 0.1-0.3
    rad/s on the first 6 ticks; no hit.
- **Grab suite** (`suite.sh`, default `light`):
  - `p2rF` and `p2rG`, final dll except for the log prefix, which only
    matters to `drivetest.py`:
    - grasp: power grip, centre f -2.6 p 5.4 a 6.6 cm, turned 48 degrees;
    - drive (`p2rG`): settle 0.065/0.025/0.065/0.082/0.044/0.045 s, sweep
      error 2.1 cm;
    - static grab and glove (1.8 m in 0.41 s, caught);
    - bottle knocked 56 and 64 cm, barrel not knocked;
    - door 51.5 and 55.0 degrees (32-61 in earlier suites), stop sign 5.5;
    - shot: 0 cm in `p2rG` (the aiming case from item 0), and in `p2rF` the
      radio moved 32.9 cm.
  - **Drive in `p2rF`.** `drivetest.py` crashed on the new per-tick
    diagnostic, then named `GRABLOG: frame`. It is now `GRABFRAME:`.
  - **Smash.** `p2rF` broke in the hand. In `p2rG` the bottle was wedged
    among the crate's bottles and slipped 0.6 s into the swing (the failure
    mode above).
  - **Drop.** It did not break in either suite (short fall onto the barrel
    lid, as in `p2wf` and item 1).
- **Not verified / follow-ups.**
  - The headset. Cover slides, mantles, turrets and recenters with an
    object in the hand: the effective-yaw logic is tested only with
    `synthcomp` (fake poses).
  - The barrel and case at `full` overshoot 6-7 cm at 90 Hz when the hand
    stops dead from 1.5 m/s (0.2-4.8 cm at 225 ticks/s). The cause: the force
    budget is nearly used up by gravity, and the vertical error correction
    takes part of what is left, so they brake at ~6.4 m/s2 instead of the
    ~7.9 the safe stop counts on. They come back smoothly in 0.3-0.6 s.
  - Radio at `light`: the lift settles in ~122-128 ms and the move down in
    45-89 ms. Gravity helps one way and hinders the other, inside the same
    force budget (the plan's 60-120 ms is met sideways: 89-111 ms).
  - Heavy objects at `full` lag more than the plan's 10-25 cm (lift: 35-37
    cm). `full` is the opt-in heavier mode.
  - A carried object can land on something next to the hand's new position
    (the hand itself is there in the world). `PhysX` pushes it out; in the
    snap A/B a bottle 11-14 cm from the carried radio moved 40 cm.
  - The drawn hand does not follow the object (item 5).
  - **Wedged objects.** A light object wedged among others (a crate bottle
    caught between its neighbours) is pushed with at most one tick of force
    per tick, starting from its measured velocity (0), so it stays put and
    slips out of the hand. `off` yanks it free with its 30 m/s command
    (smash loop: 1 of 9 `light` runs and the `p2rG` suite).
    - Fixed by the follow-up below ("item 2 follow-up: wedged objects"): the
      crate's invisible walls were the main cause, plus a breakaway.
  - **The view base moves the gun hand too.** `TraceLineView` changes where
    the view is pulled back from walls, and both hands are placed from that
    view base (`UpdateArms` `center`). So in the cases it fixes (the player's
    own hand sphere or held object on the wall-pull traces), the gun hand no
    longer moves back with the view either. The gun-hand code is unchanged
    (`HandTarget(0)`, `GunBarrel`, `MuzzleStart`, ADS, `CalcWeaponFire`), and
    the suite's shot test aims as before (yaw 11.0 / pitch 2.5 on the first
    iteration, then within 0.1 / 1.0 degrees). A headset check is due.
    The aim test on the final dll is still open (see the results of the
    follow-up below, "Shot").
- **Test tools** (scratchpad `p2w/`, `p2r/`): `weighttest.py` (all phases,
  per mode; `TPSTEP` moves Walker between runs; `EXTRA`/`RESET` add commands,
  e.g. `fpscap 90`), `glog.py` (GRABLOG parser), `sagtest.py`, `snaptest.py`,
  `p2r/snapcomp.py` (compensated turn), `p2r/velspike.py` (forced
  `Actor.Velocity`), `p2r/runjump.py` (run start, old rule recomputed
  offline), `p2r/snapknock2.py` (target placed on the path measured with
  `off`), `p2r/smashloop.sh`, `perfhold.py`. New test commands: `fpscap
  <hz>`, `synthcomp on|off` (fake poses: a reference that follows every
  body turn, like cover and turret compensation), `griptune framevel`.

**Boneworks plan, item 2 follow-up: wedged objects come free (25/10; review fixes the same afternoon).**
User rule: nothing grabbable may become impossible to lift or move. Item 2's
report left a case that broke it: with `objectweight` `light` or `full`, a
crate bottle in level 1 could stay stuck while the hand pulled away, and then
slip out of the hand (smash loop run 4: error 34.8 cm, measured velocity 0,
command 1 m/s towards Walker). `off` "freed" it with its 30 m/s command.
- **What really held the bottles** (novr, level 1, scratchpad `p2b/`).
  - **Invisible walls.** Three `YBlockingVolume`s surround the crate: two
    thin walls (9 and 15 units thick, z 868-1014 and 868-1036, up to 1.9 m
    above the crate top) and a block over the crate itself (z 860-942: the
    crate's top; the crate bottles stand in it, centres at z 940-958).
    `p2b/vols.py` reads their convex hulls: `BrushComponent` `BrushAggGeom`
    +0x204, `ConvexElems` +0x24 (count +0x28), 0x64 bytes per `FKConvexElem`:
    `VertexData` +0x00 (count +0x04), `FacePlaneData` +0x3C (count +0x40),
    planes in the component's axes. Their brush components have
    `BlockRigidBody` on (+0x14C bit 0x40) and a body instance, and
    `BlockZeroExtent` off, so zero-extent traces (`WorldBlocked`, the view
    and gun traces) go through them, but PhysX does not.
    - Walker, coming from CP04, stands outside the wall nearest to him and
      reaches over it. A held bottle pushed towards that wall at 0.5 m/s
      stopped 3 units from it while its target went through (error up to
      43 cm, `p2b/walltest.py`); a bottle thrown at 4.7 m/s bounced back
      140 degrees off the throw.
    - The smash-loop wedge of item 2 had its command pointing towards
      Walker, through that wall. With `off`, the 30 m/s command made the
      bottle tunnel or thrash against it.
  - **The canonical grip jams.** At the grab the canonical pose turns the
    bottle 75-160 degrees in 0.1-0.2 s. Among its neighbours, the rifle cases
    and the guard rails the turn jams: the origin ends 20-45 cm from its
    target while the grip point is often near the palm.
  - **What `off` does there.** The 30 m/s yank thrashes: in the same pulls
    (`p2b/b_off_*`) the bottle jumped up to 55 cm in one tick, reversed by
    more than 1 cm 223-245 times per pull, and held still with an error std
    of 35-44 mm (`p2c/wedgemetric.py`). One bottle thrashed for 1 s before it
    came free.
- **Fix 1: held objects pass the invisible walls they touch**
  (`HeldVolumes`, `VolumesTick`; weight on only, `griptune passvolumes 0` =
  as before).
  - `Scan` collects the `BlockingVolume`s (class kind 7, name index of
    `BlockingVolume`) with `BlockRigidBody` and a body within 30 m
    (`g_blockVols`: component, owning actor, box).
  - Each tick with an object in the hand, a wall becomes an ignored PhysX
    pair with the object (`setActorPairFlags`, like Walker's bodies) only if
    the object touches it or is inside it (bounding sphere + 4 units against
    the convex hull, `InsideVolume`) and the wall is not a floor for it.
    **Floor** (`VolumeIsFloor`): the bottom of the object's box is above the
    top of the wall's box (1 unit tolerance). A wall that becomes a floor
    while held (a crate bottle lifted above the block) is made solid at once
    and stays solid until the release (`volFloor`; without that memory the
    pair flipped every tick at the threshold). So a bottle put back on the
    crate stays on top, and a bottle slammed onto the crate stops on its top.
    Logs: `GRAB: held <mesh> passes the invisible wall ...`, `GRAB: held
    <mesh> is above the invisible wall ...: solid again (a floor for it)`.
  - The first version ignored every wall whose box was within the object's
    radius + 30 units, the crate's block included: a bottle put back on the
    crate sank into it, and after 10 s the pair stayed ignored for good.
  - On release each ignored pair goes to `g_volRestore` (at most 64). It
    becomes solid again as soon as the object's bounding sphere (+2 units)
    is out of the wall's hull, or the object is on top of it (floor), so
    PhysX never has to push it out: `GRAB: <mesh> solid again for the
    invisible wall ..., N s after release (out of it | on top of it)`. Still
    inside after 10 s: checked again once a second (before: left ignored for
    the rest of the level). Grabbed again with the weight on: the entry moves
    to the held object (`held.volIgn`); with the weight off it waits for the
    release (before: dropped, the pair stayed ignored).
  - **Stored pointers** (brief rule 7). Every PhysX pair call first checks the
    wall (`VolumeAlive`): the actor is not `bDeleteMe`, is still class kind
    7, still has that component as `CollisionComponent`, the component still
    has `BlockRigidBody`, and `CompNx(component)` is still the stored NxActor.
    A wall toggled by Kismet or streamed out (its NxActor freed) is dropped
    without a PhysX call.
  - `InsideVolume` now measures the margin in world units (plane normal
    `R^-1 n`), not divided by the mean scale; the crate's three volumes have
    scale 1, so their results do not change (`p2c/hullcheck.py` replicates
    the test from live memory: the deep crate bottles are 6-16 units from
    the block, 46-120 from the thin walls).
  - Known limit: an object pushed against a level-boundary invisible wall
    can still be carried through it and dropped on the far side.
- **Fix 2: breakaway** (`BreakawayStep`, in `Drive`'s weight path; `off`,
  hinged objects and objects over `breakkg` (30 kg) unchanged).
  - **Blocked.** Over a window of at least 30 ms and 2 ticks, the PhysX
    velocity measured after each step along the expected one (last command
    plus one step of gravity), relative to Walker, sums to less than
    `breakratio` (35%) of the expected speed; the grip point (`held.anchor`,
    the origin for palm grips) is more than `breakerr` (8 cm) from its target
    and the error is not shrinking. The sum, not each tick: a bottle
    vibrating against the crate measured 0.4-0.8 m/s of a 1.2-1.5 m/s command
    every tick without moving, and never counted as blocked. A heavy object
    lagging in free air follows its command, so it is never "blocked": the
    weight in free air does not change.
  - **Gate.** No breakaway when a wall or a pawn is between the grip point
    and its target (`WorldBlocked` with a new `pawns` flag: TRACE_Pawns 0x1,
    a `YGamePawn` always blocks), when the object's bounding sphere + 8 units
    touches a live pawn's cylinder (`TouchingPawn`, WorldInfo.PawnList, any
    pawn but Walker), or when the line crosses an invisible wall that is a
    floor for the object (`SolidVolumeBetween`: walls made solid as floors
    in this hold, and nearby walls not ignored whose top is under the
    object's bottom; box slab test on the cached boxes). Zero-extent traces
    do not see `BlockingVolume`s: without this a bottle put back on the
    crate and pressed 10 cm under its top was pushed through the block at
    force x19. A first version vetoed every pull more than 30 degrees below
    the horizontal instead; it also stopped the breakaway for crate bottles
    jammed by the canonical grip (their target lies lower), which then
    vibrated in place for 1.3 s until the hand lifted. Objects over `breakkg`
    (30 kg) never get a breakaway: `WorldBlocked` does not trace other
    KActors, and a barrel dragged against friction is not wedged.
  - **Ramp.** The linear force budget doubles every `weightbreak` ms (30,
    `griptune breaktime`), towards `off`'s command for the grip point
    (half the error per tick plus hand and wrist motion), converted to the
    centre of mass (`v - w x arm`), at most the hand's speed + `breakcap`
    (4 m/s) relative to Walker (before: `maxSpeed`, 30 m/s for a bottle: the
    law that made `off` thrash). Rotation stays on the weight budget
    (`breakturn 0`): turning a bottle into the canonical pose while it is
    still among its neighbours made it bang into them at 3-10 m/s.
  - **Free.** It follows at least half its command for at least 2 ticks and
    8 ms (3 ticks at 250 Hz, 2 at 90) and has come 3 cm closer, or its grip
    error is under 3/4 of `breakerr` (6 cm; the hysteresis stops a bottle
    resting against a rail from entering and leaving every 10-20 ms). Then
    the weight budget applies at once, and the starting velocity is cut to
    what the weight path wants (the safe-stop speed relative to Walker),
    linear and angular: nothing of the yank is left.
  - **Window sums.** When the hand comes back (error under the window's
    first error), the window restarts with its sums too; before, only the
    tick count restarted, and the logged averages ("22.6 m/s" commanded,
    2.7 m/s sideways) and the new vibration baseline were inflated.
  - **Blocked again** within 0.3 s (window 8 ms, 2 ticks): the ramp resumes
    at half the force it had. If it is blocked again where it started (within
    3 cm), the yank did not help: treated as a give-up. Also a give-up: the
    third "again" in a row (`blocked again N times in a row`). A bottle lying
    on the ground with its target partly in the ground reached the palm
    (under 6 cm), was pushed back over 8 cm and restarted every 100-200 ms:
    134 reversals over 1 cm in one hold (the cycle already existed in item
    2's follow-up logs, e.g. `smI_light_2`).
  - **Give-up.** At the full drive and still blocked for `breakgive` (100 ms),
    after 1 s in total, or when it thrashes: sideways or backwards faster
    than 1 m/s and than twice the sideways vibration measured in the blocked
    window + 0.5 m/s, for at least 2 ticks and 30 ms, and only from force x4
    on. The first version gave up after 2 ticks over 1 m/s: the vibration
    the weight path already had (0.4-1.3 m/s) ended the breakaway after 8 ms
    at force x1 (smash loop run 7). The velocity is cut as on "free". A new
    attempt needs the error to change by 5 cm (hand back) or 10 cm (pulled
    further) after 0.25 s, or simply 0.5 s (before: a still hand never
    retried).
  - **Time, not ticks.** Every window and test above is in ms with a minimum
    tick count, so 90 Hz in the headset behaves like the 250 Hz of the novr
    runs.
  - **Aborted** (frame jump, no PhysX measurement, `breaktime 0`): the
    stored previous command becomes the weight command (`weightCmdV/W`), so
    the next tick's measured base is clamped to it + 0.5 m/s, not to the
    inflated yank (snap turn during a yank).
- **Fix 3: no dead band (`WedgeStep`, weight on; changed by follow-up 2
  below: regrip instead of release while the palm touches the object; the
  release only with the palm off it, `weightwedge` 1 s again after the
  follow-up 2 review).** A wedged object with no
  way out (its target pose inside the crate, the hand still) sat in the hand
  43 cm from the palm for 5 s, neither freed nor dropped (smash loop run 7 on
  the first final dll; `smG_7` on the one before: 13 s). The slip rule acts
  only beyond 60 cm (80 over 30 kg). Now:
  - the grip point's velocity is taken from its positions (world, an
    exponential mean over 0.15 s: vibration in place averages out);
  - while the grip error is over `breakerr` and that velocity under 0.15 m/s,
    outside an active breakaway and not touching a pawn, time accumulates;
  - over `T` the hand lets go: `GRAB: left hand lets go of <mesh>: wedged
    for N s, grip error E cm ...`, then `released ... (wedged)`, like a
    slip. Its linear and angular velocity are zeroed first: it is not moving,
    but PhysX still reports the vibrating contacts' velocity, and the falling
    tracker saw a bottle "hit" at 5.6 m/s 4 ms after the release and broke it. `T` = `weightwedge` (1 s, `griptune wedgetime`) at 2 x `breakerr`
    (16 cm), shorter farther away (`T x 16 / e`, at least 0.3 s), longer near
    the threshold (3 s at 8 cm: an object pressed a little into a table stays
    in the hand);
  - it resets when the object moves (over 0.3 m/s) or the error drops under
    3/4 of `breakerr`. Pressed against a pawn it does not count (the pawn
    moves; the slip rule still applies);
  - it does not count during the first second after the grab: the canonical
    grip wants to turn a crate bottle 110-130 degrees, its neighbours block
    the turn, and the grip error reaches 20-30 cm with the hand still. Without
    the grace the hand let go after 0.6-0.9 s, before the test even lifted
    it (4 of 12 smash runs on the build before); lifted, it usually comes
    free (the previous dll freed the same bottle at force x2 on the lift).
- **Slip rule (item 2, revised).** The window restarts when the error has
  shrunk by 0.25 m/s on average since the window started, or when the
  measured velocity towards the target, averaged over the window, is
  0.25 m/s or more (item 2's rule, averaged). The follow-up's first version
  kept only the former, which would drop a heavy object that follows but
  lags more than 80 cm with a flat error. The vibrating bottle that the
  measured-velocity test kept "held" is now let go by `WedgeStep`, which
  looks at positions.
- **No pop.** `held.weightCmdV/W` keep the weight command without the
  breakaway. Release (the throw starts from it), the smash check and the
  held hits of the hit pipeline use it instead of `lastCmdV/W`; with no
  breakaway they are equal, so nothing changes there.
- **Log.** `GRAB: breakaway: <mesh> blocked for N ms (measured ... on
  average, sideways S, error ...): force xM, doubling every 30 ms`, `came
  free after N ms (force xM) ...`, `is at the hand ...`, `thrashing (pushed
  back by something) ...`, `did not come free at the full drive ...`,
  `blocked again where it started ...`, `blocked by a wall or an enemy
  between it and the hand ...`, `blocked again N times in a row ...`,
  `pressed against a pawn ...` (first 60, or under `grablog`). `GRABLOG`
  lines add `| breakaway xM`, `| breakaway freed` and `| grip err N cm`. The
  release line ends with the hand speed.
- **Tuning** (ini keys appear in the Advanced menu; no Basic option: these
  are fixes, not choices):

  | ini / `griptune` | Default | Meaning |
  |---|---|---|
  | `weightbreak` / `breaktime` | 30 ms | Force doubling time while blocked; 0 = no breakaway |
  | `weightwedge` / `wedgetime` | 1 s | Still and 16 cm from the palm this long: let go (shorter farther away); 0 = never |
  | `breakerr` | 8 cm | Grip error over which a blocked object counts as wedged |
  | `breakratio` | 0.35 | Measured / commanded speed under which it is blocked |
  | `breakgive` | 100 ms | At the full drive and still blocked: give up |
  | `breakturn` | 0 | 1 = the torque budget ramps too |
  | `breakkg` | 30 kg | No breakaway above this mass |
  | `breakcap` | 4 m/s | Yank speed cap over the hand's speed, relative to Walker |
  | `passvolumes` | 1 | Held objects pass the invisible walls they touch (`BlockingVolume`) |

- Test command: `breakdrop <ms>|off` opens the fake grip <ms> after a
  breakaway frees the object (release just after freeing).
- **Results on the final dll** (novr, level 1 CP04, scratchpad `p2c/`,
  `p2c/battery.sh` and the close-out runs `p2c/close_run*.txt`; the game no
  longer pauses unfocused, `specopsvr_keepfocus`).
  - **Smash in the hand** (`smK`, `light`, crate bottle; 12 runs planned).
    Only 5 runs were logged. 2 broke the bottle in the hand: run 1 after two
    breakaways that ramped to force x18 and x20 and ended on "did not come
    free at the full drive", no longer the 8 ms thrash give-up of run 7;
    run 2 came free at x2 in 24 ms, was blocked again 12 ms later and
    reached the hand at x2. In 1 run the 0.3 s watchdog let go ("no hands")
    0.8 s after the grab. 2 runs did not grab. After its agent was stopped
    the loop kept running as an orphan (until 15:15, relaunching the game
    every 3 runs), so it overlapped other tests on the same game. These
    numbers are not a success rate. **Open:** the smash-in-hand success rate
    with the weight on, and the time spent wedged (`p2c/wedgemetric.py`),
    for the final physics regression.
  - **Wedge release.** `WedgeStep` fired on the build before the 1 s grace
    (`p2c/s3_put`: "wedged for 0.66 s, grip error 25.4 cm, moving 0.03 m/s
    (limit 0.63 s)", released at 0.0 m/s). It has not been seen firing on
    the final dll yet.
  - **Hit pipeline** (`p2b/hitall.sh p2close2_hit`, summoned dummy,
    `light`; previous dll in brackets, `hitall3`):
    - `hitthrow`: held hit on the head at 1.5 m/s, release at 5.9 m/s (hand
      4.86), flight 3.78 m/s, 73 degrees off the throw after 18 ms [1.5;
      5.8 / 4.68; 3.73 m/s, 72.5 degrees].
    - `hitdrop2`: the dropped radio hits Spine3 at 1.7 m/s [1.8].
    - `regrab2`: no hit on the regrab, wrist 0.0-0.1 rad/s [0 hits].
    - `heldswing`: one hit on Spine1 at 2.7 m/s in each fast swing, closest
      approach 24 units; none while pressed, pulled back or pushed slowly
      [3.1 m/s, 24 units, same pattern]. The first run (`p2close2_hit`) is
      void: the `hs_start` screenshot stalled the game for 0.29 s and the
      0.3 s watchdog let go of the radio ("no hands") before the swings.
  - **Grab suite** (`suite.sh p2close`; grasp, drive and static grab were run
    again after the orphan loop was stopped):
    - Grasp: power grip, half extents 3.7 3.6 16.3 cm, turned 48 degrees [50].
    - Drive: settles in 0.059-0.100 s, tail error at most 0.4 cm, rotation
      0.0 degrees; sweep at 1.5 m/s: error at most 1.7 cm [0.023-0.081 s,
      0.3 cm, 1.9 cm].
    - Static grab (`reach.py bottle_quad01`, hand still): **let go after
      2.0 s.** The canonical grip jams the crate bottle among its neighbours
      (grip error 12-20 cm). Three breakaways ramp to force x32, x26 and x25
      and each gives up after 140-150 ms ("did not come free at the full
      drive"). Then `WedgeStep`: "wedged for 0.86 s, grip error 18.7 cm,
      moving 0.02 m/s", released at 0.0 m/s. On the previous dll the same
      test held the bottle for 13 s, until the scripted release. So a crate
      bottle grabbed and held still now leaves the hand after about 2 s.
      Lifting it within that time is meant to free it, but that has not been
      measured on the final dll.
    - Glove static: locked, flick 1.6 m/s, flies 1.5 m in 0.39 s, caught in
      flight [1.4 m/s, 1.8 m, caught].
    - Knock: the bottle moved 30.8 cm [63.2; the distance varies from run to
      run]; the barrel 0 cm (a static collection, as before).
    - Drop: bottle_round02 broke at 6.2 m/s. Smash: bottle_quad01 broke in
      the hand 5.2 s after the grab.
    - Hinges: door 60.1 degrees [61.6], stop sign 6.7 degrees [5.4].
    - Shot: no `SHOT:` line. The fake trigger (`pad fake`) never fired, the
      game polled the pad 0.03 times per tick (the same "NO SHOT" as `aim6`
      on the previous dll). The follow-up does not touch the gun code (only
      `props.cpp`, `props.h` and `control.*` changed, with no gun-hand
      function), but the gun-aim check on the final dll is still open.
    - Props tick: 0.17-0.19 ms on average over the suite (copy B shares the
      CPU).
  - **Not run on the final dll:** the pull series (`light`, `full`, release
    after a breakaway), putting a bottle back on the crate (z after 2 s and
    after 12 s), the 90 Hz series, the barrel/case `light`/`full` matrix and
    the fast 2.5 m/s lift at `full`, `p2b/aimtest.py`, the perf A/B.

**Boneworks plan, item 2 follow-up 2: a held wedged object stays in the hand (25/10 evening).**
User rules: nothing grabbable may become impossible to hold, and hand
interaction must look real (no jitter, no popping). The follow-up above broke
the first one: on its final dll the suite's static grab (`reach.py
bottle_quad01`, hand held still) lost the crate bottle 2.0 s after the grab.
The canonical grip wants to turn the bottle 80-125 degrees inside its
neighbours, three breakaways yanked it at force x32, x26 and x25 with the hand
still and gave up, then `WedgeStep` let go ("wedged for 0.86 s, grip error
18.7 cm"). Weight `off` is unchanged by everything below.
- **Hand still** (`HandStillStep`): the palm, in the world, has not moved
  `regripmove` (2 cm) for 0.25 s and its speed (exponential mean over 0.1 s)
  stays under `regripspeed` (3 cm/s; speed test added by the review below);
  at the grab the hand speed decides. In the world, not relative to the object: the
  canonical grip turning the object is not the hand pulling, and an object
  that follows the hand is not blocked.
- **No yank with a still hand.** `BreakawayStep` starts no new breakaway while
  the hand is still (a running one finishes). The breakaway stays for real
  pulls: the hand moving away from a blocked object.
- **Blocked** (in `WedgeStep`): after the grip blend (`blendT`, the target no
  longer moves relative to the hand), the grip point (`held.anchor`) is more
  than `regriperr` (4 cm) from its target and its error has not dropped by
  2 cm within `regriptime` (0.2 s), both on the error's exponential mean over
  0.05 s; no active breakaway, not touching a pawn
  (`TouchingPawn`, as before: a pawn moves, and the hit pipeline's held
  presses must not change the grip). A first version used `WedgeStep`'s
  "still" test (grip-point velocity under 0.15 m/s, 0.15 s mean): the jammed
  bottle bounced 0.4-2.4 cm per tick (120 Hz) without getting closer, the mean
  stayed above the limit and the regrip came 0.97 s after the grab (`st_0`).
  An object that is arriving, even heavy and slow (3 m/s2 net at `full`: 6 cm
  in 0.2 s), gets 2 cm closer well within the window. The threshold was
  `breakerr` (8 cm) at first: in the first final session a crate bottle
  vibrated 5.8-7.9 cm from its target (rotation 11-15 degrees, 1-2 cm steps)
  for the whole 12 s hold, 444 reversals (`p2r2/old_F_still_1`). The mean
  keeps a vibrating error from counting as progress at every minimum.
- **Regrip** (`Regrip`, `AnchorAtPalm`, `AnchorHere`), with the hand still and the palm still touching the object
  (within `regripreach` of the object's bounds box; 8 cm like the grab zone
  in this follow-up, 16 cm since the review
  in its own axes, `PalmToObject`; box from `WeightSetup`): as if the hand
  closed on it where it is.
  - The grip point (object axes; the pivot of rotation, sag and droop, and
    the point the breakaway, the wedge test and the slip rule measure)
    becomes the point of the object that will touch the palm after the palm
    rest below. Before the review it was the palm itself, up to 8 cm outside
    the box or inside the object.
  - The target pose (hand axes) becomes the object's real pose. With the
    weight on, minus sag and droop, so that `WeightPose` adds them back to
    exactly that pose: a fixed-point iteration (up to 6 steps) from the real
    pose, each step corrected by the gap between the real pose and what
    `WeightPose` makes of it. The first version (one forward step and its
    inverse) left a residual of the order of sag x droop.
  - **Settle** (`regripsettle`, 0.15 s): every tick the target follows the
    real pose again, so the drive holds the object still relative to the hand
    without pushing it and the contacts settle it; then the target stays
    there. The first version blended the target over 0.25 s from the old
    pose instead: the drive kept pushing into the neighbours during the
    blend, and a pose taken mid-bounce stayed inside them (`dn_1`: the bottle
    kept vibrating 6-8 cm from its target, under `breakerr`, for good).
  - Nothing pops: the target goes to the object, not the object to the
    target, with the whole sag and droop (they have their own clock since the
    review, `WeightBlend`), and the regrip tick's command already starts from
    the new target; the drawn fingers move to the new pose through their own
    80 ms filter (`PoseFingers`). The canonical rotation does not come back.
  - **Palm rest** (`RestOnPalm`, since the review): at the first hand move
    after the regrip, the target slides by the palm-rest shift in
    `regriprest` (0.25 s), so the object comes back onto the palm: the
    nearest point of its box to 1 cm from the palm, or, with the palm inside
    the box, out along the palm normal (at most 10 cm). Details in the review
    section below.
  - Log: `GRAB: left hand re-grips <mesh> where it is (N): blocked 0.20 s
    with the hand still, grip error E cm, A deg from the wanted pose, palm D
    cm from it, rest on the palm R cm (computed C, N along the palm normal),
    residual E cm A deg`. `GRABLOG` lines add `| palm D cm`, `| still`, `| regrip`
    (settle ticks) and `| stuck`.
- **Palm off the object** (hand pulled away, then still): no regrip, since an
  object floating away from the palm is not a real grip. After `regriptime`
  with the hand still the drive stops pushing it into the obstacle
  (`held.stuck`): at most `stuckspeed` (0.1 m/s) towards the target relative
  to Walker, rotation at most 0.5 rad/s. Pushed with the full budget the
  bottle bounced 1-2 cm per tick for as long as the hand stayed there (`dn_1`,
  406 reversals in 3 s). The full drive comes back as soon as the hand moves.
  Still blocked like this, the hand lets go after `weightwedge` (1 s at a
  16 cm grip error, shorter farther away; see Release).
- **Release.** While the palm touches the object the hand never lets go of
  it, except through the slip rule (60 cm, 80 over 30 kg). The timed
  release (`weightwedge`) only acts while the palm is off the object. This
  follow-up set its default to 0; the review set it back to 1 s, because
  with 0 a blocked object the palm no longer touched stayed "held" for good
  (`smR4`: 3 s at a 24 cm grip error, palm 19.5 cm away). The weight path's slip rule now
  measures the grip point, not the origin. The crate bottle is 47 cm long
  with its origin at the bottom, and the canonical turn put the origin 60 cm
  from its target while the neck was in the hand: with the hand still it
  slipped out 0.42 s after the grab, before the regrip (`lat_0`). Palm grips
  (anchor = origin) are unchanged.
- **Exploratory runs** (novr, level 1 CP04, `light`, scratchpad `p2r2/`,
  `p2r2/wedge2.py` = `p2c/wedge.py` plus `still`, `down`, `side` and `lat`
  actions; `p2r2/trace.py` prints the per-tick grip error, palm distance and
  step):
  - Still hold, first version (`st_0`): regrip 0.97 s after the grab; after it
    the error stays at 0.1-0.2 cm with 0 cm steps until the scripted release
    20 s after the grab.
  - With the progress test and the 0.25 s blend (`stB_0`, `stC_0`): regrip
    0.34-0.39 s after the grab, 18-20 reversals over 1 cm per hold. With the
    settle (`stD_0`): 1 reversal.
  - Hand pushed 20 cm down into the crate (`dnD_1`) or pulled 25 cm towards
    Walker (`sdD_2`), then still: a second regrip 0.2 s after the hand
    stopped (palm 7.2 and 0.0 cm from the box: the hand slid along the
    bottle). The settle brought the error from 26 cm to 0.5 cm in 2 ticks.
  - Palm-off case forced with `griptune regripreach 2` (`dnE_1`): the bottle
    bounced 1.2-1.5 cm per tick until the hand counted as still (0.18 s after
    it stopped), then 0.0-0.2 cm per tick for 4.3 s at a grip error of
    16.8 cm, still held, and it came up with the hand afterwards.
  - Hand moved 25 cm sideways (`latE_0`): held for 10.5 s until the scripted
    release.

- **Results on the final dll** (one session, novr, level 1 CP04, `light`,
  `p2r2/final.sh`, output `p2r2/final.txt`; dll md5 357cbf9b...; copy B ran
  at 72 fps; no other script on this copy):
  - **Still hold** (`wedge2.py still`, 3 crate bottles, hand still 12 s, then
    lifted 30 + 40 cm): all three held until the scripted release 20 s after
    the grab, lifted 63-80 cm, final error 0.2 cm, error std at the end
    0.00 mm, no breakaway. Two needed a regrip, 0.37-0.45 s after the grab
    (grip error 15.5 and 16.4 cm, 108 and 118 degrees from the canonical
    pose); 0-8 reversals over 1 cm per hold, all before the regrip.
  - **Lift out** (`wedge2.py pull`, 5 crate bottles, lifted 30 cm at 0.5 m/s
    0.6 s after the grab, then 40 cm at 1 m/s): **5 of 5** came out, 62-75 cm
    up, error at the end 0.2-0.4 cm, none slipped. Four regripped 0.36-0.50 s
    after the grab, before the lift; the fifth (`grabtest` took a bottle 189 cm
    away, which flew to the hand) came free in three breakaways at force x2-x3
    during the lift.
  - **Smash in the hand** (`p2b/smashloop.sh light 9 smR`, crate bottle):
    **9 of 9** broke in the hand ("smashed against something in the hand",
    hand 2.2-4.9 m/s). Every run regripped 0.31-0.38 s after the grab, and
    none had a breakaway. In run 4 the bottle did not come up on the 25 cm
    lift (a wall between the grip point and its target: no breakaway) and
    stayed at 20+ cm for 3.1 s, held but stuck, then broke when the hand
    came back down. So 8 of 9 lifts worked in this series (16 of 17 over the
    session). Wedged time (`p2c/wedgemetric.py`): still beyond 8 cm for
    0.03-0.07 s in 8 runs (3.35 s in run 4).
  - **Hit pipeline** (`p2b/hitall.sh p2r2_hit`, summoned dummy; the
    follow-up's close-out in brackets): `hitthrow` held hit on the head at
    1.6 m/s, release 6.0 m/s (hand 4.70), flight 3.62 m/s, 77.9 degrees off
    after 25 ms [1.5; 5.9 / 4.86; 3.78 m/s, 73.2 degrees]; `heldswing` one
    hit on Spine1 at 2.5 m/s in each fast swing, closest approach 24 units,
    none pressed, pulled back or pushed slowly [2.7 m/s, 24 units, same
    pattern]; `hitdrop2` dropped radio hits Spine3 at 1.8 m/s [1.7];
    `regrab2` 0 hits, wrist 0.1 rad/s [0, 0.0-0.1].
  - **Grab suite** (`suite.sh p2r2S`):
    - Grasp: power grip, half extents 3.7 3.6 16.3 cm, turned 46 degrees [48].
    - Drive: settles in 0.059-0.109 s, tail error at most 0.4 cm, rotation
      0.0; sweep at 1.5 m/s: error at most 1.7 cm [0.059-0.100 s, 0.4, 1.7].
    - **Static grab (the regression): held 13.0 s, until the scripted
      release**; regrip 0.35 s after the grab (grip error 8.2 cm, 87
      degrees, palm 0.0 cm), no breakaway [let go after 2.0 s].
    - Glove static: locked, flick 1.3 m/s, flies 1.8 m in 0.41 s, caught.
    - Knock: bottle moved 55.7 cm; barrel 0 cm (a static collection).
    - Drop: bottle_round02 broke at 5.1 m/s. Smash: bottle_quad01 broke in
      the hand 5.1 s after the grab.
    - Shot: fired this time; aim converged in 2 iterations (error 0.1
      degrees), the radio moved 163 cm.
    - Hinges: door 57.9 degrees [60.1], stop sign 5.3 degrees [6.7].
    - Props tick: 0.162-0.180 ms on average.
  - Unit tests 65/65; build without warnings.

- **Tuning** (ini keys appear in the Advanced menu; no Basic option: this is a
  fix, not a choice):

  | ini / `griptune` | Default | Meaning |
  |---|---|---|
  | `weightregrip` / `regriptime` | 0.2 s | Blocked this long with the hand still: regrip where it is (palm on it) or stop pushing (palm off); 0 = off: yanks with a still hand again, and only `weightwedge` lets go (the follow-up's behaviour with `weightwedge` 1) |
  | `regriperr` | 4 cm | Grip error (0.05 s mean) over which a still object counts as blocked |
  | `regripreach` | 16 cm (8 before the review) | The palm at most this far from the object's box for a regrip; farther, the timed release |
  | `regripmove` | 2 cm | The palm moving this far counts as the hand moving |
  | `regripspeed` | 3 cm/s | The palm faster than this (0.1 s mean) counts as the hand moving (review) |
  | `regriprest` | 0.25 s | After a regrip, at the first hand move, the object slides back onto the palm in this time; 0 = stays where it was re-gripped (review) |
  | `regripsettle` | 0.15 s | After a regrip the target follows the object this long |
  | `stuckspeed` | 0.1 m/s | Blocked, palm off, hand still: the most the drive pushes towards the target (0 = full drive) |
  | `weightwedge` / `wedgetime` | 1 s (0 in this follow-up, 1 again after the review) | Timed release of a wedged object, only with the palm off it; 0 = never |

- **Limits.**
  - At the grab the jammed bottle still bounces among its neighbours until the
    regrip, for the blend plus 0.2 s (0.31-0.50 s after the grab in the final
    session), and for about 0.45 s after the hand stops on a blocked object
    (0.25 s to count as still, then `regriptime`; 0.18-0.2 s before the
    review, which made the regrip wait for a still hand).
  - Until the hand moves, a regrip with the palm at the edge of the reach
    leaves the object up to 16 cm from the still palm (the palm rest comes
    with the first move).
  - With the palm off a stuck object, the drawn fingers still close on the
    target pose (at most 3 cm towards the object). Making the drawn hand
    follow the object is item 5.
  - A real push into something solid that the zero-extent traces do not see
    (the hand pushed down into the crate) still gets a breakaway that pushes
    the object into it for up to 150 ms, as in the follow-up.

**Item 2 follow-up 2, review fixes (25/10 night).** The review of the
follow-up above found one high, three medium and four low issues. All are
fixed in `src/props.cpp`, with the weight on only (`objectweight off` is
unchanged); the gun hand, aiming and `CalcWeaponFire` are not touched. Test
scripts and logs: scratchpad `p2r3/`.
- **High: a regrip popped the object up.** `Regrip` and the settle set
  `grabAt` = now with a 0.02 s `blendT`, and `HeldTarget` passed that arrival
  blend (0.35-0.58 at 90-144 Hz) to `WeightPose`. So on every settle tick only
  that fraction of sag and droop came back, while `AnchorHere` had removed all
  of it: the target sat (1-b) x sag above the object and was turned back by
  (1-b) x droop, then jumped back to the full sag when the settle ended.
  Reproduced on the old dll (`p2r3/press.py`, `radio_unit_04` x1.25, 8 kg,
  `full`, 144 Hz, regrip after the radio was lowered onto the ground,
  `p2r3/old_radio_full_144b`): over the settle the applied sag was 1.07-1.14 cm
  instead of 3.93 and the droop 4-5 degrees instead of 14; the radio rose
  5.2 cm at up to 1.44 m/s and turned 56 degrees (integrated, up to 5.2 rad/s)
  within 0.4 s, then swung back. These numbers start 62 ms after the regrip,
  so the real ones are larger.
  - Fix: sag and droop now have their own clock (`weightAt`, `weightBlendT`,
    `WeightBlend`), set at the grab only. The regrip, its settle and the palm
    rest change `grabAt`/`blendT`, not this clock.
  - Found while testing the fix: the inverse itself was wrong for soft droop.
    The one-step inverse, and a fixed-point iteration tried next, do not
    converge when the droop is at its cap with c > 1. Here c = |arm| x m x g
    / `krot`, in radians per unit sine of the arm's angle from the vertical:
    about 1.6 for the 33 kg barrel at `full`, whose model droop then
    overshoots the vertical. Taken from the floor with a still hand, the
    residual reached 16-18 cm and 40 degrees (twice the cap). The barrel
    jumped 9.6-12.0 cm and turned 26-28 degrees, up to 1.5 rad/s
    (`p2r3/pb_rg_barrel_full_144`, `_90`).
  - `AnchorHere` now solves the inverse exactly. The droop rotates the arm
    (centre of the bounds minus the pivot) inside its vertical plane, so the
    problem has one variable: phi_T - theta(phi_T) = phi_R, with theta(phi) =
    min(c sin phi, cap) x blend. It is solved on a 1-degree grid plus
    bisection, keeping the root nearest the real pose. The target is then the
    real pose without the sag, turned back by theta about the pivot. The pivot
    is the grip point, moving with the object, or the palm for palm grips. The
    regrip log line gives the residual of `WeightPose` on the result
    (`residual E cm A deg`).
  - Low, same area: on the regrip tick the command was still built from the
    old, blocked target, so one more full push went into the obstacle.
    `Drive` now recomputes the target, error, lever arm, rotation and desired
    velocity when `WedgeStep` returns 1 (regrip or settle tick). In the slow
    pull run `battery1/R_slow_0`, the regrip tick still moves 2.3 cm, from
    the command before it; the ticks after it move 0.0-0.28 cm (before:
    1.2-3.6 cm for 2-4 ticks).
- **Medium: the grip after a regrip.** The grip point was the raw palm, up to
  8 cm outside the box or inside the object, and nothing restored a resting
  grip. Now `AnchorAtPalm` computes a palm rest in the object's own axes and
  box. With the palm outside the box, the object moves towards the palm until
  the nearest point of the box is 1 cm from it. With the palm inside (an
  object pressed onto a table), it moves along the palm normal until the palm
  leaves it on the back-of-hand side, plus 1 cm. Outside, the shift is at
  most the palm distance minus 1 cm. Inside it is at most 10 cm; if more,
  there is no rest. The grip point becomes the point that will touch the
  palm, and the target stays the real pose. The shift (hand axes) is applied
  at the first hand move (`RestOnPalm`, smoothstep over `regriprest`, 0.25 s),
  not at the regrip, because the object is still blocked then. Log: `GRAB:
  left hand moves: <mesh> back on the palm, D cm in 0.25 s`. The canonical
  rotation does not come back: it was what jammed the crate bottle. The first
  version copied `Grab`'s palm rest (centre at half thickness along the palm
  normal, palm within 70% sideways). With a palm normal slanted to the box
  faces it left the palm still 5-7 cm off: `battery1/R_slow_0` went from 5.3
  to 6.7 cm after the rest. That is replaced by the nearest-point rule above
  (results below).
- **Medium: a slow pull counted as a still hand.** `HandStillStep` now also
  needs the palm speed (world, exponential mean over 0.1 s) under
  `regripspeed` (3 cm/s), and the regrip itself now requires the hand to be
  still. A faster hand resets the regrip time and the settle.
- **Medium: an object the palm no longer touched stayed held for good.**
  `weightwedge` is back to 1 s (`wedgeTime` 1, ini `weightwedge 1`). It still
  applies only with the palm off the object, so a still hand on the object is
  never released. Seen in the smash loop below: when the lift left a blocked
  bottle behind (a wall between it and the hand, so no breakaway), the hand
  let it go 0.5-0.65 s later ("wedged for 0.53 s, grip error 30.9 cm, palm
  21.1 cm from it"). In `smR4` it had stayed in the "hand" at 24 cm for over
  3 s.
  - With the old 8 cm reach, this brought back the original regression. In
    the suite's static grab (`p2r3S`), the canonical turn left the crate bottle
    9.2 cm from the still palm, so there was no regrip. The hand let it go
    1.85 s after the grab: "wedged for 0.86 s, grip error 18.7 cm, palm 9.2 cm
    from it", word for word the first follow-up's failure. In `p2r2` the same
    grab happened to regrip with the palm 0.0 cm away.
  - So `regripreach` is now 16 cm, twice the grab zone. Within it, a still
    hand regrips and the palm rest brings the object back at the first move.
    Beyond it (a hand clearly pulled away: 18-21 cm in the smash loop, 19.5 cm
    in `smR4`), the timed release applies.
- **Low: `weightregrip 0`.** With `weightwedge` back to 1 this is again the
  previous follow-up's behaviour (yanks with a still hand, timed release).
  The ini and docs say so, and that `weightwedge 0` as well means never
  letting go.
- **Low: `TouchingPawn` per tick.** It is now cached for 0.1 s and read only
  while the object counts as blocked (the wedge release too). As before, the
  regrip time does not accumulate while the object touches a pawn.
  - A first version read it only at the decision and let the time accumulate.
    The radio pressed against the dummy with a still hand then switched
    between "stuck" (0.1 m/s) and the full drive at every cache change. The
    dummy took a held hit at 1.5 and 2.0 m/s in `heldswing`'s pressed phase
    (`p2r3/hitall.txt`; 0 in every earlier run).
- **Low: comment nits.** The wrap and the `props.h` list are fixed, and the
  444 reversals are cited as `p2r2/old_F_still_1`.
- **Tuning** (griptune; no new ini key):

  | `griptune` | Default | Meaning |
  |---|---|---|
  | `regripspeed` | 3 cm/s | The palm faster than this (0.1 s mean, world) counts as the hand moving |
  | `regriprest` | 0.25 s | After a regrip, at the first hand move, the object slides back onto the palm in this time (0 = it stays where it was re-gripped) |
  | `regripreach` | 16 cm | Was 8: the palm at most this far from the box for a regrip; farther, the timed release |
  | `wedgetime` (`weightwedge`) | 1 s | Back from 0 |

- **Held object pressed onto the ground, hand still, then lifted 30 cm**
  (`p2r3/press.py`, `press2.py`; novr, level 1 CP04). The object is spawned
  20 cm from the hand (the case 40 cm) and taken with `grabtest`. It is
  lowered until it stops on the ground, pressed 6 cm further in one move, and
  held still 2.5 s. `REGRAB` runs then let it go on the ground and take it
  again with the hand still: a heavy object grabbed on the floor, the case
  the review predicted would pop. The measures are the worst regrip of the
  run over 0.4 s from its first tick. z rise is the highest the origin got
  above its height before the regrip; rotation is the measured angular
  velocity integrated.

  | Run (dll) | Hz | Regrips | z rise | Max step | Rotation | w max | Lift | Final err |
  |---|---|---|---|---|---|---|---|---|
  | radio full, old dll | 144 | 1 | 5.2 cm | - | 56 deg | 5.2 rad/s | - | - |
  | radio light / full (1) | 144 | 1 / 1 | 0.0 / 0.0 | 0.0 / 0.2 cm | 0.2 / 0.6 deg | 0.06 / 0.07 | 29.8 cm | 0.2 cm |
  | radio light / full (1) | 90 | 1 / 1 | 0.0 / 0.0 | 0.0 / 0.2 cm | 0.5 / 0.3 deg | 0.13 / 0.03 | 29.6 cm | 0.4 cm |
  | case light / full (1) | 90 | 1 / 1 | 0.0 / 0.0 | 0.0 / 0.0 | 0.3 / 0.1 deg | 0.03 / 0.01 | 29.6 cm | 0.4 cm |
  | case full, barrel full (1) | 144 | 1 / 1 | 0.0 / 0.0 | 0.2 / 0.2 cm | 0.3 / 0.6 deg | 0.04 / 0.05 | 29.8 cm | 0.2 cm |
  | barrel full REGRAB, iterative inverse (1) | 144 / 90 | 5 / 5 | 9.6 / 12.0 cm | 0.75 / 1.23 cm | 25.7 / 27.8 deg | 1.37 / 1.47 | 29.4 / 29.2 cm | 0.1 / 0.4 cm |
  | barrel full REGRAB (2) | 144 / 90 | 2 / 1 | 0.2 / 0.4 cm | 0.2 / 0.2 cm | 1.1 / 1.4 deg | 0.36 / 0.35 | 29.8 / 30.0 cm | 0.1 / 0.4 cm |
  | barrel light REGRAB (2) | 144 / 90 | 1 / 1 | 0.0 / 0.2 cm | 0.0 / 0.28 cm | 0.2 / 1.0 deg | 0.07 / 0.09 | 29.8 cm | 0.1 / 0.4 cm |
  | case full REGRAB (2) | 144 / 90 | 2 / 1 | 0.0 / 0.0 | 0.2 / 0.0 cm | 0.4 / 0.4 deg | 0.11 / 0.10 | 29.8 / 29.6 cm | 0.1 / 0.4 cm |
  | radio full REGRAB (2) | 144 | 2 | 0.0 | 0.2 cm | 1.4 deg | 0.37 | 30.0 cm | 0.1 cm |
  | barrel full REGRAB (3) | 144 | 2 | 0.2 cm | 0.2 cm | 1.2 deg | 0.34 | 30.0 cm | 0.2 cm |
  | case full REGRAB (3) | 144 | 2 | 0.0 | 0.2 cm | 0.4 deg | 0.10 | 29.8 cm | 0.2 cm |
  | radio full REGRAB (3) | 90 | 1 | 0.4 cm | 0.2 cm | 4.0 deg (a) | 2.24 (a) | 38.6 cm | 0.4 cm |

  (1) intermediate dll with the fixed-point inverse; it converged for these
  (residual 0.02 cm). (2) dll with the analytic inverse, before the
  nearest-point palm rest (residual 0.00 cm, at most 0.06 degrees). (3) final
  dll. Pressed at `light`/`full`, the barrel usually did not regrip: it tilts
  about its ground contact and the grip error stays at 1.9-2.3 cm. `grabtest`
  grips the radio with the back of the hand (palm normal away from it), so its
  palm rest is refused (computed 42-96 cm) and the palm stays 4-7 cm off. This
  is an artefact of the test grab. (a) The radio was already turning at
  1.3-2.1 rad/s before the regrip, twisted on the ground by the canonical
  turn. After the regrip this dropped to 0.04 rad/s within 0.1 s. The palm
  was 10.9 cm away (inside the new 16 cm reach); the 9.3 cm palm rest came
  with the lift, which ended with the palm 1.9 cm from the box.
- **Results on the final dll** (md5 aa80070f...; novr, level 1 CP04, `light`;
  scripts `p2r3/final2.sh` blocks 2-3 and `p2r3/final3.sh`, output
  `p2r3/final2.txt`, `final3.txt`; earlier partial batteries on intermediate
  dlls in `p2r3/battery1/`):
  - **Crate bottles, still hold 12 s (3) and lift out (5)** (`wedge3.py`,
    = `p2r2/wedge2.py` plus a `slow` action): **8 of 8** held until the
    scripted release, lifted 62-80 cm, error at the end 0.2 cm (1.2 in one),
    0-8 reversals over 1 cm per hold, largest step 1.6-4.4 cm, no breakaway,
    none slipped or let go (`p2c/wedgemetric.py`: 8 PASS). Seven regripped
    0.31-0.46 s after the grab, with the palm 0.0-8.6 cm from the box. After
    the lift the palm was 0.9-1.6 cm from the box in six of them (before the
    review: `F_lift_4` 5.4 cm for the whole hold). `F3_lift_0` regripped a
    third time, blocked at the top with the hand still, and ended 7.8 cm off:
    no hand move came after that regrip, so no palm rest. On the regrip tick
    the object still moves 0.6-2.5 cm, from the command of the tick before.
    In the four ticks after it, it moves at most 0.35 cm (before: 0.5-4.6 cm,
    `F_still_2`, `F_lift_4`, `smR4`).
  - **Slow pull** (regrip with the hand still, then up at 5 cm/s for 4 s; 3
    bottles): no regrip and no breakaway during the pull. Each bottle came
    up with the hand (23-28 cm, error 0.2-1.1 cm), wedgemetric 3 PASS.
  - **Hand moved 25 cm away, still 3 s, then back and up** (`lat`, 2): both
    held 10.5 s until the scripted release, 3-5 reversals, PASS. In
    `F2_lat_1` two breakaways gave up (force x26, x22). Then, with the hand
    still 25 cm aside, the hand regripped: palm 6.9 cm from the box, inside
    the 16 cm reach. The palm rest (5.8 cm) came on the way back.
  - **Objects taken from the floor** (REGRAB, rows (3) above): barrel and case
    at `full`, 144 Hz, and radio at `full`, 90 Hz. No pop: z +0.0-0.4 cm,
    0.4-4.0 degrees, residual 0.00 cm.
  - **Smash in the hand** (`p2b/smashloop.sh light 9 smT`, crate bottle):
    6 of 9 broke in the hand ("smashed against something in the hand", hand
    3.6-4.9 m/s); `p2c/wedgemetric.py` 9 PASS, 0-7 reversals.
    - In runs 1-3 the 25 cm lift left the bottle in the crate: "blocked by a
      wall or an enemy between it and the hand: no breakaway", the previous
      follow-up's rule. With the palm 17.9-19.7 cm from it, the hand let it go
      0.62-0.63 s later (`weightwedge`), so nothing was smashed. Runs 1-3 are
      one session, with the same three bottles as runs 4-6 and 7-9 after each
      relaunch, but reached from another direction.
    - Same pattern in the intermediate battery (`p2r3/battery1`, runs 7-9).
      Before the review such a bottle stayed "held" at 20+ cm (`smR4`, 3.1 s)
      and broke only when the smash brought the hand back down onto it.
  - **Hit pipeline** (`p2b/hitall.sh p2r3_hit3`, summoned dummy; the p2d run
    before this review in brackets):
    - `hitthrow`: held hit on the head at 1.6 m/s; release 4.6 m/s (hand
      3.56); flight 2.68 m/s, 80.2 degrees off after 25 ms [the same: 4.6 /
      3.56; 2.66 m/s, 81.1 degrees].
    - `heldswing`: one hit on Spine1 at 2.4 m/s in each fast swing; none
      pressed, pulled back or pushed slowly [the same]. The intermediate dll
      had a pressed hit; see `TouchingPawn` above.
    - `hitdrop2`: the dropped radio hits Spine3 at 1.8 m/s.
    - `regrab2`: 0 hits, wrist 0.0 rad/s.
  - **Grab suite** (`suite.sh p2r3T`):
    - Grasp: power grip, half extents 3.7 3.6 16.3 cm, turned 46 degrees.
    - Drive: settles in 0.058-0.109 s, tail error at most 0.4 cm, rotation
      0.0; sweep at 1.5 m/s: error at most 1.6 cm.
    - **Static grab (the regression): held 13.0 s, until the scripted
      release.** On the intermediate dll with the 8 cm reach it was let go
      after 1.85 s (palm 9.2 cm, see above).
    - Glove static: locked, flick 1.3 m/s, flies 1.8 m in 0.41 s, caught.
    - Knock: bottle moved 54.6 cm; barrel 0 cm (a static collection).
    - Drop: `bottle_round02` did not break, its largest change 5.0 m/s
      against a threshold of 5. This test is borderline: 4.6 m/s, no break,
      in the p2d suite before this review; 5.1 and 5.3 m/s, broke, in others.
      Here the regrip's palm rest moved the bottle 8.9 cm onto the palm
      before the lift.
    - Smash: `bottle_quad01` broke in the hand 5.1 s after the grab.
    - Shot: aim converged in 2 iterations (error 0.1 degrees), the radio
      moved 162 cm.
    - Hinges: door 60.9 degrees, stop sign 5.6 degrees.
    - Props tick: 0.167-0.175 ms on average, at most 0.98 ms. The p2r2
      close-out measured 0.162-0.180 ms, and copy B was running during both.
  - Unit tests 65/65; build without new warnings.
- **Limits.**
  - A crate bottle jammed among its neighbours can still vibrate after a
    regrip with the hand still. In `p2b/smS_light_7` (intermediate dll), the
    settle did not calm it: PhysX contacts moved it at about 0.8 m/s while
    the command was 0. Afterwards it shook 0.3-2 cm per tick at a 1-3 cm
    grip error, under `regriperr`, so no new regrip, for 0.5 s until the hand
    moved: 19 reversals. Earlier series show the same pattern with 7-8
    reversals (`smR_light_2/4/6`). Not addressed here.
  - With the palm more than `regripreach` (16 cm) from a blocked object, the
    hand lets it go after `weightwedge`, even if the object later could have
    come free. Example: a bottle left behind by a lift that a wall blocked
    (no breakaway).
  - Until the first hand move after a regrip, the object stays where it was,
    up to 16 cm from the still palm.

**Hand spheres must not block the gun (25/10; bug found by copy B's aim baseline).**
- **The bug.** Copy B's aim baseline saw shots start "clipped" onto a
  `KActorSpawnable` at the muzzle: the impact sat on the shot's start. That
  actor is the mod's own invisible hand sphere (stage 3, `ProxyCreate`). Two
  traces met it:
  - `MuzzleStart` traces from the right shoulder (`Arm_Right`) to the muzzle
    with `TraceLine`, and starts the shot 2 units before whatever it hits.
    A palm sphere on that line pulled the start back behind the hand, and
    the shot left from inside or just behind the sphere;
  - the game's own shot trace (`CalcWeaponFire`, from our start) and the aim
    marker's trace then hit the sphere.
  The left sphere sits on the barrel with the left hand on the handguard
  (front of the handguard, or in front of the muzzle); copy B also saw the
  right palm sphere on the shoulder-muzzle line at some wrist poses.
- **The fix: the spheres take part in no trace.**
  - `PrimitiveComponent` +0x14C (`tools/ue3.py props PrimitiveComponent`):
    `CollideActors` 0x1, `BlockActors` 0x4, `BlockZeroExtent` 0x8 (zero-extent
    traces: shots, marker, view), `BlockNonZeroExtent` 0x10 (box traces),
    `CanBlockCamera` 0x20, `BlockRigidBody` 0x40 (PhysX).
  - The native `SetTraceBlocking` (`UPrimitiveComponent_execSetTraceBlocking`,
    0x007B8B50, Ghidra) only writes bits 0x8 and 0x10, with no reattach: the
    traces read them on every call. `ProxyNoTraces` does the same write after
    `SetStaticMesh` in `ProxyCreate`: a new sphere goes from 0xC3007D to
    0xC30065 (`PROPS: <side> hand body created (..., collision flags
    0xC3007D -> 0xC30065: no traces)`). `BlockRigidBody` stays, so PhysX
    contacts are unchanged: the sphere still pushes and knocks props, and
    walls still stop it.
  - `ProxyTick` re-reads the bits every tick (one `TrySafeReadUint32`) and
    turns them off again if anything set them back (`PROPS: <side> hand
    sphere was blocking traces again ...`). Never logged in the tests.
  - The mod's own traces also skip a sphere if one is hit anyway:
    `TraceLine` (used by `MuzzleStart`, the aim marker and the other gun and
    body checks) restarts 6 units past it, at most twice; `TraceLineView`
    already skipped it (`IsHeldActor`). Both, and the shot hook after the
    game's `CalcWeaponFire`, call `props::HandSphereTraced`, which turns the
    bits off again and logs `PROPS: WARNING <who> hit the <side> hand sphere
    ...` (at most 20 lines). New: `props::IsHandSphere`. Never logged in the
    tests.
  - Held objects are not touched: a radio held in front of the muzzle still
    stops the shot, as a real one would.
  - The gun hand and aiming code (`HandTarget(0)`, `GunBarrel`, `MuzzleStart`'s
    geometry, ADS, `CalcWeaponFire`'s start and direction) are unchanged; only
    what the traces see changed.
- **Test: `p2d/spheregun.py`** (scratchpad; level 1 CP04 after `relaunch.sh`,
  novr, 144 Hz cap). Same setup as `tools/weapontest.py`: `firelog on`,
  `spread off`, `synth 0 -30 0`, `synthaim 0 -35`, `synthhand 40 20 -30`,
  weapons given and equipped the same way; `twohand off`, so the gun follows
  the right hand only and the grip point stays put while the left hand moves.
  - The grip point in `synthleft` coordinates comes from `armdebug`'s
    `ARMS grip: ... dist` (the left controller's distance from the drawn
    grip, 1 unit = 2 cm) at four `synthleft` points 8 cm apart
    (trilateration): rifle `69.8 10.0 -45.7` (0.30 units from the grip),
    pistol `37.9 11.3 -35.1` (the synthetic cup grip, 0.10 units).
  - Poses: `grip` = the controller on the grip point; `drawn` = on the drawn
    grip after its slide towards the right hand (rifle, 8.4-8.7 units);
    `seg` = the palm (sphere centre, from the `GRABLOG` palm) moved onto the
    right shoulder -> muzzle segment, 80 % towards the muzzle; `front` = the
    palm on the barrel axis 3 units past the shot start. Sphere positions and
    flags are read from memory (`KActorSpawnable` with the `Sphere` mesh).
  - Per shot (`SHOT:` line): clipped = impact within 0.5 units of the start;
    own hand = impact `on KActorSpawnable` within 6 units of a sphere; start
    pulled back = shot start over 1 unit behind the nominal muzzle point
    (median of the `grip` pose); shot vs barrel axis; shot vs marker.
  - A/B in the same session, same harness: the dll from before the change
    (built from `snap_p2d_spheretrace`) and the new one.

  | Weapon, pose (left sphere: off axis / along / off the shoulder-muzzle segment, units) | Before: shots, clipped, own hand, pulled back | After |
  |---|---|---|
  | M4 grip (0.8 / 9 / 9) | 12, 0, 0, 0 | 13, 0, 0, 0 |
  | M4 drawn (1.0 / 9 / 9) | 12, 0, 0, 0 | 13, 0, 0, 0 |
  | M4 seg (before 3.6 / 40.8 / 5.0; after 3.6 / 26.4 / **0.07**) | 13, **13, 13, 13** | 12, 0, 0, 0 |
  | M4 front (0.1 / 40.4 / 3.1) | 13, **13, 13**, 0 | 13, 0, 0, 0 |
  | M9 grip (5.1 / 3.3 / 1.5) | 6, 0, 0, 0 | 6, 0, 0, 0 |
  | M9 seg (before 3.5 / 17.7 / 5.4; after 3.5 / 7.3 / **0.1**) | 6, 0, 0, **6** | 6, 0, 0, 0 |
  | M9 front (before 0.4 / 17.0; after 1.5 / 15.0) | 6, 0, 0, **6** | 6, 0, 0, 0 |
  | **Total** | 68 shots: **26 clipped on the own hand sphere, 25 starts pulled back** | 69 shots: 0, 0, 0 |

  - Shot vs barrel axis: max 0.039 degrees before and after (rifle and
    pistol). Shot vs marker: max 0.063 degrees after (rifle 0.063, pistol
    0.033). Before, one pistol shot read 4.19 degrees: the second shot of a
    burst, the drawn gun kicked 1.4 units by `GunKick` and the marker still
    from the frame before (same impact point); not a sphere.
  - The shot start sits on the nominal muzzle point in every pose after the
    fix (rifle 37.3-37.4 units along the barrel, pistol 13.6); before, the
    rifle `seg` shots started at 34.8-35.6 and the pistol ones at 9.0-11.1.
  - The marker trace ended on the hand before (rifle `seg`/`front`: marker
    on the sphere, impact 0.0 units from the start); after, 84 units away
    on the ground for every rifle pose.
  - The plain `grip` pose did not clip in this synthetic pose even before:
    the left palm sphere sits 0.8 units from the barrel axis but 9 units
    along it, behind the muzzle and 9 units off the shoulder-muzzle line.
    How close it gets to that line depends on the wrist and stock pose
    (copy B's poses). `seg` and `front` put the sphere exactly where it
    blocked; after the fix the sphere sat 0.07 units (rifle) and 0.1 units
    (pistol) from the shoulder-muzzle segment and nothing changed.
  - No `hand sphere` warning line in any run: the bits stay off.
- **Right sphere (`spheregun.py --right`).** Left hand off (`synthleft 0 0
  0`: its sphere parked), `synthaim 0 -10`, the gun moved until the barrel
  axis passes through the right shoulder (0.07-0.23 units off): the
  shoulder-muzzle trace then runs along the barrel, 2.0-2.2 units from the
  right palm sphere (radius 2.5).
  - Before: M4 12 shots, none pulled back; M9 6 shots, **3 started behind
    the hand** (6.0-4.3 units behind the barrel origin instead of 13.6 in
    front; start 3.8 units off the axis; shot vs marker 0.65 degrees).
  - After: M4 13 shots and M9 6 shots, 0 clipped, 0 pulled back, start on
    the muzzle point (37.2-37.4 / 13.6-13.7), shot vs axis at most 0.030
    degrees, shot vs marker at most 0.022.
- **Walls still stop the sphere (`p2d/wallsphere.py`).** Walker in front of
  the CP04 crate as in `p2b/walltest.py`; the wall is the crate's third
  `YBlockingVolume` (blocks rigid bodies only). The free left hand moves
  forward at 0.3 m/s for 90 cm; the sphere is read from memory at ~100 Hz,
  the palm from `GRABLOG`. Depth > 0 = inside the wall, units.
  - Before (old dll): palm up to 4.6 inside; while the palm was inside, the
    sphere stayed outside in 43 of 56 samples; the sphere centre never got
    past -0.8 (-1.7 before the first reset).
  - After: palm up to 4.6 inside; sphere outside in 44 of 56 samples; centre
    at most -1.0 (-1.8 before the first reset). Same as before.
  - In both, once the palm is more than 15 cm past the sphere for 0.15 s the
    stage-3 rule puts the sphere back on the palm, and PhysX pushes it out
    again: 8-10 jumps over 4 units in 0.4 s, separation up to 25 cm. That is
    the existing teleport rule (item 5 of the plan replaces it), not this
    change.
  - The bottle knock (suite) still works, see below.
- **Regressions** (final dll, novr, level 1 CP04):
  - Hit pipeline (`p2d/hitall.sh p2d_hit`, summoned dummy; previous dll in
    brackets, `p2r2_hit` 17:09): `hitthrow` hit the head with the held radio
    at 1.5 m/s [1.6]; `heldswing` one hit per fast swing (Spine1, 2.4 m/s
    [2.5]), none pressed, pulled or pushed slowly; `hitdrop2` Spine3 at 1.8
    m/s [1.8]; `p1m/regrab2.py` 0 hits, wrist 0.0-0.1 rad/s [0, 0.1].
    `f0r/regrab.py` (the old script) again logged a held hit at 3.1 m/s
    after an 81.7 rad/s wrist tick: its release and re-grab arrive in the
    same 0.5 s command poll, so the hand turns 80 degrees while holding
    (documented under item 1; `regrab2` waits 0.8 s).
  - Grab suite (`suite.sh p2d_s`; previous dll at 17:19 in brackets): grasp
    unchanged; drive settle 0.051-0.108 s [0.059-0.109], sweep error max 1.6
    cm [1.7]; static grab, glove static, smash, shot (radio moved 163 cm
    [163]), door 60.9 degrees [57.9] and sign 5.6 [5.3] as before; knock
    bottle moved 49.7 cm [55.7] (`left hand knocked bottle_round02 at 1.2
    m/s`), barrel 0.0 [0.0]. Drop (break): did not break, largest change
    4.6 m/s (threshold 5) [broke at 5.1]: that grab hit the item-2 regrip
    and the bottle was let go at 0.8 m/s; run again on the same build it
    broke at 6.0 m/s. The drop test sits at the threshold (5.1-5.3 m/s in
    most runs, it also failed at 2.2-3.2 m/s in older runs).
  - `PROPS: tick` 0.164-0.178 ms on average [0.167-0.174].
  - Build without warnings, unit tests 65/65. No new option: this is a fix.
    README's interactive-objects bullet says the hands never stop your own
    shots or the marker.
- **Limits.**
  - Not checked in the headset (VDXR, Steam Link) with real wrist and stock
    poses; the novr poses above cover the geometry (sphere on the axis, on
    the shoulder-muzzle line, in front of the muzzle).
  - Copy B's tree has its own diagnostics for this (`TraceLineActor`, the
    `| muzzle clip <name> held <0|1>` fields of the SHOT line): at the merge
    they can stay; with the spheres out of the traces they should only show
    held objects.

**Boneworks plan, item 3: impact haptics scaled by impulse (25/10).**
- **Goal.** A thud when a held radio hits a wall, a tick when a bottle taps
  the ground, a felt slap with the empty hand; one pulse per contact, none
  while holding still, during a snap turn or while an object settles into
  the hand. Option `impacthaptics off|low|full` (Basic menu "Impact
  haptics", default `full`; `low` = amplitude x 0.5; `off` = no impact
  pulses, and grab and knock keep the old fixed pulses: 50 ms and 20 ms at
  3500 us).
- **How the controllers get it.** See "Haptics": `PulseHand(hand, ms, amp,
  freqHz)`, mixed per frame with the game rumble by `vr::pure::HapticMix`.
  On SteamVR Input and on the OpenXR backend amplitude, frequency and
  duration all reach the runtime (`TriggerHapticVibrationAction` ->
  `InputShim` -> `xrApplyHapticFeedback`); on the legacy API only the
  amplitude (as microseconds). Hands: props `h = 1` (left) is
  `PulseHand(0)`, as the existing pulses.
- **The detector looks at the physics, not at the hand.** In the tick before,
  PhysX got a velocity command (`held.lastCmdV`, after every cap; for the hand
  sphere its `SetLinVel` of `ProxyTick`). What is missing after the step,
  minus one step of gravity (the sphere has none), was taken by a contact:
  `Dv = command - g dt (down) - measured` (`NxVelocities`, x 50), J = kg x
  |Dv| (N s). In free air Dv is ~0 whatever the hand does, tracking jitter
  included: measured 0.000 m/s median, 0.020 at most over 1179-1195 ticks of
  holding a radio still for 10 s (144 Hz). The held object's velocity is now
  read also with `objectweight off` (one `NxVelocities` per tick, read only;
  the old path is otherwise unchanged).
  - `HeldImpact` (in `Drive`, before this tick's command) and `SphereImpact`
    (in `ProxyTick`, mass `griptune hapspherekg` 5 kg, only for the free,
    empty hand above `hapspeed` 0.5 m/s).
- **One pulse per contact: `vr::pure::ImpactEdge`.** The plan's per-tick rule
  ("J above twice the previous tick and Dv above 0.8 m/s") missed 1 of 3
  ground contacts in the first test (`p3h/h3`): a corner of the radio met
  the sloped sand, the radio turned, and the 1.5 m/s went away in 8 ticks of
  0.42-0.75 m/s. Now:
  - a contact starts, when armed, on a tick whose loss is above half of
    `hapdv` (0.4 m/s) and above twice the previous tick's loss (the plan's
    rising edge: dragged over the sand or over another radio, friction takes
    0.18-0.48 m/s per tick, growing slowly while the hand speeds up; summed
    up, that looked like a hit in `p3h/w5`); since the review (below) also
    from calm, and with the loss scaled to 1/144 s above 144 Hz;
  - for `hapwindow` (50 ms) the loss vectors are summed; it counts at most
    as the approach speed: the minimum, over the 3 ticks before the contact
    (since the review: the ticks of the last `hapapproach` 25 ms, at least
    2), of the measured velocity along the lost direction. Pressing an object
    that already rests on the ground pushes the command into the ground every
    tick (0.23-7 m/s) but nothing approaches; and with `objectweight off`
    the old drive bounces a blocked radio up and down (6 cm, command up to
    9 m/s): one tick down at 6.8 m/s after three going up gave a full pulse
    in `p3h/F2_radio_off` before this rule;
  - it fires once when that passes `hapdv` (0.8 m/s), with J = kg x the loss
    at that moment (since the review: the felt mass x the loss, see below);
  - then it re-arms only after the contact ends: loss under 0.4 m/s for
    `haprearm` (40 ms). Pressed against a wall, or shaking against an
    obstacle, it does not repeat; at most one pulse per `haprefr` (80 ms) per
    hand, shared by held object, slap, knock and grab.
- **Mute windows.** The contact still uses up its pulse, logged as `not
  pulsed (<why>)` with `grablog`/`hapticlog`.
  - Frame jump or snap turn: `FrameJump`, and any turn of the hand frame
    above 10 degrees in a tick (also with `objectweight off`), for 0.2 s.
  - Game hitch (a tick over 50 ms), for 0.2 s. In one 124 ms hitch PhysX
    dropped the still radio 8 cm (a fake 1.3 m/s); after a 108 ms one the
    radio pressed on the ground was pushed up 5 cm and the drive brought it
    back down at 2 m/s 40 ms later.
  - The object settling into the hand (`grabAt` + `blendT` and the weight
    blend, + 50 ms) and the settle after a regrip.
  - A breakaway and 0.3 s after it. The radio pressed 20 cm into the ground
    starts a breakaway; its "thrashing" phase lifts it 5 cm and, back to the
    weight, the drive slams it down at 2 m/s: a second impact made by the
    drive (2 pulses per contact in `p3h/F3_radio`).
  - Sphere: put back on the palm (`SetRBPosition`, 50 ms), parked, hand
    speed above 8 m/s (tracking jump), sphere more than 15 cm from the palm
    (catching up with the hand: after a jump of the fake hand it gave a
    6.5 m/s "slap" in `hitthrow.py`).
  - Sphere: the hand beyond a surface the sphere was stuck on. When the
    sphere starts lagging (over 15 cm) while a contact blocks it (over
    0.4 m/s lost in that tick) and the hand is not jumping, `ProxyTick`
    stores the stuck point and the direction to the palm; slaps stay muted
    until the palm is back on the near side (or 2 s). Without it the empty
    hand pushed 30-50 cm into a dune gave a second pulse on the way up: the
    sphere goes through the sand (put back on the palm, or tunnelling under
    its 10 m/s catch-up command; traced with `p3h/slaptrace.py`) and hits it
    from below.
- **Pulse (plan formula, unsourced; `vr::pure::ImpactPulse`).** amp =
  min(1, 0.2 + J / `hapj` (5)) x gain, ms = 12 + 8 x min(J, 4), 90 Hz above
  5 kg, 225 Hz up to 5 kg. A radio (8 kg at the level's scale 1.25, 5.1 kg
  spawned at scale 1, see item 1) saturates already at the threshold (0.8
  m/s x 5.1 kg = 4.1 N s: amp 1, 44 ms, 90 Hz); a 0.8 kg bottle gives
  0.36-0.44 at 0.9-1.5 m/s (225 Hz). Slap: J with 5 kg, so also full.
  (Before the review: since then J is the felt impulse, see below.)
  - Grab (`GrabPulse`; before 50 ms at 3500 us for everything): 0.4 + 0.5 x
    min(1, kg / 30), 30-50 ms (bottle 0.43 / 31 ms, radio 0.53 / 35 ms,
    33 kg barrel 0.9 / 50 ms; since the review 0.6 + 0.3 x min(1, kg / 30),
    not halved by `low`), and for a catch (hand-object speed above 0.5
    m/s, gravity gloves) at least the impact pulse of the reduced mass
    (kg x 5 / (kg + 5)) times that speed.
  - Knock (`ProxyKnockStatics`; before 20 ms full): impact pulse of the
    reduced mass times the hand speed. Since the review not on the gun hand
    (see "Haptics"): there the old 20 ms pulse stays.
- **Log.** `HAPTIC: <side> hand held|slap|grab|knock <mesh>: J ... N s (dv ...
  m/s, ... kg) -> amp ..., ... ms, ... Hz (full|low) at (x, y, z)` (object,
  sphere or palm): the first 30 of a session always, all of them with
  `grablog`/`hapticlog`, which also log the muted contacts (`not pulsed
  (<why>)`).
  `HAPTIC out: <support|gun> hand amp ... freq ... Hz dur ... ms us ...
  (impact pulse | game rumble / fixed pulse, <backend> via action | legacy
  pulse | dry run, no VR)`: every frame sent, with `hapticlog N`; without a
  headset the XInputGetState hook computes what `UpdateHaptics` would send.
- **Tests (novr, level 1 CP04, 144 Hz; scripts in `scratchpad/p3h`).**
  Final build, series `p3h/final4.sh` (output `p3h/final4.txt`), the
  earlier runs `final2`/`final3` in brackets. Every hapt.py run grabs a
  spawned object in the fake left hand, holds it still, probes the ground
  slowly (0.6 m/s, no pulse: under `hapdv`), then lowers it 3 times at
  1.5 m/s to 20 cm under the contact and back up.
  - Radio (8 kg), `objectweight light`: 10 s still, 1195 ticks, 0 HAPTIC
    lines, Dv 0.000 median / 0.020 m/s max; 4 snap turns of 45 degrees (8
    frame jumps) 0 lines; 3 contacts, 3 pulses (J 8.5-11.9 N s, amp 1.00 =
    formula, 44 ms, 90 Hz), plus one breakaway impact muted. [`final3`: 2 of
    3 contacts gave a second pulse from the breakaway, fixed above.]
  - Radio, `objectweight off` (no frame jumps: the old drive chases the
    arc): 10 s still 0 lines; 4 snap turns 0 lines; 3 contacts, 3 pulses.
    [`final2`: one 6.8 m/s pulse during the slow probe, fixed above.]
  - Bottle (0.8 kg): 3 contacts, 3 pulses, amp 0.36-0.44 at J 0.80-1.22,
    within 0.004 of the formula, 18-22 ms, 225 Hz.
  - `impacthaptics low`: 3 pulses, amp 0.19-0.22, within 0.004 of half the
    formula; grab 0.21 instead of 0.41. `off`: 0 HAPTIC lines (grab and
    knock back to the old fixed pulses).
  - Empty-hand slap, 3 descents at 1.5 m/s at two places on the dunes: one
    pulse each (J 5.8-7.5 N s), 6 descents out of 6. [`final2`: the way up
    gave a second pulse at one place, fixed above.]
  - Held radio swung into the summoned dummy (`heldswing.py`, the capsule
    is a vertical wall): one pulse on each fast swing (J 7.1-9.4 N s), none
    while pressed against it for 2.5 s, on the slow pull back or on the
    0.5 m/s push. The hit lines are the same as before.
  - Firing rumble (`p3h/fire.py`, trigger 600 ms): every `HAPTIC out` frame
    of the gun hand is `amp 0.490 freq 150 Hz dur 20.0 ms us 1910` (= 600 +
    motor/20) with `full`, `low` and `off`; the unit test checks all 65536
    motor values against the old code.
  - Static bottle grabbed from the crate (`reach.py`): grab pulse only
    (0.43, 31 ms); in `final` two contacts 4 s after the grab, during a
    regrip settle, were logged as muted.
  - `PROPS: tick` with a held radio, same session: 0.196 / 0.200 ms on
    average with `full`, 0.196 / 0.182 ms with `off` (two 30 s windows each,
    the radio moved every 2 s); 0.186-0.199 against 0.190-0.204 ms in the
    earlier runs: no measurable cost.
  - Shared hit pipeline (`p3h/hitall.sh`, summoned dummy): `hitthrow`
    held hit on the head at 1.5 m/s, throw 2.65 m/s (wanted 4.63), object
    ends in front of the dummy [item 2 baseline 2.68 / 4.63]; `heldswing`
    hits on swings 1 and 4, none on 2, 3, 5; `hitdrop2` dropped hit on
    Spine3 at 1.8 m/s; `regrab` hit on Arm_Right at 2.8 m/s. Same as before.
  - Grab suite (`suite.sh`): canonical grip, drive settle 0.058-0.109 s and
    sweep 1.6-1.7 cm, static grab, glove catch, knocked bottle 54.6 cm, drop
    and smash break, shot 165.7 cm, door 60.9 deg, stop sign 5.4 deg: the
    same as the item 2 baseline (`p2r3/suite3.txt`) in `final2` (one build
    earlier) and `p3h/suite5.txt`. The other run on the final dll differed
    (`p3h/final4.txt`: the static bottle wedged in the crate after
    a regrip and slipped on reach.py's 2 m jump of the fake hand, door 49.4
    deg; the door ranged 51.0-61.6 deg in the item 2 suites): run-to-run
    variance of item 2's wedge logic, since the haptic code only reads
    PhysX velocities and writes the log and the pulse.
  - Unit tests 96/96 (31 new: `TestHapticMix`, `TestImpactPulse`,
    `TestImpactEdge`).
- **Review fixes (26/10).** Tests in `scratchpad/p3h`: `final_r.sh`
  (output `final_r.txt`) and, on the final dll, `post_r.sh`
  (`post_r.txt`); replay tool `p3h/replay.py`.
  - **Frame rate (`vr::pure::ImpactEdge`, `ImpactEdgeTune`).** The
    thresholds per tick were tuned only at 144 Hz.
    - Above 144 Hz a continuous loss (a corner that turns the object,
      pressing) takes a x dt per tick: start and re-arm now use dvN = dv x
      max(1, (1/144 s) / dt).
    - Below 144 Hz the loss is not scaled down. The review proposed to scale
      every rate to 1/144 s; rejected for the slower rates: a hard hit takes
      the same speed in one tick at any rate, and the logged ground contacts
      at 45 Hz take 0.78-0.93 m/s per 22 ms tick, 0.25-0.29 once scaled.
      Replaying the GRABLOG lines of the baseline runs (`p3h/replay.py`,
      the detector in Python; it reproduces every logged pulse of the old
      code) the scaled rule lost 3 of 5 contacts at 45 Hz; unscaled, all 5,
      and the baseline radio dragged over the sand at 45 Hz gave no pulse
      (`p3h/B_drag45`, 4 drag phases).
    - A contact also starts without the doubling when the loss in the
      ticks of the last 17 ms (at least 2) was under a quarter of the
      threshold: the baseline at 120 Hz missed a soft contact (0.36 then
      0.59 m/s per tick from calm, never doubling; `B_drag144`, descent 0).
      Friction that grows while dragging (0.18-0.48) is never calm before.
    - The approach speed is the minimum over the ticks of the last
      `hapapproach` 25 ms, at least 2 (3 at 144 Hz as before, 2 at 90 and
      45, 5 at 225); before, 3 ticks, i.e. 67 ms at 45 Hz. With one tick at
      45 Hz an object that bounced down for one tick after going up counted
      as approaching.
    - Replay of every saved hapt.py log of item 3 (75 contact files): the
      new rule fires exactly where the old one did, plus the soft contact
      above. `TestImpactEdgeRates` runs the same physical cases at 45, 90,
      120, 144 and 225 Hz (still, hard hit then pressing, soft contact,
      sand corner, drag on sand, approach history): one pulse per contact
      and none while dragging; the onset of friction from calm on another
      radio (w5 ramp) counts as one contact below 100 Hz.
  - **Gun hand (`GunHandHaptics`) and two hands.** No slap from the left
    hand while it is on the gun, two-handing or busy with the physical
    reload; there the knock and grab keep the old fixed pulses, as on the
    right hand (knock 20 ms at 3500 us; before the fix up to 1.0 for 44 ms
    at 90 or 225 Hz). The two hand spheres are not an ignored pair: a slap
    is muted ("other hand") while the other sphere is within both radii + 2
    cm, latched for `hapwindow` + 10 ms because the pulse fires a few ticks
    after the first touch, when the equal-mass spheres have already moved
    apart. The radii come from the bounds (7 cm: touching at 5.8-6.7 units
    between centres), not from `kProxyScale` (5 cm).
    - `p3h/twohand.py` (`R_th`): the left hand moved onto the right palm at
      2.5 m/s, 3 sweeps: 0 pulses, 5 contacts muted "other hand" (before:
      one 0.83 pulse per sweep); at 1 m/s the spheres share the speed and
      nothing reaches the threshold. Slap into the sand at 1.5 m/s with the
      free left hand: 2 of 2 pulses (0.72); with the left hand on the gun
      ("ARMS: left hand on the weapon", "AIM: two-handed grip"): 0 pulses,
      2 of 2 muted "hand on the gun".
  - **Hits of held objects (`OnPropHit`).** A held-source hit pulses with
    the reduced mass object-hand times the speed of the hitting point,
    with the held object's mute windows (`HeldHapticQuiet`) and the shared
    80 ms refractory: the physical contact of the same swing logs "not
    pulsed (33-42 ms after the last pulse)". `hitall.sh` (`R_hit`): the
    head hit of `hitthrow` (1.5 m/s) now pulses (0.97, 43 ms); the
    `heldswing` swings 1 and 4 pulse as hits (1.00); `regrab` 1.00; the
    hit lines are those of item 4's final run (head 1.5 m/s, Spine1 on
    swings 1 and 4, none on 2, 3, 5, dropped Spine3 1.7 m/s, Arm_Right 2.8
    m/s). The spawned radio flying into the grip next to the dummy "hits"
    its head 11-12 ms after the grab (held source): covered by the
    refractory in `R_hit`, muted as "settling into the hand" since.
  - **Felt impulse (low).** With J = kg x dv every object of 5 kg or more
    was full at the threshold: radio 8.5-11.9 N s and slaps 5.8-7.5, always
    1.00. Now J is the felt impulse: held object, hit, knock and catch with
    the reduced mass object-hand (`haptichandkg` 5 kg: radio 8 kg -> 3.08
    kg, bottle 0.8 kg -> 0.71), slap with `hapticslapkg` 2 kg. Measured
    (`final_r.txt`, amplitudes within 0.005 of 0.2 + J/5): radio 0.75-1.00
    at 0.89-1.31 m/s, bottle 0.33-0.40, slaps 0.53-0.80 at 0.82-1.50 m/s.
    Heavy objects still reach full strength soon (a 33 kg barrel: 4.3 kg
    felt, full from 0.93 m/s).
  - **Grab confirmation (low).** 0.6 + 0.3 x min(1, kg / 30), 30-50 ms,
    not halved by `low` (bottle 0.62 / 31 ms, 5 kg radio 0.65 / 33 ms); a
    catch adds the impact part, which `low` halves.
  - **Ini keys (low).** `hapticdv`, `hapticfull`, `hapticrefractory`,
    `hapticrearm`, `hapticwindow`, `hapticapproach`, `haptichandkg`,
    `hapticslapkg`, `hapticslapspeed` map to the griptune keys (control.cpp,
    ranges checked), with comments in `deploy/specopsvr.ini`, so the
    Advanced menu lists them; the startup log shows the 9 `GRIP: hap...`
    lines.
  - **Results on the dll of `final_r.sh`** (one build before the final
    one, which differs only in the hit pulse's mute windows):
    - 120 Hz (the ini cap): radio held still 10 s, 1196 ticks, 0 lines, Dv
      at most 0.020 m/s; 4 snap turns, 8 frame jumps, 0 pulses; 3 contacts
      3 pulses (+1 breakaway muted); `objectweight off` 0 / 0 / 3 of 3;
      bottle 3 of 3; `low` 0.17-0.20 (half); `off` 0 lines; slaps 3 of 3
      at a clean spot; at spot 0, 5 pulses in 3 descents: in 2 of them the
      hand first hit the radio left on the sand by the radio test (z
      654-655, its top) and then the sand (z 639), two contacts.
    - 90, 45 and 225 Hz (`fpscap`; 225 ran at 4-5 ms ticks): radio 3 of 3
      each (0.72-1.00), still 0 lines, snap turns 0 pulses; bottle 3 of 3
      each; slaps 3 of 3 each.
    - Drag (radio pressed 5 cm into the sand, dragged 1 m sideways at
      1 m/s and back, `p3h/drag.py`): 0 pulses while dragging at 120, 90,
      45 and 225 Hz; two pulses on the way back (45 and 225 Hz) were hits on
      radios left at that spot by the earlier drag tests (225 Hz: 2.55 m/s
      taken in one 5 ms tick while the radio caught up with the hand at 5.2
      m/s; probe contact at 130 cm instead of ~168: landing on a radio).
      Repeated at clean spots on the final dll: see below.
    - Firing rumble (`fire.py`): every gun-hand frame `amp 0.490 freq 150
      Hz dur 20.0 ms us 1910` with `full`, `low` and `off`.
    - Static bottle grabbed from the crate (`reach.py`): only the grab
      pulse (0.62, 31 ms).
    - `PROPS: tick` holding a radio: 0.193 / 0.193 ms with `full`, 0.193 /
      0.192 with `off` (item 3: 0.196 / 0.200 and 0.196 / 0.182).
  - Unit tests 148/148 (`TestImpactEdgeRates`: 30 checks; 6 more in
    `TestImpactPulse`).
  - **Results on the final dll** (`post_r.txt`, `drag2.txt`):
    - Drag at clean spots (sand ~1.6 m under the hands; at spot 8 a dune
      23 cm under them), 120, 90, 45 and 225 Hz, 2 descents each: 8 of 8
      contacts pulsed (0.80-1.00), 0 pulses in the 16 drag phases (there and
      back) and in the 8 lifts, also at 45 Hz, where dragging took 1.3-2.0
      m/s per tick (median), up to 6.2.
    - Radio at a sloped spot (`P_radio`): 3 s still 0 lines, 2 snap turns 0
      pulses, the one contact pulsed (0.75); then item 2's wedge release on
      the slope ended the run. Slaps 3 of 3 at a clean spot (0.60-0.71).
    - Hit pipeline (`P_hit`): the hit lines of `R_hit` and of item 4 (head
      1.5 m/s held; Spine1 on swings 1 and 4; dropped Spine3 1.8 m/s;
      Arm_Right 2.8 m/s); hit pulses 0.97-1.00; the grab-time "hits" of the
      spawned radio muted "settling into the hand" (4 of 4).
    - Grab suite (`suite.sh p3hP`): canonical grip (power, same extents),
      drive settle 0.067-0.109 s and sweep 2.1 cm, static grab, glove catch,
      knocked bottle 52.5 cm, barrel 0, drop and smash break, shot 164.2 cm,
      door 59.0 deg, stop sign 5.4 deg; item 4's final suite
      (`p4t/suite1.txt`): 0.033-0.108 s, 1.5 cm, 53.8 cm, 162.5 cm, 58.8
      deg, 5.6 deg. HAPTIC lines of the session: grab 0.61-0.63 (door 0.90
      / 50 ms), knock 0.50 and 0.89, the smash 1.00.
- **Limits.**
  - Not felt in a headset. Quest through SteamVR's legacy path codes the
    amplitude as duration and ignores the frequency; the plan's formula and
    thresholds are unsourced and need a headset pass (all are `griptune`
    keys and, since the review, ini keys).
  - At 45 Hz (ASW) a single tick cannot tell a hard hit from friction:
    resistance that takes more than 0.8 m/s from a moving object within
    50 ms, starting from calm, pulses once (also friction setting in when
    the held object starts rubbing on another one).
  - The threshold (0.8 m/s) ignores gentle placements: lowering a bottle on
    the ground at 0.6 m/s gives no tick (the probe phase of every test).
  - Contacts of a held object with Walker's own bodies are ignored pairs
    (no pulse). A held object hitting an enemy was felt only when PhysX
    stopped it (the capsule, fast swings in `heldswing`): the hits the
    pipeline found with its traces gave no pulse (`hitthrow`, 4 runs of 4).
    Fixed in the review: `OnPropHit` pulses for held-source hits.

**Boneworks plan, item 4: throwing from the peak, wrist spin, lever arm, energy cap (25-26/10; phase 1).**
- **Goal.** Throws go where and as hard as the swing meant, objects spin
  with the wrist, and heavy objects lob. Before (`classic`), a throw was the
  hand's average velocity over the last 60 ms (8 history samples, so 49-58 ms
  at 144-120 Hz) times `throwboost`. A thrower slows down before the hand
  opens, so the average sits under the peak: on the plan's ramp (4 m/s, then
  2 m/s at release) it gave 2.7 m/s. The spin was whatever the drive
  commanded on the last tick, error correction included, and the centre of
  mass of an object held away from the hand stopped dead on release.
- **Option.** `throwmode peak|classic` (Basic menu "Throw style", default
  `peak`; ini line with comment). `classic` is the old code path, unchanged
  (same 8-sample average, same cap, no spin set). `throwboost` gets the
  choice 1.1 (SteamVR's default); the default stays 1.3 until a headset test.
  `throwenergy <J>` (ini, Advanced menu) = `griptune energy`; since the
  review fixes also `throwplace`, `throwfull`, `throwglitch` (ini, Advanced
  menu) = `griptune peakmin|peakfull|peakout` (see "Review fixes" below).
- **History (`HandHist`, `HistPush`, `HistIdx`).** 16 samples (was 8) of the
  hand point plus the hand axes (`ax`) and the time the pose was read (`pt`),
  pushed every game tick in `TickImpl` for both hands whether or not they
  hold something, so the wrist's angular velocity exists before the grab.
  `HistVelocity` and `HandJumped` still read only the newest 8 samples:
  every older user (classic throw, smash check, hit pipeline, frame jumps,
  glove) is unchanged. `FrameJump` turns the
  stored axes with the positions. Drive's own smoothed wrist feed-forward
  (`g_wristW`, reset at each grab) is left as it was: moving it would change
  item 2's tuned drive; the throw computes its own wrist velocity from the
  axes history instead.
- **Sample times: when the pose was read.** `UpdateHeadTracking` (hooks.cpp,
  start of the game-thread Draw) stores `ProfMs()` in
  `vr::control::g_poseStampMs`; the props tick, later in the same Draw after
  the script tick and `UpdateArms`, stores it with the sample (`pt`; the
  tick time if the stamp is missing or more than 50 ms old). With the props
  tick's own time (`t`, still used by everything else) the work in between
  varied, and single samples read 5-10% off, sometimes far more: in the
  first final run one ramp throw (of 10 that day) took a 4.39 m/s sample
  from the window edge (the hand was at 2.7 m/s there) and left at 3.2 m/s. With the pose
  time the per-sample speeds follow the commanded ramp within 0.01-0.02 m/s.
  Since the review fixes the stamp is written only on the game-thread path
  (`ViewportDrawThunk::Detour`, one read per tick); with `headsample render`
  (2-3 reads per tick on the render thread) the stamp is 0 and the history
  uses the tick time, as before item 4.
- **Peak estimate (`vr::pure::PeakThrow`, unit-tested).** Per sample k, the
  velocity from sample k and the first older sample at least `peakdt`
  (6 ms) earlier: one frame at 90-144 Hz, two at the 225 ticks/s of an
  uncapped novr game. Samples faster than `peakjump` (12 m/s) relative to
  Walker (`PawnVelocity`, so a throw while running is not a jump) are
  dropped as tracking jumps. The peak is the fastest sample within `peakwin`
  (110 ms, Alyx's 10 frames at 90 Hz), averaged with its two neighbours,
  weights 1:2:1. The plain 3-sample mean of Alyx loses ~0.2 m/s on the plan's
  sharp ramp at 120 Hz (first in-game runs, still with tick times: 3.68 and
  3.79 m/s); 1:2:1 loses 0.17 (3.83, exactly what the game gives now) and
  still averages out single-sample noise. Since the review fixes the
  samples are the hand relative to the hand-frame centre (reference 0, not
  Actor.Velocity), isolated outliers are replaced by their 3-median first,
  and slow releases use the release-time average (see "Review fixes").
- **Wrist (`w`).** For the same sample pairs: the world-frame rotation from
  the older axes to the newer (as in `Drive`), axis x angle / dt, averaged
  over the 5 samples around the peak. A sample over `spinjump` (50 rad/s) or
  1.5 rad is a jump and is left out. Found in the item-0 `regrab.py` test:
  `synthleftrot` turns the fake hand 80 degrees in one tick (167 rad/s), and
  the release in that tick spun the radio at the 55 rad/s cap. Now `0+1
  rejected`, spin 0.2 rad/s. For a slow release (review fixes, below) the
  spin is instead the dt-weighted mean of the same checked per-sample rates
  over the release window.
- **Lever arm.** The centre of mass (component bounds centre) relative to
  the hand point now, taken into the hand axes and turned back into the
  hand axes of the peak sample: v = v_hand + w x r. Then `throwboost`, then
  the item-2 weight limit (with `objectweight` on, the object starts from its
  last weight command plus what the hand's force adds in 60 ms), then the
  caps. `SetAngVel(w)`, at most `spinmax` (55 rad/s, under the 60 of
  `RaiseMaxAngVel`). With the weight on, the spin starts from the last weight
  command plus what the hand's torque adds in 60 ms (`ThrowSpinLimit`, never
  under `alphaMin` x 60 ms).
- **Energy cap.** |v - Walker's velocity| <= sqrt(2 E / kg), E = `throwenergy`
  (`vr::pure::ThrowEnergyCap`, through `CapScale` like the existing 1.5 x
  `maxSpeed` cap; the lower of the two applies). The plan's 150 J
  [unsourced] was chosen for a 60 kg barrel (2.24 m/s); with the item-1
  calibration `masskg 40` the fuel barrel weighs 33.2 kg and left at 3.0 m/s,
  so the plan's "barrel <= 2.3 m/s" failed at the shipped settings (it passed
  only with `masskg 72`). Since the review fixes the default is 85 J
  (0.5 x 33.2 x 2.26^2): bottle (1.6 kg) 10.3 m/s, radio (5.1-8 kg)
  5.8-4.6, fuel barrel 2.26, the 52 kg rifle case 1.8.
- **Fallback.** Fewer than two usable samples: peak uses the 60 ms average,
  with the energy cap (`GRAB: throw peak: no usable hand history ...`).
- **Logs.** Every peak throw: `GRAB: throw peak: hand 3.91 m/s 58 ms before
  release (3+5 samples averaged of 16, 0+0 rejected), lever 0.02 m/s, object
  3.82 m/s before the caps, cap 13.49 m/s (energy 13.49 m/s at 1.6 kg), spin
  0.1 rad/s (wrist 0.1), v (...) w (...)`. With `grablog` on, `GRABTHROW:
  samples (ms ago: m/s) ...` lists every sample's speed. The existing `GRAB:
  thrown object flies at ...` line keeps its old text (p1m/throwdir.py parses
  it) and appends the speed without the gravity of the elapsed time and the
  measured spin (and the commanded one with `peak`).
- **Test command.** `synthleftramp f r u v1 ms1 v2 ms2 [release]`: the fake
  left hand speeds up from 0 to v1 cm/s in ms1, then goes to v2 in ms2
  (high-resolution clock). `release` opens the fake grip at the end and the
  hand goes on at v2 for 50 ms: the props tick sees the grip open one tick
  later, and without the tail that last sample was a hand frozen in place
  (0.4-0.8 m/s instead of 2), which no real hand does.
- **Measured, final dll** (one session, novr, level 1 CP04, `throwboost 1`,
  `objectweight light`, `synthsmooth on`; the game ran at ~120 ticks/s, the
  history samples 8.3 ms apart; `scratchpad/p4t/final.sh` with
  `p4t/throw4.py`, output `p4t/final2.txt`):

  | Test | `peak` | `classic` | Plan |
  |---|---|---|---|
  | Ramp 0 -> 4 m/s in 150 ms -> 2 m/s in 60 ms, release at 2 m/s, bottle | 3.83 m/s in 6 of 6 | 2.76, 2.80, 2.79 m/s | 4 +- 0.3; classic ~3 |
  | Wrist yaw 10 rad/s, hand still, bottle: spin measured by PhysX | 10.0, 10.0, 10.0 rad/s | 10.2, 9.9 rad/s | 10 +- 1 |
  | Same: centre of mass speed (9 cm from the hand) | 0.91 m/s (x3) | 0.0 m/s | |
  | Ramp + wrist pitch 10 rad/s stopping 10 ms before release: spin | 10.0, 10.0 rad/s | 4.1, 4.2 rad/s | |
  | Fast ramp (7 m/s, 5 at release), fuel barrel 33.2 kg, weight light | 3.0, 3.0 m/s (cap 3.00) | | |
  | Same, weight off | 3.0 m/s (6.74 before the cap) | 6.2 m/s | |
  | Same, barrel at 59.8 kg (`masskg 72`), weight light | 2.2, 2.2 m/s (cap 2.24; PhysX 2.22) | | <= 2.3 |
  | Radio, constant 3 m/s | 3.0 m/s | (3.0, the same speed) | |

  - Per-sample speeds of a ramp throw (`GRABTHROW`, ms ago: m/s): 0:2.00
    8:2.18 17:2.46 25:2.74 34:3.02 42:3.30 50:3.58 59:3.87 67:3.89 75:3.66
    84:3.45 92:3.23 100:3.01 109:2.78 117:2.55: the commanded ramp sampled
    every 8.3 ms. The flight speed measured by PhysX is 1-3% under the
    command (3.79-3.80 m/s at 25 ms, 3.68-3.73 at 108 ms, both without
    gravity: the bodies' damping).
  - Earlier runs of the same day (before the pose times, `p4t/t4*.txt`):
    ramp 3.78-3.85 m/s in 9 runs, then 3.20 once in the first final run (the
    timing glitch above).
  - Lever direction: the held bottle's own PhysX velocity on the last tick
    before release was (-14..-23, -40..-44, 6) units/s; the throw command
    was (-14, -42, 0). With `classic` the centre of mass stopped at release.
  - In steady wrist rotation `classic` already spins right (the drive's
    feed-forward), so the plan's "effectively random" holds only for a
    changing rotation: a wrist that stops just before release left the
    object at the drive's decaying 4 rad/s, `peak` gives the 10 rad/s of the
    flick.
  - Weight on: the barrel's 7 m/s swing became 3.41 m/s through the weight
    limit (21 m/s2 x 60 ms over its lagging speed), then 3.0 through the
    energy cap. Weight off: 6.86 m/s before the caps, 3.0 after.
- **Test harness caveat.** `synthleftvel` without `synthsmooth on` moves the
  fake hand in GetTickCount64 steps of ~16 ms: single samples then read up
  to twice the real speed and the peak picks them (stock `hitthrow.py`:
  peak samples of 8.5-8.9 m/s where the 60 ms average reads 3.6-4.8 and the
  smooth hand of the same script moves at 4.5; the radio's weight limit,
  aMax x 60 ms over its own speed, held the releases at 5.5-5.8 m/s, where
  `classic` gives 4.6-6.2). Throw tests must send `synthsmooth on`; the
  stock hit-pipeline scripts do not, so their thrown speeds carry this
  artifact. A headset delivers a new pose every frame.
- **Hit pipeline** (feature 0; the thrown-hit speed depends on the release).
  `p4t/hitall.sh` (relaunch, summoned dummy, `hitthrow`, `heldswing`,
  `hitdrop2`, `regrab`) plus `hitthrow.py ... 350` (Walker 3.5 m from the
  dummy, so the wind-up does not touch it):

  | Test | Before (snapshot dll, 25/10 23:46) | After (final dll, 26/10 00:57, `p4t/final2.txt`) |
  |---|---|---|
  | `hitthrow` | held hit on the head 1.5 m/s; release 6.0 m/s (hand 4.70), bounced off (77 deg), no thrown hit | held hit on the head 1.5 m/s; release 5.8 m/s (peak sample 8.91 from the step artifact, weight limit), bounced off (78 deg), no thrown hit |
  | `hitthrow` from 3.5 m, stock (no `synthsmooth`) | release 4.7 m/s (hand 3.63), thrown hit Spine1 (side trace) at 5.5 m/s | release 5.8 m/s (hand average 3.56, peak sample 8.88: the artifact), thrown hit Spine1 at 5.9 m/s |
  | from 3.5 m, `synthsmooth on`, same session A/B | `classic`: release 5.9 m/s, thrown hit Spine3 (side trace) at 6.0 m/s | `peak`: release 5.9 m/s, thrown hit Spine1 at 6.0 m/s |
  | `heldswing` 3 m/s | Spine1 2.4 and 2.7 m/s, none pressed or slow | Spine1 2.8 and 2.5 m/s, none pressed or slow |
  | `hitdrop2` | dropped, Spine3 1.8 m/s | dropped, Spine3 1.7 m/s |
  | `regrab` | 0 hits, wrist 0.0-0.1 rad/s | the release and the re-grab commands arrived in one batch (no release; the 80-degree hand jump swings the held radio into the arm: Arm_Right 2.8 m/s, the same as item 3's run); `p4t/regrab_slow.py` (batches 1 s apart), twice: same actor, wrist 0.0-0.1 rad/s, 0 hits, release spin 0.1 rad/s (the 80-degree sample rejected once, `0+1`) |

  The stock `regrab.py` sends the release and the re-grab 0.2 s apart and
  the mod reads commands every 0.5 s, so whether they merge is luck (they
  did not in the baseline run, did in item 3's and in 3 of 3 runs here).
  A constant-speed throw leaves at the same speed in both modes (A/B row);
  `peak` differs when the hand slows before release or the wrist turns. The
  thrown impact speed follows the release speed plus the fall (5.9 -> 6.0).
- **Cost.** One more 44-byte copy per hand per tick (axes and pose time);
  the estimate runs only at release. `PROPS: tick` on the final dll: 0.182-
  0.197 ms on average in the hit-pipeline phase, 0.205-0.218 during the
  throw series (a prop spawned, grabbed and thrown every 6.5 s, up to 8
  flying objects followed by the hit pipeline); 0.164-0.190 in the earlier
  runs today; item 3's runs 0.186-0.204. The per-tick code differs from the
  snapshot only by that copy, so the spread is the workload.
- **Grab suite** (`suite.sh`): `p4s1` (00:32, the build before the pose
  times) and `p4fin` (01:03, final dll), output `p4t/suite1.txt`,
  `p4t/suite_final.txt`:
  - grasp: power grip, centre f -2.6 p 5.5 a 6.6 cm, turned 46 degrees
    (both);
  - drive: settle 0.033-0.108 s / 0.066-0.108 s, sweep error 1.5 / 1.6 cm
    (item 3: 0.058-0.109 s, 1.6-1.7 cm);
  - static grab, glove (1.8 m in 0.41 s, caught), bottle knocked 53.8 /
    52.1 cm, barrel not knocked, smash broke in the hand, shot moved the
    radio 162.5 / 164.0 cm, door 58.8 / 60.0 degrees, stop sign 5.6 / 5.7
    degrees: all as in items 2 and 3;
  - drop: broke at 5.4 m/s in `p4s1`; in `p4fin` it did not break (largest
    change 4.6 m/s, threshold 5), the flaky landing documented in items 1
    and 2. That release was `v (0, 0, 0) w (0, 0, 0)` (hand still), which
    is what `classic` gives too. Two more `throwtest.py ... drop` runs on
    the final dll right after: broke at 6.0 and 6.2 m/s.
- **Phase 2 (runtime pose velocities): not done.** It needs the runtime's
  `vVelocity`/`vAngularVelocity` and the tracking-to-world mapping (plan:
  x' = vx sy - vz cy, y' = vx cy + vz sy, z' = vy, angular negated for the
  determinant -1). Without a headset no runtime delivers poses, so the
  mapping cannot be checked, and a wrong sign would throw backwards. Phase 1
  uses the same tracked positions the game draws.
- **Not verified / follow-ups.**
  - The headset: real throws (noise, `handpredict`, 90/120 Hz), the 1:2:1
    weighting and the outlier filter, the 12 m/s and 50 rad/s jump limits,
    the 85 J cap, the 1-2 m/s place/throw blend, and whether `throwboost`
    should move to 1.1.
  - The fake hand releases one tick after its last ramp pose; real grip
    opening timing on a controller is not modelled.
- **Unit tests** 113/113 (17 new checks: `TestPeakThrow`: the plan's ramp at
  144 and 90 Hz, the 60 ms average for comparison, a 10 rad/s wrist, ticks
  denser than poses, a 50 cm tracking jump, running at 6 m/s, an 80-degree
  wrist jump, a 30 rad/s wrist, short history, the energy caps). After the
  review fixes 180/180 with item 5's (all at `peakdt` 6 ms; new: outlier
  step and out-and-back, a glitch inside a throw, the placement ramp, the
  release-time averages with a wrist jump and a glitch, the blend curve,
  the 85 J caps).
- **Review fixes (26/10).** Three medium and two low findings of the item-4
  review, all fixed. Tests on the final dll in one session (novr, level 1
  CP04, 120 ticks/s), A/B against the item-4 dll (`p4t/tested_d3d9.dll`) in
  the same session; scripts `scratchpad/p4fix/final.sh` (`walkstop.py`,
  `throw4b.py`), `p4fix/chain2.sh`; output `p4fix/chain2_out.txt` (A/B:
  `p4fix/chain_out_run1.txt`).
  - **Walker's own motion was read as a throw.** The history samples are
    world positions; `PeakThrow` compared every sample in the 110 ms window
    with Walker's velocity *now* (`PawnVelocity`, Actor.Velocity). Stopping
    after a walk (Walker brakes from 3.92 m/s to under 1 m/s in ~60 ms) and
    opening the hand at once, the "peak" was the walking speed of before; on
    starting to walk, a standing sample was the farthest from the reference
    and the object stopped dead in the world. Now every `g_hist` sample also
    stores the centre of the hand frame of the same tick (`fc`, from
    `SetHandFrame`, the same `UpdateArms` call as the hand pose; `fok` = this
    tick's centre; `FrameJump` moves it with the samples), `PeakThrowVel`
    searches the peak on `pos - fc` (hand relative to Walker, reference 0)
    and `Release` adds back Walker's velocity at release (`FrameVelocity`,
    the hand-frame centre over 30 ms, the same as item 2's weight code)
    after `throwboost`: the boost multiplies only the hand's own motion
    (letting go while running no longer adds 30% of the run). The caps use
    the same `FrameVelocity`. Without a centre in every sample, or with
    `griptune pawnframe 0`, the old world path is used (log: `in the world,
    Actor.Velocity`). Test `p4fix/walkstop.py`: `pad fake` walk forward at
    3.92 m/s, hand still relative to Walker, bottle, `throwboost 1.3`; grip
    opened 30/60/100 ms after the walk input ends, or 50/100 ms after it
    starts (GetTickCount64 timers, +-16 ms; the log gives the real braking
    time). The ideal is Walker's speed at release ("frame"):

    | Release | item-4 dll (`peak`) | final dll `peak` | `classic` | Walker at release: frame (Actor.Velocity) |
    |---|---|---|---|---|
    | 30 ms after the stop | 6.3 m/s ("hand 4.34 m/s 50 ms before release") | 1.95, 2.61 | 4.1 | 1.95, 2.61 (1.26, 1.45) |
    | 60 ms after | 5.2 | 1.04, 1.04 | 1.5 | 1.04 (0.82) |
    | 100 ms after | 4.9 | 0.38, 0.43 | 0.6 | 0.38, 0.43 (0.31, 0.34) |
    | 50 ms after starting | 0.1 (stopped dead) | 0.91, 0.92 | 0.9 | 0.91, 0.92 (1.16, 1.17) |
    | 100 ms after starting | 0.1 | 1.91, 1.91 | 2.2 | 1.91 (2.16) |

    With the fix the hand samples read 0.00 m/s throughout
    (`GRABTHROW: samples (ms ago: m/s, hand minus frame) 0:0.00 8:0.00 ...`)
    and the object leaves at Walker's speed (the 30 ms rows differ by the
    timer jitter: braking began 25 and 17 ms before release). Not tested:
    vehicles (none drivable at level 1 CP04); the frame centre moves with
    the camera, so a moving base is in `FrameVelocity` by construction.
  - **Every grip release took the peak.** Putting an object down with a
    slowing hand launched it at the peak of the last 110 ms, and a single
    wrong sample under the 12 m/s jump limit became the peak (half of it
    kept by the 1:2:1 mean). Three changes:
    - *Isolated outliers.* Each sample whose velocity is farther from the
      component-wise median of its 3 (k-1, k, k+1) than max(`peakout`
      0.5 m/s, 20% of the median's speed) takes the median (a Hampel
      filter); then the old peak search and 1:2:1 mean run on the cleaned
      samples. A clean peak is untouched (plan ramp 3.85 m/s at 144 Hz,
      3.75 at 90 Hz, as before). A plain median on every sample (the
      review's suggestion) was tried first and lost up to 0.13 m/s on a
      sharp peak (3.80 and 3.70, the latter at the plan's lower bound), so
      it was dropped. A step (controller snapping back into view) and a
      one-frame out-and-back glitch (two opposite velocities in a row) are
      both removed (unit-tested: 5 cm at 90 Hz = 4.5 m/s raw -> 0.00).
    - *Place or throw.* The peak counts in full only from `peakfull`
      (2 m/s, relative to Walker) up; under `peakmin` (1 m/s) the object
      leaves with the release-time average and the wrist rotation over the
      same window; between the two a smoothstep blend
      (`vr::pure::ThrowPeakBlend`), so there is no step between a gentle toss
      and a slightly harder one. The lever arm blends the same way (peak
      w with the peak axes, release w with the axes now).
    - *Release-time average from the checked samples.* The window is
      `classic`'s (newest sample back to the first at least 60 ms older, 8
      samples at most), but the average is the dt-weighted mean of the
      per-sample velocities and wrist rates already checked (jumps over
      12 m/s / 50 rad/s out, isolated outliers replaced). On a clean history
      it equals the `classic` average (the one-tick velocities add up to the
      displacement). Found in this review's own test run: the first version
      took the rotation between the window's end samples, and `regrab`'s
      80-degree one-tick wrist turn (167 rad/s, rejected per sample) read as
      24 rad/s over the 50 ms window, under the 50 rad/s limit: the
      released radio got 11.6 rad/s and a 8.2 m/s lever and left at
      5.2 m/s. Now `regrab_slow.py` gives `0+1 rejected`, lever 0.05 m/s,
      object 0.0 m/s, 0 hits, twice.
    - Measured (bottle, `throwboost 1.3`, `p4fix/throw4b.py`):

    | Test | item-4 dll | final dll `peak` | `classic` |
    |---|---|---|---|
    | Place: `synthleftramp 1 0 -1 60 50 10 100 release` (0.6 -> 0.1 m/s) | 0.73, 0.73 m/s (peak 0.58) | 0.27, 0.27, 0.32 (peak 0.56, weight 0.00, release 0.21-0.24) | 0.32, 0.27 |
    | Glitch: hand still, 5 cm step, release ~40 ms later | 3.73, 3.82 (raw sample 5.74-5.87) | 0.00, 0.00 (raw 5.84 / 5.72, "1 replaced", peak and release 0.00) | 1.11 |

    A noisy real headset hand was not measured (no headset here).
  - **Energy cap and the barrel.** See "Energy cap" above: default 85 J;
    fuel barrel 2.26 m/s, PhysX 2.25 without gravity, weight light and off
    (item-4 dll: 3.00); `classic` still 6.0 (no energy cap there, as before).
  - **Unit tests at the shipped `peakdt`.** All `TestPeakThrow` cases now
    run with 6 ms. "Ticks denser than poses" (225 ticks/s, hand at 144 Hz)
    is run both ways: with each sample stamped with its pose time (what the
    game does) 3 m/s reads 3.00 +- 0.02 (was accepted 2.5-3.8 with tick
    times at 8 ms); with tick times (only `headsample render`, or the
    `synthsmooth off` test hand) the double-step samples are isolated
    outliers and the estimate falls to the single-step 2.34 m/s (-22%; the
    old 1:2:1 gave 3.5, +17%). Documented in the test.
  - **Pose stamp with `headsample render`.** See "Sample times" above.
  - **Item criteria on the final dll** (`throwboost 1`, as in item 4): ramp
    3.82-3.83 m/s in 6 of 6 (`classic` 2.78, 2.78, 2.94); wrist 10 rad/s
    with the hand still: spin 10.0 x3 (now through the release-time wrist
    average, peak weight 0; `classic` 9.9, 10.0), centre of mass 0.91 m/s;
    flip spin 10.0, 10.0 (`classic` 4.0, 4.0); constant 3 m/s: 3.00 (bottle,
    radio); barrel 2.26.
  - **Hit pipeline** (`p4t/hitall.sh fzh`, final dll): `hitthrow` held hit
    on the head 1.5 m/s, release 5.6 m/s (the `synthsmooth off` step
    artifact: raw samples 8.73 m/s, 11 replaced, weight limit), bounced off,
    no thrown hit (item 4: 5.8, the same); `heldswing` Spine1 2.4 and 2.7
    m/s, none pressed or slow; `hitdrop2` dropped, Spine3 1.8 m/s; stock
    `regrab` merged the release and re-grab batches again (the hand jump
    swings the held radio into the arm, Arm_Right 2.8 m/s, as in items 3
    and 4); `regrab_slow` twice: 0 hits, release 0.0 m/s. `hitthrow` from
    3.5 m: stock 5.8 m/s, thrown hit Spine3 5.9 m/s; `synthsmooth on` 5.8,
    Spine1 6.0; `classic` 5.9, Spine3 6.0.
  - **Cost.** One more 13-byte copy per hand per tick (frame centre and
    flag); the rest runs only at release. `PROPS: tick` 0.209-0.215 ms on
    average during the throw series, 0.180-0.195 in the hit phase (item 4:
    0.205-0.218 and 0.182-0.197), at most ~20 ms per 30 s window as before
    (spawning).
  - **Logs.** `GRAB: throw peak: hand 3.83 m/s 67 ms before release
    (fastest raw sample 3.89 m/s, 0 replaced by their median; 3+5 samples of
    16, 0+0 rejected), at release 2.76 m/s, peak weight 1.00, relative to
    Walker, frame 0.00 m/s, lever 0.00 m/s, object 3.83 m/s before the
    caps, cap 10.16 m/s (energy 10.16 m/s at 1.6 kg), ...`.
  - **Grab suite** (`suite.sh fzs`, final dll): grasp power grip, centre f
    -2.6 p 5.5 a 6.6 cm, turned 46 degrees; drive settle 0.067-0.100 s,
    sweep error 2.3 cm (the drive is untouched; item 5's two runs gave 1.6
    and 2.2); static grab; glove 1.8 m in 0.41 s, caught; bottle knocked
    55.4 cm, barrel not knocked; drop broke at 6.1 m/s; smash broke in the
    hand; shot moved the radio 163.4 cm; door 60.0 degrees, stop sign 5.6
    degrees: all as in items 3-5.

**Boneworks plan, item 5: the drawn free hand follows physics (26/10).**
`solidhands off|held|full` (Basic menu "Solid hands", default `held` since
the review fixes: the orchestrator's decision of 26/10 05:12, because with
`objectweight light` on by default and `off` the drawn hand closed on empty
air 12-20 cm from a lagging heavy object; `configversion 10` turns an ini's
`solidhands off` into `held`, any other choice stays). In Boneworks the drawn body follows physics, not
the controller. Here physics stays driven by the controller (`g_handWorld`,
`Drive`, the hand spheres); only where the free hand is drawn changes.
- **Where.** `props::RenderHandPose` is called in `UpdateArms` just before
  `WriteHand(1)`, only with the left hand off the gun (`gripBlend` 0) and the
  left controller present; otherwise `RenderHandNone` resets it. The gun hand
  (`HandTarget(0)`, `WriteHand(0)`) is computed and written earlier in the
  same frame and reads nothing the new code writes. `g_handWorld[1]` stays the
  physics target. No offset either, at once, while the left hand is on the
  gun, two-handing or reloading (`GunHandHaptics(1)`), whatever the grip
  blend. With `off` the pose passed to `WriteHand(1)` is a copy of
  the target (no arithmetic), so nothing changes.
- **Sources** (`SolidSource`):
  - (a) held object (`held`, `full`): where the grip puts the hand on the
    REAL object pose: hand axes `haI = ra^T oa`, origin `op - haI^T rp`, with
    the grip relation (`rp`, `ra`) from `HeldTarget` WITHOUT sag and droop
    (they are the hand giving way, so they show in the hand: the sagging
    radio pulls the drawn hand down). The offset is split at the palm: palm
    displacement plus a rotation about the palm, each with its own dead zone
    and cap, so a bottle wedged 100 degrees away does not throw the wrist
    20 cm. Fixed at the grab (`Held.solid`, like the weight), free hand
    only, and only for objects grabbed with `objectweight` on (review fix:
    with `off` the old drive bounces a pressed radio 6-12 cm per tick, and
    the drawn hand on it shook as a ~2 cm, ~20 Hz sawtooth; without weight
    the object follows the hand 1:1 anyway). `SolidHeld` (the grab flag AND
    the live mode) gates source (a), item 2's stuck-instead-of-re-grip and
    `HeldShape`'s sag-free grip together, so switching `solidhands off`
    from the menu during a hold returns all three to `off` at once.
  - (b) constrained object (door, stop sign; always weight 0, so not
    affected by the gate above): the palm on the grabbed point
    (`pt - palm` of `ConstrainedDrive`), no rotation.
  - (c) empty hand, `full` only: the palm on the hand sphere (`ProxyTick`),
    which objects with a PhysX body stop (verified on the rifle case's
    lid; the sandbag wall and the ground near CP04 did not stop it, see
    below).
- **Shaping** (pure, unit-tested: `vr::pure::SolidShape`,
  `SolidFilterAlpha`): soft dead zone `m x smoothstep(m / dead)` below
  2.5 cm / 8 degrees (Rietzler's detection threshold; 1 mm of PhysX jitter
  becomes 0.005 mm), full above it (same slope, no step), cap 25 cm / 45
  degrees, then a first-order filter in the target hand's axes, 40 ms when
  the offset grows and 100 ms when it shrinks. Kept in the hand's axes, so a
  snap turn or a cover slide carries it with no world-space state to shift
  (the plan's cover-slide note). Back under 0.5 mm and 0.05 degrees with no
  source, the state is reset to exactly zero (written pose = target again).
- **Arm reach** (`armstretch`): `ArmReach` (shoulder bone, animation arm
  length x armstretch x 0.97, the same as the grip slide) bounds the drawn
  wrist: no farther from the shoulder than `lim` = the reach or the
  target's distance, whichever is larger. Since the review fixes
  (`vr::pure::ReachClamp`, unit-tested) a wrist beyond `lim` is projected
  onto the sphere of radius `lim` around the shoulder along its own
  direction: only the part that lengthens the arm goes, the sideways part
  (a radio's sag with the arm straight out) stays, and the result is
  continuous (projection onto a ball). The first version shortened the
  offset towards the target instead: with the target on the reach boundary
  (arm straight, common when reaching) that deleted the whole offset (`t` =
  0 for any outward or sideways offset; 40+ frames of "wrist 0.00 cm" in
  `p5f_press_held`), and near the boundary controller jitter switched the
  drawn sag on and off. The length removed (`RenderHand.dCut`) is added to
  `HeldShape`'s finger reach, so the fingers still close on the real
  object. Logged as `cut X cm to the arm's reach`; every `SOLID:` line now
  also gives the drawn wrist offset in the target's axes.
- **Mandatory parts.**
  - Feedback guard: `MeasurePalm`, `HeldShape` and `PoseFingers` measure
    against the pose actually written to the IK (`RefHand`: the last target
    given to `Tick` composed with that frame's written offset, kept in the
    target's axes), not the target. Without it the offset enters
    `g_palmLocal` and the grip frame, which move the palm the offset is
    computed from (closed loop). `griptune shguard 0` restores the old
    reference for comparison only. `HeldShape` closes the fingers on the
    sag-free grip at the drawn hand (= the real object when the offset is
    whole); item 2's `fingerreach` still pulls them to the real object.
    `MeasureHandInWeapon(1)` is skipped while the drawn left hand is more
    than 1 mm off its controller (it would feed `HandTarget(1)`). The first
    version skipped it from any non-zero offset: with `full` and an empty
    hand the sphere's shaped offset is a few hundredths of a mm almost every
    frame, so the left hand's relation could stay unmeasured after a
    cinematic. `ReleaseArms` and `UpdateArms`' no-arms return now also clear
    the drawn hand and `g_supportHandSepCm` (locked scenes, turrets, death:
    HUD opaque again, measurement free).
  - Sphere teleport rule (`full`, left sphere): from 3 cm of separation a
    world-only trace (`WorldBlocked`, at most every 0.1 s, meshes from
    `shwall` 15 units of bounding radius count as walls, 40 for the frame
    jumps) checks the line sphere -> palm. Blocked: no `SetRBPosition` back
    onto the palm until the palm is beyond `shtele` (45 cm; Valve resets at
    0.6 m), and the push towards the palm is capped at 0.3 m/s (sideways
    free). Before the cap the sphere, driven at up to 10 m/s into the rifle
    case, bounced 7 cm per tick (raw offset 10-17.5 cm); after it, 13.12 cm
    with a standard deviation of 0.00 over 1.4 s.
  - `Nearest()` from the sphere (`full`, left hand, sphere within `shcap`
    of the palm): no grabbing through a tabletop the drawn hand rests on.
  - `full` also makes the bodiless level meshes around the left sphere solid
    (`SupportsAround`, as around a held object): many level 1 surfaces have
    no PhysX body, and the sphere passes through them. Whether this makes the
    sandbag wall near CP04 stop the sphere is not verified: the palm got only
    ~7 units under the top of its bounding box before the arm's reach, and
    the sphere stayed on the palm (the round bags may be lower than the box).
- **Weight interplay.** With `held`, item 2's re-grip "where it is" is
  replaced by its "stuck" mode while the palm touches the object (no push
  against the obstacle with the hand still, the grip unchanged). The re-grip
  moved the grip into the empty space under the table: the radio pressed
  12 cm into the rifle case with `objectweight light` kept the drawn hand on
  it for 0.45 s, then the drawn hand sank 12 cm into the case and the radio
  stayed 12 cm above the palm for the rest of the hold (`p5_pr4`).
- **Wrist HUD:** the overlay sits on the real controller, so it fades from
  5 cm to 10 cm of separation (`g_supportHandSepCm`, `SetOverlayAlpha`).
  On the OpenXR backend `OverlayShim::SetOverlayAlpha` only stored the
  value, used as a visibility cutoff (> 0.01): the HUD stayed opaque and
  vanished at ~9.95 cm. Now the alpha is multiplied into the alpha channel
  of CPU-pixel overlays at upload (the wrist HUD uses `SetOverlayRaw`; the
  quad blends unpremultiplied source alpha), and a change of alpha re-uploads
  them. `vr_wrist.cpp` resets its last-sent alpha whenever it creates the
  overlay (a new session could reuse the old handle value). Not verifiable
  without a headset.
- **Measured** (novr, level 1, 120 Hz cap; scripts in the scratchpad):
  - Radio pressed into a table (`p5_press.py`: the level radio on its rifle
    case, lifted 8 cm, pressed 20 cm down, `objectweight light`, `held`):
    logged grip error 14.41-14.42 cm, drawn palm 14.41 cm (shaped = raw above
    the dead zone, filter settled), bones (`SOLID: palm bones`, knuckles from
    the SpaceBases against a knuckle reference taken before the press)
    14.22-14.58 cm from the target, 0.1-2.1 mm from the written pose. No
    re-grip, no jitter (raw constant to 0.01 cm). Item 2's `GRABLOG err`
    reads 2.2 cm more: it includes the 2 cm sag of the target.
  - Same with `objectweight off` (`p5_pr3`, first version): the old drive
    pushes the radio into the case at 7-11 m/s and it bounces 6-12 cm per
    tick (raw 10-21 cm; the object's shake is the old `off` behaviour); the
    drawn hand on it moved 15.4-17.7 cm in a ~2 cm sawtooth at ~20 Hz, which
    was new with this item (review, high). Fixed: source (a) only for objects
    grabbed with weight on (above; the re-run is in the review-fix results
    below).
  - Door (`hingetest.py door ... -40 0 0 1000`, `held`): the grabbed point
    lags the palm 4.7-11 cm while the door swings (59.8 degrees), 6.56 cm at
    the end of travel (`GRABLOG ... constrained err 6.6 cm`); in that test
    the palm target is out of the arm's reach, so the drawn wrist offset was
    cut to 0.
  - Empty hand (`full`, `p5_free.py`): palm target 16 cm below the top of the
    rifle case, sphere on the case, drawn palm 13.12 cm above the target,
    steady (above). Grip closed there: it took the radio lying on the case
    next to the drawn hand, nothing under it.
  - `g_palmLocal` drift (`p5_drift.py`, 60 s, spawned radio with
    `objectweight full`, the hand moving 10 cm back and forth along 8
    directions): `held` (drawn offset 2.6 cm on average, 4.3 at most):
    first -> last 0.45 mm, 95% of the samples within 0.54 mm of the median,
    one transient over 1 cm. `shguard 0`: 42 mm first -> last (the loop the
    guard closes). `off`: stable within 0.3 mm for 45 s, then transients of
    2-15 cm (the bones not reaching the target, below). Final series
    (`p5_final_out/drift.txt`): `held` first -> last 0.30 mm, 95% within
    0.32 mm, none over 1 cm, whole range 0.5-0.6 mm (drawn offset 2.4 cm
    on average, 4.2 at most); `off` 4.1 mm, two transients over 1 cm.
  - Final series, radio press (`p5_final_out/press_held.txt`, view pitched
    down): grip error 14.41 cm, drawn 14.41 cm; screenshots
    `p5f_shot_held/pressed_b.png` (fingers closed on the radio's edge on the
    case) and `p5f_shot_off/pressed_b.png` (`off`: the hand 14 cm lower,
    inside the case; item 2 re-grips after 0.45 s). The camera looks down
    along the press, so the 14 cm show mostly as depth; the numbers are the
    evidence.
  - Gun, per frame (`p5_gun3.py`, the decisive test): a fixed pose, the
    left hand holding a radio (`objectweight full`; with `full` its drawn
    bones move 1.0 unit = 2 cm, `GUNLOG lh` z 7.42 -> 6.42), `solidhands`
    switched off/full every second for 8 s, `GUNLOG` every frame with the
    barrel axis to 6 decimals (added for this). Aim (12, 5): 600 off frames
    and 476 full frames, barrel axis mean (0.863985, -0.495949, 0.086971) in
    both, standard deviation 5e-7..1e-6 in both (the gun mesh's own
    frame-to-frame noise); the gun hand's target `g_handWorld[0]` has ONE
    value in all 1076 frames; the marker takes the same values in both
    modes. Aim (0, 0) and aim (0, 0) in ADS: the same (axis means equal to
    1e-6). With the left hand on the gun (`synthleft 55 18 -30`, "left hand
    on the weapon") and `full`: no offset at all (no `SOLID:` line).
  - Gun, per shot (`p5_gun2.py`, two series): six aims, each with `off`,
    `full`, `off`, one shot plus one in ADS. Off/full and off/off differ by
    the same amounts: fire origin 0.3 / 0.2 units, muzzle start 0.1 / 0.1,
    barrel axis 0.001 / 0.000 (printed with 3 decimals), shot end 7.6 /
    3.9 (first series 3.5 / 6.7), aim marker 6.5 / 0.3 (first series 6.9 /
    13.2); ADS toggles 6 / 6 / 6. The marker spread is the far end of the
    trace (no hit, 20000+ units away): 3e-4 rad of the per-shot axis noise
    is 6.5 units there. Exactly 0 between two shots is not reachable even
    off/off; the per-frame test above is the exact one.
  - The orchestrator's check (26/10 04:52) read "the drawn left hand sits
    2.30 cm off in every ADS sample while the left hand is on the gun": in
    `p5_gun2` the left hand was holding the radio, not on the gun (the 2.30
    cm is the radio's sag), and `RenderHandPose` already ran only with
    `gripBlend` 0. It now also returns no offset, at once, while the left
    hand is on the gun, two-handing or reloading (`GunHandHaptics(1)`:
    `g_leftOnGun`, `g_twoHandEngaged`, `LeftHandOnReload`).
- **Hit pipeline** (`p4t/hitall.sh p5f`, summoned dummy, final dll,
  `solidhands` off as first shipped; `p5_final_out/`; with `held` and `full` after the review fixes: below): `hitthrow` held hit on the
  head 1.6 m/s, release 5.6 m/s (hand 4.70), bounced off (78.6 deg), no
  thrown hit; `heldswing` 3 m/s: Spine1 2.5 and 2.8 m/s, none pressed or
  slow; `hitdrop2`: dropped, Spine3 1.8 m/s; `regrab` (stock, the batches
  did not merge this time) and `p4t/regrab_slow.py`: wrist 0.0 rad/s on the
  first 6 `Drive` ticks, 0 hits. All as in item 4 (1.5 / 5.8 / 78 deg; 2.8
  and 2.5; 1.7; 0.0-0.1).
- **Grab suite** (`suite.sh p5fs`, first final dll, `solidhands` off; `held` and `full` after the review fixes: below): grasp power
  grip, centre f -2.6 p 5.5 a 6.6 cm, turned 46 degrees; drive settles in
  0.066-0.108 s, tail error at most 0.4 cm, 1.6 cm while sweeping at 1.5 m/s;
  static grab, glove (caught in flight), bottle knocked 54.5 cm, barrel not
  knocked, drop broke at 5.4 m/s, smash broke in the hand, shot moved the
  radio 124.5 cm, door 60.8 degrees, stop sign 5.5 degrees: as in item 4
  (`p4fin`: same grasp, 0.066-0.108 s, 52.1 cm, 164.0 cm, 60.0 and 5.7
  degrees; the shot distance and the sign vary run to run). Again on the
  exact final dll (after the gun-hand guard, `suite.sh p5z`): same grasp,
  drive 0.025-0.108 s, sweep 1.5 cm, bottle 56.1 cm, drop broke at 5.4 m/s,
  smash broke, shot 166.4 cm, door 60.2, sign 5.3 degrees.
- **Cost.** `RenderHandPose` runs in `UpdateArms` (one component pose, one
  `HeldTarget`, a slerp; with `solidhands` on one `ArmReach` call), outside
  the `PROPS: tick` timer. `PROPS: tick` during the `held`/`full` tests:
  0.155-0.186 ms on average, 0.31-0.99 ms at most (8.3-8.6 ms only in the
  windows with the test-only `spawnprop`). The sphere trace runs at most
  every 0.1 s and only with the sphere 3-45 cm off the palm.
- **Unit tests** 162/162 (14 new checks, `TestSolidShape`).
- **Pre-existing, found here:**
  `MeasurePalm` accepts knuckles up to 15 cm from the reference, so when the
  IK cannot reach the target (hand target out of the arm's reach: the
  rifle-case tests, `hingetest`), the shortfall enters `g_palmLocal` (seen
  at 2-15 cm). With `solidhands` on this also bent the drawn hand (it turns
  about that palm: the review-fix reach test drew the wrist 6.7 cm off for a
  2.7 cm offset with the arm straight out), so since the review fixes
  `UpdateArms` tells props when the written left wrist is beyond the full
  `ArmReach` (`SetHandBeyondReach`) and `MeasurePalm` keeps the last palm
  then (the palm is rigid with the hand). Only with `solidhands` on (the
  only case where `ArmReach` is read there); `off` is unchanged. Not
  covered: a target within reach by distance that the IK still cannot reach
  (joint limits: the rifle-case press puts the palm target ~1 m below the
  eyes, and the palm local there still reads (-12.6, -5.2, -1.4) cm instead
  of (8.5, 0.3, 0.1)); it only matters for a drawn rotation, and there was
  none in that test (drawn palm = raw, steady). And `ProxyCreate` re-spawns the sphere every tick when the
  hand target is absurd (a diverged test script sent the hand 4e9 cm away:
  3745 spawns until reset).
- **Review fixes (26/10 morning), measured on the final dll** (novr, level 1
  CP04, 120 Hz cap unless noted; scripts `p5x_*.py`, outputs in the
  scratchpad's `p5x_final/`):
  - Radio pressed into the rifle case with `objectweight off` and `held`
    (`p5_press.py ... off held 20 2.5`): 520 frames with `solidlog` on and
    not one `SOLID:` offset line, i.e. the written pose is the target in
    every frame (drawn-wrist range 0); the object itself still bounces
    (GRABLOG error 0.1-60 cm, 301 reversals: the old `off` drive).
  - Same with `objectweight light`: drawn palm = raw 12.01 cm over the last
    200 frames, drawn wrist range per axis 0.06 mm; item 2's stuck mode in
    348 frames, 0 re-grips. At the lifted pose before the press (target at
    the arm's reach) the sag is now drawn: drawn palm 2.04 cm, wrist 1.73 cm
    with 1.10 cm cut (first version: "wrist 0.00 cm" for 40+ frames).
  - Arm reach (`p5x_reach.py`, spawned radio, `objectweight full`, hand held
    still 30 to 100 cm forward): raw 2.72-2.74 cm and 16.5 degrees at every
    distance; drawn palm 2.73 cm everywhere; drawn wrist 0.45 cm at 30-45 cm
    (no cut), 0.37-0.42 cm at 60-100 cm with 0.20-0.26 cm cut; palm local
    (8.44-8.48, 0.31, 0.09-0.10) cm throughout. Before the `MeasurePalm`
    gate (same build otherwise) the palm local drifted to (-8.6, -10.1, 1.0)
    cm from 70 cm on and the drawn wrist to 6.7 cm.
  - Door (`p5x_door.py`, `hingetest` plus `solidlog` and a 1.2 s hold): pull
    40 cm, door to its stop (59.7 degrees): grabbed point 8.6 cm from the
    palm target, drawn wrist 6.72-6.80 cm over the last 120 frames (5.1 cm
    of the outward part cut: target beyond the reach), per-axis range 1.4
    mm. Pull 25 cm: the door stopped at 8.2 degrees (blocked, the drive
    asking 3.2 m/s at 0.0), grabbed point 20.8 cm away, drawn wrist
    20.65-20.73 cm (1.5 cm cut), per-axis range 1.3 mm.
  - Dynamic (`p5x_dyn.py`: spawned radio, side sweeps of 50 cm at 2, 3 and
    4 m/s and back, a 2 m/s lift, a throw ramp to 4 m/s; per-frame
    `SOLID:`): drawn wrist peaks 2.0 / 5.2 / 10.0 cm (`light`) and 5.4 /
    13.1 / 20.8 cm (`full`), 1-2 reversals of its motion per phase (the lag
    building and catching up) against 3-7 of the raw offset; largest step
    1.4 cm per frame (`light`, 4 m/s: the hand itself moves 3.3 cm per
    frame). At 90 Hz (`fpscap 90`): 1-3 reversals, peaks 2.3 / 5.5 / 10.5
    cm, largest step 2.2 cm. Held still: 0 reversals, steps under 0.06 mm.
    Throw: 3.73 m/s for 3.83 wanted in `held` and 3.72 in `off`
    (first battery), unchanged.
  - Gun, per frame (`p5x_gun.py`, 30 s per aim, `solidhands` toggled every
    2 s, ~3700 `GUNLOG` frames per aim): left hand holding a radio with
    `held`, aims (12, 5) and (0, 0), hip and ADS; empty left hand with
    `full`. In every case the gun hand's target has ONE value over all
    frames of both modes, the barrel-axis means differ by 2e-8 to 4e-7
    (frame noise 5e-7 to 7e-6), aim marker means by 0.00-0.04 units, fire
    origin 0.00. Per shot (`p5_gun2.py`, `full`): off vs off and off vs full
    differ by the same noise (marker 7.1 / 7.6 units at 20000+ units,
    shot end 9.1 / 6.4, fire origin 2.2 / 2.2, barrel axis 0.001 / 0.001);
    ADS toggles 6 / 6 / 6.
  - Hit pipeline with the new default `held` (`p4t/hitall.sh p5xh`):
    thrown test hits the head (held, 1.6 m/s); held swing Spine1 2.3 and 2.7
    m/s, none pressed or slow; dropped radio Spine3 2.0 m/s; stock regrab
    1 hit at 2.8 m/s with the release and regrab in one poll (as in item
    6), `regrab_slow` wrist 0.0 rad/s, 0 hits.
  - Grab suite with `held` (`suite.sh p5xhs`): same grasp, drive settle
    0.025-0.108 s, sweep 1.6 cm, static grab, glove catch, bottle knocked
    43.8 cm, barrel not knocked, drop broke at 5.7 m/s, smash broke in the
    hand, shot moved the radio 123.9 cm, door 59.6, sign 5.4 degrees.
  - With `full` (`p4t/p5x_hitall.sh p5xF "solidhands full"`, `p5x_suite.sh
    p5xFs "solidhands full"`): hit pipeline the same (head 1.5 m/s; held
    swing 3.4 and 3.5 m/s, none pressed or slow; dropped Spine3 2.0 m/s;
    `regrab_slow` 0 hits). The swing speeds are run-to-run noise: in one
    session (`p5x_hs_ab.sh`) `held` gave 2.4 / 2.7 and 2.8 / 2.4 m/s, `full`
    2.7 / 2.8 twice, `off` 3.4 / 2.7 (and in that `off` run one more hit
    at 2.6 m/s 1.4 s after the second swing, right after a 33.9 ms hitch,
    before the slow push started: the old path, not this item). Suite: same
    grasp, drive 0.058-0.100 s, sweep 1.6 cm, glove catch, bottle knocked
    53.3 cm, barrel not knocked, drop broke at 5.9 m/s, smash broke in the
    hand, shot moved the radio 164.7 cm, door 61.0, sign 5.4 degrees. The
    static grab did NOT take the bottle: the one the script picked lay on
    the ground beyond the guard rail (palm target there, `GRABLOG nearest
    -`), and the script's straight path to it went through the rail (made solid by `SupportsAround` around the held radio
    of the drive test). With `full` the sphere, and the drawn hand on it,
    stopped on the rail (screenshot `p5xFs_rq/reach.png`: the drawn hand on
    the rail, `held`'s `p5xhs_rq/reach.png`: under it), and the grab starts
    from the sphere, so nothing was in reach: `full` working as designed
    (no grabbing through a surface), but it means an object behind a
    solid surface has to be reached around it. Run again right after
    (Walker elsewhere, so the script picked a bottle standing on the
    crate), `full` took it.
  - Unit tests 190/190 (new: `ReachClamp`, the `configversion 10` change).
- **Not verified:** the headset (comfort of a drawn hand away from the real
  one, the 2.5 cm / 8 degree dead zone, the 25 cm / 45 degree caps, the
  wrist HUD fade on both backends); sliding a held object along a surface,
  walking and cover slides while holding with `held`; (c) on a table at a comfortable height (in novr Walker's
  arm reaches only ~95 cm below the eyes; level 1 tables and sandbags have
  no PhysX body until `SupportsAround`); the plan's swept `SingleLineCheck`
  stays deferred.

**Boneworks plan, item 6: analog grip strength (26/10).**
`analoggrip on|off` (Basic menu "Analog grip", default off). With object
weight on (`light` or `full`), the free hand's pull and twist on a held
object follow how hard the grip is squeezed: a loose grip, just past the
grab point, makes objects lag, hang and turn back slowly; a full squeeze
gives item 2's behaviour exactly. With `objectweight off`, and for hinged
objects (`ConstrainedDrive`), nothing changes. The grab and release
threshold (0.6) is unchanged, and so are the gun hand and aiming
(`HandTarget(0)`, `GunBarrel`, `MuzzleStart`, ADS, `CalcWeaponFire`): the
change lives in `Drive`'s weight path and `WeightOf` only.
- **Input.** `vr_input.cpp GripAxis`: the analog grip axis
  (max of `rAxis[2].x/.y`), without the 1.0 that `GripValue` returns as soon
  as the grip button bit is set (under OpenXR that bit is set from 0.5,
  `vr_openxr.cpp`). A controller whose axis stays at 0 while the button is
  pressed (binary grip) reads 1.0, a firm grip, so it sees no change.
  `ReadGestureSample` fills `GestureSample.leftGripV/rightGripV`, and
  `ApplyGestures` stores them in `vr::control::g_gripAnalog[2]` (0 right,
  1 left, like `g_gripRaw`). `props.cpp GripValueNow(h)` uses, in order:
  the `synthgrip` test value, the controller value while `g_gripRaw[h]` is
  set, otherwise 1.0 (`grabsim`, `grabtest`: firm).
  - The decisions are pure functions in `pure_utils.h`, covered by
    `tests/unit_tests.cpp` (`TestAnalogGrip`): `GripAxisOf` (the axis; NaN or
    negative components count as 0, a binary grip reads 1.0),
    `GripHeldValue` (1.0 without a pressed grip, or with the axis at rest or
    NaN), `GripFactorOf` (the curve) and `GripFilterStep` / `GripPeakOf`
    (the filter below). Under OpenXR the button bit is set from 0.5, so an
    axis of 0.5-0.6 is a held object at the loosest grip (f = 0.4, tested).
- **Curve** (`GripFactor` = `GripFactorOf`): fraction f = 0.4 at grip 0.6,
  rising in a straight line to 1.0 at grip **0.9** (`analoggripfirm`; below
  0.6: 0.4). The plan's line ended at 1.0; the review (26/10) moved full
  force to 0.9, because a worn Touch, or an Index (whose OpenXR
  `squeeze/value` comes from the force sensor), may never reach 1.0, and
  `analoggrip on` would then mean permanently reduced control, or a hard,
  tiring squeeze to get it. Looseness L = (1 - f) / (1 - 0.4), 0 = firm,
  1 = loosest.
- **What it scales** (`WeightOf`, every user of it follows: `Drive`,
  `WeightPose`, `HeldShape`, `Release`, `ThrowSpinLimit`):
  - Force: f x weightforce (x2 with `light`). Torque: f x weighttorque.
  - Angular gain towards the target pose: x (1 - 0.5 L) (`gripturn` 0.5 at
    the loosest), in the weight path's rotation command, only while the
    object moves freely. Blocked (a breakaway in progress, or stuck with the
    hand still) it turns as with a firm grip (review, 26/10): a crate bottle
    turned slowly into the canonical grip while the breakaway pulled it out
    jammed between its neighbours and was let go as wedged (smash in the
    hand at grip 0.6: broke 1 time in 6; with `gripturn 1` 3 of 3; with
    this rule 3 of 3; final A/B below).
  - Droop stiffness: krot / (1 + L) (`gripdroop` 2 at the loosest), so an
    object held off-centre hangs up to twice as far, but never past
    item 2's droop cap (10 degrees `light`, 20 `full`). A first version also
    doubled the cap: the 75 kg rifle case at `full` then hung 40 degrees,
    pointed into the ground, climbed against its target at 60-80 cm and
    slipped out of the hand (`g_case60_full`, first build): a refusal, so
    the cap stays. (In that session `off` and 1.0 also re-gripped the case
    1-3 times, and 1.0 once let it go as wedged: the earlier runs had moved
    Walker 20-30 m onto other ground. In the final series, after a
    relaunch, no heavy run re-gripped, wedged or slipped.)
  - Not scaled: sag, the turn floor (`weightturn`), the max speed.
- **Floor (user rule: nothing may become impossible to lift).**
  `aMax = max(f x force / kg, g + liftmin, min(gripfloor x g, firm))`, with
  `firm` = item 2's value `max(force / kg, g + liftmin)`. The item 2 lift
  floor (`liftmin`: net 3 m/s2 at `full`, 6 at `light`) is never scaled by the
  grip, so the heaviest props rise at a loose grip exactly as at a firm one,
  and a loose grip never drops below 1.3 g (`gripfloor`) unless the firm grip
  is itself lower. At f = 1 the formula is item 2's, unchanged. For the
  heaviest props the loose grip therefore changes only the turning (gain,
  and torque down to the turn floor).
  - Against the plan's "a_max > 1.3 g for 60 kg": `light` (the default)
    gives 21 m/s2 = 1.40 g at grip 0.6; `full` gives 18 m/s2 = 1.20 g, which
    is item 2's floor for a firm grip too (the plan's 1.3 g was 1200 N /
    60 kg, a firm-grip value with the plan's original force).
- **Filter** (`GripStep` -> `vr::pure::GripFilterStep`, once per `Drive`
  tick, before `HeldTarget` because the droop depends on it). The grab starts
  from f = 1.
  - An upper envelope of the wanted fraction: a firmer value is taken at
    once, a looser one only `griphold` (100 ms) after the last maximum, then
    with the time constant `gripease` (80 ms). Grip noise is tracked by its
    upper envelope.
  - The fraction in use follows that envelope: down with it (continuous),
    up with the time constant `griprise` (25 ms). Until the review (26/10) a
    firmer grip was taken in one tick: a step of force, gain and, for an
    object held off-centre below the droop cap, of the target rotation (up to
    half the cap). Measured below.
  - All three are exponential (`1 - exp(-dt / tau)`), so the response is the
    same at 90 and 250 Hz (before: `dt / tau`).
- **Throw budget** (`Release`: last command + aMax x 60 ms; `ThrowSpinLimit`:
  torque): computed with the firmest fraction of the last `gripthrow` ms
  (300; `WeightOfThrow`, `GripPeakOf`: six 50 ms slots, i.e. the last
  250-300 ms), never with a looser one than now. Until the review only the
  100 ms hold protected the throw: a hand that opens over 150-250 ms while
  throwing (normal) had already lost force at the release, and the radio's
  throw lost up to a quarter of its speed (measured below). A grip that
  stays loose for the whole throw still throws weakly (that is the
  feature).
- **Options and tuning.** `analoggrip` (Basic). Ini (Advanced menu) and
  `griptune`:

  | ini / `griptune` | Default | Meaning |
  |---|---|---|
  | `analoggripmin` / `gripmin` | 0.4 | Share of force and torque at the loosest grip |
  | `analoggriploose` / `griplo` | 0.6 | Grip value of the loosest hold |
  | `analoggripfirm` / `griphi` | 0.9 | Grip value of the firmest hold (1.0 until the review) |
  | `analoggripturn` / `gripturn` | 0.5 | Angular gain at the loosest grip |
  | `analoggripdroop` / `gripdroop` | 2 | Droop stiffness / this at the loosest grip (cap unchanged) |
  | `analoggriphold` / `griphold` | 100 ms | A looser grip takes effect this long after the last firmer value |
  | `analoggripease` / `gripease` | 80 ms | ...then with this time constant (ini key added in the review) |
  | `analoggriprise` / `griprise` | 25 ms | A firmer grip takes effect with this time constant (0 = in one tick, as before the review) |
  | `analoggripthrow` / `gripthrow` | 300 ms | Throws use the firmest grip of this window (0 = the grip of now, as before the review) |
  | `analoggripfloor` / `gripfloor` | 1.3 g | Loose-grip acceleration floor (or the firm value, if lower) |

  All curve values except the plan's 40%-at-0.6 line are [unsourced]. An ini
  value out of range is clamped and logged (`CONFIG: analoggripfirm 1.1 out
  of range (0.1-1), using 1.`); before the review it was ignored, and the
  Advanced menu, which steps `analoggripfirm 1` by +0.1, then showed 1.10
  while 1.0 stayed in use. A non-number is ignored.
- **Test command.** `synthgrip <0..1> [noise] [after ms] [ramp ms] | off`:
  a fake analog grip value for both hands, with optional uniform noise of
  +-noise on every read, optionally applied `after ms` later
  (`GRAB: synthgrip X now`), and with `ramp ms` reached in a straight line
  from the previous fake value (`... now (ramp start)`, `GRAB: synthgrip
  ramp done at X`): a hand that opens slowly. It neither grabs nor releases
  (that stays `grabsim`). Since the review the pending value is applied in
  every props tick (`SynthGripTick`), also with nothing held (before: only
  inside `GripStep`, so the line could come late or never), on the
  QueryPerformanceCounter clock (before: `GetTickCount64`, 16 ms steps); a
  new `synthgrip` stops a running ramp. A `GRAB: throw grip: f 0.63 now, 1.00
  best of the last 300 ms; hand budget 4.67 m/s (2.96 with the grip of now)
  over the object's 3.76 m/s, wanted +3.65 m/s` line is logged at each throw
  with `analoggrip on`, and `GRABLOG` lines end in `... throw f X`. Log: `GRAB: grip 0.60 (left hand, light): pull 160 N of 400
  (40%), twist 10.0 of 25.0 N m, turn gain x0.50, droop x2.00 (cap 10 deg);
  ... accel ... = ... g (net up ...)` once per settled change (0.2 s steady;
  a noisy grip writes nothing), and with `grablog` every `GRABLOG` line ends
  in `| grip v f F N tau N m`.
- **Measured (novr, level 1 CP04, 144 Hz, final dll, `p6/final` in the
  scratchpad; `gripweight.py`: still 2 s, 3 side steps 1.5 m/s x 0.6 s, 2
  lifts 1.5 m/s x 0.4 s, a 3 rad/s yaw; medians).** The plan's F_max line:
  at 0.6 / 0.8 / 1.0 the log gave 160 / 280 / 400 N with `light` and 80 /
  140 / 200 N with `full`, i.e. 40 / 70 / 100% (the plan's 480 / 840 /
  1200 N assumed 1200 N full force; item 2 settled on 200 N). With full
  force at 0.9 since the review: 160 / 320 / 400 N `light` and 80 / 160 /
  200 N `full` (40 / 80 / 100%; `p6x/final/smoke_*.txt`). The tables below
  are the first final series, with the curve ending at 1.0.

  | Object (light) | grip | side step: settle / lag at 1.5 m/s / max lag | lift: 90% of hand speed after / max lag | turn: alpha max / rot err max | still: err std |
  |---|---|---|---|---|---|
  | radio 5.1 kg | off | 0.050 s / 0.3 / 2.1 cm | 0.041 s / 3.4 cm | 188 rad/s2 / 2.1 deg | 0.20 mm |
  | radio | 1.0 | 0.066 s / 0.3 / 2.4 cm | 0.042 s / 2.8 cm | 189 / 2.1 | 0.00 mm |
  | radio | 0.8 | 0.083 s / 0.8 / 2.8 cm | 0.050 s / 4.0 cm | 152 / 3.1 | 0.22 mm |
  | radio | 0.6 | 0.100 s / 3.5 / 4.8 cm | 0.125 s / 8.4 cm | 102 / 4.4 | 0.20 mm |
  | bottle 1.7 kg | off / 1.0 / 0.6 | 0.042 / 0.042 / 0.084 s, lag 0.2 cm in all | 0.033 / 0.033 / 0.049 s | 151 / 156 / 141, 1.5 / 1.5 / 1.9 deg | 0.22 mm |

  | Heaviest props | grip off / 1.0 / 0.6 | net upward accel (lift) | lift max lag | side lag at 1.5 m/s | turn alpha max |
  |---|---|---|---|---|---|
  | barrel 59.8 kg (masskg 72), light | | 6.5 / 6.3 / 6.1 m/s2 | 19.6 / 19.6 / 19.8 cm | 8.4 / 8.4 / 8.9 cm | 6.4 / 8.0 / 8.0 rad/s2 |
  | barrel 415 kg (masskg 500), light | 0.6 only | 6.7 m/s2 | 19.6 cm | 8.8 cm | 8.0 |
  | barrel 59.8 kg, full | | 3.0 / 3.0 / 3.0 | 36.4 / 36.2 / 36.0 cm | 15.8 / 15.5 / 15.6 cm | 3.3 / 3.3 / 4.0 |
  | rifle case 75 kg (masskg 46, scale 1), full | | 3.0 / 2.9 / 3.1 | 36.2 / 36.6 / 36.0 cm | 14.8 / 14.7 / 14.5 cm | 4.0 / 4.0 / 3.3 |

  - The heaviest props lift at grip 0.6 as at a firm grip (log at 0.6:
    `accel 21.0 m/s2 = 1.40 g (net up 6.0)` light, `18.0 m/s2 = 1.20 g
    (net up 3.0)` full; the 415 kg barrel 1.40 g). No slip, no wedge, no
    re-grip in any heavy run of the final series. The level's heaviest
    grabbable prop is the 52 kg rifle case (item 2).
  - `gripfloor` at work: the radio at `full` and grip 0.6 gets 19.5 m/s2 =
    1.30 g (0.4 x 39 would be 15.6).
  - `analoggrip on` at grip 1.0 matches `off` within run-to-run noise in
    every row (same numbers: f = 1).
  - Bottle: still 1:1 at the loosest grip (lag 0.2 cm; settle 42-84 ms,
    the metric is tick-quantised: 42 ms in the first session); its small
    droop doubles (0.25 -> 0.5 degrees). The radio and the barrels are at
    the droop cap already, so their droop does not change.
  - The plan's "barrel alpha <= 8 rad/s2 at 3 rad/s": 8.0 (`light`, 0.6),
    4.0 (`full`, 0.6).
  - First session (before the droop-cap fix, same numbers otherwise): radio
    0.6 lag 3.6 cm, barrel and case at `light` net up 5.8-6.1 m/s2.
- **Tightening without a step (review, `p6x/gripdroop.py`).** Every object
  of the first series was at the droop cap or had almost no droop, so the
  instant tightening was never seen. Radio at `light` with the droop
  stiffness raised so that its firm droop is 4 degrees, below the 10 degree
  cap (`griptune krot 178`: 11.9 degrees at krot 60 with the cap off), hand
  still, 120 Hz; `griprise 25` (new) vs `0` (one tick, as before):

  | phase | target step max per tick | object angular accel max | rot error max | droop |
  |---|---|---|---|---|
  | loosen 1.0 -> 0.6 (both) | 0.4 deg | 51 rad/s2 | 0.7 deg | 4 -> 8 deg |
  | tighten 0.6 -> 1.0, rise 25 | 1.1 deg (first tick: 28% of 4) | 92 rad/s2 | 1.6 deg | 8 -> 4 |
  | tighten 0.6 -> 1.0, rise 0 | 4.0 deg (all in one tick) | 279 rad/s2 | 4.0 deg | 8 -> 4 |
  | noisy 0.8 +- 0.05, rise 25 | 0.2 deg | 22 rad/s2 | 0.3 deg | 4.0-5.3 (std 0.32) |
  | noisy 0.8 +- 0.05, rise 0 | 0.4 deg | 53 rad/s2 | 0.3 deg | 4.0-5.4 (std 0.23) |

  (`p6x/final/droop.out`, final dll; position error std 0.4-1.7 mm in every
  phase.) The upper envelope still makes a small sawtooth under noise (the
  droop moves within 1.3 degrees, about a third of the noise band); its
  rising edges are now filtered, and the target moves at most 0.2 degrees per
  tick. An exploratory run had a 16 ms frame at the command (first step 1.9
  degrees = 47% of 4): with a frame hitch the first step grows with the
  frame time.
- **No pops, no jitter** (`gripdyn.py`, grip changed while holding; first
  series):
  - Radio held still, grip 1.0 -> 0.6 -> 1.0: error std 0.23 / 0.0 mm, the
    largest object step 0.2 cm per tick (as held still at 1.0), target step
    0.0 degrees. Barrel (33 kg): 0.27 / 0.0 mm, 0.2 cm.
  - Radio moving at 1.5 m/s while the grip goes to 0.6 and back to 1.0:
    largest error 3.4 cm, largest object acceleration 112.7 m/s2, the same
    as the firm-grip move (112.7; 116.9 on the way back).
  - Noisy grip, 0.8 +- 0.1 on every tick: hold-still error std 0.203 mm,
    the same as 0.8 without noise (0.203); a side step's error std 4.3 mm
    vs 5.2 mm without noise (barrel: 38.6 vs 38.9 mm, lag-dominated). The
    filter holds f in 0.69-0.85 (upper envelope). `GRAB: grip` lines: 3 per
    run (none from the noisy phase).
- **Throws, first series** (`gripthrow.py`, radio, ramp to 4 m/s, release
  at 2.8 m/s, `throwboost 1`): all four cases left at 3.81-3.84 m/s. The
  review showed that this test could not tell them apart (boost 1 instead of
  the default 1.3, and the loose budget does not bind at a 3.8 m/s peak), so
  the claim "opening the hand does not weaken the throw" was unproven, and
  for a hand opening over more than 100 ms it was false.
- **Throws after the review** (`p6x/gripthrow2.py`, radio 5.1 kg, `light`,
  default `throwboost 1.3`, hand ramp to 6 m/s in 150 ms then down to 3 m/s
  in 60 ms and release; `firm` = grip 1.0; `open` = grip 0.9 (full force)
  falling in a straight line to 0.55 over 200 ms, ending at the release
  (`synthgrip 0.55 0 10 200`); `opennowin` = the same with `griptune
  gripthrow 0`, i.e. the budget from the grip of now as before the review;
  `loose` = grip 0.6 all along). Speed of the object before the caps and 25
  ms after the release, 3 throws each, with the energy cap off (`griptune
  energy 0`) to see the budget alone:

  | case | f at release / used | hand budget (m/s over the object's last command) | object before caps | flies at |
  |---|---|---|---|---|
  | firm | 1.00 / 1.00 | 4.67, not reached | 7.46 m/s | 7.33-7.34 m/s |
  | open (new) | 0.60-0.62 / 1.00 | 4.67, not reached (2.81-2.91 with the grip of now) | 7.46 | 7.33 |
  | opennowin (old) | 0.60-0.63 / same | 2.81-2.96, limited | 6.53-6.71 | 6.41-6.58 (-10 to -12%) |
  | loose | 0.40 / 0.40 | 1.87, limited | 5.62-5.63 | 5.50-5.51 (-25%) |

  (`p6x/final/thr_noenergy.out`, final dll; a first run on the build before
  the last fix gave the same numbers within 0.15 m/s.) The object lags a
  little more while the hand opens (its last command about 3.75 m/s instead
  of 4.4), and the firm budget makes that up. With the default energy cap
  (85 J: 5.75 m/s for the 5.1 kg radio) `firm`, `open` and `opennowin` all
  fly at 5.64 m/s (2 each) and `loose` at 5.51: for the
  radio the old loss only showed in throws below the energy cap; lighter
  objects have a larger budget (the bottle: 14 m/s firm, 5.6 loose), heavier
  ones a lower energy cap. Spin (`ThrowSpinLimit`) uses the same firmest
  fraction; it was not measured separately.
- **Cost.** `PROPS: tick` holding the radio for 75 s (`perfhold.py`, light,
  same session): 0.200-0.203 ms on average with `analoggrip off`,
  0.200-0.204 ms with it on and a noisy grip; at most 0.35-0.76 ms (18.8-19.2
  only in the windows with the test-only `spawnprop`).
- **Regressions (default `analoggrip off`; numerically the same code path:
  f = 1 gives item 2's formulas exactly).**
  - Shared hit pipeline (`p4t/hitall.sh`, summoned dummy): the throw test
    hits the head (held, 1.5 m/s, as in the first battery: 1.6); held swing 2 hits in the 2 fast swings (2.7 m/s)
    and none pressed or slow; dropped radio hits Spine3 at 1.8 m/s. The
    regrab test (`f0r/regrab.py`) gave one hit at 2.8 m/s (Arm_Right) in
    the final run and in 3 reruns: the release and the regrab commands
    arrived in the same poll (log: `grabsim left off` and `grabsim left on`
    at the same millisecond), so there was no regrab and the held radio
    swung through the 80-degree hand turn (wrist 84 rad/s), a real swing.
    Item 4's final run (`fin2_hit_rg`, 00:58) shows the identical line. With
    the commands in separate polls (`p4t/regrab_slow.py`, 2 runs, and the
    first battery today): same actor regrabbed, wrist 0.1 rad/s, 0 hits.
  - Held swing with a LOOSE grip (`analoggrip on`, `synthgrip 0.6`):
    2 hits in the fast swings (2.7 / 2.6 m/s), none pressed or slow.
  - Grab suite (`suite.sh p6Fs`): same grasp, drive settle 0.059-0.108 s,
    sweep 1.6 cm, static grab, glove catch, knock bottle 26.5 cm, smash
    broke, shot moved 165 cm, door 60.6, sign 5.5 degrees. The drop test
    did not break (4.1 m/s, threshold 5); three reruns: broke at 5.7, not
    at 4.0, broke at 6.0 m/s (pre-existing scatter around the threshold:
    the same "did not break" at 01:06 in item 4's run and in item 1's
    suite). The first battery on the previous build (text-only difference)
    broke at 6.2 m/s, knock 44.1 cm, door 60.8, sign 5.4.
- **After the review: final series** (final dll, novr, level 1 CP04,
  `fpscap 120` (the orchestrator's cap at the time), `solidhands held` (the
  default since item 5's review); `p6x/final` in the scratchpad):
  - Plan values with the curve ending at 0.9: `GRAB: grip` lines 160 / 320 /
    400 N `light`, 80 / 160 / 200 N `full` at 0.6 / 0.8 / 1.0.
  - Heaviest props at grip 0.6 (`gripweight.py`): barrel 59.8 kg `light`
    net up 6.3 m/s2 (log 1.40 g), side lag 7.9 cm; barrel `full` 2.9 m/s2
    (1.20 g); rifle case 75 kg `full` 2.8 m/s2 (1.20 g, run-to-run 2.8-3.1).
    No slip, wedge or re-grip line in any heavy or radio run. Radio
    (`light`): off / 1.0 / 0.8 / 0.6 settle 0.050 / 0.050 / 0.075 / 0.117 s,
    lag at 1.5 m/s 0.2 / 0.2 / 0.4 / 3.4 cm, turn alpha max 187 / 196 / 154
    / 80 rad/s2, still error std 0.21 / 0.22 / 0.21 / 0.0 mm.
  - Frame rates (review: the filter and the rotation term depend on dt).
    `fpscap 90` (90 ticks/s) and `fpscap 250` (195-225 ticks/s reached):
    radio grip 1.0 / 0.6 settle 0.056 / 0.123 s at 90, 0.049 / 0.096 s at
    250, lag at 1.5 m/s 3.4-3.5 cm at 0.6 in both; `gripdyn.py` hold-still
    error std 0.13-0.27 mm at every grip and both rates, no oscillation;
    `gripdroop.py` tighten first step 1.4 degrees at 90, 0.8 at 250 (1.1 at
    120: the exponential filter gives the same curve in time), noisy grip
    target step at most 0.2 degrees at both.
  - With `analoggrip on` and a loose grip (`synthgrip 0.6`), which the
    first series never ran (it is not item 2's code path):
    - Smash in the hand (`throwtest.py ... smash`, alternated with
      `analoggrip off`): off 3/3 broke, loose 3/3 broke (`smash_ab.txt`;
      also 3/3 in a run before; before the blocked-turn rule: loose 1/6).
    - Static crate grab (`reach.py`, alternated): off 3/3 and loose 3/3
      held until released by the grip; one more loose run and one firm run
      in the suite before the last fix let the bottle go as wedged (the
      known crate-bottle scatter, also at a firm grip).
    - Glove catch (`glovestatic.py`): caught in flight, then held at grip
      0.60 (40%) and released by the grip. (Three runs later in a used level
      ended "target gone" 0.4 s after the lock, with `analoggrip off` too:
      the bottle had been changed by earlier tests.)
    - Held swing into the summoned dummy: 2 hits at 2.8 / 2.8 m/s in the
      fast swings, none pressed or slow.
  - Shared hit pipeline (`p4t/hitall.sh`, default `analoggrip off`): throw
    hits the head at 1.5 m/s; held swing 2 hits (2.5 / 2.7 m/s), none
    pressed or slow; dropped radio hits Spine3 at 1.9 m/s; regrab (one run,
    commands in separate polls this time): same actor regrabbed, wrist 0.1
    rad/s, 0 hits (the run before the last fix: the same).
  - Grab suite (`suite.sh p6xFs`): grasp as before, drive settle
    0.025-0.109 s, sweep 1.6 cm, static grab held, glove catch, knock bottle
    55.7 cm, knock barrel 0 cm (the same 0 in the first series: the barrel is
    part of a static collection), drop broke at 5.8 m/s, smash broke, shot
    moved the radio 164 cm, door 60.0, sign 5.1 degrees.
- **Unit tests** 234/234 since the review (`TestAnalogGrip`, 44 checks:
  binary grip -> 1, 0.5-0.6 under OpenXR -> the loosest fraction, NaN axis
  and NaN value, out-of-range values, the curve with firm at 0.9 and at 1.0,
  hold timing (no change before 100 ms, 0.62 of the way after 180 ms),
  tightening without a step (first tick at most 0.17 of 0.6 at 120 Hz, there
  in 100 ms), the same response at 90 and 250 Hz, the noise envelope (f
  within 0.70-0.776 for 0.8 +- 0.05, step at most 0.02 per tick: 0.034
  without the rise filter), the throw window (firm for 300 ms, released
  after) and its off switch (`gripthrow 0`), and a long pause. The first
  final series had 180 (190 after the item-5 review fix) and no pure helper.
- **Not verified:** real controllers. Every in-game test uses `synthgrip`,
  which comes before the controller path; novr has no runtime and the
  gesture simulation writes only 0 / 1, so `GripAxis` ->
  `GestureSample.leftGripV` -> `g_gripAnalog` -> `GripValueNow` never ran in
  the game. Its decisions are covered only by the unit tests. Also
  unverified: the analog axis through SteamVR's legacy Touch bindings and
  through the OpenXR shim, whether 0.9 is a comfortable full squeeze on
  Touch and Index, whether a loose grip feels like "hang and swing" (the
  velocity drive has no momentum, so a loose grip lags and turns slowly but
  does not swing like a pendulum), and the headset feel of the 40% curve.
- **Open, for the orchestrator:** the plan's "a_max > 1.3 g for 60 kg" holds
  for the default `light` (1.40 g); with `full` the loose grip keeps item
  2's firm floor, 1.20 g (net 3 m/s2). Item 6 does not lower it; raising it
  would mean a higher `liftmin` at `full` (item 2).

**Thrown objects hurt, stagger or knock down (gameplay plan, item 1, 26/10).**
Option `propthrowdamage on|off` (default on, Basic menu "Thrown objects
hurt", ini line with comment). It needs `propcombat`, which feeds it the hit
events. Code: `props.cpp` `ThrowDamage` (called at the end of `OnPropHit`),
`ThrowRecord`/`ThrowRecordAdd` (in `Release`), `vr::pure::ThrowHitTier` and
`vr::pure::ThrowDamageSpeed` (`pure_utils.h`, unit-tested in `TestThrowHit`).
- **Which hits.** Only item 0's events with `kind` `kFlying`, `source`
  `kThrown` (released by the grip at 1.5 m/s or more) and a pawn target.
  Dropped, gloved, shot or knocked objects and held swings (item 4) do
  nothing here. The pipeline's targets are already only hostile pawns
  (`GatherPawns` -> `PawnHostile`), and `ThrowDamage` asks `PawnHostile` again
  (cached). Squad mates, allies and civilians get nothing, not even a
  stumble. Measured: a radio thrown at a summoned `YPawn_Lugo` (250 HP, with
  an `AIController`) bounced off with no `PROPS: hit` line (`PROPS: pawn
  YPawn_Lugo: squad 1 ... (squad member) -> not a target`); HP stayed 250.
- **Energy and tiers** (`ThrowHitTier`). E = 0.5 kg v^2. kg = `GetBodyMass`
  x 40 (`kThrowKgPerUnit`, the default `masskg`). It used `masskg` until the
  review fix (26/10): the tiers then moved with the player's "Weight of
  objects" feel setting (at `masskg 20` the radio was 2.5 kg and never heavy
  on the body). v is the impact speed relative to the enemy (the hit point's
  PhysX velocity minus the pawn's `Actor.Velocity` +0x140), after the glitch
  filter below. Defaults (`ThrowHitParams`):
  - nothing under 3 m/s or 4 J (the object only bounces off);
  - **heavy** from 30 J with at least 3 kg, or on the head (bone name with
    "head" or "neck", and the hit body within reach of the contact, see
    below) from 15 J with at least 1 kg. Damage 20 + 0.4 x (E -
    30 J; on the head E - 15 J), at most Walker's own melee damage
    (`YGamePawn.m_meleeDamagePawns` +0x1C48, read from Walker: 40);
  - otherwise **light**: damage 3 + 0.15 x E, x1.5 on the head, at most 15.
  - Examples: a bottle (1.6 kg) on the body at 6 m/s is 29 J, light, 7-8
    damage; a bottle on the head at 6 m/s is heavy, 26; a radio (5.1 kg) at
    the 85 J throw cap (5.8 m/s) is heavy, 40. A can (under 1 kg) is never
    heavy.
  - **Head only within reach** (review fix, 26/10). Item 0 finds the bone
    with two zero-extent traces (along the motion, then sideways to the
    enemy's axis), kept only when the body they touch is within the
    object's half size + 12 units of the contact. When both miss, the
    fallback is the PhysicsAsset body closest to the contact
    (`ClosestBodyBone`), at any distance. A bottle that only clipped the top
    of the capsule (r 15, h 45) then read `head (closest body)` and was
    knocked down as a head hit (z 729-732 in the first round). Now
    `ClosestBodyBone` also returns the distance, `PropHit.boneNear` is true
    for the traces and for a closest body within the same reach, and only a
    near head bone counts as a head. The hit line shows `(closest body, out
    of reach)` and the throw line `(head bone out of reach: body)`.
- **Light: `Pawn.TakeDamage`.** Parms read in memory with `f1/funcinfo.py`
  (ParmsSize at UFunction+0xA6). `YGamePawn.TakeDamage` has ParmsSize 0x44
  and iNative 0:
  - Damage @0;
  - InstigatedBy @4: Walker's controller (`Pawn.Controller` +0x214);
  - HitLocation @8;
  - Momentum @0x14: flight direction x kg x v;
  - DamageType @0x20: `YDamageType_Melee`;
  - HitInfo @0x24 (`TraceHitInfo`, 0x1C bytes): Material, PhysMaterial,
    Item @+8 = -1, LevelIndex @+0xC = -1, BoneName @+0x10 = the hit bone,
    HitComponent @+0x18 = the pawn's mesh;
  - DamageCauser @0x40: the object.

  Its locals run to +0xAC, so the C++ buffer has 0x80 spare bytes.
  **`YDamageType_Melee` has `m_neverKills`** (`YDamageType` +0x88 bit 0x1;
  the class default reads 0x83), so a thrown object never kills.
- **Heavy: `YGamePawn.HandleMeleeAttack`**, the function Walker's melee calls
  on its victim. ParmsSize 0x28, iNative 0: attacker @0 (Walker), Momentum @4,
  Damage @0x10, DamageType @0x14 (`YDamageType_Melee`), doResetPawn @0x18 and
  ResetLocation @0x1C (both 0). The locals moveDir, targetRot,
  canPlayTakeHit, vaultMove and YPC follow, up to +0x44.
  - **Gate:** `IsMeleeAttackable(attacker, checkStanding,
    specificAttackType)`, ParmsSize 0x10, return value @0xC. The mod calls it
    with Walker, 1 and `YMAT_None`. Enum `YE_MeleeAttackType`: None, Punch,
    TakeHumanShield, ReleaseHumanShield, CoverInvalid, LandingKick,
    VaultKick, ZipKick, FastRope.
    - The native `AYGamePawn_execIsMeleeAttackable` (0x00636310) calls the
      interface at pawn+0x478, slot +8 = 0x00EC0BF0.
    - That returns false: for a pawn with +0x5FC bit 0x40000 (set while
      knocked down), unless the attack is a kick; for the dead (vtable
      +0x588/+0x58C); when checkStanding finds a non-hostile attacker
      without the override; and when the current special move (vtable
      +0x6C0, its +0x130) refuses the melee.
    - Measured with `ucall`: true on a standing dummy, false 2 s after a
      knock-down.
  - No knock-down within 1.5 s of the last one on the same enemy (two
    objects in one throw: the second is light).
  - **Only pawns with a `YPlayerReplicationInfo`** (`Pawn.PlayerReplicationInfo`
    +0x36C; review fix, 26/10). In the first round the game crashed: after a
    throw knock-down of a dummy with a generic `AIController` and no PRI, B
    started the execution (special move 0 -> 27 at 15:49:17.8), and right
    after `PROPS: shot hit YPawn_MarauderAssault` the log read `CRASH: game
    thread exception 0xC0000005 at 008323C4` (minidump
    `specopsvr_crash_253881156.dmp`); the render-thread watchdog stalled and
    the game exited. The first write-up called this "the execution stayed in
    27/26 for over 10 s"; it was a crash.
    - Cause (Ghidra): 0x008323C4 is in `FUN_00832350`, a `YGamePawn` virtual
      (13 vtables). For a pawn with a controller, +0x4BF == 4, health > 0,
      knocked down (+0x5FC bit 0x40000) and +0x5FC bit 8 clear, it calls
      `FUN_007f3710(pawn+0x36C)` (Cast to `YPlayerReplicationInfo`, 0 for
      null) and reads +0x2D4 of the result (`m_canBeDamagedInDBNO`, bit 0x40)
      with no null check.
    - Real enemies have a PRI: the squad standings use its team, and
      `CheckExecute` reads `m_canBeExecutedInDBNO` from it (+0x2D4 bit 0x20).
      A pawn without one now gets the light tier, which never knocks down:
      `(not knocked down: no YPlayerReplicationInfo: the game crashes on a
      knocked-down pawn without one)`. Walker's own melee can still put such
      a pawn in that state; that is the game's path, not the mod's.
    - The tests give the dummy a spawned `YPlayerReplicationInfo` (`ucall pawn
      Spawn YPlayerReplicationInfo`, written to +0x36C): it reads
      `canBeExecutedInDBNO yes`.
  - If the gate says no, the hit is light instead. Logged as `(not knocked
    down: IsMeleeAttackable false ...)`.
  - **Momentum's direction matters, its size does not.** `ucall` on a dummy
    with an `AIController`:
    - |Momentum| 20, 500 and 5000 along Walker -> dummy all gave special
      move 9 (`YSPM_TakeHit`), a 3.5 m stagger (176-177 units) and
      `m_isKnockedDown` (+0x5FC bit 0x10000) ~1.5 s later;
    - -500 sent it the other way (191 units towards and past Walker).

    So the mod passes the object's horizontal flight direction (fallback:
    Walker -> enemy) x kg x v.
- **Only pawns with a controller.** Without one (the summoned dummy, scene
  pawns) the game kills the pawn with any damage. `HandleMeleeAttack` with 1
  damage gave special move 53 (`YSPM_PlayDeathAnim`) and a ragdoll; the light
  `TakeDamage(8, Melee)` took a dummy from 75 to 0. So no controller means no
  effect, logged as `(no effect: no controller: the game would kill it with
  any damage)`. Tests use the dummy plus `ucall 0x<pawn>
  SpawnDefaultController`.
- **Stun (optional, never seen working).** A light hit on the head uses
  `YDamageType_Stun` with 1 damage instead of the melee type. This happens
  only when the controller is a `YAIController` with `m_mayReactToStun`
  (+0x3E8 bit 0x8000000). Class defaults leave that bit off; the Kismet
  action `OnToggleAIMayReactToStun` turns it on (which scenes do is not
  known). `YDamageType_Stun` is prefetched by the incremental scan
  (`kPfDamageStun`).
  - That type has no `m_neverKills` (flags 0x82), and a heavy throw leaves an
    enemy at exactly 1 HP. Since the review fix the stun needs health above
    1; at 1 the hit is a plain light melee hit, which cannot kill.
  - No stun has been seen in the game. The only stun test (`t9_stun2`) ran on
    a `YAICtrl_Humanoid` dummy, which takes no damage at all from any source
    (see "Real AI controller" below), so it proves nothing either way.
- **Real AI controller (`YAICtrl_Humanoid`), unresolved.** A dummy given
  `ControllerClass` = `YAICtrl_Humanoid` before `SpawnDefaultController`
  (controller possessed: `Controller.Pawn` = the dummy) ignores damage from
  the game itself, not only from the mod (`f1/yaictrl.py`, 26/10 18:00):
  - direct `ucall TakeDamage 10 ... YDamageType_Melee`: health 75 -> 75;
  - direct `ucall HandleMeleeAttack pawn 500,0,0 30 YDamageType_Melee`:
    special move 0 -> 9 (the stagger plays), health 75 -> 75.
  - Not the cause: `bGodMode`/`bPGodMode` false, `bCanBeDamaged` true on the
    pawn, `m_isInDemiGod` and `m_immortal` false,
    `m_receivedMeleeDamageMultiplier` 1, `m_armorMultiplier` 0 (the same on
    the `AIController` dummies, which do take damage).
  - Different: `YAIBaseController.m_aiDefinition` (+0x3B0) is None; the
    level's spawners set it on real enemies. The damage path through it was
    not traced (two approaches, then the time box).
  - So the effect on real enemies is not proven: everything below is
    measured on the dummy with a generic `AIController` (plus a spawned PRI
    for the heavy tier). Dummy-verified only until a real-AI case passes.
- **Outcome in the log** (review fix). The line and the 30 s counters used to
  say `HandleMeleeAttack (stagger / knock-down)` whenever the call returned.
  Now `ThrowDamage` reads health (+0x304), the special move (+0x491) and the
  knock-down bits (+0x5FC & 0x50000) before and after the call and logs
  them. If none changed: `HandleMeleeAttack: no effect (health, special move
  and knock-down flags unchanged)` (or `TakeDamage: no effect`), counted
  apart.
- **Tracking-glitch filter.** At each grip release `ThrowRecordAdd` stores,
  per component (at most 16 records): the speed given to the object relative
  to Walker, a trusted speed and the release height.
  - With `throwmode peak`, the trusted speed is that speed: the peak samples
    are already cleaned (jumps over `peakjump` dropped, isolated outliers
    replaced by their median).
  - With `classic` (the 60 ms average keeps the jumps), it is at most the
    cleaned estimate of the same history: `PeakThrowVel`, max of peak and
    release average, x `throwboost`, plus the lever, +10% and +0.5 m/s.
  - `suspect` (review fix, 26/10) is set only when a jump really touched the
    throw:
    - `HandJumped` fires: a move over 25 m/s across at least 11 ms, among
      the 16 samples of the history (`maxN` = `kHistN`; at most 110 ms back;
      the other caller keeps the 8 newest, about 35 ms at 225 ticks/s and
      55 ms at 144 Hz). No real hand reaches 25 m/s.
    - With `classic`: a sample over `peakjump` was rejected and the 60 ms
      average is above the cleaned estimate (trusted < given).
    - With `peak`, a rejected sample alone no longer counts: the given speed
      is already built without it. Before the fix any sample over
      `peakjump` (12 m/s relative to Walker) made the throw suspect, so the
      hardest real throws (10-15 m/s) could never knock anyone down, while a
      gentle radio toss did. To check in the headset: whether a real hard
      overhand throw fires `HandJumped`.
  - The record also keeps the trusted velocity in the world (Walker's
    velocity at release + the trusted part along the throw).
  - At the hit (`ThrowDamageSpeed`), the damage speed is the impact speed but
    at most sqrt(r^2 + 2 g dz) + 1 m/s. r = |trusted world velocity - the
    enemy's velocity| (shown as `against the enemy` in the log), dz =
    release height - hit height; without a record, at most 15 m/s. Before
    the fix r was the speed relative to Walker, so a throw on the run was
    cut to the relative speed and an enemy running into the object did not
    take it harder. An object pushed faster by something else does not hit
    harder than the throw.
  - A `suspect` throw is at most light.
  - Test command `synthleftjump f r u ms`: the fake left hand jumps by
    (f, r, u) cm after ms and stays there (`SYNTH: left hand jumped`).
- **Logs.**
  - One line per thrown hit on an enemy: `PROPS: throw hit
    YPawn_MarauderAssault: 5.1 kg at 5.7 m/s (impact 5.7, release 5.3
    trusted / 5.3 given, 5.3 against the enemy) = 82 J -> heavy, 40 damage,
    health 75 -> 35, special move 0 -> 9, knock flags 00000 -> 00000,
    HandleMeleeAttack (stagger / knock-down)`. The special move changes in
    the call; the knock-down bits come about 1-2 s later.
  - Every 30 s, next to `PROPS: hits`: `PROPS: thrown hits on enemies in
    30 s: N (k knock-downs, l light, s stun, t too soft, r refused (no
    controller), e called with no effect; j with a tracking jump at
    release)`. "knock-downs", "light" and "stun" count only calls with an
    effect.
- **Cost.** Nothing per tick besides one record per release. A hit makes at
  most 3 `ProcessEvent` calls (`IsMeleeAttackable`, then `HandleMeleeAttack`
  or `TakeDamage`), a GNames lookup the first time a bone name is seen
  (cached), and the log line.
- **Attacker check** (review fix). Walker (`g_tickPawn`) is passed as the
  attacker, so `ThrowDamage` does nothing unless Walker's class is a
  `YGamePawn` (not a vehicle or turret pawn).
- **Measured.** Setup: level 1 CP04, novr, god mode, a fresh
  `YPawn_MarauderAssault` per test with an `AIController`, Walker 3.5 m
  away, `synthsmooth on`. Scratch script `f1/throwdmg.py <out> <mesh> <m/s>
  <z aim> <controller> [--exec] [--extra=...] [--hp=N]` (review fix round:
  `f1/throwdmg2.py`, with `--pri` for a spawned `YPlayerReplicationInfo`);
  outputs in `scratchpad/f1/` and `scratchpad/f1x/`.

  First round (before the review fixes):

  | Test | Log | Result |
  |---|---|---|
  | Bottle, chest, 6 m/s | 1.6 kg at 6.6 m/s = 36 J -> light, 8 damage | HP 75 -> 67, no stagger (`t1_light_final`) |
  | Bottle, chest, dummy without controller | 31 J -> none (no controller) | HP 75 -> 75 (`t1_noctrl`) |
  | Radio, chest, 5.5 m/s | 5.1 kg at 5.6-5.7 m/s = 80-84 J -> heavy, 40 damage | HP 75 -> 35, special move 9, staggered 3.5 m back, knocked down (`t2_heavy`, `t2_heavy_exec3_1`) |
  | Bottle, head | head (closest body), 5.9 m/s = 29 J -> heavy, 26 damage | HP 75 -> 49, knocked down (`t3_head`). A closest-body "head" at the capsule top: since the fix, a body hit unless within reach |
  | Radio, chest, with `synthleftjump 0 0 15 110` | `1+0 rejected`; 80 J, tracking jump at release -> light, 15 damage | HP 75 -> 60, stays up (`t4_glitch`). Since the fix not suspect (see below) |
  | Radio, chest, real health set to 10 (+0x304 and `m_health` +0x1B24) | 80 J -> heavy, 40 damage, health 25 -> 1 | alive at 1 HP, down (special move 56) (`t5_lowhp_heavy`) |
  | Bottle, chest height, real health set to 6 | head (side trace), 31 J -> heavy, 26 damage, health 20 -> 1 | alive at 1 HP (`t5_lowhp_light`; meant as a light test, it went heavy) |
  | Radio at a summoned `YPawn_Lugo` | no hit (squad member, not a target) | HP 250 -> 250 (`t6_lugo`) |
  | Bottle, head, from 2.2 m, `YAICtrl_Humanoid` with `m_mayReactToStun` set | 4.1 m/s = 14 J -> stun, 1 damage, `TakeDamage (stun)` | no visible stun, HP unchanged; the `YAICtrl_Humanoid` dummy takes no damage at all (`t9_stun2`) |

  Review fix round (26/10 17:55-18:01, final dll, one run each):

  | Test | Log | Result |
  |---|---|---|
  | Radio, chest, AIController + PRI, then B (3 runs) | 82, 88, 67 J -> heavy, 40/40/35 damage, special move 0 -> 9 | 3 of 3 knocked down and executed: Walker 27 / dummy 26 at 0.25-0.61 s, HP 0 at 2.76-2.96 s; no crash (`x_exec1`-`3`) |
  | Walker's own melee (B), then B, same dummy setup (3 runs) | `SPECIALMOVE: 0 -> 8 (PlayMeleeAttack)`; `EXECUTE: ... execution allowed ... canBeExecutedInDBNO yes` | 3 of 3: 75 -> 35, special move 9, knocked down 1.9-2.5 s after B, executed, HP 0 at 2.76 s (`x_melee1`-`3`, `f1/meleeexec2.py`) |
  | Radio, chest, AIController, no PRI | 96 J -> light, 15 damage, `(not knocked down: no YPlayerReplicationInfo ...)` | HP 75 -> 60, stays up (`x_nopri`) |
  | Radio, chest, PRI, `synthleftjump 0 0 15 110` | `1+0 rejected`, release 5.8 trusted / 5.8 given, not suspect -> heavy, 40 | knocked down: the jump did not touch the given speed (`x_jump15`) |
  | Radio, chest, PRI, `synthleftjump 0 0 40 110` (31 ms before release) | `HandJumped`: tracking jump at release -> light, 15, `(not knocked down: tracking jump at release)` | HP 75 -> 60, stays up (`x_jump40`) |
  | Radio, chest, AIController, no PRI, health written to 1 | light (no PRI), `TakeDamage(15, YDamageType_Melee)`, health 14 -> 1 | alive at 1 HP: the light tier never kills (`x_lowhp_light2`; the bottle on 5 HP had regenerated to 15 before the hit: 15 -> 7, `x_lowhp_light`) |

  - Health regenerates: `YGamePawn.m_health` (+0x1B24, `YS_RecoverValue`
    {m_currentValue, m_maxValue}) refills after 4.5 s, in 4 s. `Health`
    +0x304 is recomputed from it, so writing only +0x304 does nothing. A
    fresh dummy regenerates at once (about 19 HP/s), so a health written just
    before a throw has grown by the hit.
  - `m_neverKills` is enforced for a pawn with a controller: TakeDamage(15,
    Melee) on 14 HP left 1, HandleMeleeAttack(40) on 25 and 20 HP left 1.
    Without a controller the same TakeDamage(8) killed (75 -> 0), so the
    clamp needs the controller; `ThrowDamage` already skips pawns without one.
  - The bone varies at the same aim: the dummy's head sits about at the
    capsule's chest height (aiming crouch). One chest-height bottle read
    `head (side trace)` at 29 units above the pawn centre, the others
    `Spine3` or `Arm_Right` at 25-32 units.
  - **Executions.** After a knock-down with special move 9 (`YSPM_TakeHit`),
    Walker moved to 80 units and pressed B (`pad fake ... 2000`). The log
    read `EXECUTE: ... execution allowed`; Walker went to `YSPM_Execute`
    (27), the dummy to `YSPM_BeingExecuted` (26), and the dummy's HP was 0
    2.8 s later.
    - Review fix round, with the PRI guard: 3 of 3 throw knock-downs and 3
      of 3 melee knock-downs executed (table above), no crash.
    - First round, dummy without PRI, 4 tries: 15:49 the execution started
      and the game crashed (see "Only pawns with a `YPlayerReplicationInfo`";
      the first write-up wrongly said it "stayed in 27/26"); 16:00:47 the game
      chose the ragdoll fall (special move 56,
      `YSPM_RecoverFromRagdollToAnimDBNO`, `t2_heavy_exec2`), B gave a plain
      melee and the dummy got up 4 s later, the game's own rule; 16:02:59 the
      mod logged `execution allowed` but the game played `PlayMeleeAttack`
      (8), not the execution (not explained); 16:03:59 executed
      (`t2_heavy_exec3_1`). `t2_heavy_exec3_2` was not an execution attempt:
      its throw missed and B gave a plain melee. Walker's own melee was run
      once (`me1`).
- **Regressions** (final dll; `p4t/hitall.sh f1_hit`, `suite.sh f1s`; output
  `scratchpad/f1/hitall_out.txt`, `f1/suite_out.txt`). The dummies of these
  scripts have no controller, so their thrown hits do nothing new.
  - Hit pipeline: `hitthrow` held hit on the head at 1.5 m/s, release
    5.8 m/s, bounced off, no thrown hit; `heldswing` Spine1 2.5 and 2.5 m/s,
    none pressed or slow; `hitdrop2` dropped, Spine3 1.9 m/s; stock `regrab`
    merged its batches again (Arm_Right 2.8 m/s, as in items 3-5). All as
    in items 4-6.
  - Grab suite: grasp power grip, centre f -2.6 p 5.5 a 6.6 cm, turned 46
    degrees; drive settle 0.059-0.108 s, sweep error 1.6 cm; static grab;
    glove 1.8 m in 0.41 s, caught; bottle knocked 53.8 cm, barrel not
    knocked; drop broke at 5.9 m/s; smash broke in the hand; shot moved the
    radio 162.7 cm; door 60.4 degrees, stop sign 5.4 degrees.
  - `PROPS: tick` 0.17-0.23 ms on average (items 4-6: 0.18-0.21). No
    `SCRIPT: exception` line in any log of the session.
  - Unit tests 254/254 (`TestThrowHit`: tiers, head, can, radio at the
    energy cap, suspect demotion, the damage-speed bound).
  - Review fix round (final dll, 18:02-18:14; `p4t/hitall.sh f1x_hit`,
    `suite.sh f1xs`; output `scratchpad/f1x/hitall_out.txt`,
    `f1x/suite_out.txt`): the same results. Hit pipeline: `hitthrow` held
    hit `head (closest body)` (within reach) at 1.5 m/s, release 5.8 m/s;
    `heldswing` Spine1 2.5 and 2.7 m/s, none pressed or slow; `hitdrop2`
    Spine3 2.0 m/s; `regrab` Arm_Right 3.1 m/s. Grab suite: grasp
    unchanged; drive settle 0.050-0.108 s, sweep 1.6 cm; static grab; glove
    1.8 m in 0.41 s, caught; bottle knocked 53.1 cm, barrel not; drop broke
    at 6.0 m/s; smash broke in the hand; shot moved the radio 162.7 cm; door
    61.3 degrees, stop sign 5.4 degrees. `PROPS: tick` 0.17 ms median
    (0.21 at most) over the three game runs; no `SCRIPT: exception` and no
    crash. Unit tests 266/266.
- **Not verified / open.**
  - Real AI enemies in combat: the feature is dummy-verified only. Only
    summoned dummies with a generic `AIController` (plus a spawned PRI for
    the heavy tier) were used. A `YAICtrl_Humanoid` dummy takes no damage
    from the game itself either (see "Real AI controller"); the cause is not
    found.
  - The stun: no stun reaction seen anywhere; the branch stays, guarded
    against killing (health > 1).
  - Whether a real hard overhand throw in the headset fires `HandJumped`
    (25 m/s) and is demoted.
  - The light hit shows no reaction on the dummy: `m_physicalHitReaction*`
    (+0x7D8, +0x820) and `m_playingHitReactionAnim` stay unchanged. Whether
    real enemies flinch is unknown. The dummy is a blue mannequin whose mesh
    package is not loaded in level 1.
  - Heavy troopers (`YPawn_HeavyTrooper`): `Summon` does not spawn one in
    level 1, so their melee resistance was not seen.
  - Moral-choice scenes: thrown objects never kill (melee damage type), so
    they cannot count as kills there; not run in such a scene.
  - The headset (real throws, real tracking glitches).

**Objects break glass, crates and sand windows (gameplay plan, item 2, 26/10).**
Option `propglass on|off` (default on, Basic menu "Objects break glass", ini
line with comment). It needs `propcombat`, which feeds it the hit events.
Code: `props.cpp` `WorldDamage` (called at the end of `OnPropHit`, after
`ThrowDamage`), `vr::pure::WorldHitDamage` (`pure_utils.h`, unit-tested in
`TestWorldHit`); test command `glasshit`.
- **Which hits.** Every item-0 event on a world target (`kTgtFractured`,
  `kTgtSandBomb`, `kTgtDestructible`), from any source: thrown, dropped,
  gloved, shot or knocked objects and held swings. The energy decides.
- **Sand windows are not `YSandBomb`.** The name tables of all level
  packages (`SP_*.upk`, 771 files, LZO-decompressed with
  `scratchpad/f2/upknames.py` and `findsb.py`) never contain `YSandBomb` or
  `YSeqEvent_SandBombTriggered`. The sand windows are `YDestructibleObject` /
  `YDestructibleObjectDelayed` with `breakingGlass_sand` materials, and the
  avalanche is level Kismet hanging on `YSeqEvent_YDODamageModApplied` (sub
  object, damage mod "Destroyed").
  - Level 1's sand-filled bus (`SP_Desert01_03_K`) has two
    `YDestructibleObjectDelayed` panes (`m01a_busGlass_warpedWindow_single`,
    `..._flatWindow_single`): health 60, mods `Cracked` at 30 and `Destroyed`
    at 0 (collision off).
  - Each pane's event leads to `SeqAct_ModifyHealth`, behaviour-tree aborts,
    squad infiltrate mode, `YSeqAct_AIFactory` (the next wave) and remote
    events (the `BusAvalanche_*` matinees).
  - The `YSandBomb` path stays for completeness (`TakeDamage` adds to
    `m_currentDamage` +0x208 and triggers at `m_health` +0x200; `m_enabled`
    +0x20C bit 1), but no level can exercise it.
- **`YDestructibleObject.TakeDamage`** (script; ParmsSize 0x44 like
  `Pawn.TakeDamage`; `scratchpad/f2/bcrefs.py` lists the bytecode
  references):
  - It returns at once unless `m_damagingEnabled` (+0x1FC bit 0x400) is set.
    The level toggles it with Kismet (`OnToggle` only flips that bit).
    - In level 1 at CP04 the bus panes read 0x9E7 (off). So do the coach
      panes, except the first one (0xDE7).
    - Bullets that hit the bus panes then do nothing either: two M4 hits left
      health 60 -> 60.
    - Logged as `no effect: damage disabled by the level`.
  - It finds the piece by comparing `HitInfo.HitComponent` with
    `m_subObjects[i].m_mesh`. Layout: `m_subObjects` +0x204, stride 0x7C,
    mesh +0x8, damage mods +0xC; each mod has stride 0x54, name +0x0, health
    threshold +0x8, `m_applied` +0x50 bit 1.
    - With `HitComponent` None, `ucall ... TakeDamage` raised an exception in
      ProcessEvent (26/10).
    - So the mod always passes the piece's mesh: the component the trace
      hit, or else the piece whose bounds centre is nearest the contact,
      within 60 units. Otherwise there is no call (`no sub-object at the
      hit`).
  - It clamps the damage to `m_maxDamagePerHit`, then calls the native
    `DamageSubObject` (vtable +0x3BC: 0x008C7930; Delayed 0x00AB0F90).
    - If `m_vulnerableToDamageTypes` (+0x378) is not empty, the damage type
      (or a parent class) must be in it. The mod checks this first (`not
      vulnerable to melee damage`).
    - It takes the damage off `m_subObjectHealths` (+0x21C).
    - It applies every mod whose threshold the new health reaches. The
      Delayed class queues them with `AddDamageToDamageArray` (vtable +0x3C0
      = 0x007B2C70) and applies them from a timer.
    - When every piece is at 0, the destroyed event fires.
- **`FracturedStaticMeshActor.TakeDamage`** (script). It needs
  `bCausesFracture` (`DamageType` +0x60 bit 0x80; `YDamageType_Melee` reads
  0xD1). Then it takes one fragment:
  - `HitInfo.Item` when `HitInfo.HitComponent` is set (what bullets pass);
  - otherwise it runs its own `TraceComponent` along `Momentum` from
    `HitLocation`.

  It removes `WorldInfo.FracturedMeshWeaponDamage` (+0x68C, 1.0) x
  `DamageType.FracturedMeshDamage` (+0x84, 1.0) from `ChunkHealth[Item]`
  (+0x204), whatever `Damage` is: one hit, one fragment, like a bullet.
  - On the Gorge01 bowls (`m03a_bowl_FRACTURED`, 8 fragments of health 1)
    the script's own trace never found a fragment. `glasshit` and `ucall
    TakeDamage` with `HitComponent` None left 8 of 8 intact.
  - So the mod passes what bullets pass: `HitComponent` = the fractured
    component (+0x1FC), and `Item` = `FCheckResult.Item` of the pipeline's
    trace (the new `PropHit.item`), which is the fragment index.
  - The hit's item counts only if it came from the zero-extent pass
    (`PropHit.boxTrace` false, set from the pipeline's `usedExt`), is an
    index of `ChunkHealth` and that fragment is still intact. A box trace's
    `Item` is not a safe fragment index: a complex check on an ordinary
    static mesh returns 0, as the bus pane's `glasshit` logged (`item 0`).
  - Otherwise (a box-trace hit, no item, or an item that fails those
    checks) `WorldDamage` first runs one zero-extent complex trace along the
    motion through the contact (30 units before it to 60 after), the same
    trace `glasshit` uses, and takes its item if it hits the same component
    and passes the same checks. The log says where the fragment came from:
    `from the hit`, `from the fragment trace` or `none found`.
  - Last fallback, when no trace gives an item: `HitComponent` None and
    `Item` -1 (the script's own search, which found nothing on the bowls).
- **Damage** (`WorldHitDamage`, `WorldHitParams`). J = 0.5 kg v^2, where v is
  the hit speed (the target does not move).
  - For thrown objects, v is bounded by the release as in item 1
    (`ThrowDamageSpeed` with the `ThrowRecord`). A throw with a tracking jump
    at release is at most light.
  - Nothing under 2 m/s or 3 J (`too soft`).
  - Damage is 1.25 x J: at most 40 for a light hit (the object itself
    under 3 kg, or under 40 J), at most 250 for a heavy one.
  - Held swings add 2 kg of arm to the mass in the energy (`armKg`), but
    not in the heavy test, which looks at the object's own mass (review
    fix). Before, the arm counted there too, so every held object over 1 kg
    was heavy: a held bottle at 6 m/s (3.6 kg, 65 J) did 81 damage and
    smashed a bus pane on its own.
  - On the bus panes (health 60, `Cracked` at 30):
    - a bottle (1.6 kg) thrown at 6 m/s is 29 J -> 36: cracked;
    - any light hit is at most 40, so a single bottle never sets off the
      avalanche. A second one does: the damage accumulates, which is the
      game's own rule, as with bullets;
    - a radio (5.1 kg) thrown at 5.6 m/s is 80 J -> 100: smashed;
    - a radio lobbed at 3.5 m/s (31 J) only cracks it;
    - a held bottle at 4 m/s weighs 3.6 kg with the arm: 29 J -> 36;
    - a held bottle at 6 m/s is 65 J with the arm, but light: 40 (capped),
      cracked. At 8 m/s still 40. A held radio (5.1 kg) at 4 m/s is heavy:
      57 J -> 71, smashed.
- **Trace change (item 0 pipeline).** The second, zero-extent trace near a
  world target now adds `TRACE_ComplexCollision` (0x20000).
  - That flag means per-polygon collision even for meshes with simple
    collision. Bullets use it: `Actor.Trace` with `TRACEFLAG_Bullet` adds it
    (`AActor_execTrace` 0x009180A0).
  - The bus panes sit inside the simple collision of the bus (`InterpActor`
    `dubai_bus_wrecked`). The old trace hit the bus 0 units from its start,
    while PhysX let the object through, so no throw ever reached the glass
    (`trace hit InterpActor (not a target)`, as in item 0's notes).
  - With the flag, the same throw hits the pane.
- **The object after the hit** (flying objects only):
  - **The pane holds and has no PhysX body** (the bus panes: `BodyInstance`
    0, so the object would fly through): the object bounces, 30% along the
    normal and 80% sideways. The normal is flipped towards the side the
    object came from, because a per-polygon trace returns the triangle's
    normal. Log `bounced off`.
    - Since the review this also happens when the hit was refused or did
      nothing: damage switched off by the level (the bus panes for most of
      the level), too soft, a damage type the target is not vulnerable to.
      Before, the bounce needed a `TakeDamage` call, so an object flew
      straight through an unarmed bus pane.
    - "Holds" means: a destructible whose piece was found and still has
      health above 0 after the hit; a fractured mesh that kept intact
      fragments; a sand window that did not trigger. A piece already
      destroyed (collision off) lets the object through.
  - **It broke, but PhysX had already stopped or bounced the object** (a
    target with a body): the object gets back 70% of its velocity from
    before the hit and flies on, as in Source. Log `keeps flying`.
    - The velocity is the centre's PhysX velocity from the tick before
      (`PropHit.cvel`, from the sweep's `cvel`), not the hit point's: for
      an end or a tip that one includes the spin and could relaunch the
      object faster than its centre was moving.
    - Not on fractured meshes: one hit takes one fragment whatever the
      energy (see above), so the rest of the mesh is still in the way.
- **Logs.**
  - One line per hit: `PROPS: glass hit YDestructibleObjectDelayed
    (destructible, glass, piece windowWarped): 5.1 kg at 5.6 m/s (impact
    5.6) = 80 J -> 100 damage (heavy), broken, health 60 -> -40 (Cracked,
    Destroyed)`.
  - For fractured meshes the line ends with `intact fragments 8 -> 7
    (fragment 0, from the fragment trace)`; for sand bombs with `damage A
    -> B of H`. A fractured-mesh call that breaks nothing reads `no effect:
    no fragment broken, intact fragments 8 -> 8 (fragment -1, none found)`.
  - Every 30 s: `PROPS: object hits on glass, crates and sand windows in
    30 s: N (b broken, d damaged, s too soft, r no effect)`. `too soft`
    counts only the energy refusal; other refusals and a fractured-mesh
    call that broke nothing count as `no effect` (before the review, any
    refused hit with 0 damage counted as too soft, and a call that broke no
    fragment as damaged).
- **Walker's melee gesture while holding an object** (`vr_input.cpp`,
  review fix). The left-hand punch gesture (forward at chest height, over
  2.8 m/s) pressed B even with an object in the left hand, so swinging a
  held bottle into a pane or an enemy also started Walker's native melee.
  It now needs `g_propHeld[1]` false: with an object in the hand the swing
  is the object's (pipeline and `WorldDamage`), not a punch. `gesturesim
  melee`: free hand 1 press, bottle held 0, after the release 1
  (`scratchpad/f2x/melee_out.txt`).
- **`CallFunction` faults** (`script_call.cpp`). A function that raised an
  exception in ProcessEvent is now disabled per `UFunction`, not per name.
  Before, an exception in `YDestructibleObject.TakeDamage` would also have
  disabled `YGamePawn.TakeDamage` (item 1) for the rest of the session.
- **Test command** `glasshit 0x<actor> <kg> <m/s>`: a fake thrown hit on a
  world target, from Walker towards the centre of its box. A zero-extent
  complex trace gives the contact, component and item, and the event goes to
  `WorldDamage`.
- **Cost.**
  - Nothing per tick.
  - A hit costs one `ProcessEvent` (`TakeDamage`), a few reads, at most one
    `SetRBLinearVelocity` and the log line; a fractured mesh hit without a
    fragment adds one trace.
  - The extra flag only touches the zero-extent pass, which runs only near a
    world target.
  - `PROPS: tick` was 0.175-0.196 ms on average in the test windows
    (`spawnprop` windows excluded). `PROPS: hits` was 0.0011 ms per tick,
    with 404 traces in 30 s.
- **Measured.** Setup: novr, god mode; scripts and screenshots are in
  `scratchpad/f2/`.
  - Level 1 CP04, with `SP_Desert01_03_K` made visible and Walker under the
    bus (`f2/bussetup.py`).
  - With `arm`, `bussetup.py` does what the scene's Kismet does when the
    fight starts: it sets `m_damagingEnabled` on the two panes and
    `bEnabled` on their two `YSeqEvent_YDODamageModApplied`.

  | Test | Log | Result |
  |---|---|---|
  | Bottle at the flat pane, not armed (`t0_disabled`) | 1.6 kg at 8.6 m/s = 61 J -> no effect: damage disabled by the level | health 60 |
  | Bottle at the flat pane, armed (`t1_bottle`) | 1.6 kg at 9.3 m/s (tracking jump at release) = 71 J -> 40 damage (light, capped), health 60 -> 20 (Cracked) | event not fired (0/1) |
  | Radio at the warped pane from 1.5 m (`t2_radio_b`) | 5.1 kg at 5.6 m/s = 80 J -> 100 damage (heavy), broken, health 60 -> -40 (Cracked, Destroyed) | both pane events fired (1/1); both panes hidden (the second at -130, from the sequence); sand avalanche and falling cars (`t2_radio_b/after.png`, `shots/avalanche_later.png`); the Kismet spawned the next wave (`PROPS: pawn YPawn_MarauderSMG ... -> enemy` 1.5 s later) |
  | Held bottle pushed into the flat pane at 3.6 m/s (`heldglass.py`) | 3.6 kg with the arm at 3.6 m/s = 24 J -> 30 damage, health 60 -> 30 (Cracked) | no avalanche |
  | Bottle thrown at 4.4 m/s at the warped pane (`t3_bottle_bounce`) | 16 J -> 20 damage, health 60 -> 40, bounced off | the bottle ended on Walker's side |
  | `glasshit` on a Gorge01 bowl (`FracturedStaticMeshActor`), bottle 6 m/s | trace hit it, item 3; 29 J -> 36 damage, broken, intact fragments 8 -> 7 (fragment 3) | `ChunkHealth[3]` 1 -> 0 |
  | `glasshit`, 0.4 kg at 3 m/s | 2 J -> no effect: too soft | unchanged |
  | Radio thrown down onto the first coach pane (`m01a_166_166_single`, damage enabled by the level, health 25, `Destroyed` at 40) (`t5_coach`) | 5.1 kg at 9.4 m/s (fell 2.2 m after the release) = 225 J -> 250 damage (heavy), broken, health 25 -> -225 | pane gone; the radio was still ahead of PhysX's contact, so no velocity change was needed |
  | Two `glasshit` bottles (6 m/s) on the armed flat bus pane | 36 damage, health 60 -> 24 (Cracked); then 24 -> -12 (Destroyed) | after the second bottle both avalanche events fired (1/1): two bottles do what one radio does |

  - From 4.5 m the radio never reached the warped pane: something of the
    bus wreck stopped it ~55 cm in front of the hand. The bottles' first
    tries passed 100 units off. From 1.5 m the radio hit at once.
  - The mods named in the log are those whose threshold the new health
    reaches and that were not applied before the hit (`m_applied`). The first
    version listed only thresholds between the old and the new health and
    missed the coach pane's `Destroyed` at 40 (`t5_coach` line above, before
    the fix); the `glasshit` rows show the fixed log.
- **Regressions** (`suite.sh f2s`, `p4t/hitall.sh f2_hit`; outputs
  `scratchpad/f2/suite_out.txt`, `f2/hitall_out.txt`). They ran on the
  build before the log fix above, which only changes the text of the
  `glass hit` line.
  - Grab suite: grasp power grip, centre f -2.6 p 5.5 a 6.6 cm, turned 46
    degrees; drive settle 0.058-0.108 s, sweep error 1.7 cm; glove 1.8 m in
    0.41 s, caught; bottle knocked 53.0 cm, barrel not knocked; drop broke at
    5.9 m/s; smash broke in the hand; shot moved the radio 169.2 cm; door 59.4
    degrees, stop sign 5.5 degrees.
  - Static grab: in the suite the bottle (already a `KActorFromStatic` from
    an earlier step, no `became physical` line) wedged and slipped out after
    1.5 s. Run again alone (`reach.py bottle_quad01 f2s_rq2 tp`): became
    physical, held 13 s, released by the grip, as in items 4-6 and 1.
  - Hit pipeline: `hitthrow` held hit on the head at 1.6 m/s; `heldswing`
    Spine1 2.5 and 2.7 m/s, none pressed or slow; `hitdrop2` dropped, Spine3
    1.8 m/s; `regrab` wrist 0.1 rad/s on the first `Drive` ticks and no hit.
  - Unit tests 266/266 (`TestWorldHit` added). No `SCRIPT: exception` line
    in the game logs left on disk (`specopsvr.log`, `.1.log`, `.2.log`).
- **Review fixes (26/10 evening).** Scripts and outputs in
  `scratchpad/f2x/`; one build, novr, god mode.
  - Held objects and the heavy test: see **Damage** above. Unit tests
    303/303; `TestWorldHit` adds the held bottle at 6 and 8 m/s (40,
    capped, light), a held 2.9 kg object at 6 m/s (light) and keeps the
    held radio heavy.
  - A real thrown object on a fractured mesh, through the pipeline. Gorge01
    (`console open TheLine_P?mission=SP_Gorge01_P?...`) loads
    `m03_custom_content_02.m03a_bowl_FRACTURED`. `f2x/fsmspawn.py` spawns a
    `FracturedSMActorSpawnable` 3 m in front of Walker (`ucall pawn Spawn`),
    gives its `FracturedStaticMeshComponent` the bowl (`SetStaticMesh`,
    true) and calls `ResetHealth`: `ChunkHealth` 8 x 1. A bottle thrown at
    it (`f0r/throwat.py`, `f2x/b3_bowl_bottle.txt`, log excerpt
    `f2x/b3_bowl_log_excerpt.txt`):
    - `HITLOG: trace 1 (extent 7): FracturedSMActorSpawnable, mesh
      m03a_bowl_FRACTURED` (the box pass);
    - `PROPS: glass hit FracturedSMActorSpawnable (fractured mesh): 1.6 kg at
      5.8 m/s (impact 5.8) = 28 J -> 34 damage (light), broken, intact
      fragments 8 -> 7 (fragment 0, from the fragment trace)`;
    - `ChunkHealth` read afterwards: `[0, 1, 1, 1, 1, 1, 1, 1]`; no `keeps
      flying` (skipped on fractured meshes).
  - Bounce on an unarmed bus pane (`f2x/b2_unarmed_flat.txt`): `1.6 kg at
    9.1 m/s = 69 J -> 0 damage (light, capped), no effect: damage disabled
    by the level (m_damagingEnabled off), bounced off`; the 30 s line
    counted it as `1 no effect` (before: no bounce, the bottle flew
    through).
  - Melee gesture with a held bottle: see above (`f2x/melee_out.txt`).
  - Coach pane once more for `keeps flying` (`f2x/b4_coach.txt`): `1.6 kg
    at 9.6 m/s = 76 J -> 40 damage (light, capped), broken, health 25 ->
    -15 (Destroyed)`, found by an end sweep before PhysX touched the pane,
    so again no velocity change was needed and the branch did not run.
  - Regressions (`suite.sh f2xs`, `p4t/hitall.sh f2x_hit`;
    `f2x/final_out.txt`): grasp power grip, centre f -2.6 p 5.5 a 6.6 cm,
    turned 46 degrees; drive settle 0.058-0.100 s, sweep error 1.5 cm;
    glove 1.8 m in 0.41 s, caught; bottle knocked 52.9 cm, barrel not
    knocked; drop broke at 5.9 m/s; smash broke in the hand; shot moved the
    radio 152.1 cm; door 60.6 degrees, stop sign 5.5 degrees. Hit pipeline:
    `hitthrow` held hit on the head at 1.6 m/s; `heldswing` Spine1 2.7 m/s
    twice, none pressed or slow; `hitdrop2` Spine3 1.9 m/s; `regrab` merged
    the release and re-grab batches again (wrist 82 rad/s, Arm_Right 2.8
    m/s, as documented under item 1). `PROPS: tick` 0.17-0.21 ms on average.
  - Static grab: `bottle_quad01` slipped out `wedged` after 3.3 s in the
    suite and after 1.5 s when run again alone (`f2xs_rq2`; the log shows
    `held bottle_quad01 passes the invisible wall YBlockingVolume`). The f2
    run saw the same once; f1x and f3 held it 13 s. This round changes no
    grab or `Drive` code (diff against `snap_f2fix_glass`: `PropHit`,
    `Sweep.cvel`, `WorldDamage`, the melee gesture), so it is left open as
    the known flake of that spot.
  - One crash when `relaunch.sh` closed the Gorge01 test session (WM_CLOSE,
    19:29:31): 0xC0000005 at 0x006D77D2 (`FUN_006d77c0`, a list unlink
    called from string helpers), `specopsvr_crash_267093343.dmp`, no mod
    frame among the stack's return addresses. That session had the
    test-only spawned bowl (`ucall` `SetStaticMesh` / `ResetHealth`, one
    fragment broken). Not reproduced; the level-1 sessions closed cleanly.
- **Not verified / open.**
  - The real bus scene reached by playing: the panes were armed by hand, as
    the scene's Kismet does. Also the other sand windows of later chapters
    (`SP_DollsHouse_04/05_SandGP`, `SP_Atrium01_03_Avalanches`, ...).
  - A level-placed `FracturedStaticMeshActor` hit by a real object: the
    real-object test used a spawned bowl (above), and the Gorge01 bowls in
    `SP_Gorge01_01b` are reachable from the start only by forcing that
    streaming level visible, with incomplete collision (thrown bottles fell
    through the floor to z -692).
  - The `from the hit` fragment path (a zero-extent pass on a fractured
    mesh) ran only through `glasshit` (item 3 on a bowl).
  - Heavy hits on fractured meshes still take one fragment (the script's
    rule); breaking several around the contact (for example
    `BreakOffPartsInRadius`) was not tried.
  - `keeps flying` (a broken target with a PhysX body, such as the level 1
    coach panes) did not occur in these runs.
  - `YSandBomb`: no level uses it.
  - Crates (`YDestructibleObject` that are not glass) and
    `m_vulnerableToDamageTypes` lists: there were none near the tested spots.
  - The headset.

**Held objects stop enemy bullets (gameplay plan, item 3, 26/10).**
Option `propshield on|off` (default on, Basic menu "Objects stop bullets",
ini line with comment, README). It does not need `propcombat`. Code:
`props.cpp` `OnOtherShot` (called by `Hooked_CalcWeaponFire` in `hooks.cpp`
for every shot that is not from Walker's current weapon), `ShieldTick` (in
`TickImpl`, before `Drive`), `ShieldKick` (in `Drive`), `HeldTraceable`;
pure functions `ShieldMaterialOf`, `ShieldDurability`, `ShieldWear`,
`ShieldShotImpulse`, `ShieldDv`, `ShieldKickCm`, `ShieldKickShape` and
`SegmentObbEntry` in `pure_utils.h` (unit-tested in `TestShield`, 34 checks).
- **Every enemy instant hit goes through our hook.** Read in memory on 26/10:
  `Default__YWeapon_*` vtable slot +0x424 is FUN_0093F510 (the instant-hit
  loop, `__thiscall(weapon, shots)`) for 80 weapon classes: AI weapons,
  snipers (M110, MSG90, Barrett), machine guns and the mounted guns
  (`YWeapon_HumveeGun`, `HeliGun`, `M2E2HMG`, `TurretMiniGun`) included. The
  4 shotguns have FUN_00B01C80 there; their pellets go through vtable +0x42C =
  FUN_00F62930, the third caller of `VR_CalcWeaponFire` (0x004A3D50). So
  `Hooked_CalcWeaponFire` sees every bullet. Rockets, grenades and mortars are
  projectiles and are not covered.
- **The game already stops bullets at a held object.** `CalcWeaponFire` does
  one `SingleLineCheck` with Source = the weapon and flags 0x22CB7 (Pawns,
  Movers, Level, Others, OnlyProjActor, LevelGeometry, SingleResult, Material,
  Terrain, ComplexCollision). A held `KActor` keeps `bProjTarget` (+0xA0 bit
  0x80000000: read on the level's `KActor`s and on the defaults of
  `KActorFromStatic` and `KActorSpawnable`), so the trace hits it even though
  the grab turns `bBlockActors` off. FUN_0093F510 then damages the hit actor
  (FUN_009F7300) and spawns the effects (FUN_00F5E6B0) from the
  `FImpactInfo`; the bullet only goes on through `YDestructibleObject`s with
  +0x1FC bit 0x800 (Ghidra).
- **Where enemy bullets hit Walker.** Zero-extent traces hit the bodies of his
  PhysicsAsset, not the cylinder. With the dummy 6 m away and no object: aimed
  at the VR head, the impact was on the arm in front of the face (41 units
  from the pawn axis, 26 in front of the head); aimed at the pawn centre, on
  the chest (18 units out); aimed at the axis at eye height, nothing (the shot
  went 9.8 km on).
- **What the mod adds, per counted hit** (`ShieldTick`):
  - **Wear.** Each object has a durability from its mesh name and mass
    (`ShieldMaterialOf`, `ShieldDurability`): fragile (plates, books, paper,
    cardboard, glass, bottles, cushions, cloth) 2; metal or sand (metal,
    steel, ammo, toolbox, barrel, tank, canister, helmet, locker, sandbag...)
    15; anything else by mass: under 3 kg 3, under 6 kg 5, under 25 kg 6
    (radio 5.1-8 kg, suitcase 16.8 kg), heavier 10. A hit costs
    `ShieldWear(m_damage)`: 1 under 100, 2 under 300, 3 above (YWeapon
    `m_damage` +0x704, read on 26/10: M4 30, AK47 50, HK417 79, M2 135,
    Mossberg 365, Barrett 1400). Traces of the same weapon on the same object
    within 40 ms count once (shotgun pellets). The wear is kept per component
    for the whole level, so a shot-out object picked up again goes on the
    next hit.
  - **Push.** `AddImpulse` at the hit point with a velocity change
    `ShieldDv` = J / kg (at least 0.5 kg), 0.2-3 m/s, where J =
    `ShieldShotImpulse(m_damage)` = 0.8 + damage / 40, 1-6 N s. An impulse
    alone lasts one tick (Drive sets the velocity again every tick), so the
    held target pose is also moved along the shot and back (`ShieldKick`:
    `ShieldKickCm` = 2.5 x dv cm, 0.8-5 cm, shape (t / 30 ms) e^(1 - t / 30
    ms), gone after 0.24 s) and turned about the hit lever (at most 0.15
    rad). With `solidhands` the drawn hand follows the object.
  - **Haptics.** `ImpactPulse(J, kg)` on the holding hand (the item-3 formula
    and gain), not muted by the refractory time; when the object is shot
    out, full amplitude for 90 ms. With `impacthaptics off` fixed pulses
    (30 ms, 90 ms).
  - **Shot out of the hand.** At the durability the hand releases it
    (`Release(h, "shot out of the hand")`) and it flies away from the head
    (review fix, 26/10: before, it flew along the shot at a fixed 3.5 m/s
    plus 1 m/s up for every mass, so an object held in front of the face went
    back through the head and the camera and landed 2.5-3.5 m behind
    Walker). `ShieldKnockDir` (`pure_utils.h`): the shot direction without
    its horizontal part towards the head, plus 0.8 sideways on the holding
    hand's side and 0.5 down (up if the object is above the eyes), so the
    flight never has a horizontal part towards the head. The head is the view
    origin of the eye just drawn (`props::SetViewOrigin`, called by the camera
    hook right before `props::Tick`; if older than 0.5 s, Walker's location
    plus `fpeyeheight`). Speed `ShieldKnockSpeed(dv)` = 0.8 + 1.2 dv m/s, at
    most 3.5 (radio 5.1 kg with the AK47 1.28 m/s, suitcase 1.04, bottle
    3.5); spin 2.5 x speed rad/s (at most 8) about the hit lever. A
    breakable object (`KActorFromStatic` with a template: the level's
    bottles) breaks instead (`BreakProp`, the game's glass effect).
  - **Log.** `PROPS: shield <mesh> (left hand) took a shot from <weapon>
    (damage D, stopped by the game|redirected from Walker): hit N of M
    (<material>, kg), pushed v m/s, kick c cm[, knocked out of the hand]`, a
    `HAPTIC: left hand shield shot ...` line, `PROPS: shield knocked away at
    v m/s, direction (x, y, z), h towards the head (horizontal), object N
    units from the eyes (view|pawn)` when it flies out, and every 30 s
    `PROPS: other weapons' shots in 30 s: N (on Walker, stopped by a held
    object, redirected, let through, friendly on a held object (no effect),
    extra pellets, knocked out, shields broken); N script calls (not
    bullets, ignored)`.
- **Only enemy bullets** (review fix, 26/10). `OnOtherShot` ran for every
  weapon whose Instigator was not Walker, Adams and Lugo included. Now
  `ShooterHostile` reads the weapon's Instigator (Actor +0xE8) and asks
  `PawnHostile` (the hit pipeline's test: squad member +0x4BC, team
  standing, enemy class; cached per pawn for 2 s). A friendly bullet on a
  held object is still stopped by the game (`bProjTarget`) but gets no wear,
  kick, haptics or knock-out, and is never redirected; it counts as
  `friendly` in the 30 s line. A weapon without an Instigator counts as
  hostile (no friend fires without one). `propshield off` still lets every
  bullet through. The check runs only for shots that touch a held object
  (or would be redirected), not for every bullet in the level.
- **Bullets only** (review fix, 26/10). There are three direct calls to
  `CalcWeaponFire` in the exe (byte scan of the `.text` section,
  `scratchpad/f3x/callsites.py`): 0x0093FEA2 in the instant-fire loop
  (FUN_0093F510), 0x00F63029 in the shotgun routine (FUN_00F62930) and
  0x00C2727C in `AYWeapon_execCalcWeaponFire` (UnrealScript). The hook reads
  its return address (`_ReturnAddress()`: MinHook enters the detour with a
  jump, so it is the caller's). A call from the script thunk is not a
  bullet: it gets no shield effect and no `propshield off` re-trace, and it
  counts as `script calls` in the 30 s line. Any other caller is treated as
  a bullet. Walker's own shots are unchanged.
- **Redirect.** If the game's trace hits Walker, `OnOtherShot` tests the
  bullet line against the oriented box of each held object (StaticMesh local
  bounds +0x94 in the component's `LocalToWorld`, `SegmentObbEntry`), from
  the start to the point of the line closest to Walker's axis (at least 4
  units past the hit point, at most 60): an object held against the body,
  with the hand or the arm inside its box, can be past the point where the
  trace met the arm. If the line enters the box first, the `FImpactInfo` is
  rewritten before the game uses it: HitActor, HitLocation (the box entry),
  HitNormal (the face), Material and PhysMaterial 0, Item -1, BoneName None,
  HitComponent = the object's component. Damage and effects then go to the
  object. A shot from behind ends at the axis, before the object in front.
  The box is the mesh's render bounds, a little larger than its collision:
  a bullet that grazes the box corner and hits Walker counts as stopped.
- **`propshield off`.** If the game's trace hits a held object, `OnOtherShot`
  returns 2 and the hook clears `BlockZeroExtent` (+0x14C bit 0x8) of the
  held components, runs the original `CalcWeaponFire` again with the same
  start and end, and restores the bit: the bullet goes on as if the object
  were not there. In a cinematic or with the game locked nothing changes
  (objects are already released there).
- **Diagnostics.** `firelog on` also logs `SHOT (other): <weapon> of
  <instigator> from ... to ..., impact ... on <class> [instant fire|shotgun|
  script call, not a bullet|caller 0x...][ (redirected to the held object)|
  (passed through the held object: propshield off)]`.
- **Test commands.** `enemyfire 0x<pawn> [shots] [x y z]` calls FUN_0093F510
  on the pawn's `m_currentWeapon` (+0x1594) after writing
  `m_fireStartLoc` (+0x17F8: eye height, 20 units towards the target) and
  `m_aimAtPosition` (+0x140C: the point, or the held object's centre). It is
  checked first: class `YWeapon*`, vtable +0x424 = 0x0093F510,
  `m_weaponAttachment` (+0x2C8) set, `m_pawn` (+0x2CC) = the pawn; the call
  is wrapped in SEH. The game's spread cone still applies. A summoned
  `YPawn_MarauderAssault` comes with a `YWeapon_AK47` ready to fire
  (flags +0x5F4 = 0x52452001, bit 0x8000 clear: start and end come from
  the two fields). `shieldtrace off` keeps the held objects out of
  zero-extent traces, like an object the game's trace misses, to exercise
  the redirect. It saves each held component's `BlockZeroExtent` bit once
  (`g_noTraceComps`) and `ShieldTraceTestTick` puts it back as soon as the
  object is no longer held or `shieldtrace on` arrives (review fix: before,
  the object held during the test stayed out of every zero-extent trace for
  the rest of the level). The flag is a `std::atomic<bool>` (written by
  `control::Poll`, read in the tick).
- **Test setup.** Level 1 CP04, novr, `relaunch.sh`; the summoned dummy moved
  6 m in front of the head, at Walker's height; fake left hand 35 cm forward
  and 5 cm down; `spawnprop` + `grabtest`. **`relaunch.sh` already turns god
  mode on** (`planetest.py launch`), so the brief's `ucall cheat God` after
  it turns god mode OFF: the first probe of this item killed Walker ("Sei
  morto!") with 9 AK47 hits. The scripts in `scratchpad/f3/` read
  `Controller.bGodMode` (+0x208 bit 0x2) and only toggle it when needed.
- **Measured (final dll, `scratchpad/f3/final.sh`, output
  `f3/final_out.txt`; dummy 6 m away, AK47, one shot every 0.5-4 s):**
  - Radio (5.1 kg, durability 5) in front of the face, shots at the head:
    5 of 5 shots `on KActorSpawnable` (stopped by the game), pulses amp 0.61
    for 28 ms, the object moved 0.9 cm per hit (planned kick 1.0 cm); the 5th
    shot pulsed 90 ms at full and shot it out: released, it landed 2.5 m away
    behind Walker. Screenshots `f3/F_radio/held.png`, `knocked.png`.
  - Suitcase (`suitcase_02_open`, 16.8 kg, durability 6): 6 of 6 shots
    stopped, 0.5-0.6 cm per hit (planned 0.8: the weight model lags a
    heavy object), shot out at the 6th, landed 3.5 m away
    (`f3/F_suitcase/afterfirst.png`: the open suitcase covers the view).
  - God mode off, radio in front of the face: 2 shots, health 115 -> 115
    both times; `propshield off`, 1 shot: `passed through the held object`,
    the impact on `YPawn_Walker`, health 115 -> 95.
  - Redirect (`shieldtrace off`, the radio out of the trace): 3 of 3 shots at
    the head hit Walker and were rewritten (`redirected from Walker`, impact
    on the radio's front face), health 115 -> 115 with god mode off. From
    behind (dummy 6 m behind Walker, radio still in front): shots at the
    pawn centre, chest, head and between head and radio hit Walker and none
    was rewritten.
  - Three traces of the AK47 in one call (like shotgun pellets): one counted
    hit (`hit 1 of 5`) and `2 extra pellets` in the 30 s line. With
    `m_damage` written to 400: wear 3 per hit (1 -> 4 -> 7 of 5), J 6, pulse
    full 44 ms, kick 2.9 cm; shot out at the third counted hit.
  - Level bottle (`bottle_round02` from a `StaticMeshCollectionActor`, a
    `KActorFromStatic` with `YKAFST_GlassMedium`, health 30, fragile, 2):
    the mod broke it at the 2nd hit (`PROPS: bottle_round02 broke (worn out
    as a shield)`). Correction (review, 26/10): the health printed by
    `f3/levelbottle.py` was read on the owner of the original static
    component (the `StaticMeshCollectionActor`), not on the held
    `KActorFromStatic`, so the original claim was unsupported. Re-measured
    with `f3x/levelbottle2.py`, which reads the held `KActorFromStatic`
    (PHYS_RigidBody, that mesh, nearest the hand): health +0x2F0 = 30 before
    the first shot, 30 after it (AK47 damage 50, `stopped by the game`),
    then the mod broke it at the 2nd hit and the actor was deleted. So the
    game's bullet damage from the summoned dummy (no controller,
    `InstigatedBy` None) does not hurt a held level bottle. With a real
    enemy the game may break it at the first shot (not measured); the log
    then says `shield gone under a shot`.
  - Cost: nothing per tick unless a shot arrived (an empty queue swap). The
    30 s lines during the battery: `PROPS: tick` 0.179-0.210 ms on average
    with an object held, test commands and `spawnprop` (its full rescan gives
    the 18-19 ms peaks); `hits` 0.0009-0.0012 ms.
- **Regressions** (`suite.sh f3s`, `p4t/hitall.sh f3_hit` on the final dll;
  outputs `scratchpad/f3/suite_out.txt`, `f3/hitall_out.txt`): grasp, drive
  (settle 0.025-0.100 s, sweep 1.6 cm), static grab (held 13 s, released by
  the grip), glove catch, knocked bottle (43 cm; 53 in items 1 and 2), barrel
  (0 cm, as before), drop and smash breaks, the shot (161 cm), door (60.8
  degrees) and sign (5.5) as in items 1 and 2. Hit pipeline: held swings 1
  hit each at 2.8 and 2.5 m/s, none pressed or slow, the drop on the dummy
  `Spine3` at 1.9 m/s, re-grab wrist 0.0-0.1 rad/s with no hit, the same as
  item 2. `PROPS: tick` 0.174-0.182 ms on average in the suite.
- **Review fixes, measured (26/10, final dll md5 6bbd53c4..., one session;
  scripts and outputs in `scratchpad/f3x/`):**
  - Knock-out away from the head (`f3x/ko.py`, dummy 6 m away, AK47 at the
    head, object tracked every 4 ms for 2 s): radio (5 of 5, out at the 5th)
    `knocked away at 1.28 m/s, direction (-0.61, -0.63, 0.49), 0.00 towards
    the head`; it started 39.7 units from the eyes and was never closer than
    39.5, at most 2.3 units of horizontal motion towards the head; it landed
    137 cm in front of Walker and 106 cm to his left (before: 2.5 m behind
    him). Suitcase (6 of 6): 1.04 m/s, never closer to the eyes than its
    start (34.0 units); it landed 46 cm in front of Walker and 50 cm to the
    left (before: 3.5 m behind). The only motion towards Walker was on the
    ground, a bounce at feet level 140 cm below the eyes. Screenshots
    `f3x/K_radio/knocked.png`, `f3x/K_suitcase/knocked.png`.
  - Friendly bullets (`f3x/friendly.py`): Lugo (`YWeapon_SteyrElite_Lugo`,
    through `enemyfire`, 3 m in front) fired twice at the held radio; the
    game stopped both on it (`on KActorSpawnable`), with no `PROPS: shield`
    and no HAPTIC line, and `PROPS: pawn YPawn_Lugo: squad 1 ... (squad
    member) -> not a target`. The dummy's shot right after was counted (`hit
    1 of 5`, pulse 0.61). 30 s line: `2 friendly on a held object (no
    effect)`. With `shieldtrace off`, Lugo's shot at the head passed through
    Walker (`on none`: friendly traces ignore Walker), so a friendly redirect
    cannot happen there anyway; the dummy's shot was redirected (`hit 2 of
    5`, `redirected from Walker`).
  - `shieldtrace`: `BlockZeroExtent` 0x8 before, 0x0 with `shieldtrace off`,
    0x8 again after `shieldtrace on` while still held, and 0x8 again after a
    release during the test (`PROPS: shieldtrace: radio_unit_04 blocks
    zero-extent traces again`).
  - Real enemies (`f3x/realshield.py`, `f3x/realshield2.py`, the tutorial
    fight of `scratchpad/realfight.md`, 4 runs, god mode on, `firelog on`):
    every one of about 700 `SHOT (other)` lines came from the instant-fire
    loop (`[instant fire]`). That covers `YWeapon_MicroUzi`, `YWeapon_M9A1`,
    `YWeapon_SAW_Adams` and `YWeapon_SteyrElite_Lugo`, and there were **0
    script calls**, so the hook saw no `CalcWeaponFire` call that was not a
    bullet. Real marauder bullets hit Walker (40 in one run, 3 in two
    others), but **none hit the held suitcase**. It was held in front of the
    face or the chest, the body turned to the nearest marauder (Controller
    Rotation yaw written; `synth`'s first value is the yaw and does not turn
    the hand frame), 6-8 m from him. Most enemy fire went to Adams and to
    cover, and the shots that reached Walker came from a second marauder at
    another angle (SMG from 12 m up and to the left). So the shield effects
    were not triggered by a real AI shot. The 30 s lines show `0 stopped by a
    held object, 0 friendly`.
  - Regressions on the same dll (`f3x/final_suites.sh`, output
    `f3x/final_suites_out.txt`): the grab suite matches item 4. Drive settle
    0.058-0.108 s, sweep 1.5 cm, static grab held 13 s, glove catch, knocked
    bottle 56 cm, barrel 0, drop and smash breaks, the shot 163 cm, door 60.6
    degrees, sign 5.1. `PROPS: tick` 0.175-0.184 ms. Hit pipeline: head
    1.5 m/s, swings 1 hit each at 3.3 and 2.7 m/s, none pressed or slow, drop
    `Spine3` at 2.0 m/s. The re-grab case logged `second grab: None
    (different actor)`, then a wrist spike of 83.8 rad/s that halved every
    tick and 1 hit (`Arm_Right` at 2.8 m/s). That is the same signature as
    the item-2 fix run (`p4t/f2x_hit_rg.out`, 82 rad/s, 1 hit), while the f3
    and f4 runs re-grabbed the same actor at 0.1 rad/s with no hit. It is an
    intermittent case of the re-grab test outside the shield code, and still
    open.
- **Not verified:**
  - A real AI bullet on a held object. The game's own stop is the same trace
    as for the dummy (the AI fires through FUN_0093F510, as the
    `[instant fire]` tag shows), but no real marauder shot touched the held
    object in 4 runs (see above). The same goes for shotgun pellets with a
    real shotgun (only the coalescing with 3 traces of one call), a real
    sniper (damage written into the AK47) and mounted guns.
  - A natural redirect: with the fake hand the held object never got inside
    Walker's body (object centre 40-50 units from the axis for hand offsets
    of 5-25 cm forward, 5-50 cm down; every shot hit the object first,
    `f3/redirect.py`). The redirect was proven only with `shieldtrace off`.
  - The headset (strength of the kick and of the pulses in the hand).

**Melee with held objects, the bottle on the head (gameplay plan, item 4, 26/10).**
Option `propmelee on|off` (default on, Basic menu "Swing objects at enemies",
ini line with comment, README). It needs `propcombat`, which feeds it the hit
events. Code: `props.cpp` `HeldMelee` (called first in `OnPropHit`),
`WalkerMeleeTrack` (start of `HitPipeline`), the melee brake in `Drive`
(`Held.meleeBrake*`), the shared calls `MeleeAttackable`, `CallHandleMelee`
and `CallPawnTakeDamage` (item 1's `ThrowDamage` now uses them too, with the
same parameters), `vr::pure::MeleeHit` (`pure_utils.h`, unit-tested in
`TestMeleeHit`, 14 checks).
- **Held objects and enemies in PhysX.** Held objects are pair-ignored only
  with Walker's own bodies (`IgnoreWalker`), not with enemies: the enemy's
  kinematic capsule and the bodies of his PhysicsAsset (arms, hands) stop
  them, and PhysX tells the game nothing. So the hit comes from item 0's
  geometric sweep of the held object (centre, and the far tip of long
  objects, from the mesh bounds and `LocalToWorld`; point velocity = hand
  velocity + wrist angular velocity x lever, or the weight command): events
  with `kind` `kHeld`.
- **When a hit counts** (`MeleeHit`): the hitting point moves at **2.5 m/s**
  or more both in the world (as the pipeline gives it) and relative to Walker
  (`PropHit.swing` = |point velocity - `FrameVelocity`|, the velocity he
  carries the hands with, as for throws). The smaller of the two decides the
  gate and the stagger tier; the damage and the momentum use the world speed.
  - Why (review, 26/10): Walker's stick walk is 3.7-3.9 m/s, above both the
    gate and the 3 m/s stagger threshold, so walking into an enemy with an
    object held still staggered him (and broke a bottle) with no swing. In
    the first real-enemy run (RF1, 20:53:13) the radio, still in the hand,
    brushed a downed marauder at 3.2 m/s while Walker moved: 15 damage.
  - It is NOT relative to the enemy: in the first round a staggering dummy's
    `Actor.Velocity` turned a held radio at 2.3 m/s into 7.6 m/s.
  - At most one counted hit every **0.6 s** per enemy (`g_meleeCooldown`).
- **Effect.**
  - From **3 m/s with at least 0.5 kg, or on the head**: Walker's own melee
    reaction, `YGamePawn.HandleMeleeAttack(Walker, momentum, damage,
    YDamageType_Melee)` (special move 9 `TakeHit`, then usually knocked down
    and executable with B). Item 1's guards apply:
    - only a `YGamePawn` with a `YPlayerReplicationInfo` (the DBNO crash in
      FUN_00832350);
    - `IsMeleeAttackable(Walker, 1, YMAT_None)`;
    - no knock-down within 1.5 s of the last one on that enemy
      (`g_knockCooldown`, shared with thrown objects).

    Otherwise the hit is light, at most 15.
  - Below that: **light**, `Pawn.TakeDamage(damage, Walker's PC, contact,
    momentum, YDamageType_Melee, bone and mesh, the object)`.
  - **Damage**: 5 + 0.4 x J, with J = 0.5 (kg + 2 kg of arm) v^2 (as in
    `WorldHitDamage`). At most 20 on the body (half of Walker's melee). On the
    head (`HeadBone` and `PropHit.boneNear`, as in item 1) x2, at most
    Walker's `m_meleeDamagePawns` (+0x1C48, 40). Examples:
    - bottle (1.6 kg): body at 3 m/s 11; head at 4 m/s 33, at 6 m/s 40;
    - radio (5.1 kg): body at 3 m/s 18, at 5 m/s 20;
    - a can (0.4 kg) never staggers, at most 15.

    Melee damage never kills (`m_neverKills`), like Walker's melee: B
    finishes the enemy.
  - kg = `GetBodyMass` x 40 (`kThrowKgPerUnit`, as in item 1, not `masskg`).
  - Momentum: the horizontal direction of the hit x (kg + 2) x v (the game's
    move only follows its direction, item 1).
- **No effect** (logged, counted apart):
  - no controller (the game kills such a pawn with any damage, item 1);
  - a pawn that is down without a `YPlayerReplicationInfo`;
  - the 0.6 s cooldown;
  - **Walker's own melee** (next point).
- **No double hit with Walker's melee (B).**
  - While Walker is in `PlayMeleeAttack` (special move 8) or
    `AIMeleeAttack` (7), and for 0.6 s after (`WalkerMeleeTrack`, one byte
    read per tick), or while his `m_currentMeleeTarget` (+0x1C44) is this
    enemy, a held hit does nothing except break a breakable object. Only this
    enemy: a target left set by an interrupted melee (never seen) must not
    switch the feature off for the others.
  - The left-hand punch gesture already presses no B with an object in the
    left hand (item 2). The gun-melee gesture (`gesture gunmelee`) now
    presses no B with an object in the right hand (`grabright`) either
    (`vr_input.cpp`). Since the review the left hand's grenade gesture is
    blocked the same way (the overhead "bottle on the head" chop met its
    conditions: forward over 3.2 m/s near head height), and both left
    gestures stay off for 0.5 s after a release (a throw's hand keeps going).
  - B after a held stagger is not refused: that is the game's rule, as after
    Walker's own melee (B on a staggering or downed enemy executes him, see
    `F8_bdelay`).
  - Walker's melee state is read before and after each call and written in
    the log line: `m_currentMeleeTarget` (+0x1C44), special move (+0x491) and
    `m_hasPerformedMeleeCheck` (+0x5F8 bit 0x200000). `HandleMeleeAttack` on
    the victim never changed them in any run. During Walker's own melee the
    game sets `m_currentMeleeTarget` to the victim and clears it at the end
    (read in the B-first runs).
- **Breakable objects** (`Breakable`: a `KActorFromStatic` with a template and
  health, i.e. the levels' bottles `YKAFST_GlassMedium`, glasses, pottery):
  a counted hit (2.5 m/s or more, even when the damage is refused) releases
  the object and breaks it with `BreakProp`, with the game's glass effect and
  sound (`PROPS: <mesh> broke (smashed on an enemy's head|smashed on an
  enemy)`). The hand lets go first, as for the smash in the hand.
- **Haptics.** A counted hit gives one 70 ms pulse on the holding hand
  (amplitude from `ImpactPulse`, at least 0.8 x the gain; `HAPTIC: <hand>
  melee ...`) instead of the generic hit pulse. With `impacthaptics off` it
  is the old fixed 70 ms pulse.
  - **Gun hand** (`GunHandHaptics`: the left hand on the gun, two-handing or
    reloading; review, 26/10): no melee pulse at all (`not pulsed (hand on the
    gun)` with `hapticlog`). The gun hand never changes, and the generic hit
    there was already muted.
  - **Slow first contact then the counted hit** (the `HitMemo.speed` upgrade,
    ~9 ms apart): the first gets the generic pulse; the melee pulse then
    replaces it instead of starting a second one. It keeps at least the
    first one's amplitude and ends 70 ms after the first one started
    (`HAPTIC: left hand melee (replaces the pulse of N ms ago)`; the hand's
    impact slot in `PulseHand` holds one pulse, so the felt result is one
    70 ms pulse).
- **Drive brake.** After a counted hit on an object that does not break, for
  2 ticks and at least 25 ms the drive keeps 10% of the commanded velocity
  along the hit (relative to Walker) and 30% of the spin. The object stops on
  the body instead of being driven through or around it. The weight command
  (`weightCmdV/W`, used by hits and release) is not braked.
  - Measured (radio, 4.5 m/s, `grablog`): commanded velocity (-143, -160)
    units/s before the hit, then (-14, -16), (-4, -4) and (-2, -3) in the 3
    braked ticks, then back to the hand within ~0.2 s (error peak 28.5 cm;
    with `solidhands held` the drawn hand stays on the object).
- **Changes to the shared hit pipeline (item 0), found with real enemies.**
  - **The enemy's arms.** A level bottle thrust at a real, crouched
    `YPawn_MarauderSMG` stopped 26 units from his axis (cylinder radius 15)
    against the PhysicsAsset bodies of his arms, smashed in the hand and gave
    no hit. The pipeline's candidate test only looked at the cylinder, and
    the box trace only hits the cylinder.
    - Held sweeps now also take an enemy within 20 units beyond the radius
      (`kHeldArmReach`) as a candidate.
    - When the box trace finds no target, a zero-extent trace along the same
      segment, 6 units longer, looks for his PhysicsAsset bodies. Its hit
      must not lie beyond whatever stopped the box trace.
    - `hitlog` shows `arm reach only`.
  - **Weapons on the ground.** The guns of dead dummies
    (`YPickup_WeaponSpawnable`: +0xA0 = 0x0A020104, bBlockActors off; its
    `SkeletalMeshComponent` blocks traces, +0x14C = 0xC30069) stopped the box
    trace of a radio passing over them ("not a target"), so the hit on the
    enemy behind did not count. Now `TracePassThrough` lets the traces go on
    past any actor with bBlockActors off that is not world geometry, not one
    of the gathered enemies and not a world target or blocking volume. This
    holds for thrown objects too.
  - **A slow first contact no longer uses up the swing.** With the arm reach,
    the radio first touched the dummy's hand at 1.9 m/s at the start of the
    swing, and the one-hit-per-swing memo then dropped the hit at 4.3 m/s
    into the chest in the same swing. Now a held hit on a pawn under 2.5 m/s
    does not block one at 2.5 m/s or more on the same pawn in the same swing
    (`HitMemo.speed`, one upgrade only). For held objects the memo speed is
    the melee one, min(world, swing): a bump while walking does not use up
    the swing that follows.
- **Log.**
  - Per hit: `PROPS: melee hit <class>[ (head)] with <mesh> (<hand> hand[,
    tip]): K kg at V m/s (swing W) = J J (with the arm) -> stagger|light|none, D
    damage[ (capped)], health a -> b, special move a -> b, knock flags a -> b,
    <what was called, or why not>[ (no stagger: <why>)]; Walker's melee
    state: target a -> b, special move a -> b, check a -> b[; it breaks]`.
  - Under the gate: `... V m/s (swing W m/s relative to Walker), under 2.5
    m/s: no effect[ (Walker's own movement, not a swing)]`.
  - Every 30 s: `PROPS: melee hits with held objects in 30 s: N (s staggers,
    l light, e called with no effect, r refused (within 0.6 s, during
    Walker's melee, other); b broke the object; t too slow)`.
- **Cost.** Nothing new per tick except one byte read (Walker's special move)
  and, for a held object near an enemy, at most one extra zero-extent trace
  when the box trace finds nothing. A counted hit makes at most 3
  `ProcessEvent` calls and one log line. During the final runs:
  - `PROPS: tick` 0.167-0.191 ms on average (0.17-0.23 in items 1-3; the
    18-19 ms peaks are `spawnprop`'s test-only rescan);
  - `PROPS: hits` 0.0010-0.0014 ms per tick on average, 0.007-0.023 ms in
    ticks with objects in motion, 0.83 ms at most in the tick of a stagger
    (the call plus the bone traces).
- **Measured, summoned dummy.** Setup: level 1 CP04, novr, god mode; a fresh
  `YPawn_MarauderAssault` with `SpawnDefaultController` and a spawned
  `YPlayerReplicationInfo`, 110 units in front of Walker; the held object is
  swung sideways through the dummy's chest or head with `synthleftvel`.
  Script `scratchpad/f4/melee.py <out> <mesh | level:<mesh>> <m/s> <z>
  [--exec] [--bfirst] [--off] [--nopri] [--new] [--grablog] [--hitlog]`;
  outputs and screenshots in `scratchpad/f4/`, final-dll log in
  `f4/final_dummy.txt`.

  Rows F1-F6 ran on the item-4 build before its last change (session of
  20:46; the last change only touched the `m_currentMeleeTarget` refusal);
  F7 and F8 on the item-4 final dll. The review-fix runs are listed after the
  real-enemy runs.

  | Test | Log | Result |
  |---|---|---|
  | Radio (spawned, 5.1 kg), chest, 4.5 m/s (`F1_radio`) | Spine3 (side trace) 4.3 m/s, 65 J -> stagger, 20 (capped), `HandleMeleeAttack`; Walker's state unchanged; 70 ms pulse, amp 1.00 | HP 75 -> 55, special move 0 -> 9, knocked down 1.87 s later |
  | Level bottle (`bottle_round02`, `KActorFromStatic`, `YKAFST_GlassMedium`), head height, 4 m/s, then B (`F2_bottle_head`) | Arm_Right 3.5 m/s -> stagger, 14; `bottle_round02 broke (smashed on an enemy)` | HP 75 -> 61, special move 9; not knocked down this time (the game's choice), so B gave a plain melee |
  | Same, earlier build (`r2b_bottle_head`) | head (closest body, within reach) 4.8 m/s, 43 J -> stagger, 40 (capped); `broke (smashed on an enemy's head)` | HP 75 -> 35, knocked down 1.8 s later; B: Walker 27 `Execute`, dummy 26, HP 0 2.9 s later |
  | Radio, chest, 3 m/s (`F3_light`) | 2.8 m/s, 28 J -> light, 15 (capped), `TakeDamage (light)` | HP 75 -> 60, no stagger |
  | Radio, B pressed with the swing (`F4_bfirst`) | the held hit landed 0.13 s before Walker's `PlayMeleeAttack`: stagger, 20 | HP 75 -> 55 only: Walker's melee added nothing on the staggering dummy |
  | Same, two earlier builds (`r4_bfirst`, the arm-reach build) | the held hit 0.54 s and 0.03 s into Walker's melee: `no effect: Walker's own melee in progress or just done`; Walker's target was set by the game before and after the call | HP 75 -> 35: Walker's melee only |
  | `propmelee off` (`F5_off`) | only `PROPS: hit` and the generic 44 ms pulse | HP unchanged |
  | No `YPlayerReplicationInfo` (`r8_nopri`, earlier build) | 4.3 m/s -> light, 15, `(no stagger: no YPlayerReplicationInfo ...)` | HP 75 -> 60, no stagger |
  | Brake (`r9_brake`, `--grablog`) | see "Drive brake" | |
  | B pressed 0.6 s before the swing (`F7_bdelay`, final dll after the last change) | Walker's `PlayMeleeAttack` hit first; the radio then only reached the staggering dummy at 2.1 m/s: `under 2.5 m/s: no effect` | HP 75 -> 35 (Walker's melee only) |
  | B sent 0.15 s before the swing (`F8_bdelay`) | the commands arrived in two polls: the held hit came first (stagger, 20), then B executed the staggering dummy (Walker 27, dummy 26), the game's own rule | HP 75 -> 55 -> 0 |
- **Measured, real enemies.** Setup: level 1, `ce cpTutorialFight`, then 4 s
  forward. The enemies are `YPawn_MarauderPistol` and `YPawn_MarauderSMG`
  with `YAICtrl_Humanoid` and a real `YPlayerReplicationInfo`, in a normal
  firefight; god mode on. Walker is teleported behind the enemy's cover and
  turned round, and the held object is thrust at the chest. Script
  `scratchpad/f4/realmelee.py`; write-up `scratchpad/realfight.md`.
  - These ran on the item-4 build before its last change (20:53).
  - Radio (`RF1_radio`): `hit YPawn_MarauderPistol bone Spine3
    (trace) ... at 4.5 m/s` -> stagger, 20 (capped), HP 75 -> 55, special
    move 0 -> 9, knocked down 1.81 s later. Then B: `execution allowed`,
    Walker 27, the enemy 26, HP 0 2.7 s later. During the teleport for B the
    radio, still in the hand, brushed the downed enemy at 3.2 m/s: `light, 15
    damage, health 55 -> 40 ... (no stagger: IsMeleeAttackable false (down,
    dead or in a move the melee does not interrupt))`. That brush no longer
    counts since the review fix (swing relative to Walker, see below).
  - Level bottle (`bottle_quad01`, scale 0.6, 0.7 kg)
    (`RF2_bottle`): `hit YPawn_MarauderSMG bone Hips (trace) ... at 4.3 m/s`
    -> stagger, 15, HP 75 -> 60, special move 0 -> 9 -> 56 (ragdoll fall),
    knocked down 1.84 s later, `bottle_quad01 broke (smashed on an enemy)`.
  - Radio swung sideways, earlier build (`real1`): `YPawn_MarauderSMG` Spine3
    4.7 m/s -> stagger, 20, HP 75 -> 55, special move 9 -> 54 -> 56, knocked
    down. B: `execution allowed`, but the game played `PlayMeleeAttack` and
    the enemy got up 3.7 s later (the ragdoll-fall path, item 1's "game's own
    rule").
  - So **real AI takes the damage and the reaction**. Item 1's open point (a
    `YAICtrl_Humanoid` dummy ignores all damage) does not hold for the
    level's enemies, which have their `m_aiDefinition`.
- **Regressions** (one run each; `scratchpad/f4/final_suites.sh`, output
  `f4/final_suites_out.txt`). The grab suite ran on the dll before the last
  one-line change (the `m_currentMeleeTarget` condition, which only a held hit
  during Walker's melee reaches); the hit pipeline, the throw and `F7`/`F8`
  ran on the final dll.
  - Grab suite (`suite.sh f4s`): grasp power grip, centre f -2.6 p 5.5 a 6.6
    cm, turned 46 degrees; drive settle 0.025-0.100 s, sweep 1.7 cm; glove
    1.8 m in 0.41 s, caught; bottle knocked 51.6 cm, barrel 0 cm; drop broke
    at 5.8 m/s; smash broke in the hand; shot moved the radio 164.1 cm; door
    60.8 degrees, stop sign 5.4 degrees. The static grab let the crate
    bottle go after 1.6 s as `wedged`: item 2's two suites (17:27, 19:33)
    did the same, item 3's held it 13 s, so it depends on the bottle, not on
    this item.
  - Hit pipeline (`p4t/hitall.sh f4_hit`): `hitthrow` held hit on the head at
    1.6 m/s (`melee ... under 2.5 m/s: no effect`), release 5.8 m/s;
    `heldswing` swing 1: Spine1 2.8 m/s (the dummy has no controller: no
    effect, 70 ms melee pulse); none pressed or slow; swing 4: **two hit
    lines** 9 ms apart, Spine1 2.4 then 3.0 m/s: the new slow-contact
    upgrade (the first under 2.5 m/s, generic 44 ms pulse, then the melee
    pulse); `hitdrop2` Spine3 1.9 m/s; `regrab` wrist 0.1 rad/s and no hit
    (the test's intended result; earlier runs had an Arm_Right hit from
    merged command batches).
  - Item 1 after the refactor (`f1/throwdmg2.py f4_throw radio_unit_04 5.5 15
    1 --pri`): `throw hit ... 80 J -> heavy, 40 damage, health 75 -> 35,
    special move 0 -> 9, HandleMeleeAttack`, as in item 1.
  - `PROPS: tick` 0.17-0.21 ms on average in the suites. No `SCRIPT:
    exception` and no crash in the session's logs.
  - Unit tests 317/317.
- **Review fixes (26/10, 23:10-23:46).** Scripts in `scratchpad/f4x/`
  (`walkinto.py`, `grenade.py`, `final_suites.sh`, output
  `final_suites_out.txt`); `f4/melee.py` for the swings. The final dll
  (23:26) ran the gestures, the walk (`W5`, after a fresh relaunch), the
  grab suite, the hit pipeline and the item-1 throw. `W1`, `FX1` and `FX3`
  ran on the build before it (23:17), whose melee and pipeline code is
  identical; the last build only added the release time for the gestures.

  | Test | Log | Result |
  |---|---|---|
  | Walk (stick forward, 3.9 m/s) with the radio held still into a dummy with controller and PRI (`W5`, final; `W1` the same) | `hit ... bone head (trace) ... at 3.9 m/s`, then `melee hit ... (head) ...: 3.9 m/s (swing 0.1 m/s relative to Walker), under 2.5 m/s: no effect (Walker's own movement, not a swing)`; the generic 44 ms pulse | HP 75, special move 0, not knocked down. Before the fix this was a head hit: stagger and 40 damage |
  | Real swing, Walker standing, radio 4.5 m/s into the chest (`FX1`) | `5.1 kg at 4.3 m/s (swing 4.3) = 66 J -> stagger, 20 damage (capped)`; 70 ms melee pulse | HP 75 -> 55, special move 0 -> 9, knocked down 1.9 s later |
  | B with the swing, object 59 units from the axis (`FX3_bfirst`) | Walker's `PlayMeleeAttack` started 55 ms before the hit; `melee hit ... at 7.4 m/s (swing 4.6) ... -> none, 0 damage ... no effect: Walker's own melee in progress or just done; Walker's melee state: target 7D9E2800 -> 7D9E2800, special move 8 -> 8` | HP 75 -> 35 from Walker's melee only (no double hit). The world speed includes Walker's lunge (7.4); the swing is 4.6 |
  | Same with the object 112 units away (`FX2_bfirst`) | Walker's melee missed and the radio never reached the dummy (the melee move carries the hands away) | no hit at all, inconclusive |
  | Grenade gesture (`gesturesim throw`) and punch (`gesturesim melee`) holding the radio, with the release in the same batch, and with an empty hand (`grenade.py`, final) | holding: no gesture; released in the same batch: no gesture; empty hand: `GESTURE: lancio della granata (4.00 m/s)` and `GESTURE: corpo a corpo (3.20 m/s)` | the left gestures never fire with an object or within 0.5 s of a release. A first version that read the release time inside the gesture code missed the release case (it fired 0.18 s after it): `gesturesim` is the only gesture update in novr, so the time now comes from `Release` |
  | Upgrade pulse (`heldswing`, swings 1 and 4, final) | `hit ... 44 ms` then 8-9 ms later `HAPTIC: left hand melee (replaces the pulse of 8 ms ago) ... amp 1.00, 62 ms` | one pulse of 70 ms in total, not two |

  - **Not run in the game:** the gun-hand mute (the left hand on the gun
    while it holds an object cannot be set up with the fake hands; the
    branch is one `if`, logged with `hapticlog`).
  - **Regressions** (final dll, once):
    - grab suite (`suite.sh f4xs`): power grip, centre f -2.6 p 5.5 a 6.6
      cm, turned 46 degrees; drive settle 0.066-0.109 s, sweep 1.6 cm;
      static grab held 13.5 s; glove 1.8 m in 0.41 s, caught; bottle
      knocked 55.2 cm, barrel 0 cm; drop broke at 5.8 m/s; smash broke in
      the hand; shot moved the radio 164.6 cm; door 60.8 degrees, stop sign
      5.5 degrees;
    - hit pipeline (`p4t/hitall.sh f4x_hit`): `hitthrow` held head hit 1.5
      m/s (`swing 1.5 ... under 2.5 m/s: no effect`); `heldswing` swings 1
      and 4 two lines each (2.5 then 3.0 m/s, the slow-contact upgrade),
      none pressed or slow; `hitdrop2` Spine3 2.0 m/s; `regrab`: one
      `Arm_Right (closest body)` hit at 2.8 m/s with a 70 ms melee pulse,
      wrist 83 rad/s on the first tick. Its release, 80-degree turn and
      regrab arrived in one command batch (23:40:04.358), so the turn
      happened in one tick: the known artifact of this test ("earlier runs
      had an Arm_Right hit from merged command batches"). The dummy has no
      controller, so the hit had no effect;
    - item 1 throw (`f1/throwdmg2.py f4x_throw radio_unit_04 5.5 15 1
      --pri`): `80 J -> heavy, 40 damage, health 75 -> 35, special move 0 ->
      9, HandleMeleeAttack`, as before;
    - `PROPS: tick` 0.166-0.194 ms on average; no `SCRIPT: exception`, no
      crash; unit tests 334/334 (8 new checks in `TestMeleeHit`).
  - Test note: after the suites, the fake stick (`pad fake 0 1 ...`) did not
    move Walker at all (`W2`, `W4`; `W3` 42 units). After a fresh relaunch
    it walked 382 units (`W1`, `W5`). It has nothing to do with held objects.
- **Long objects** (the plan asks which long objects can be grabbed). In the
  18-level census the grabbable long ones are:
  - chairs (`office_chair`, `terrace_chair`, `white_condo_dining_chair`, bar
    stools);
  - `shovel` (1, in StealingWater) and `tarp_stick_01_03` (Atrium02);
  - rifle cases.

  The only loose pipe, `electrical_pipe_02` (1 instance), is excluded by
  "pipe" (a structure), and the seats (`A8_seat`, vehicle seats) by "seat".
  No whitelist change: the tip sweep already covers long objects. Not tested
  in a level that has them.
- **Not verified / open.**
  - The headset: real swing speeds, the 70 ms pulse, the brake as seen in VR.
  - A tip hit of a long object: the sweep exists since item 0, but every run
    here hit with the centre.
  - The 0.6 s cooldown in the game. The pipeline already gives one hit per
    swing and one per (object, enemy) every 0.5 s, so it only matters for a
    second object or the 0.5-0.6 s window; not run.
  - Heavy troopers (melee-resistant through `IsMeleeAttackable`): none in
    level 1.
  - Whether the game knocks the enemy down or only staggers him is its own
    choice: one of the two bottle runs on the dummy stayed up.

**Explosive bottles you can carry and throw (gameplay plan, item 5, 26/10).**
Option `propexplosive on|off` (default on, Basic menu "Explosive bottles",
ini line with comment). It does not need `propcombat`. Code: `props.cpp`
`CarrierCreate`, `CarrierDamage`, `CarrierShot`, `CarrierTick`,
`CarrierReset` (block "Bombole esplosive da lanciare"), the `explosive` flag
of `Prop` (set by `ScanObject`), `MakePropDynamic(..., compOut)`; test
command `explhit <damage>` (0 = enough to explode; first carried bottle).
- **The dud fix.** The only explosives of Spec Ops are `YExplosiveActor`
  subclasses (SRAssets): level actors with `bNoDelete`, `bWorldGeometry` and
  `bMovable` off (Actor +0x9C = 0x25405108, +0xA0 = 0xCB010020 on the level-1
  bottle). The stage-2 scan listed their mesh (`gas_tank` r 21.6 units,
  `scuba_bottle`) as a static prop, but `KActorFromStatic.MakeDynamic`
  refuses an owner that is not a `StaticMeshActorBase`: highlighted, then
  "could not become physical" and the mesh blacklisted. Now the scan
  recognises the owner class (`ExplosiveClass`, cached per class, name looked
  up at the start of a pass) and keeps only the whole mesh (`m_statMeshComp`
  +0x22C, not the destroyed one, not once exploded). With `propexplosive off`
  they are left out of the grab list (measured: `grabtest gas_tank` with the
  bottle next to the hand: "nothing in range").
- **The game's script** (bytecode read from memory with a small disassembler,
  scratchpad `f5/udis.py`; properties with `f5/explprops.py`):
  - `TakeDamage`: `m_currentHealth` (+0x200, default `m_defaultHealth`
    +0x1FC = 50 for the propane bottle) -= damage; `m_firstControllerWhoHitMe`
    (+0x210) keeps the first instigator. At health <= 0 and not exploded:
    `ServerExplode` at once if `m_numHits` (+0x214) > 0, otherwise
    `SetTimer(0.15, 'ServerExplode')`. If it survives (and 0.4 s since the
    last hit): `m_numHits++`, and at the first hit `notifyAI` after
    `m_timeBeforeNotifyingAI` (1.3 s): `YAIEventPropagator.SendEvent_GrenadeImpact`
    at **Location**, grenade type 5, time to explosion = health / (hits x
    `m_healthReductionPerSecondAndHit`).
  - `Tick` burns health down by `m_healthReductionPerSecondAndHit` per
    second and hit: **0 on the propane bottle**. There is no burning bottle
    in this game (unlike HL2's ignite): a weak hit only takes health off.
  - `ServerExplode`: `m_exploded` (+0x224 bit 0x1), `Explode` (particles
    `m_explPSC` +0x234, sound `m_explSound` +0x238, ground decal from
    `CheckAboveSand` under the mesh, sand fog, the destroyed mesh
    `m_destroyedStatMeshComp` +0x230 shown, the whole one hidden), then the
    damage: `YDamageManager.AddDamageItem` -> `ExecuteInstantDamageItem` with
    `YDamageEvaluationParams` `m_damageParamsName` (`PropaneBottle_DmgParams`:
    base damage 450, range 50-250 units, x1.75 on enemy AI, knock-down 500,
    sand stun on AI 600 units, `m_damageOriginator` true). **The damage is
    centred on `m_statMeshComp.Bounds.Origin`, not on Location**, and only
    runs if `m_firstControllerWhoHitMe` has a Pawn with a current weapon.
  - No checkpoint record: `YExplosiveActor` derives from `Actor` and has no
    `CreateCheckpointRecord`/`ShouldSaveForCheckpoint` (nor do `Actor` and
    `KActor`; the classes that have them are listed in GObjects), so a
    checkpoint never stores a moved or exploded bottle.
- **The carrier (`CarrierCreate`)**, at the grab or at the glove lock (not on
  a hand knock, not on a shot at the bottle still in place: the game handles
  that one): the original component is hidden (`SetHidden`), its collision
  switched off (`SetActorCollision(false, false, false)`) and its PhysX body
  disabled (`SetBlockRigidBody(false)`: in this exe `FUN_00627520` raises
  `NX_AF_DISABLE_COLLISION` on the NxActor, vtable +0x64), then a
  `KActorSpawnable` is spawned at the component's pose with its scale
  (`SetScale3D` before `SetStaticMesh`), mesh and per-instance materials
  (`MeshComponent.Materials` +0x200). The original leaves `g_props` and
  `g_noBody` at once. Grab and glove then use the carrier's component
  (`MakePropDynamic` returns it through `compOut`). If the spawn fails the
  original is restored.
  - Measured (level 1): `gas_tank` carrier 0.341 units = 13.7 kg (masskg 40),
    centre 0.0 cm from the original's, collision flags 0xC3007D -> 0xC30038.
    Held like any prop (weight light: 29 m/s2, sag 3 cm); the glove pulls it
    5.5 m in 0.63 s and catches it. Thrown, it leaves at 3.5 m/s at most (the
    85 J energy cap at 13.7 kg): about a metre of toss.
- **Where it explodes (`CarrierSync`, `CarrierDamage`).** Every tick the
  YExpl's Location (+0x54) is written so that its mesh centre sits on the
  carrier's (`notifyAI` reads Location). Before any damage also
  `ForceUpdateComponents(false, true)` (`Actor`, iNative 0): the hidden mesh,
  its bounds (the damage origin), the particles, the sound and the destroyed
  mesh move to the carrier. Then the game's own `TakeDamage`
  (instigator: the shooter's Controller, or Walker's): a lethal one first
  hides the carrier and switches off its collision (`RecycleInternal`, the
  `KActorSpawnable` pool routine), takes it out of the hands and the glove,
  and sets `m_numHits` to at least 1 so `ServerExplode` runs at once instead of
  0.15 s later with the bottle already gone from the hand.
  - Moving the `bWorldGeometry`, non-movable actor did not crash in any run
    (8 test setups that moved the bottle next to Walker from Python with the
    same Location write and `ForceUpdateComponents`, and every damage the mod
    applied). Measured after every move:
    the mesh centre 0.0 cm from the carrier; the particle component at the
    carrier ((-47447, -21986, 641) after an explosion 5.5 m from home); the
    scorch decal under it (screenshot `f5/out/t12_real2/explosion.png`).
- **Triggers.**
  - **A bullet on the carrier**, Walker's (`OnShot`, now with the weapon:
    `YWeapon.m_damage` +0x704 and the Controller) or a hostile pawn's
    (`OnOtherShot`; Adams and Lugo do nothing, as for the shields; also a
    shot redirected from Walker onto the held bottle, see the review fixes):
    the weapon's raw damage and damage type, close to the game's rule. Measured: the M4 does 30, so the
    first shot takes the bottle from 50 to 20 ("still whole"), the second
    blows it up; a marauder's AK47 does 50 and blows it up at once.
  - **A hard landing**: a carried bottle followed in `g_falling` (dropped,
    thrown, gloved) whose velocity changes by more than 12 m/s within 50 ms,
    from above 3 m/s. The plan said 8 m/s; measured, a bottle let go from the
    raised hand (about 1.9 m) landed with an 8.4 m/s change and blew up next to
    Walker, so the threshold went to 12. Dropped from 2.5 m: 10.9 m/s, no
    explosion (`PROPS: explosive landed without exploding (largest change
    10.9 m/s, threshold 12)`); from 6 m: 16.3 m/s, explosion. The landing
    change was about 1.26 times the fall speed, so 12 m/s is a fall of about
    3 m (not run at exactly 3 m). A throw never reaches it (3.5 m/s cap): it
    is a drop from a roof onto enemies.
  - Not done: other explosions nearby (chain reaction) and a burning phase
    (the game has none).
- **Lost or changed.** `CarrierTick`: the YExpl gone (sublevel unloaded) ->
  the carrier is recycled; exploded by the game on its own -> the carrier is
  removed; the carrier lost (destroyed, recycled) -> the bottle goes back to
  its spot, visible and solid again (measured with `ucall <carrier>
  RecycleInternal`: `back at its spot (carrier lost)`, flags 0xC3007D again).
  A new Walker (`CarrierReset`; not after a cutscene with the same Walker,
  see the review fixes): live bottles back home, live carriers
  recycled; objects of an unloaded level fail `vr::script::LiveObject` (now
  exported from `script_call`).
  - **Checkpoint load**: Walker killed (`ucall pawn Suicide`) while holding the
    carrier, and once after an explosion: the game reloads the level; the
    bottle comes back whole at its own spot (-28560, -23784, 56), health 50,
    not hidden, a new object. No crash, no `SCRIPT:` exception.
  - The option only governs new grabs: a bottle already carried keeps
    exploding when shot or dropped hard after `propexplosive off`.
- **Measured.** Level 1, novr, god mode. The level's only propane bottle sits
  in `SP_Desert01_05_K` (made visible from Python at CP04, already visible at
  `cpTutorialFight`) and the test scripts move it next to Walker first
  (scratchpad `f5/common.py setup()`). Scripts in scratchpad `f5/`:
  `reach2.py gas_tank <out> tp [hold]`, `shootexpl.py <out> <shots> dummy`,
  `droptest.py <out> <cm>`, `realexpl.py <out> behind`; outputs in `f5/out/`.

  | Test | Log | Result |
  |---|---|---|
  | Grab in the zone (`reach2`) | `now carried by KActorSpawnable: mass 0.341 units ... centre 0.0 cm from the original's` | held, lifted, turned (`t4_reach`) |
  | Gravity glove from 3.1 m | `locked KActorSpawnable`, `flies 5.5 m in 0.63 s`, `caught it in flight` | caught (`t7_glove`) |
  | Two M4 shots, carrier next to the summoned dummy | `30 damage ... 50 -> 20, still whole`, then `20 -> -10, EXPLODED; moved 1.8 m from its spot, mesh centre 0.0 cm from the carrier` | dummy 75 -> 0 (`t5_shot`; final dll `tf_shot`: same, 3.1 m from home) |
  | Dummy's AK47 (`enemyfire`) on the carrier held by Walker | `GRAB: left hand released KActorSpawnable (exploded)`, `hit (enemy shot): 50 damage ... EXPLODED` | dummy 75 -> 0, Walker (god) 115 (`t11_hold`) |
  | Dropped from 6 m beside the dummy | `impact at 16.3 m/s ... EXPLODED` | dummy 75 -> 0 (`t8_drop6m`) |
  | Dropped from 2.5 m | `landed without exploding (largest change 10.9 m/s, threshold 12)` | dummy 75 (`t10_drop250`, `tf_drop250`) |
  | Real fight (`cpTutorialFight`, Walker behind the marauders' cover, carrier 1.9 m from a `YPawn_MarauderPistol`, 6.3 m from a `YPawn_MarauderSMG`, both `YAICtrl_Humanoid` with a PRI) | `30 damage ... still whole; moved 23.4 m from its spot`, 7 s later `EXPLODED` | both marauders 75 -> 0 within a second (pistol ragdoll, SMG physics 10); the next wave came in. After the first, non-lethal shot the pistol marauder 1.9 m away did not move for 3.5 s (special move 0): no flight seen (`t12_real2`) |

  Walker in god mode survived every blast at 1-2.6 m. Without it he dies:
  the first run (god toggled off by mistake) killed him at 1.1 m, and the
  game restarted from the checkpoint. The blast hurts whoever is in range,
  Walker included (`m_damageOriginator`).
- **Cost.** `PROPS: tick` 0.170-0.191 ms on average (items 1-4:
  0.17-0.23); 0.91 ms at most in the window with an explosion (the game's
  explosion runs inside the tick). Scan pass 7.3-8.0 ms over 192,949 objects
  with 05_K visible (6.9-7.0 ms over 189,163 before, same 1/45 per tick).
- **Levels with explosives** (name tables of the 771 `SP_*.upk`,
  `f5/findexpl.py`; a name means the class is referenced there, the count is
  not known): propane bottle in Desert01 (`_05_K`), Desert02 (`_A_K`),
  CityGate01 (`_01b_K`), Gorge01 (`_02b_K`), RadioMan02 (`_04_F01_K`,
  `_04_F02_K`), StealingWater01 (`_02_K`, `_04_K`, `_11_K`); scuba tank in
  Bridge (`_01_k`) and ShipsGraveyard (`_02a`, `_02b_deco`, `_03`); fire
  extinguisher in Bridge and StealingWater01; `YExpl_Crate`,
  `YExpl_PropaneTank` and the gas-bottle groups elsewhere. Only `gas_tank`
  was grabbed in the game; the others pass the scan only if their mesh is
  2-55 units and not excluded by name (the bottle groups are, by
  "destructible").
- **Regressions** (final dll, 23:00-23:10; `p4t/hitall.sh f5_hit`,
  `suite.sh f5s`; output `scratchpad/f5/hitall_out.txt`, `f5/suite_out.txt`).
  The pipeline and grab code are untouched apart from the explosive branches,
  and the results match items 1-4. Hit pipeline: `hitthrow` held hit `head
  (closest body)` at 1.5 m/s, release 5.8 m/s; `heldswing` Spine1 2.4 + 3.0
  (the item-4 slow first contact) and 2.7 m/s, none pressed or slow;
  `hitdrop2` Spine3 1.9 m/s; `regrab` Arm_Right 3.1 m/s. Grab suite: grasp
  power, centre f -2.6 p 5.5 a 6.6 cm, turned 46 degrees; drive settle
  0.059-0.108 s, sweep 1.6 cm; static grab; glove 1.8 m in 0.41 s, caught;
  bottle knocked 52.7 cm, barrel not; drop broke at 5.9 m/s; smash broke in
  the hand; shot moved the radio 165.6 cm; door 60.6 degrees, stop sign 5.5
  degrees. `PROPS: tick` 0.169-0.190 ms on average. No `SCRIPT:` exception,
  no crash dump in the session. Unit tests 327/327 (nothing new is pure
  math: the threshold is a constant).
- **Not verified / open.**
  - The AI fleeing from a hit bottle (`notifyAI`): its event fires at the
    carrier's Location, but no marauder was seen moving away (the time to
    explosion it sends is 0, since nothing burns).
  - Scuba tanks, extinguishers and the other levels: not run.
- **Review fixes (27/10).** Scripts `scratchpad/f5x/fixtest.py
  glove|keep|die|hold`, screenshots in `f5x/out/`.
  - **Enemy bullets redirected onto a held bottle now hit it.** In
    `OnOtherShot` the bottle check used only the game's hit actor, so a shot
    that hit Walker's arm or chest after crossing the held bottle's box was
    rewritten onto the carrier (shield push, wear, haptics) but never damaged
    the YExpl. That is the usual case for a bottle held in front of the chest.
    Now `queueCarrier` runs on both paths: a direct hit on the carrier, and
    the rewrite (`enemy shot redirected from Walker`). Decision for
    `propshield off`: the option only decides whether held objects stop
    bullets, not whether a bottle is hit. A direct hit on a held bottle
    damages it, and the bullet goes on through it (return 2, as before). A
    bullet that hits Walker after crossing a held bottle's box also damages
    it, and still reaches Walker (`enemy shot through the hold, propshield
    off`). Measured with the bottle really held (`reach2.py ... hold`, lifted
    to 21 units above Walker's Location, 38 from his axis), `shieldtrace off`,
    and the dummy's AK47 aimed from the muzzle through the bottle's centre at
    Walker's axis. For the test the bottle's health was set to 100, so one
    bottle took two shots:
    - propshield off: `impact ... on YPawn_Walker`, then `hit (enemy shot
      through the hold, propshield off): 50 damage (YDamageType_AK47) ...
      100 -> 50, still whole`;
    - propshield on: `on KActorSpawnable ... (redirected to the held object)`,
      then `hit (enemy shot redirected from Walker): 50 damage
      (YDamageType_AK47) ... 50 -> 0, EXPLODED`. The dummy went 75 -> 0, and
      Walker (god) stayed at 115 (`f5x/out/hold_on.png`).
    Before the fix the second shot produced no `PROPS: explosive ... hit`
    line at all.
  - **Cutscenes no longer reset the bottles.** `hooks.cpp` passes
    `PlayerPawnIfViewTarget()`, which is null whenever the view target is not
    Walker (in-engine scenes, camera handoffs, turrets). After such a pause
    `pawn != lastPawn` ran `CarrierReset`, and every thrown or placed bottle
    jumped back home. `TickImpl` now keeps the last Walker it saw: the object,
    its full FName, its WorldInfo, and the world time of its last tick.
    `CarrierReset` runs only if one of these differs, or if the world time
    went backwards (a reloaded level starts again from 0). Measured:
    1. a carrier was hit once and moved 5.3 m;
    2. `ucall pc SetViewTarget 0x<dummy>` for 27 s, then back to Walker;
    3. the log showed `same Walker back after 27.3 s without a pawn: 1
       explosive carrier(s) kept where they are`. The carrier was the same
       object, moved 0.0 cm, the original stayed hidden, health stayed 20.
    Then Walker was killed (`ucall pawn Suicide`) with that carrier alive. The
    new log line `new Walker (level or checkpoint): 1 explosive carrier(s)
    reset` appeared, and the level reload brought the bottle back home
    whole. The rest of the pawn-change reset (lists, hands, the item-6
    `WeaponReset`) is unchanged.
  - **Gravity glove: no impact explosion while the glove moves the
    bottle.** The impact check in `g_falling` skipped only held components.
    Gloved ones (Locked, Flying, Hover) were still checked, and the glove's
    stop at the palm (flight speed, then about 1 m/s hovering) could look
    like a 12 m/s impact next to Walker's hand. `Falling.gloveUntil`: while
    any glove holds the component in state Locked or above, and for 0.15 s
    after (longer than the 50 ms impact window), a carried bottle skips the
    impact check. A change that would have counted is logged once as
    `PROPS: explosive: X m/s change while the glove moved it (...), not an
    impact`. Other breakables keep the old behaviour. Measured: three uncaught
    7.4-7.5 m pulls (2.5 and 4.5 m above the palm, hand still or pulled back
    3 m/s during the flight), grip opened 0.25 s after the flick. Each gave
    `not caught, dropped` and no explosion. **The reviewer's case did not
    occur**: speeds read from the YExpl Location at about 2 ms intervals
    (20 ms windows) peaked at 10-16 m/s. The glove slowed the bottle
    smoothly toward the receding palm (`Trem` floor 0.12 s, 16 -> 2 m/s over
    0.25 s), so no 50 ms change reached 12 m/s, and the new log line never
    fired. The guard is in place but has not fired in the game.
  - **Components follow a damaged bottle (low).** `Carrier.syncC` holds the
    carrier centre at the last `ForceUpdateComponents`. `CarrierTick`
    re-syncs:
    - at health <= 0 (the 0.15 s timer is running), as soon as the carrier
      has moved 2 cm (1 unit);
    - at `m_numHits` > 0, every 20 cm (10 units). A bottle already hit goes
      off at once on the game's next lethal damage, for example another
      explosion, without passing through the mod.
    Before, the sync ran once, so a bottle hit and then carried away
    exploded where it was first hit. Measured: `explhit 30` (50 -> 20,
    hits 1), then the carrier moved 5.3 m. The YExpl's mesh centre ended
    19.8 cm from the carrier (below the 20 cm step); before the fix it would
    have stayed 5.3 m behind.
  - **Stale pointers (low).** `Carrier` keeps the full FName (`ObjId`:
    index +0x2C and number +0x30) of the YExpl and the carrier. Before, it
    kept only the index, and `KActorSpawnable_3`/`_4` share it.
    `CarrierPutBack`, shared by `CarrierRestore` and `CarrierReset`, touches
    the original only while it is still the YExpl's `m_statMeshComp`. In
    `TickImpl`, `p` (a pointer into `g_props`) is cleared after a failed
    `Grab` that shortened `g_props` (a bottle without a carrier), so the
    hand-sphere keep entry and the knock no longer pick a shifted element.
  - **Damage type (low).** A bullet on a carrier now passes the weapon's own
    `YWeapon.m_damageType` (+0x248, a class; read from the class defaults:
    `YDamageType_AK47`, `_AssaultRifle`, `_SMG`, ...; used only if it is a
    live `DamageType` subclass) instead of `YDamageType_Explosive`. The
    damage is still the weapon's raw `m_damage`: there is no range falloff
    and no AI or difficulty scaling as in the game's instant-hit path (an
    approximation). Without `m_damage` the shot does 25. Impacts and
    `explhit` stay Explosive. Measured: `50 damage (YDamageType_AK47)`. The
    hit log line now names the type.
  - **Regressions** (fix dll, 01:40-01:53; `suite.sh f5xs`, `p4t/hitall.sh
    f5x_hit`, output in `scratchpad/f5x/`). The results match item 6. Grab
    suite:
    - grasp: power, centre f -2.6 p 5.5 a 6.6 cm, turned 46 degrees;
    - drive: settle 0.058-0.100 s, sweep 1.6 cm;
    - static grab: took;
    - glove: 1.8 m in 0.41 s, caught;
    - bottle knocked 51.9 cm, barrel not;
    - drop broke at 5.7 m/s; smash broke in the hand;
    - shot moved the radio 164.3 cm;
    - door 59.5 degrees, stop sign 5.5 degrees.

    Hit pipeline:
    - `hitthrow`: head (closest body) at 1.6 m/s;
    - `heldswing`: Spine1 at 2.5 and 2.6 m/s, nothing when pressed or slow;
    - `hitdrop2`: Spine3 at 1.9 m/s;
    - `regrab`: Arm_Right at 2.8 m/s.

    `PROPS: tick` averaged 0.160-0.201 ms during the fix tests (0.201 in the
    window with the held explosion). Unit tests 334/334.
  - A checkpoint save taken while a bottle is carried or after it exploded:
    not forced (no `ForceCheckpointSave`, to leave the saves alone); by the
    class layout nothing of it is saved.
  - The headset: carrying 13.7 kg, the toss, shooting a bottle you hold.

**Enemy weapons picked up by hand (gameplay plan, item 6, 27/10).**
Option `weaponpickup on|off` (default on, Basic menu "Pick up enemy weapons",
ini line with comment). It does not need `propcombat`. Code: `props.cpp` block
"Armi dei nemici raccolte con la mano" (`WeaponGrab`, `WeaponDrive`,
`WeaponHandover`, `WeaponTakeAmmo`, `WeaponRelease`, `WeaponPose`), kind 8 of
`ClassKind` / `ScanObject` (`g_weapons`), `CompNx` (the weapon's body). Test
commands `weapontest` (the left hand takes the nearest gun on the ground within
3 m, no zone) and `weapongrip x y z pitch yaw roll` (the gun's pose in the
hand). Scripts in scratchpad `f6/` (`final.py`, `t6ak.py`, `t7.py`), outputs
in `f6/out/`.
- **The game's pickups** (bytecode read with scratchpad `f5/udis.py`, outputs
  `f6/dis_*.txt`; properties with `tools/ue3.py props YPickup_WeaponSpawnable`):
  - Guns dropped by enemies are `YPickup_WeaponSpawnable` from the game's pool
    (`YOP`; 10 hidden at the map origin in level 1). `m_pickupMesh` (+0x204) is
    a `SkeletalMeshComponent` with the weapon's PhysicsAsset: one body for the
    AK47 (`WP_AK`, bone `AR_Root`), two for the M4 (`WP_M4`). Flags +0x210:
    `m_pickupHidden` 0x2, `m_physicsDisabled` 0x4, `m_hasPhysAsset` 0x200;
    `m_weaponClass` +0x2D4, `m_ammoInMagazine` +0x2DC, `m_ammoInReserve` +0x2E0,
    `m_weapon` (archetype) +0x2F0, `m_initTemporarily` +0x2FC bit 0x1 (DBNO
    drops: no collision, destroyed after 2 s; skipped).
  - At rest the game puts it to sleep in ~0.4 s (`OnSleepRBPhysics` ->
    `DoSleepActivation`: `PHYS_None`, no PhysicsAssetInstance,
    `m_physicsDisabled`). `ConditionalReactivatePhysics` brings the body back
    in the same tick (measured), but with the PhysicsAsset's fixed (kinematic)
    bodies: `SetComponentRBFixed(false)` (`ActorComponent`, iNative 0) makes
    them dynamic, as `StartPhysic` does when the gun is tossed.
  - The pose of the gun is the root bone, not the component: the component
    keeps the actor's rotation (zero) with its origin on the root body
    (`SyncActorToRBPhysics` moves only the location), and the bones (SpaceBases
    +0x274, component space) carry the rotation. `WeaponPose` = LocalToWorld x
    SpaceBases[0]. Bone convention checked on the AK on the ground: the muzzle
    bone (34.56, 0.02, 3.22 in the reference skeleton) rotated by the root's
    quaternion gives (32.16, 13.05, -0.38) against (32.2, 13.1, -0.4) read.
    Weapon meshes: barrel along +X, sights +Z, magazine -Z, the root at the
    point that is attached to `Weapon_Right` (the pistol grip).
  - **SetRBAngularVelocity overwrites the linear velocity** of a skeletal mesh:
    `USkeletalMeshComponent::SetRBAngularVelocity` (`FUN_00e1b7f0`) also sets
    each body's linear velocity to w x (body - root body), zero for the root.
    The first drive (linear, then angular) left the gun on the ground: command
    29 m/s, measured 1-3 m/s, measured angular velocity equal to the command;
    a single `SetRBLinearVelocity` of 5 m/s lifted it 78 cm. `WeaponSetVel`
    now sets the rotation first, then the linear velocity added
    (`bAddToCurrent`): the root gets v, other bodies v + w x r (rigid motion).
  - Self-destruction: `YPickupActor.InitializeSelfDestruction` sets
    `SelfDestroyTimer` after `m_destroyTimer` (15 s) if `!m_respawnEnabled`;
    the timer destroys the pickup (`DestroyPickup` -> `DisablePickup`: back
    into the pool) unless a player sees it (within 30 m, in front of the view,
    `FastTrace`), then it retries every second. Only `InitializeFromWeaponToss`
    arms it (when `!m_bPickupNeverDisapear`, false for the AK47 and M4, true
    for the RPG, Milkor, SAW, Barrett); `CreatePickupActor` (the path
    `ServerDropWeapon` and the dummy's `Suicide` take) does not, so the tested
    pickups had no timer: the tests arm it with `ucall <pickup>
    InitializeSelfDestruction`. Which death path real enemies take is not
    verified (see below).
  - `YPickup_Weapon.DoPickup(Pawn)` is the game's own pick-up without the
    animation (the special move 31 `DoPickupMove` ends in
    `YArsenalManager.ServerDoPickup` -> `DoPickup` too): with more than one
    weapon `ServerDropWeapon(current, self)` (the gun in the hand becomes a new
    pickup at the weapon mesh and falls), then `ServerAddWeapon(m_weaponClass,
    self)` (`ServerInitWeaponAmmo` with the pickup's `m_ammoInMagazine` into
    `m_currentAmmoCount` +0x764 and `m_ammoInReserve` into `m_currentMagAmmo`
    +0x778, equipped), then `DisablePickup` (back into the pool,
    `m_pickupMesh` None). `ServerAddWeapon` returns the weapon already owned
    if the class is the same: `DoPickup` on a gun Walker has would only drop the
    gun in the hand, so that case takes the ammo instead.
  - Ammo: `YArsenalManager.CanAddAmmoFromWeaponPickup(pickup, out index, out
    secondary)` and `AddAmmoFromWeaponPickup(pickup)` (exec thunks 0x5AADB0 and
    0xDAD000 -> vtable +0x3B4 / +0x3B8; iNative 0, callable through
    ProcessEvent). The add puts what fits into the weapon, calls the pickup's
    `OnAmmoTakenByWeapon` and an emptied pickup goes back into the pool.
- **Candidates**: `ClassKind` 8 = `YPickup_WeaponSpawnable` (name looked up at
  the start of a scan pass), in the incremental scan (`ScanObject`): not the
  class default, not hidden or deleted, not `m_pickupHidden`, with
  `m_hasPhysAsset`, not `m_initTemporarily`, a mesh, within 8 m of Walker
  (4 m until 27/10 evening, when the gravity gloves started pulling guns); at
  most 32. With `weaponpickup off` none. `grabuse` (grip in the zone = A):
  the interaction scan lists every `Y*Pickup*` again, and
  `InteractionAffordance` skips a gun only for the hand that really takes it
  (`vr::props::WeaponForHand`, review fix 27/10): the gun in the hand (both
  hands), and for the left hand a `YPickup_WeaponSpawnable` (or subclass,
  `ClassIsA`) with `m_hasPhysAsset` and without `m_initTemporarily`, while
  `weaponpickup` and `grabprops` are on and the props tick ran in the last
  0.5 s (not in a cinematic, not with the game locked). So with `grabprops
  off`, for the right hand, and for guns the hand skips, the grip near a gun
  presses A again, as before item 6. Guns back in the pool
  (`WeaponPickupGone`: `m_pickupMesh` None, `m_pickupHidden`, or hidden) are
  never interactions: after a hand-over the pickup stays where `DoPickup`
  took it, on the right hand, and in the first review run it made the gun
  hand pulse every 0.3 s. Placed
  `YPickup_Weapon` (weapon racks, `PHYS_None`, no PhysicsAssetInstance) stay
  with `grabuse`. Trace pass-through (`TracePassThrough`) is unchanged: kind 8
  passes as before.
- **Grab** (`WeaponTryGrab`, left hand only): grip closed (within 0.25 s of
  the press, once per press) with the palm within `grabzone` of the gun's
  bounds; the gun wins over a physics object if it is nearer. The zone sets
  `g_propZone[1]` and holding sets `g_propHeld[1]`: no LB (grenade), no
  `grabuse` A, no two-handed grip on the gun in the right hand, no grenade or
  punch gesture, as for objects. Cyan highlight brackets around the gun in
  reach. If Walker already has a gun of that class
  (`FindWeaponInAresenal(m_weapon)`) and `CanAddAmmoFromWeaponPickup` says
  yes, the touch takes the ammo and the hand stays empty; otherwise the hand
  takes the gun: `ConditionalReactivatePhysics` (asleep) or `StartPhysic`
  (not rigid), `SetComponentRBFixed(false)`, `IsTimerActive` / `GetTimerRate`
  / `ClearTimer('SelfDestroyTimer')`, Walker's bodies ignored
  (`IgnoreWalker`; `CompNx` reads the body from the PhysicsAssetInstance for
  the held and the last released gun only), max angular velocity 60 rad/s,
  `WakeRigidBody`, a 50 ms pulse on the left hand.
  - **Every body** (review fix): `CompNxAll` / `WeaponBodies` return all of
    `PhysicsAssetInstance.Bodies` for the two weapon components (one body for
    any other object). `IgnoreWalker`, the hand spheres' `ProxyPair` (one
    entry per body) and `RaiseMaxAngVel` loop over them. Before, only
    `Bodies[0]` was covered: the second body of the M4 and of the pistols
    still hit Walker's kinematic capsule and both spheres, and kept PhysX's
    7 rad/s cap.
  - **Rifle mass** (review fix, orchestrator rule): the weapons'
    PhysicsAssets use density 1000, so in PhysX the AK47 weighs 295.9 kg, the
    M4 371.8 kg, the M9A1 131.7 kg (the MicroUzi 1.6 kg). Driven by velocity,
    a 300 kg gun rammed props and corpses like a barrel. `WeaponLighten`
    scales every body's mass and mass-space inertia by `4 kg / total`
    (`NxActor::setMass` / `getMass` slots 45 / 46, `setMassSpaceInertiaTensor`
    / `getMassSpaceInertiaTensor` slots 47 / 48 = +0xBC / +0xC0: the asserts
    "Actor::setMassSpaceInertiaTensor: Actor must be dynamic!" and
    "Actor::getMassSpaceInertiaTensorVal: ..." of NpActor.cpp sit in those
    functions, read in the running game with scratchpad `f6x_vt.py`). The
    distribution is unchanged, so the drive is too; collisions are those of
    a 4 kg rifle (`kWeaponKg`, at `masskg` kg per unit). Lighter guns stay
    as they are. The change lives as long as the bodies: the game recreates
    them from the PhysicsAsset when it puts the gun to sleep at rest or
    returns it to the pool, and a re-woken held gun is lightened again. At
    4 kg the weight limits (force 200 N, light 400) give 50-100 m/s^2, so the
    gun would not lag anyway: the drive stays the weightless one.
- **In the hand** (`WeaponDrive`, every tick): the root goes to the grip pose
  in the left hand's IK frame (the `Weapon_Left` bone: X forward, Y up, Z to
  the left, measured with the fake hand; not forward-right-up as the right
  hand). Default `weapongrip 6 4 0 0 0 -90`: with the hand's axes the AK lay on
  its side (magazine to the right) with the fist on the magazine; roll -90 and
  6 cm forward, 4 cm up put the pistol grip in the fist and the stock along
  the forearm (four poses compared, `f6/out/grip`; the M4 sits the same way,
  `f6/out/t3/lifted.png`). The gun arrives in the grip in 0.10 s plus 0.10 s
  per 180 degrees (slerp from the grab pose). Velocity = gain (0.5) x error /
  dt + hand velocity + wrist spin, at most 30 m/s relative to Walker, plus w x
  (bounds centre - root) because PhysX moves the centre of mass; angular =
  wrist + axis-angle x gain / dt (at most 40 rad/s). Bones not updated (gun out
  of view: the root from the bones more than 4 units from the actor, which
  follows the body): position from the actor, no rotation correction. The
  game putting the held gun to sleep is undone (`ConditionalReactivatePhysics`
  again). Stuck over 60 cm away for 0.3 s (after the arrival plus 0.5 s): it
  slips out. The fingers close to the fist (no shape solve for guns). With
  `solidhands held|full` the held gun is a solid-hands source like a held
  object (review fix, `SolidSource`): the drawn left hand is the hand implied
  by the gun's real pose (root pose x inverse grip), shaped and filtered as
  for objects, after the arrival and not while the bones are stale. In free
  air the gun follows within ~1 cm and the offset stays in the dead zone;
  pressed into the ground the drawn hand stays on the gun (below). The
  `weapongrip` angles: pitch about Z (positive up), yaw about Y (positive to
  the right), roll about X, in the `Weapon_Left` frame (before the review the
  first two used the right hand's axes, so they were swapped; the default has
  both at 0). The command is queued and applied by the game-thread tick (it
  arrives on the render thread). `IsHeldActor` includes the held gun (view
  traces ignore it); both hand spheres ignore its pair.
- **Release**: opening the grip throws it with the hand's 60 ms average x
  `throwboost` (at most 12 m/s) and the wrist spin (at most 20 rad/s); the
  self-destroy timer is set again (`InitializeSelfDestruction`) if it was
  cleared. Both hand spheres keep ignoring it for `relgrace` (0.3 s) or until
  it is out of the sphere (`g_relGrace[s][1]`, as in `Release`; review fix:
  before, a throw over 1 m/s made the left sphere / gun pair solid on the
  next tick with the sphere inside the gun). A throw is checked like an
  object's (`g_throwCheck`: the "thrown object flies at" lines). Pairs with
  Walker stay ignored until the game puts the gun to sleep (that destroys the
  body).
- **Hand-over** (`WeaponHandover`): the gun's root within 15 cm of the right
  hand's IK target (which is where the right-hand gun's root sits) for 0.1 s,
  after the arrival. A new class: `DoPickup(Walker)`; the hand state is
  dropped first (the call returns the pickup to the pool). Same class: the
  ammo (`WeaponTakeAmmo`, at most once a second; full: a log line, the gun
  stays in the left hand). Haptics: 40 ms on the left hand only (review fix:
  the 60 ms pulse on the right hand is gone; the gun hand never changes).
  `WeaponAlive` also requires the pickup's `m_weaponClass` to be the one
  grabbed (the pool reuses actors and meshes).
- **Measured** (level 1 CP04, novr, god mode, summoned `YPawn_MarauderAssault`
  dummies killed with `ucall <pawn> Suicide`, which drop their AK47; final dll,
  `f6/final.py`, `t6ak.py`, `t7.py`, output `f6/out/final_stdout.txt`):

  | Test | Log / measure | Result |
  |---|---|---|
  | Grab in the zone (palm on the AK on the sand, grip) | `left hand took YWeapon_AK47 (WP_AK, grip in the zone; ammo 27+9, 1 physics body, mass 295.9 kg = 7.398 units, arrives in 0.16 s); self-destroy timer cleared`, `had 15.0 s` | gun in the hand, highlight brackets before the grip (`final/A_reach.png`, `B_held.png`); no `GESTURE`/`INTERACT` line |
  | Weapontest from 1.7 m (development run) | error 15 -> 41 -> 12 -> 0.5 -> 0.2 cm in ~0.15 s, measured velocity = command | flies into the hand, then holds |
  | Hand moving up to 1.13 m/s (75 ticks) | `WEAPONLOG` error mean 0.44 cm, max 1.30 cm, rotation 0.0 degrees | follows |
  | Held behind the head 21 s (the game would destroy a gun it cannot see) | same mesh, timer count 0, still driven | kept |
  | Released behind the head (timer armed by hand, `t7.py`) | `self-destroy timer set again`, timer count 1 | gone (pool) 15.5 s after the release |
  | Hand-over to the right hand | `DoPickup done; in the hand YWeapon_M4Carbine (40+120) -> YWeapon_AK47 (27+9), pickup was YWeapon_AK47 with 27+9; weapons 2 -> 2`, `WEAPON: YWeapon_AK47`, `RECOIL: YWeapon_AK47`, `STATE ... arma=YWeapon_AK47` | AK in the right hand on the controller pose, the game's HUD shows the AK and its ammo, the M4 falls as a new pickup (40+120) and the game offers it ("hold A ... M4A1"; `final/D_swapped.png`) |
  | Firing and reload after the swap | 6 `SHOT` lines with the AK, `m_currentAmmoCount` 27 -> 21; `gesturesim reload` -> `GESTURE: ricarica`, X sent, 21+9 -> 30+0 | the physical (honest) reload works on the new gun |
  | Two-handed grip after the swap (no left controller: the left hand takes the gun's grip) | `ARMS: left hand on the weapon (0 units from the grip)` | left hand on the AK's handguard (`t6ak/after_dopickup.png`) and on the M4's after a swap back (`t6/after_dopickup.png`) |
  | Touch on an AK with the AK in the hand | `touch: ammo for YWeapon_AK47 (slot 1): 23+0 in the weapon -> 23+6 (m_currentAmmoCount + m_currentMagAmmo), pickup 23+6 -> 23+6, the pickup is empty and gone` | ammo taken, the hand stays empty, the pickup gone |
  | Zone grab of the M4 the swap dropped (Walker has AK + M9A1) | `took YWeapon_M4Carbine (WP_M4, grip in the zone; ammo 40+120, 2 physics bodies, mass 371.2 kg)` | held like the AK (`t3/lifted.png`) |
  | `weaponpickup off`, grip on an AK on the ground | no `WEAPON:` line | nothing taken |

  The masses in this table are what `GetBodyMass` returned with the
  `weapons_physmat` density 1000 (296 and 371 kg at 40 kg/unit), before the
  review's rifle cap (4 kg, see "Rifle mass" above and the review table below).
  - Wrist display: it reads the current weapon's class and ammo every tick
    (`PublishWristData`, `RedirectNewProjectiles`); after the swap the class
    is the AK (the `WEAPON:` and `STATE` lines above), not seen in a headset.
  - Cost: `PROPS: tick` 0.169-0.197 ms on average over the run (item 5:
    0.170-0.191); at most 4.0 ms in the 30 s window with the grab and the
    hand-over (`ConditionalReactivatePhysics`, the timer calls and `DoPickup`
    run once), 0.4-1.4 ms in the others. Scan pass 6.9-8.8 ms over 189,168
    objects (unchanged).
- **Regressions** (final dll, 00:55-01:10; `p4t/hitall.sh f6_hit`,
  `suite.sh f6s`; outputs `f6/hitall_out.txt`, `f6/suite_out.txt`). The
  object paths are unchanged apart from `CompNx` (a branch for the two weapon
  components only), `RaiseMaxAngVel` (now through `CompNx`, same body for
  objects) and the hand spheres' keep lists (the held gun, null otherwise).
  Hit pipeline: `hitthrow` held hit `head (closest body)` at 1.5 m/s, release
  5.8 m/s; `heldswing` Spine1 3.4 and 3.5 m/s, nothing in the pressed and slow
  phases; `hitdrop2` Spine3 1.9 m/s; `regrab` Arm_Right 2.8 m/s (item 5: 3.1).
  Grab suite: grasp power, centre f -2.6 p 5.5 a 6.6 cm, turned 46 degrees;
  drive settle 0.033-0.109 s, sweep 2.3 cm (1.5-1.7 in every earlier suite;
  `drivetest.py` run again right after: 1.7 cm, settle 0.058-0.109 s); static
  grab (released "wedged" as in items 2, 5); glove 1.8 m in 0.41 s, caught;
  bottle knocked 54.4 cm, barrel not; drop broke at 5.9 m/s; smash broke in
  the hand; shot moved the radio 179.5 cm; door 61.4 degrees, stop sign 5.4
  degrees. Unit tests 334/334 (nothing new is pure math).
- **Review fixes, measured** (27/10 06:00-06:16, final dll; scratchpad
  `f6x/fix.py`, `fix2.py`, `real.py`, `real3.py`, outputs `f6x/*_out.txt`,
  `f6x/out/`). `fix.py` ran on the build before `WeaponPickupGone` and the
  body report line (drive, release and grip code identical); the rest on the
  final dll.

  | Test | Log / measure | Result |
  |---|---|---|
  | AK grab (dummy) | `mass 4.0 kg = 0.100 units, PhysX mass 295.9 kg before the rifle cap`; `1 physics body in the hand: Walker pairs ignored 20/20, right hand sphere 1/1, left hand sphere 1/1, max angular velocity 60 rad/s on 1/1` | lightened, all pairs set |
  | M4 grab (dropped by the swap) | `2 physics bodies, mass 4.0 kg ..., PhysX mass 371.8 kg before`; `2 physics bodies in the hand: Walker pairs ignored 40/40, right hand sphere 2/2, left hand sphere 2/2, max angular velocity 60 rad/s on 2/2` | both bodies covered |
  | M4 moving, hand up to 1.13 m/s (95 ticks) | `WEAPONLOG` error mean 0.46 cm, max 1.20 cm, rotation max 0.1 degrees (AK before the fix: 0.44 / 1.30 cm) | follows |
  | M4 wrist jumps (`synthleftrot` 0 0 60, 0 70 0, back to 0) | under 3 degrees after 8, 5 and 8 ticks (60 -> 31 -> 2.4 -> 6.8 -> ...), then 0.1 degrees and 0.2-0.4 cm | no reversal, no lag |
  | Hand-over (dummy AK) | `DoPickup done; ... M4Carbine (40+120) -> AK47`; `HAPTIC out` in the next 4 s: support hand 6 lines (the 40 ms pulse), gun hand 0 | no pulse on the gun hand. In the first run, before `WeaponPickupGone`, the pooled AK pickup on the right hand pulsed the gun hand every 0.3 s |
  | M4 held pressed into the sand (grabbed on the ground, the hand left there; `solidlog`) | `WEAPONLOG err 7.0 cm 10.8 deg`; `SOLID: held object, raw 7.25 cm 10.8 deg, shaped 7.25 cm, drawn palm 7.19 cm ... 10.7 deg` | the drawn hand stays on the gun (`f6x/out/S_ground.png`) |
  | M4 thrown (`synthleftvel 500 0 250 300`, `grabsim left off 220`, `hapticlog`) | `released YWeapon_M4Carbine (grip), 5.9 m/s`; `thrown object flies at 5.67 m/s (wanted 5.85), 0.2 deg off the throw ... after 25 ms`, 5.27 m/s after 108 ms (5.78 without gravity); `left hand sphere ends the release grace 0.158 s after release, object 17.2 cm from the palm` | a real throw; no slap line, no depenetration kick |
  | `grabuse` zone: left hand (IK target) 8 cm from the M4 on the ground, no grip | `grabprops on`: no `INTERACT` line; `grabprops off`: `INTERACT: left hand at YPickup_WeaponSpawnable (8 cm)`; on again: none | A again when the hand path cannot take the gun |
  | Real marauders, `cpTutorialFight` (`real.py`, two runs, killed with Walker's gun in 2-6 shots) | a `YPawn_MarauderSMG` dropped a `YWeapon_MicroUzi` (run 1), then a `YWeapon_M9A1` and a `YWeapon_MicroUzi` (run 2): `YPickup_WeaponSpawnable`, +0x210 = 0x1B45 (`m_hasPhysAsset` set), `m_initTemporarily` 0, no timer (Actor.Timers empty, `m_destroyTimer` 15), asleep (Physics 0, no PhysicsAssetInstance) | normal drops, taken by the hand path |
  | Real drop grabs | M9A1 (Walker carries one, ammo full): `touch: YWeapon_M9A1 full (15+120), the pickup keeps 13+73`, then `took YWeapon_M9A1 (WP_M9A1_BERRETTA ... 2 physics bodies ..., PhysX mass 131.7 kg before)`; MicroUzi: `took ... 2 physics bodies, mass 1.6 kg` (under the cap, unchanged), `2 physics bodies in the hand: ... 40/40, 2/2, 2/2, 2/2` | the "ammo full" touch branch reached |
  | Real drop hand-over | `DoPickup done; in the hand YWeapon_M4Carbine (40+120) -> YWeapon_MicroUzi (28+50), pickup was YWeapon_MicroUzi with 28+50; weapons 2 -> 2`, Walker's current weapon the MicroUzi (`f6x/out/real_uzi.png`) | works on a real enemy's gun |
  | Cost | `PROPS: tick` 0.151-0.196 ms on average in the three sessions (before: 0.169-0.197) | unchanged |

  So the real death path (Walker's bullets; in run 2 a hit reaction at 15 HP
  before the kill) leaves normal `YPickup_WeaponSpawnable` drops without a
  self-destroy timer, like the dummy's `Suicide`; no DBNO
  (`m_initTemporarily`) drop was seen. The 15 s timer path
  (`InitializeFromWeaponToss`) stays tested only with the timer armed by hand.
- **Regressions after the review fixes** (final dll, 06:15-06:28;
  `f6x/final_suites.sh`, output `f6x/final_suites_out.txt`). The object
  paths changed only in `IgnoreWalker`, `ProxyPair` and `RaiseMaxAngVel`,
  which now loop over `CompNxAll` (one body for every object but the guns).
  Grab suite: grasp power, centre f -2.6 p 5.5 a 6.6 cm, turned 46 degrees;
  drive settle 0.058-0.108 s, sweep 1.6 cm; static grab, both spheres end
  their release grace; glove 1.8 m in 0.41 s, caught; bottle knocked 49.8 cm,
  barrel not; drop broke at 5.9 m/s; smash broke in the hand; shot moved the
  radio 175.9 cm; door 59.5 degrees, stop sign 5.2. Hit pipeline: `hitthrow`
  held hit `head (closest body)` at 1.6 m/s, release 5.4 m/s; `heldswing`
  Spine1 2.5 and 3.3 m/s, nothing in the pressed and slow phases; `hitdrop2`
  Spine3 1.8 m/s; `regrab` no hit line, as in item 7's final run (trace hit
  nothing, 26-32 units from the axis). `PROPS: tick` 0.170-0.201 ms (item 7:
  0.171-0.200). No script exception, no crash dump. Unit tests 2815/2815.
- **Not verified / open.**
  - DBNO: no marauder went down-but-alive in the two real kills, so what a
    downed-then-dead enemy leaves is still not seen. `m_initTemporarily`
    drops are skipped by the hand and stay with `grabuse` (A).
  - Left-handed mode (`hand left`): the grip was tuned on `Weapon_Left` (X
    forward, Y up, Z left); with the arms swapped, side 1 is Walker's right
    arm (forward-right-up) and the gun would sit rotated. Not tested.
  - In the run, `gesturesim reload` followed by `synthleft 0 0 0` left the
    drawn left hand in the air next to the AK although the log said "on the
    weapon" (`final/E_twohand_4s.png`); the same check without the reload sim
    shows the hand on the gun, so this looks like the gesture sim's fake pose,
    not the swap. Not checked on a swap-free weapon.
  - Snap turns: the held gun is not carried by `CheckFrameJump` like held
    objects; it catches up through the drive (up to 30 m/s).
  - The grip pose was tuned on screenshots of the fake hand only; it needs the
    headset, as does the feel of the hand-over distance (15 cm) and dwell
    (0.1 s).
  - Placed weapons (`YPickup_Weapon` on racks), ammo boxes and grenade belts
    stay with `grabuse` (A).

**Fake grenades: a thrown can or bottle flushes enemies (gameplay plan, item 7, 27/10).**
Option `fakegrenade on|off` (default on, Basic menu "Fake grenades", ini line
with comment). It does not need `propcombat`: with `propcombat` off the hit
pipeline does not run, and a direct hit is caught by the touch rule below.
Code: `props.cpp` block "Finta granata" (`FakeGrenadeLanded`,
`FakeEnemiesNear`, `FakeFriendNearest`, `AiPropagator`,
`FakeGrenadeOnPawnHit`), `Falling.fakeDone`, Prefetch entry
`kPfAiPropagator`. The trigger is in the `g_falling` loop of `TickImpl`,
before the carrier explosion and the break on impact. Scripts in scratchpad
`f7/` (`dummytest.py`, `realfake.py`, `probe.py`, `bots.py`, `fparms.py`,
`callers.py`), screenshots in `f7/real/`.
- **What the real grenade does** (read from memory and the exe, 27/10):
  - Script callers of `YAIEventPropagator.SendEvent_GrenadeImpact` (bytecode
    scan of all functions, `f7/callers.py`): `YGrenadeThrow.SpawnGrenade` (at
    the throw: `(None, End, 5, m_aiImpactType, -1, false)`, End = the predicted
    landing point), `YProj_Physical.Explode` (`(None, Location, 0, 0, 325,
    true)`, not for stun and sticky) and `YExplosiveActor.notifyAI` (`(first
    controller who hit it, Location, time to the blast, m_grenadeType,
    damage range, true)`).
  - The grenade's native bounce prediction (code at 0x00BA2EB5-0x00BA2FAE,
    `esi` = the `YProj_Physical`): for each predicted impact point it sends
    `SendEvent_GrenadeImpact(InstigatorController +0x224, point, m_startTime
    +0x4E8 + m_currentPredictionTime +0x4E0 - now, m_aiImpactType +0x621, -1,
    false)` and calls `m_causerPawn (+0x5C8).GrenadeImpactImminent(point, 300,
    type)` (the FName at 0x01505924 is used only there, FUN_00800690; 300 is
    the float at 0x0112CD40).
  - The mod does the same two calls at the landing.
- **The event** (`f7/fparms.py`, reflection):
  - `SendEvent_GrenadeImpact` is final native, not static, iNative 0, Parms
    0x20: Instigator **Controller** (not a Pawn, as the plan said) @0, pos
    @4, estimatedTimeToImpact @0x10, grenadeType (`YE_GrenadeType`: Frag 0,
    Stun 1, Sticky 2, Mortar 3, RPG 4, PropaneTankSmall 5 ... SCAR 9) @0x14,
    MaxRange @0x18, explosion (bool, mask 1) @0x1C.
  - `Get()` (static) returns the class default object: the singleton is
    `Default__YAIEventPropagator`. It is looked up by name in the incremental
    scan (Prefetch); if not seen yet, one GObjects pass at most every 30 s.
  - The exec thunk (0x00C22AA0) calls FUN_0099A570, which queues an event
    object (vtable 0x0133D754). Its handler FUN_00961D10:
    - MaxRange -1 becomes 325 (0x0142F0C0).
    - Without `explosion`, the event is dropped if a danger younger than 3 s
      lies within 75 units (50 x 1.5).
    - It registers a danger lasting time + 5.2 s.
    - For every Kynapse entity of the bot class (list at
      `[[0x014EC808]+0x14]+8`, entity +0x34 position in (Y, Z, X), +0x18 bot)
      within MaxRange, it writes the danger to bot +0x164C (pos, lifetime,
      time stamp, type, explosion) and posts event 0x1B.
  - The mod sends `(Walker's PC, landing point, 2 s, Frag, -1, false)`.
- **The bark.** `YGamePawn.GrenadeImpactImminent(Loc, rad, grenadeType)`
  (event, iNative 0, Parms 0x11) calls
  `PlaySpecialActionOnNearbyPawn(GetGrenadeSpecialActionSetType(type), rad,
  None, self, false, Loc, true)`. Frag, Stun, Sticky and Mortar map to
  `YSAST_IncomingGrenade` (114). The closest living pawn within `rad` of
  `Loc`, Walker excluded, runs it (`Marauder_IncomingGrenade_s2/_s3` sets
  exist). Called on Walker. `YGamePawn.m_specialActionSetTypeRep` (+0x1D04:
  set type byte, time stamp float) shows which special action a pawn last
  ran: that is how the bark is measured.
- **When.** A tracked object (`g_falling`) thrown by the hand (`kThrown`) or
  let go by the glove (`kGlove`) changes velocity by more than 2.5 m/s within
  50 ms, from above 2 m/s. That is the break detector's measure with lower
  thresholds, so a bottle that breaks on landing counts too. Not while the
  glove holds it, nor for 0.15 s after (`now >= Falling.gloveUntil`, review
  27/10: the 50 ms window still holds velocities the glove steered, and a
  sharp re-aim could read as an impact; the carrier code uses the same
  guard).
  - An enemy within 3 m: `FakeEnemiesNear` walks `WorldInfo.PawnList`, uses
    `PawnHostile` (squad, team standing), and measures the distance to the
    cylinder. The first such impact decides; without an enemy near, a later
    bounce can still count.
  - **Touch** (review 27/10): the object's bounds sphere reaches the nearest
    enemy's cylinder at the impact (distance to the cylinder < bounds
    radius). The object hit him, not the ground: no warning, counted as "a
    throw that hit an enemy". This covers `propcombat off` (no pipeline) and
    soft contacts below the pipeline's threshold.
  - Bounds sphere radius 15 units or more (30 cm radius, about 60 cm
    across): too big, no warning. Measured: `bottle_quad01` at scale 0.6 r
    8.3 (about 33 cm across), `radio_unit_04` r 17.4.
  - **Squadmates and civilians** (review 27/10). The handler never reads the
    instigator: it writes the danger into every Kynapse bot within MaxRange
    (3D distance from the point to the bot's position, which sits within
    ~20 units of the pawn's Location), squadmates included. The bark goes to
    the closest living pawn within 300 units, of any side. A real grenade
    does the same, but bottles are unlimited. So `FakeFriendNearest` finds
    the closest living non-hostile `YGamePawn` (Adams, Lugo, a civilian;
    not Walker) within 325 + 25 units, by 3D distance from its centre:
    - MaxRange becomes (his distance - 25 units) instead of -1;
    - if that does not reach the nearest enemy's centre with 25 units to
      spare, there is no warning ("... but YPawn_Adams is 2.1 m from it: the
      warning would reach him too, no warning"; 30 s counter "with a
      squadmate as close as the enemy");
    - when the warning goes out, every squadmate is at least 1 m farther
      from the point than the nearest enemy, so the bark (closest pawn) is
      an enemy's too.
    The log line says the reach: "(SendEvent_GrenadeImpact, reach 6.5 m, no
    squadmate in it)" or "reach 4.2 m, short of YPawn_Lugo 4.7 m away".
  - The group: the hostile pawns within the event's reach (325 units, or
    less as above) get a 10 s cooldown. A landing near any pawn still in
    cooldown is refused.
  - A throw that the hit pipeline counts as a hit on a pawn
    (`FakeGrenadeOnPawnHit`, `PropHit` kind flying on a pawn) is not a fake
    grenade. The 30 s counter counts it only when the pawn is hostile
    (`PawnHostile`; review: kTgtPawn also covers Adams, Lugo and civilians).
  - One log line per decision: `PROPS: fake grenade: <mesh> (r, thrown|gravity
    glove) landed at (...) (dv), <d> m from <class> (<n> enemies within 3 m):
    AI warned ... IncomingGrenade bark (asked); <k> enemies on a 10 s
    cooldown`, or `too big`, or `that group was warned X s ago`. Plus `PROPS:
    fake grenades in 30 s: ...` next to the other 30 s counters.
- **Measured on the dummy** (level 1 CP04, `f7/dummytest.py`; a summoned
  `YPawn_MarauderAssault`, no controller):
  - A bottle landing 1.4 m beside the dummy: warning sent, bark asked, and
    the dummy's `m_specialActionSetTypeRep` went from 0 to 114 at 73.50.
  - A second bottle 8.5 s later, 0.7 m from it: "that group was warned 8.5 s
    ago (cooldown 10 s), no warning".
  - The radio (r 17.4) 1.3 m away: "too big for a grenade, no warning".
  - A bottle thrown at the chest at 7.5 m/s: "PROPS: hit YPawn_MarauderAssault
    bone Arm_Right ... (thrown)", no fake grenade line; the 30 s line counts
    "1 throws that hit an enemy instead".
  - With `fakegrenade off`: no line, no bark.
  - A bottle tossed at the legs that touched the cylinder softly (2.7 m/s
    change, no pipeline hit) counted as a landing 0.1 m away: a toss at
    his feet.
- **Measured on real AI** (`cpTutorialFight`, scratchpad `realfight.md`;
  `f7/realfake.py`: Walker 5 m behind the nearest marauder, a bottle aimed
  1.3 m beside him):
  - First wave, in combat:
    - Two throws, each landing 0.7-1.2 m from a `YPawn_MarauderSMG`. On the
      second, the danger reached the 3 bots within 325 units: the SMG, Adams
      and Lugo (`bot+0x164C` = the landing point, life 7.2 s, type 0,
      explosion 0). The event goes to every bot in range, squadmates too.
    - Both times the SMG then moved straight away within 10 s: 1.1 -> 11.3 m
      from the bottle (path 10.8 m), and 1.3 -> 7.8 m (path 9.1 m; crouched
      at the end).
    - The IncomingGrenade bark was not recorded on the second throw (no 114
      on any pawn in 10 s). The first throw's first 6 s of events were lost
      (output truncated).
  - **Control** (same SMG crouched in cover, `fakegrenade off`, bottle at
    1.2 m): he did not move in 10 s (0.0 m).
  - Second wave, just spawned and idle (they stand still until the fight
    reaches them, as the first wave did in its first ~10 s):
    - Two throws. The bark ran both times: 114 on the nearest marauder 0.2 s
      after the landing.
    - The danger reached all 4 bots.
    - Nobody moved.
  - Manual probes before the code, with `ucall` (only one ucall runs per
    command batch, so these were danger-only or bark-only): 5 danger events
    (explosion false and true, times 2 and 5, MaxRange -1 and 325) 0.8-1.2 m
    from a first-wave marauder. None moved within 6-10 s. Under the same
    conditions, the danger plus the bark from the mod moved him twice.
  - **Squadmates in the 05:29:33 throw** (re-read from the `realfake.py`
    output for the review): Adams 5.9 m and Lugo 5.7 m from the bottle got
    the danger; in 10 s Adams moved 0.0 m net (path 0.8 m, stayed crouched)
    and Lugo 0.0 m. Both ran special action 121 at +0.2 s, which is
    `YSAST_SandCough` (enum `YE_SpecialActionSetTypes`, read from memory),
    not a grenade reaction. So no reaction was seen, but the squad rule
    above keeps them out anyway.
- **Review fix, measured 27/10** (final dll; scratchpad `f7x/`):
  - Dummy (`f7x/dummyfix.py`, level 1 CP04, a new dummy per case):
    - Bottle 1.4 m beside it: "AI warned as by a frag grenade
      (SendEvent_GrenadeImpact, reach 6.5 m, no squadmate in it),
      IncomingGrenade bark (asked)"; the dummy's special action 0 -> 114.
      Adams and Lugo were 39-40 m away.
    - `propcombat off`, bottle thrown at the dummy's body: "bottle_quad01
      (r 8.3 units, thrown) touched YPawn_MarauderAssault (0.1 m from its
      cylinder, 4.4 m/s change): a hit on him, not a landing, no warning";
      no bark (special action stayed 0).
    - Gravity glove: Walker 1.6 m from the dummy with his back to it, a
      bottle 5.7 m from the dummy pulled with a flick and not caught
      ("GLOVE: left not caught, dropped" at 39.58): "bottle_quad01 (r 8.3
      units, gravity glove) landed ... 1.8 m from YPawn_MarauderAssault ...
      AI warned" at 40.14, 0.56 s after the glove let go (past the 0.15 s
      guard). This is the first test of the `kGlove` source.
  - Real AI, `cpTutorialFight` (`f7x/realalt.py`; enemies given god mode so
    Adams and Lugo do not kill them during the series). Walker 5 m **in
    front of** the marauder's cover, between him and the start point (not
    flanking), a bottle aimed 1.3 m beside him, 10 s watched per throw.
    Pairs: off first, then on, on the same marauder while he is still
    crouched in cover more than 8 m from Adams and Lugo; two fights.
    | throw | target | bark 114 | target in 10 s |
    |---|---|---|---|
    | on | SMG in cover | yes | stood up and left: 12.9 m net (path 17.9 m), then `PerformMelee` (108) on the squad |
    | off | Pistol in cover | - | 0.0 m |
    | on | same Pistol | yes | stood up, 2.0 m away (1.9 -> 3.6 m from the bottle) |
    | off | SMG in cover (2nd fight) | - | 1.2 m sideways in cover, still crouched |
    | on | same SMG | no (he ran `WeaponJamming`, 181) | stood up, 1.1 m, still 0.9 m from the bottle |
    | off | Pistol in cover (2nd fight) | - | 0.0 m |
    | on | same Pistol | yes | 0.0 m (path 0.8 m), still crouched |
    - So with the warning, 2 of 4 marauders left their cover (one of them
      only 2 m), 1 stood up in place and 1 stayed. Without it, 0 of 3 left
      (a 1.2 m shuffle in cover). With the earlier flanking runs: 4 of 6
      moved with the warning, 0 of 4 without. The README's "can make them
      leave their cover" is right; it is not a sure flush.
    - Squadmates: 12-16 m from every landing in these throws, 0.0 m moved,
      no danger written to them (reach 6.5 m, no squadmate in it).
    - **Squad rule on real AI:** after the first throw the SMG fought Adams
      at the start point; the next "on" throw landed next to them:
      "bottle_quad01 landed 0.9 m from YPawn_MarauderSMG (1.5 m from his
      centre), but YPawn_Adams is 1.4 m from it: the warning would reach
      him too, no warning". No 114 on any pawn and no new danger in the
      bots.
- **Regression** (final dll, scratchpad `f7/final_suites_out.txt`). Grab
  suite `suite.sh f7s`: same results as item 6 (door 60.9 degrees, stop sign
  5.5, shot moved the radio 169 cm, drop and smash broke the bottles, glove
  caught it in flight, barrel 0.0 cm as before). Hit pipeline `p4t/hitall.sh
  f7_hit`: ht, hs (2 swing hits, 0 for press, pull and push) and hd as
  before. In rg the second grab took the same actor this time: wrist
  0.1-0.2 rad/s and 0 hit lines, the expected result. The earlier runs grabbed
  a different actor, with an 82 rad/s spike and 1 hit. The hitall throw
  logged "radio_unit_04 (r 17.4 units, over 15) landed 1.2 m from
  YPawn_MarauderAssault: too big". Unit tests 2815/2815.
  Review fix (27/10, scratchpad `f7x/final_suites_out.txt`): grab suite
  `suite.sh f7xs` as before (drive settle 0.06-0.11 s, static grab took and
  released bottle_quad01, glove caught it in flight, knock bottle 54.2 cm,
  barrel 0.0 cm, drop and smash broke the bottles, shot moved the radio 163
  cm, door 59.4 degrees, stop sign 5.4). `hitall f7x_hit`: ht and hd as
  before; hs 1 hit on the first swing, 0 on press, pull and push, and 2
  lines on the second swing (2.3 then 2.9 m/s, 8 ms apart: held hits, which
  the fake grenade code does not touch; the f4 and f4x runs had the same 2);
  rg 0 hit lines. Props tick 0.165 / 10.65, 0.191 / 18.80, 0.200 / 18.89 ms
  (same as items 6 and 7). Unit tests 2823/2823.
- **Not verified / open.**
  - How often it flushes enemies: 4 of 6 moved with the warning (2 of 4
    from the front, see the review fix above), 0 of 4 without, 0 of 5 with
    the danger alone (no bark). Small samples; whether the move comes from
    the bark, the danger or both is not separated. A headset playtest
    against several groups decides how useful it feels.
  - Behaviour trees: the grenade subtrees (`bt_subbehavior_grenade`
    `sub_ReactionToGrenade_MoveAway` / `_Enemy_Response`,
    `misc_delay_grenade_reaction`, `aiTuning_grenadeDodgeDelay_normal`) were
    only listed. `sub_ReactionToGrenade_MoveAway` has "not
    GrenadeExplodedNearby (20 s, 1000 units, explosions only)" decorators. So
    a real blast nearby may block the move-away for 20 s; the mod never sends
    `explosion = true`.
  - The bark did not show on the second first-wave throw (pawns busy with other
    special actions such as 197 `EnemyHitPlayer` and 116 `Reloading`; a
    direct `RunSpecialActionFromSetType 114` on that SMG also returned false
    once). The game's priority rules decide, as for a real grenade.
  - Walker's real grenade could not be thrown in novr for a comparison
    (`pad fake` LB and `gesturesim throw` gave no grenade in
    `cpTutorialFight`).
  - **Known risk, for the headset pass** (review 27/10): the only scene
    guards are `g_cinemaActive` and `g_gameLocked`. In scripted sections that
    are not cinematics and where Walker cannot throw grenades (weapons
    lowered, walk-only parts), a thrown object can still send the danger and
    the bark to scripted hostile bots (the idle second wave got both). The mod
    has no flag for "Walker may use weapons", and "already in combat" is not
    a usable test (item 9: marauders get Walker's position as their target
    0.2-13 s after spawning, without any fight). Not tested there.
- **Props tick** (review 27/10): the 0.17-0.20 ms average with an 18-19 ms
  peak seen in the item-7 suites was already there before item 7. Item 6's
  final hit pipeline run (same `hitall` scenario, 26/10 06:26-06:27):
  0.170 / 10.68, 0.198 / 18.91 and 0.201 / 18.87 ms (average / peak per 30
  s window), against 0.171 / 10.53, 0.193 / 19.16 and 0.200 / 18.67 with
  item 7. The fake grenade adds no per-tick work (one branch per tracked
  object; the checks run only on an impact). `AiPropagator`'s fallback (one
  GObjects pass) runs on an impact tick only if the incremental scan has not
  seen `Default__YAIEventPropagator` yet, at most once every 30 s.
  - Cost: the check runs only at an impact of a thrown object; no per-tick
    work. The props tick in the throw tests was 0.18-0.21 ms on average with
    an 18-19 ms peak in every window, including the one before the first
    landing (spawnprop and grab).

**Objects make noise for the AI (gameplay plan, item 9, 27/10).**
Option `propnoise on|off` (default **off**, as the plan says; Basic menu
"Objects make noise", ini line with comment; menu, ini and README call it
experimental and not verified since the review, see the conclusion below). Code: `props.cpp` block
"Rumore per distrarre" (`PropNoise`, `NoiseSource`, `g_noiseStats`,
`g_noiseNext`), `pure_utils.h` `PropNoiseLoudness` (unit tests
`TestPropNoise`). It reuses item 7's `AiPropagator()` (Prefetch
`kPfAiPropagator`, prefetch only since the review) and `FakeEnemiesNear`
(log only). Scripts in scratchpad
`f9/` (`dummynoise.py`, `realnoise.py`, `probe.py`, `final.sh`), results in
`f9/real/*.json`, `f9/final_out.txt`.
- **The event.** `YAIEventPropagator.SendHearEvent_Melee(Controller ctrl,
  vector pos, optional float Volume)`: final native, iNative 0, Parms 0x14
  (ctrl @0, pos @4, Volume @0x10), read by reflection 27/10. It is the
  event the game sends for Walker's own melee: the only script caller is
  `YGamePawn.PerformMeleeAttack`, as `Get().SendHearEvent_Melee(
  self.Controller, self.Location, <default>)`; the default volume is the
  float 1.0 at 0x011375D4. `Actor.MakeNoise` is an opcode native that
  ProcessEvent ignores, and its C++ goes to UE3's HearNoise, which this
  Kynapse AI does not use.
- **What the native does** (Ghidra, 27/10):
  - The exec thunk 0x00475790 calls FUN_00439150 with `this` = the
    propagator. That function reads the radius from `this+0x58`
    (`m_hearEventMeleeAttack`) at once, converts the controller to a Kynapse
    handle (FUN_007BAF20) and queues the event (FUN_0081CB10: type 0 Hear,
    subtype 2, handle, position, radius, volume). A null handle drops it.
  - The handler (vtable 0x0133D85C slot 1, FUN_00FD23E0) calls FUN_00A36440
    on every bot. FUN_00DCDB20 accepts the event when the bot is within
    `radius x entity+0x130` in plan and `radius x entity+0x134` in height
    (radius 0 = always). Measured on the marauders' Kynapse entities: 1.0 and
    0.5, so a 6 m noise is heard within 6 m around and 3 m up or down.
  - For a Hear event FUN_00E428A0 stores a hearing stimulus with the
    position passed (not Walker's), the instigator handle, the subtype, the
    volume and a time stamp, and posts it to the bot.
  - Ranges on `Default__YAIEventPropagator`: melee 300 (6 m), death 300,
    silenced shot and silenced impact 500; shot, impact and explosion 0
    (unlimited).
- **What the mod does.** In the `g_falling` loop of `TickImpl`, right after
  item 7's landing check and before the explosive and break checks:
  - An impact of an object that was thrown, dropped, knocked by the hand
    sphere or let go by the glove (not while the glove holds it), with the
    stage-4 measure: a velocity change above 3 m/s within 50 ms, from above
    3 m/s.
  - A bottle smashed in the hand (the stage-4 in-hand smash, just before
    `BreakProp`) also counts, with the speed before the stop.
  - At most one event per second (`g_noiseNext`); impacts inside that second
    are counted only.
  - One impact = one event (review fix, see "Review fixes" below): the 50 ms
    window sees one impact for several ticks, so the first detecting tick
    latches it, and the event goes out when the window closes, with the
    largest change of the window.
  - **Loudness** (`PropNoiseLoudness`): sqrt(E / 40 J) with E = 1/2 kg dv^2,
    clamped to 0.5-2. Glass that breaks in this impact (KActorFromStatic
    template class `YKAFST_Glass*`) is x1.5, up to 2.5. The event radius is
    300 x loudness (3-15 m). The mod writes it to the singleton's `+0x58`
    for the call only, then writes the old value back. All of this runs on
    the game thread, and the queued event carries its own copy of the radius.
    The volume stays 1.0, as in the game: no reader of it was found in the AI.
  - The instigator is Walker's controller (`Pawn+0x214`).
  - Off in cinematics and while the game is locked.
- **Log lines.**
  - Per event: `PROPS: noise: <mesh> (thrown|dropped|gravity glove|knocked|
    smashed in the hand) at (x, y, z), <dv> m/s change, <kg> kg[, glass
    breaks] -> loudness L, heard within R m (300 x L): SendHearEvent_Melee;
    <n> enemies within it, nearest enemy <class> <d> m away`.
  - Every 30 s: `PROPS: object noises in 30 s: N hear events (G breaking
    glass, F failed), K impacts within 1 s of the last`.
- **Measured on the dummy** (level 1 CP04, `f9/dummynoise.py`, fakegrenade
  off). This run used the implement round's build from BEFORE the
  glass-template fix (07:23 log lines, props.cpp last changed 07:25); the
  fix only touches the glass branch, which spawned bottles never reach. The
  review-fix rerun is under "Review fixes" below.
  - Bottle (`bottle_quad01` at scale 0.6, 0.73 kg) thrown at 6.4 m/s, landing
    2 m from the dummy: `PROPS: noise: bottle_quad01 (thrown) ... 10.9 m/s
    change, 0.73 kg -> loudness 1.04, heard within 6.2 m (300 x 1.04):
    SendHearEvent_Melee; 1 enemy within it, nearest enemy
    YPawn_MarauderAssault 1.7 m away`.
  - Radio (5.14 kg) at 5.8 m/s: 7.7 m/s change -> loudness 1.96, 11.8 m.
  - Bottle thrown hard at the ground (8.3 m/s, flight 0.44 s): 8.6 m/s
    change -> 0.82, 4.9 m. The spawned bottle is a KActorSpawnable: it does
    not break.
  - `propnoise off`: no line.
  - One event per throw. The 30 s line of the intermediate run counted "9
    impacts within 1 s of the last" for 2 throws; with no per-impact latch at
    the time, most of those 9 were the same impact seen again on the
    following ticks of its 50 ms window (~7 ticks at 144 Hz), not bounces.
  - Level bottles (KActorFromStatic, breakable), `propnoise on`:
    - `bottle_round02` dropped from height: `PROPS: noise: bottle_round02
      (dropped) ... 6.4 m/s change, 1.65 kg, breaks -> loudness 0.91, heard
      within 5.5 m`, then `broke (impact at 6.4 m/s)`.
    - `bottle_quad01` smashed in the hand: `(smashed in the hand) ... 6.4
      m/s change, 1.68 kg, breaks -> loudness 0.93, 5.6 m`.
    - That run found a bug: the break was not seen as glass (", breaks", 0
      breaking glass). The template at KActorFromStatic +0x2F4 is a class,
      so `ClassName` gave "Class"; the code now uses its `ObjectName`. Rerun
      on the implement round's last build (after the fix):
      - dropped: `PROPS: noise: bottle_round02 (dropped) ... 6.3 m/s change,
        1.65 kg, glass breaks -> loudness 1.35, heard within 8.1 m`;
      - smashed in the hand: `(smashed in the hand) ... 6.2 m/s change, 1.68
        kg, glass breaks -> loudness 1.35, heard within 8.1 m`;
      - `object noises in 30 s: 1 hear events (1 breaking glass, 0
        failed)` (scratchpad `f9/glass_out.txt`).
  - Before the loudness was calibrated, the reference was 10 J: every throw
    reached the cap (10.9-14.3 m/s changes, because the bounce reverses the
    velocity). It is now 40 J.
- **Measured on real AI** (`cpTutorialFight`, scratchpad `realfight.md`;
  `f9/realnoise.py` and `f9/probe.py`; enemies kept alive with
  Controller +0x208 bit 0x2). What the marauders target was read from
  `YAICtrl_Humanoid.m_attackTargetPosition` (+0x504, flag
  `m_hasAttackTargetPosition` +0x4F0 bit 0x100), together with the pawn
  yaw and position.
  - **Mod path, on** (`f9/real/on_064932.json`): Walker 7.4 m behind a
    `YPawn_MarauderSMG`, and a bottle landed 3 m beside him: `PROPS: noise:
    bottle_quad01 (thrown) ... 14.3 m/s change, 0.73 kg -> loudness 1.37,
    heard within 8.2 m (300 x 1.37): SendHearEvent_Melee; 2 enemies within
    it`.
    - Walker had been teleported 2.3-7.4 m from the two marauders just
      before the throw, so his closeness and the noise cannot be told apart.
    - The target's attack target was 17 m from Walker (the squad) 1.77 s
      before the impact, and Walker's exact position (0.0 m) at the first
      sample after it (+0.25 s): it switched somewhere between -1.8 s and
      +0.25 s, possibly before the noise. (The implement report said "about
      1 s after the impact"; that was wrong.)
    - He turned to face Walker (7-15 degrees) at +5.5 s, never the bottle.
    - The second marauder, 2.3 m from Walker, moved his attack target onto
      Walker 2.25-3.95 s after the event.
    - Both enemies inside the 8.2 m radius ended on Walker's exact spot.
      That is the pattern a backfire would give, and also the pattern of the
      controls without a noise (below): this run cannot tell them apart.
  - **Controls without the event.** They show the marauders find Walker's
    exact position on their own:
    - `propnoise off`, Walker 12 m behind: target on Walker at +4.4 s,
      facing him at +5.6 s.
    - Two `probe.py` runs where the event call failed (`ucall` by name does
      not find `Default__YAIEventPropagator`, so no event was sent): in the
      first, the two marauders' attack target was on Walker about 1.7 s and
      4.9 s after they appeared, 16 m away; in the second, one was already on
      Walker 8 s after they appeared and the other about 13 s after (14-16 m).
    - `probe_event_071343.json` (event sent by address at 0): a marauder
      appeared with no attack target and held Walker's exact position 0.2 s
      later, 16 m away, 3.8 s before the event. The event changed nothing:
      target still on Walker, facing him (7-8 degrees) 6 s later.
  - **Event with Walker out of the fight** (`probe.py`, `BACK=1500`,
    `f9/real/probe_event_070955.json`): Walker was moved 30 m back, 48 m from
    the marauders. 8 s later both already targeted Walker's exact position
    (0.0 m), 4 s before the event. Then the same `SendHearEvent_Melee` was
    sent by `ucall` 2 m beside one of them, on the far side from Walker:
    - his attack target stayed on Walker;
    - he turned from 1 to 71 degrees off Walker (42 from the point) in
      0.6 s, but the other marauder made the same kind of turns before the
      event.
  - **Conclusion (rewritten after the review).** In a firefight already
    under way there is no measurable difference with the noise: the
    marauders target Walker's exact location 0.2-13 s after they appear
    (mostly within 5 s), at up to 48 m and with Walker outside the fight,
    with or without a noise, and no attack target ever moved to the impact
    point. That does NOT show that the noise cannot give Walker away:
    - the stimulus carries Walker as its instigator (FUN_00E428A0 stores the
      instigator handle, Walker's controller), and FUN_007DA400 accepts the
      event only for bots hostile to that instigator, so "heard Walker" is a
      plausible way for the AI to get his real position;
    - the only mod-path run with marauders not yet on Walker (on_064932)
      cannot separate the noise from Walker's teleport next to them;
    - enemies not yet in combat were not tested at all.
    So: backfire not excluded, distraction not shown, unaware enemies
    untested. The menu, ini and README call the option experimental and not
    verified, say that it may draw enemies to the player, and no longer say
    that it "only matters for enemies not yet fighting".
  - The dummy's "1 enemy within it" in the log line is only a cylinder
    distance count by the mod: the summoned dummy has no controller and no
    Kynapse bot, so it cannot hear anything. Whether bots accept the event
    is measured by the noise probe (below).
- **Not verified / open.**
  - Enemies that are not yet in combat (patrols, idle guards) were not found
    in level 1. The first wave of `cpTutorialFight` locks on to Walker as it
    appears; the second wave was not reached (the `WAVE2` run of `probe.py`
    caught a late first-wave marauder instead). Whether enemies not yet in
    combat turn to or walk to the noise is untested. A
    headset playtest in a level with idle enemies decides it; until then the
    option stays off.
  - No direct read of the bot's hearing memory (Kynapse) was found. Since
    the review, the `noiseprobe` hook shows which bots store the stimulus
    (2 of 2 marauders in the radius, see "Review fixes"); what the bot then
    does with it is still read only through behaviour.
  - The volume parameter's use in the AI was not found (passed as 1.0).
  - Cost: the check runs only at an impact of a followed object, with no
    per-tick work. PROPS tick on the implement round's build: 0.17-0.18 ms on average in
    the noise tests (0.5-1.1 ms at most), 0.17-0.20 ms in the hit pipeline
    with the same 11-19 ms peaks as item 7 (spawnprop and grab).
- **Regression, implement round** (scratchpad `f9/final_out.txt`; the grab
  suite and the dummy run used the build from before the glass-template
  fix, the hit pipeline the build after it):
  - Grab suite `suite.sh f9s`: same as item 7 (door 60.0 degrees, stop
    sign 5.4, shot moved the radio 162 cm, drop and smash broke the bottles,
    glove caught it in flight, barrel 0.0 cm, bottle knocked 52 cm).
  - Hit pipeline `p4t/hitall.sh f9_hit`: ht as in item 7 (held contact on
    the head at 1.5 m/s, no effect; the throw blocked by the dummy, HP 75),
    hs 2 swing hits and 0 for press, pull and push, hd 1 hit, rg 0 hit
    lines.
  - Unit tests 2823/2823 (`TestPropNoise`: 8 new checks).
  - The suites ran with `propnoise off` (the shipped default), so
    `PropNoise` returns at its first line there. The template-name fix came
    after the grab suite and before the hit pipeline; the glass check above
    is on the implement round's last build.
- **Review fixes (27/10, fix round; scratchpad `f9x/final.sh`,
  `f9x/final_out.txt`).**
  - **One impact, one event.** The 50 ms window saw one impact again on
    each tick for ~50 ms (~7 ticks at 144 Hz), and only the 1 s limiter
    stopped repeated events. Now `Falling.noiseLatch` (set on every entry of
    the same component, cleared when the window no longer sees the impact)
    takes each impact once. The event is deferred: `noiseDv` keeps the
    largest change of the window, and the event goes out when the window
    closes, after 0.1 s at most (`kNoiseHold`), or at once when the impact
    breaks the object or makes a carried explosive blow up in the same tick
    (with `breaks` true, so glass gets its x1.5). A catch by the hand or the
    glove within that time drops the pending noise. The 30 s counter now
    counts impacts (bounces), not ticks.
  - **No full GObjects walk.** `PropNoise` takes the singleton only from
    the prefetch (`Prefetched(kPfAiPropagator)`); while the incremental scan
    has not seen it yet (just after a level load) the log says `no
    YAIEventPropagator yet` and no event is sent. `AiPropagator()` (with
    its 30 s fallback walk) is left to the fake grenade, which calls it only
    with an enemy within 3 m.
  - **Noise probe (test only).** Console command `noiseprobe on|off` (no
    menu, no ini). On the first `on` the tick installs a MinHook detour on
    FUN_00E428A0, the function that fills a bot's hearing stimulus. Its
    only caller in the event handler, FUN_00A36440, calls it for a Hear
    event only after FUN_007DA400 (hostile to the instigator) and
    FUN_00DCDB20 (inside radius x hearing factor) have both accepted the
    event for that bot.
    - Disassembled 27/10: `__thiscall(stimulus, subtype, instigator handle,
      const float* pos, float volume)`, `ret 0x10`. The prologue `56 8B F1
      E8` is checked before hooking. The position is the event's, swizzled
      by FUN_004B61B0 to (y, z, -x) with no scale.
    - From the return address 0x00A367A3 the caller's `ebp` is the bot
      handler (FUN_00A36440 uses it as `this`), and `[ebp+4]` the Kynapse
      entity, whose position (+0x34..+0x3C, same swizzle) names WHICH bot
      heard.
    - `PropNoise` arms the probe with the impact point. 1.5 s later the tick
      writes `PROPS: noise probe: N bot(s) stored a hear stimulus for the
      noise at (...) in 1.5 s, instigator handle 0x...; M other hear
      stimuli: bot at (...) d m from the noise, pawn <class> ...`. A
      stimulus counts as ours when its subtype is 2 (melee) and its position
      is within 2 units of the impact.
  - **Dummy rerun on the fix build** (level 1 CP04, `noiseprobe on`):
    - Bottle thrown at 6.4 m/s: `PROPS: noise: bottle_quad01 (thrown) ...
      10.8 m/s change, 0.73 kg -> loudness 1.03, heard within 6.2 m`.
    - Radio: 9.4 m/s change, loudness 2.00, 12.0 m.
    - Hard throw: 8.5 m/s change, 0.81, 4.9 m.
    - `propnoise off`: no line.
    - One event per throw. The 30 s line read `3 hear events (0 breaking
      glass, 0 failed), 2 impacts within 1 s of the last`: these are bounces
      now, where the implement round counted 9 for 2 throws.
    - Probe: `0 bot(s) stored a hear stimulus ...; 0 other hear stimuli`
      for every event. That is the expected negative control: the summoned
      dummy has no Kynapse bot, and Adams and Lugo are not hostile to
      Walker.
    - Level bottles: dropped `bottle_round02`: `PROPS: noise: bottle_round02
      (dropped) ... 6.4 m/s change, 1.65 kg, glass breaks -> loudness 1.37,
      heard within 8.2 m`, logged 1 ms before `broke (impact at 6.4 m/s)`.
      The deferred event is sent in the break tick with the glass factor.
      Smashed in the hand: `bottle_quad01 (smashed in the hand) ... 7.9 m/s
      change, 1.68 kg, glass breaks -> loudness 1.72, 10.3 m`. Both 30 s
      lines: `1 hear events (1 breaking glass, 0 failed), 0 impacts within
      1 s of the last`.
    - PROPS tick 0.178-0.203 ms on average in those windows.
  - **Real AI with the probe** (`cpTutorialFight`, scratchpad
    `f9x/realnoise.py`, a copy of `f9/realnoise.py` whose wait skips pawns
    without a controller; `f9x/real_out.txt`, `f9x/real/on_102305.json`).
    The first try in the same session as the dummy test failed at once: the
    moved-away dummies (no controller) counted as "enemies", so the script
    found no marauder with a controller. Rerun after a fresh relaunch:
    - Walker 3.7 m behind a `YPawn_MarauderSMG`, bottle thrown to land 3 m
      beside him. It clipped the arm of the second marauder
      (`PROPS: hit YPawn_MarauderPistol bone Arm_Left ... at 6.7 m/s`) and
      landed: `PROPS: noise: bottle_quad01 (thrown) at (-35426, -23736,
      102), 6.5 m/s change, 0.73 kg -> loudness 0.62, heard within 3.7 m
      ... 2 enemies within it`.
    - **Probe: `2 bot(s) stored a hear stimulus for the noise at (-35426,
      -23736, 101) in 1.5 s, instigator handle 0x00010000; 0 other hear
      stimuli: bot at (-35500, -23788, 83) 1.8 m from the noise, pawn
      YPawn_MarauderSMG 0x5C392800 (95 units); bot at (-35404, -23733, 66)
      0.4 m from the noise, pawn YPawn_MarauderPistol 0x02CCC800 (0
      units)`.** So the event does reach the AI: both marauders inside the
      radius passed the hostility test and the radius test, and each stored
      a hearing stimulus with the impact point and Walker's controller as
      instigator. The bots' hearing factors read 1.0 / 0.5, as before.
    - Behaviour: both had their attack target on Walker's exact position
      (0.0 m) already before the throw (Walker had just been teleported
      3.6-3.7 m behind them) and kept it for the 9 s; neither attack target
      ever went to the bottle. The target turned from 63 to 7 degrees off
      the bottle within 1.6 s, but Walker stood in nearly the same direction
      from him (2 degrees off), so the turn does not say which one he turned
      to.
    - What stays open: received is now proven, effect is not. Whether a
      hearing stimulus with Walker as instigator makes an unaware enemy go
      to the point or to Walker needs enemies that are not yet on Walker,
      which `cpTutorialFight` does not offer (they lock on within 0.2-13 s
      of appearing). Not tried again in this round (time box): it stays a
      headset or other-level check, and the option stays experimental and
      off.
- **Regression, fix round** (scratchpad `f9x/final_out.txt`, the fix-round
  dll; all suites with `propnoise off`, the default):
  - Grab suite `suite.sh f9xs`: door 61.4 degrees, stop sign 5.3, shot
    moved the radio 163 cm, drop and smash broke the bottles, glove caught
    it in flight, barrel 0.0 cm, bottle knocked 44.7 cm (52 in item 9's
    run: the knock distance varies run to run), drive settle 0.058-0.108 s.
  - Hit pipeline `p4t/hitall.sh f9x_hit`: ht held contact on the head at
    1.5 m/s, no effect; hs 1 hit line on the first swing and 2 on the
    second, 0 for press, pull and push; hd 1 hit; rg 0 hit lines (traces
    hit nothing).
  - PROPS tick 0.16-0.20 ms on average, peaks 10-19 ms in the hit pipeline
    windows (spawnprop and grab), as before.
  - Unit tests 2852/2852 (the count includes item 10's tests).

**Disarming enemies: hit the gun and he drops it (gameplay plan, item 10, 27/10).**
Option `disarm on|off` (default on, Basic menu "Disarm enemies", ini line with
comment, README). It needs `propcombat`: the checks run inside the hit
pipeline. Code: `props.cpp` block "Disarmo" (`EnemyWeaponBox`, `HitsWeapon`,
`WeaponSweep`, `DisarmHands`, `Disarm`, `DisarmTick`, `DisarmFindPickup`),
the pickup list `g_wpnPool` filled by `ScanObject` (kind 8), and
`pure_utils.h` `DisarmHit` and `SegmentHitsObb` (unit tests `TestDisarm`).
Scripts in scratchpad `f10/` (`disarmtest.py`, `dummyseries.sh`,
`realdisarm.py`, `probe.py`, `probe2.py`, `final.sh`); logs and screenshots
in `f10/out/`.
- **What gets hit.** The enemy's current weapon, `YGamePawn.m_currentWeapon`
  (+0x1594, read by reflection), and its mesh `YWeapon.m_weaponMeshComp`
  (+0x2E0). The mesh is a `SkeletalMeshComponent` attached to the hand; its
  `LocalToWorld` follows the animation. It does not block traces (+0x14C =
  0xC30020: no CollideActors), so the test is geometric, like the pipeline's
  cylinder test.
  - **The box.** Its orientation is the component's `LocalToWorld`. Its size
    comes from the bounds of the `SkeletalMesh` (+0x20C -> `Bounds` +0x3C),
    which in this game are twice the mesh. The AK47 reads X extent 45.5 (91
    units = 1.8 m) for a 0.9 m gun. Halved, the box runs from -10.9 to 34.6,
    and item 6 measured the AK's muzzle bone at 34.56. Halved boxes, compared
    with the world bounds of the component (+0xEC, from the PhysicsAsset
    bodies): AK47 15.0 / 20.1 / 11.5 against 14.1 / 18.7 / 8.3. Halved,
    Lugo's Steyr, Adams' SAW and Walker's M4 come out at plausible lengths
    (1.04, 0.91 and 1.18 m).
  - **The test.** First the world bounds of the component (a cheap
    rejection), then `vr::pure::SegmentHitsObb`: the motion segment in the
    box's space against the box grown by a margin.
- **Who hits.**
  - Every sweep of the pipeline: held objects (centre and tip), and objects
    in flight with origin `kThrown` (`WeaponSweep`, after `SweepHits`).
    Margin: the sweep's box extent + 2 units. Only enemies whose axis is
    within 90 units of the segment are checked.
  - **The free hand** (`DisarmHands`): the palm (IK target + `g_palmLocal`)
    from the last tick to now, grown by the hand sphere's radius + 1.5 units
    (5 units).
    - Only a hand with no object, no glove pull and no enemy gun
      (`g_propHeld`), and never the gun hand (`GunHandHaptics`: the right
      hand without an object; the left hand on the gun, two-handing or
      reloading).
    - Not during a tracking jump (`HandJumped`).
    - A gesture starts at 1.5 m/s relative to Walker and ends under 1 m/s,
      as for held swings.
  - Enemies: those of `GatherPawns` (hostile, alive, within 15 m).
  - Each weapon is hit once per throw or gesture: `g_hitMemo`, with the
    weapon as the target (the hand's state stands in for the component).
- **How hard** (`DisarmHit`, momentum in kg m/s):
  - held object: (kg + 2 kg of arm) x the smaller of the world speed and the
    speed relative to Walker (as item 4), from 2.5 m/s;
  - empty hand: 2 kg x the same, from 3 m/s;
  - thrown object: kg x the speed relative to the enemy, from 3 m/s.

  From 4 kg m/s he **staggers**; from 8 the **gun drops**. Examples: a hand at
  3 m/s 6 (stagger), at 4 m/s 8 (drop); a bottle held at 2.5 m/s 9 (drop); a
  thrown bottle at 5 m/s 8 (drop), at 3 m/s 4.8 (stagger); a thrown can (0.4
  kg) at 8 m/s 3.2 (nothing).
- **The effect** (`Disarm`, run after all objects are tested and before the
  pipeline's `PropHit` events):
  1. **Refused** during Walker's own melee (as item 4), or when
     `IsMeleeAttackable(Walker, 1, YMAT_None)` is false (down, dead, or in a
     move the melee does not interrupt).
  2. **Stagger**: `HandleMeleeAttack(Walker, momentum, 5, YDamageType_Melee)`,
     Walker's melee reaction. It interrupts firing and reloading and covers
     the change of state. Item 4's guards apply: a controller, a
     `YPlayerReplicationInfo` (the DBNO crash in FUN_00832350), and at most
     one knock-down every 1.5 s (`g_knockCooldown`). It also sets item 4's
     0.6 s melee cooldown.
  3. **Drop** (momentum 8 or more): `YArsenalManager.ServerDropWeapon(weapon,
     None)` on the enemy's arsenal (+0x1590). It is the game's own death path
     (`YGamePawn.OnTossWeapon` -> `ServerTossBestWeapon` ->
     `ServerDropWeapon`; bytecode read with `f5/udis.py`, `f10/dis_*.txt`):
     - `CreatePickupActor`: a `YPickup_WeaponSpawnable` from the game's pool,
       physical, with ammo. `m_initTemporarily` is set only with
       `m_performDeathDBNOSpawning`, which was clear on live enemies.
     - Then `RemoveWeapon`: `UnequipWeapon`, `SetWeaponActive(None)` (the
       pawn and the controller are left with no weapon), `Cleanup` and
       `Destroy`.

     The drop is skipped for a turret's weapon (`m_turret` +0x2D0), for
     `m_pickupDroppingDisabled` (+0x268 bit 0x1000000), and for a weapon with
     no `GetPickupClass` (it would vanish with no pickup).
  4. **The fallen gun** is found in the same tick (`DisarmFindPickup`): the
     pickup with that class within 1.5 m of where the gun was. It is pushed
     along the hit (2.5 m/s horizontal, 1.5 m/s up, 8 rad/s of spin; this
     tick and the next, `WeaponSetVel` of item 6). The scan keeps every
     `YPickup_WeaponSpawnable` within 30 m, at the map origin or without a
     mesh in `g_wpnPool` (at most 64).
     - Level 1's 10 pool pickups lie at the origin with +0x210 = 0x1145, so
       neither `m_pickupHidden` nor `m_hasPhysAsset` is set.
       `SetPickupSkelMesh` turns that into 0x1B45.
     - A pickup returned to the pool after a pick-up keeps 0x1B45 but loses
       `m_pickupMesh`. The first version filtered on the hidden flag and
       missed both.
  5. **Haptics**: a 60 ms pulse on the hand that hit (held or empty), at
     least 0.7 x the gain (`HAPTIC: left hand disarm`). Never on the gun hand,
     and none for a thrown object.
  6. **One hit per throw or gesture.** A counted weapon hit is that
     throw's or gesture's hit on that enemy: `g_hitMemo` gets (object,
     enemy), and same-tick `PropHit` events for that pair are dropped. A
     radio thrown at the dummy's AK touched his body 41 ms later. Item 1's
     light `TakeDamage` then cut the stagger short (special move 9 -> 0
     within 0.1 s, health 70 -> 62).
     - A held breakable (a bottle) breaks on the gun, as on the body (item
       4): `Release` then `BreakProp`, `smashed on an enemy's weapon`.
- **Never `YAIController.RemoveWeapon`** (the Kynapse brain's own removal,
  vtable +0x480 = 0x00C19D60). In the first probe it was called by `ucall`
  on a real marauder with the gun still in his arsenal. 8 ms later the game
  crashed: `0xC0000005 at 00DE7834`, in FUN_00de7820, which walks a list of
  entries at +0x70 of the brain weapon object, with one entry invalid
  (minidump `specopsvr_crash_313866015.dmp`, 27/10 08:29).
  - Called after `ServerDropWeapon` instead, it did not crash in 10 s.
  - Without it (what the mod does), 30 s of fight did not crash, and
    neither did `WorldInfo.ForceGarbageCollection(true)`, which frees the
    destroyed weapon.
  - `console obj garbage` is not a command in this build ("Command not
    recognized").
- **What the AI does unarmed** (probes on real marauders, `cpTutorialFight`,
  god mode on every pawn).
  - It stays where it is, in cover, and stops shooting: 0 `SHOT` lines from
    the disarmed `YPawn_MarauderPistol` in 30 s. It did not charge Walker
    when he stood 3 m away. The marauders have no unarmed behaviour.
  - Hence the **pick-up again** (`DisarmTick`). After 5 s, once he is up
    again (special move 0, not down), his gun lies within 3 m, and Walker
    does not hold it (`IsHeldActor`) or did not take it (the pickup is
    "gone"), `YPickup_Weapon.DoPickup(pawn)` runs. It is the game's own
    pick-up (`ServerAddWeapon`, `EquipWeapon`, `AddWeaponToAI`), with no
    animation. The probe then saw the marauder move again and fire 32 shots
    in the next 20 s; `AddWeaponToAI` hands the new weapon to the brain.
  - Otherwise he stays unarmed. After 30 s the mod stops watching (one log
    line).
- **Logs.**
  - Per hit: `PROPS: disarm hit <pawn>'s <weapon> with <mesh | the left
    hand> (<held|thrown|empty hand>, K kg): V m/s (swing W relative to
    Walker | against the enemy) = P kg m/s -> drop|stagger, <what was
    called>, ServerDropWeapon: weapon X -> None, pickup <name>, knocked
    along the hit; health a -> b, special move a -> b, knock flags a -> b`.
    Under the threshold: `-> too slow|too soft, no effect`.
  - Afterwards: `PROPS: disarmed <pawn> T s ago: picked his weapon up again
    (DoPickup) | stays unarmed (<why>) | dead or gone (weapon D m from him)`.
  - Every 30 s: `PROPS: hits on enemy weapons in 30 s: N (d dropped, s
    staggered, t too soft, r refused, e with no effect); m picked their
    weapon up again, u stayed unarmed`.
  - `hitlog N` adds a `HITLOG: disarm <hand> hand palm (...) at V m/s` line
    per tick of a fast free hand, with the distance to each nearby weapon
    box and whether the step swept through it.
- **Cost.** Per sweep (held or flying objects only) and per tick of a free
  hand over 3 m/s: per enemy within 90 units, about 100 bytes read and the
  box test. A hit adds at most 4 `ProcessEvent` calls (`IsMeleeAttackable`,
  `GetPickupClass`, `HandleMeleeAttack`, `ServerDropWeapon`) and the log
  line. The re-arm check is one pass over at most 16 records per tick.
- **Measured, summoned dummy** (final dll, level 1 CP04, novr, god mode; a
  fresh `YPawn_MarauderAssault` with its AK47 110 units in front of Walker,
  given `SpawnDefaultController` and a spawned `YPlayerReplicationInfo`
  unless noted). The left hand (or the held object) starts 40 units to the
  side of the weapon box and crosses it sideways with `synthleftvel`.
  Script `f10/disarmtest.py`, series `f10/dummyseries.sh`, output
  `f10/out/dummyseries_out.txt`, screenshots `f10/out/d_*.png`.

  | Test | Log | Result |
  |---|---|---|
  | Hand, 5 m/s (`d_hand`) | `4.5 m/s (swing 4.5) = 8.9 kg m/s -> drop, HandleMeleeAttack (stagger), ServerDropWeapon: weapon YWeapon_AK47 -> None, pickup YPickup_WeaponSpawnable, knocked along the hit`; `HAPTIC: left hand disarm` 60 ms | HP 75 -> 70, special move 9, knocked down 1.7 s later; the AK on the sand to the side (`d_hand_after.png`) |
  | Hand, 3.4 m/s commanded (`d_soft`) | `3.0 m/s = 6.0 kg m/s -> stagger, HandleMeleeAttack (stagger)` | HP 75 -> 70, special move 9, the AK stays in his hands |
  | Hand, 2.4 m/s (`d_slow`) | no line (the hand check needs 3 m/s) | nothing |
  | Hand, 5 m/s, no controller (`d_noctrl`) | `-> drop, no stagger (no controller: the game would kill it with any damage), ServerDropWeapon ...` | HP 75, the AK on the ground |
  | Hand, 5 m/s, controller, no PRI (`d_nopri`) | `-> drop, no stagger (no YPlayerReplicationInfo: ...)` | HP 75, the AK on the ground |
  | Hand, 5 m/s, `disarm off` (`d_off`) | no line | nothing |
  | Spawned bottle held, 3 m/s (`d_bottle`) | `bottle_round02 (held, 1.6 kg): 2.7 m/s = 9.9 kg m/s -> drop, HandleMeleeAttack ...` | HP 75 -> 70, knocked down; a spawned bottle is a `KActorSpawnable` and does not break |
  | Level bottle held, 3 m/s (`d_levelbottle`) | `-> drop ...`; `GRAB: left hand released KActorFromStatic (smashed on an enemy's weapon)`; `PROPS: bottle_round02 broke (smashed on an enemy's weapon)` | HP 75 -> 70, special move 9, knocked down |
  | Radio thrown at 5 m/s, released 50 units before the gun (`d_throw`) | `radio_unit_04 (thrown, 5.1 kg): 5.8 m/s against the enemy = 29.5 kg m/s -> drop, HandleMeleeAttack ...`; no `PROPS: hit` for the body | HP 75 -> 70, special move 9 -> 0 after 0.7 s (no other call in the log). Before the one-hit-per-throw rule, the same throw's body hit 41 ms later gave item 1's light `TakeDamage` (70 -> 62) and the stagger ended within 0.1 s |

  The generic `AIController` of the dummy walks away after a stagger (item
  1's note): 30 s later it stood 40-43 m from its gun, so the pick-up again
  was not testable on the dummy (`stays unarmed (his weapon is out of reach)`).
- **Measured, real enemies** (final dll; `ce cpTutorialFight`, walk 4 s;
  `YPawn_MarauderSMG` and `YPawn_MarauderPistol` with `YAICtrl_Humanoid` and a
  real PRI; god mode on Walker and on every other pawn, so their health does
  not move). Script `f10/realdisarm.py`: Walker is turned to face back along
  the enemy's barrel (Controller yaw) and put 45 units from the gun. The palm
  is then sent 30 units to the side of the gun and swept across it at 5 m/s,
  aimed at where the gun is at that moment. The marauders move all the time,
  so the sweep is retried (at most 6 tries) until a `PROPS: disarm hit`
  line. Output `f10/out/final_out.txt`.

  | Enemy | Log | Afterwards |
  |---|---|---|
  | `YPawn_MarauderSMG` (MicroUzi), hit on the first try | `4.9 m/s = 9.7 kg m/s -> drop, HandleMeleeAttack (stagger), ServerDropWeapon: weapon YWeapon_MicroUzi -> None, pickup YPickup_WeaponSpawnable, knocked along the hit; special move 0 -> 9` | 54 `KnockDownPawn`, 56 recover from the ragdoll, knock flags 0x50000 for 7.8 s; up after 9.1 s; `picked his weapon up again (DoPickup) (weapon 2.2 m from him)` at 9.8 s; then 51 `SHOT` lines from his MicroUzi in the window |
  | `YPawn_MarauderPistol` (M9A1), second try | `4.9 m/s = 9.8 kg m/s -> drop, ...` | the same sequence; up after 9.2 s, `picked his weapon up again (weapon 2.9 m from him)` at 10.0 s, firing again 4 s later |
  | Both | `WorldInfo.ForceGarbageCollection(true)` afterwards | no crash; `PROPS: hits on enemy weapons in 30 s: 1 (1 dropped ...); 1 picked their weapon up again` in each window |

  Earlier development runs on the same fight (`r5`, before the
  `g_wpnPool` fix): the same two hits. The SMG picked his gun up again
  (0.8 m). The Pistol's pickup was not found: it had been the SMG's
  recycled pool pickup, which had lost its mesh. He stayed unarmed and did
  not fire for the rest of the window.
- **Regressions** (final dll, once; `f10/final.sh`, output
  `f10/out/final_out.txt`):
  - Grab suite `suite.sh f10s`: grasp power, centre f -2.6 p 5.5 a 6.6 cm,
    turned 46 degrees; drive settle 0.058-0.108 s, sweep 1.6 cm; static grab
    released "wedged" (as in items 2, 5, 6); glove 1.8 m in 0.41 s, caught;
    bottle knocked 44.7 cm (items 6-9: 49.8-55), barrel 0 cm; drop broke at
    6.1 m/s; smash broke in the hand; shot moved the radio 123 cm (162-180
    before; this test measures from just before the last of its shots);
    door 59.5 degrees, stop sign 5.2. The knock and shot paths are not
    touched by this item (the disarm code only reads the hands).
  - Hit pipeline `p4t/hitall.sh f10_hit`: as in item 9. ht: the held contact
    on the head at 1.6 m/s, no effect. hs: 2 swing hits (Spine1 2.7 and 3.4
    m/s), 0 for press, pull and push. hd: 1 hit (Spine3 1.9 m/s). rg: 0 hit
    lines. No disarm line: these swings pass the dummy's AK at under 2 m/s
    or miss it.
  - Unit tests 2852/2852 (`TestDisarm`: 29 checks, impulse tiers and the
    box test).
  - `PROPS: tick` 0.171-0.206 ms on average in all final sessions (items
    7-9: 0.17-0.20); the 11-20 ms peaks are `spawnprop`'s test-only rescan.
    No `SCRIPT: exception`, no crash.
- **Not verified / open.**
  - The headset: real slap speeds, whether 3 / 4 m/s for the empty hand
    feel right, the 60 ms pulse.
  - **The pick-up again is instant.** `DoPickup` has no animation, so the
    gun jumps from the ground (up to 3 m away) into his hands. The probes
    measured 2.2 and 2.9 m. The game's own pick-up move (special move 31
    `YSPM_PickUpWeapon`) exists, but how to drive it for the AI was not
    found.
  - **The knock-down is the game's own choice.** Every real-enemy stagger
    ended in a knock-down of about 8 s, and B executes him then (review
    fixes below, case a).
  - Heavy troopers, turret gunners and Lugo's or Adams' guns (never targets)
    were not met.
  - The brain's weapon list keeps an entry for the dropped weapon, because
    `YAIController.RemoveWeapon` is not called. The review run below covers
    death after a disarm and after a re-arm, each after a forced garbage
    collection, and 150 s of fight. Sessions longer than that with many
    disarms are still untested.
  - **Scripted enemies.** Nothing tells a Kismet-driven hostile that is not
    in a cinematic (a hostage-taker-type scene) from a normal one, and a drop
    could stall a sequence that expects his gun. Not met in the tests
    (story scenes are avoided by design); a headset check.
  - Left-handed mode: the hand check uses side 1 (the left hand) like the
    rest of the props code; not run with `hand left`.

**Disarming enemies, review fixes (27/10).** Snapshot `snap_f10fix_disarm`;
scripts in scratchpad `f10x/` (`final.sh`, `disarmtest2.py` with `--dir` and
`--gesture`, `realdeath.py`); output `f10x/out/final_out.txt` and
`f10x/out/b0_out.txt`; screenshots `f10x/out/*.png`.
- **Death of a disarmed or re-armed real enemy (high).**
  - **What the brain list holds.** `FUN_00de7820`, the function that
    crashed, looks an entry up in the brain weapon object's list
    (+0x70, count +0x74) by a byte id (entry +4, compared with the YWeapon's
    byte +0x21D). Its caller `FUN_00e4b810` sits next to `RemoveWeapon` in
    the YAIController native table and writes two ints into the entry (+8,
    +0xC): this looks like `ReportWeaponAmmoState(weapon, current, max)`,
    which `GetWeaponsStatus` calls with `m_currentWeapon`. `RemoveWeapon`
    (0x00C19D60) only queues a Kynapse command.
  - So the entries are Kynapse objects keyed by a weapon id. They do not
    look like pointers to the YWeapon, so garbage collection should not
    leave them dangling. The 08:29 crash fits the queued removal freeing an
    entry while the arsenal still reported ammo for that weapon.
  - **Measured** (`realdeath.py`, cpTutorialFight, real `YAICtrl_Humanoid`
    marauders with real PRIs; god mode on Walker and the squad, taken OFF
    the target before the kill; every kill after
    `WorldInfo.ForceGarbageCollection(true)`):

    | Case | Log | Death |
    |---|---|---|
    | (a) disarm, GC, B during the knock-down (`YPawn_MarauderPistol`, M9A1) | `9.9 kg m/s -> drop, ... ServerDropWeapon: weapon YWeapon_M9A1 -> None`; GC done | B: special move 56 -> 26, health 75 -> 0 after 3.0 s, controller None after 3.3 s; `PROPS: disarmed YPawn_MarauderPistol 7.6 s ago: dead or gone` |
    | (c) disarm, re-arm, GC, shot (`YPawn_MarauderSMG`, MicroUzi) | `10.0 kg m/s -> drop`; `picked his weapon up again (DoPickup) (weapon 2.9 m from him)` 7.8 s later | the squad shot him within 2 s of losing god mode: health -5, controller None |
    | (b) disarm, GC, Walker's M4 (second `YPawn_MarauderSMG`) | `9.8 kg m/s -> drop`; he re-armed 9.8 s after the hit, just before the first burst | 75 -> 45 -> -42 in two bursts, controller None |
    | (b0) disarm, GC, Walker's M4 at once (`YPawn_MarauderPistol`, M9A1; `f10x/out/b0_out2.txt`) | `9.7 kg m/s -> drop`; down and unarmed at the first burst | the first two bursts missed (script aim); he re-armed 7.8 s after the hit (`weapon 1.8 m from him`), then 75 -> 45 -> -60, controller None |
    | Then 150 s of fight (and 60 s in the b0 session), every marauder mortal, GC every 60 s | no `CRASH`, no `SCRIPT: exception`; 2 of 6 marauders died in the 60 s | game alive; `PROPS: tick` 0.170-0.190 ms |

    A death by gunfire while still unarmed was not reached: twice the enemy
    re-armed (7.8 and 9.8 s after the hit) before the M4 killed him. A death
    while unarmed is covered by the B execution (a). In that case the
    controller and brain go away with the dropped weapon's entry still
    listed, after a garbage collection.
    A first b0 try (`b0_out.txt`) lost its sweep to a bad palm reading: the
    script waited 223 s and the fight ended. The script now skips a sweep
    longer than 200 units.

  - `YAIController.RemoveWeapon` is still not called: nothing misbehaved.
- **Gestures yield to the disarm (medium).** The forward slap at a gun held
  at chest height is the punch gesture (B), and a high slap at a raised
  rifle is the grenade gesture (LB). Both fired before the palm reached the
  gun.
  - `DisarmHands` now publishes `vr::control::g_disarmNearMs[h]`: from
    1.5 m/s relative to Walker, the free palm within 12 units (24 cm) of a
    weapon box, now or along its velocity over the next 0.15 s. It holds
    0.3 s after the last reading, and one log line marks the start (`PROPS:
    disarm: the left hand is going for <pawn>'s <weapon> ...: punch and
    grenade gestures wait`).
  - In `vr_input.cpp` the left punch and grenade gestures then send no
    button. They take the same waits as a sent gesture (punch 0.8 s,
    throw 0.5 s), so nothing fires at the end of the slap: `GESTURE: corpo a
    corpo (3.20 m/s) not sent: the left hand is going for an enemy's weapon
    (disarm)`.
  - Measured on the dummy (`disarmtest2.py hand 5 --dir=fwd --gesture=melee
    | throw`: the palm runs forward along Walker's view into the AK, with
    `gesturesim` in the same command batch). The near line comes 25 ms after
    `GESTURESIM`, the disarm (`4.4 m/s = 8.8 kg m/s -> drop`, 75 -> 70,
    special move 9, knocked down 1.7 s later) 67 ms later, then `GESTURE:
    corpo a corpo (3.20 m/s) not sent` and `GESTURE: lancio della granata
    (4.00 m/s) not sent`. No B, no `PAD`.
  - Control (`f10x/b0.sh`, the left hand away from the gun, no enemy weapon
    near): `GESTURE: corpo a corpo (3.20 m/s)`, `buttons 0x2000 sent to the
    game`; `GESTURE: lancio della granata (4.00 m/s)`, `buttons 0x0100`. (The
    first control, right after a relaunch, sent nothing: the synthetic left
    hand still rested on the gun, so `leftFree` was false. That is not
    related to this change.)
- **One hit per swing or throw when the body comes first (medium).**
  `WeaponSweep` now skips the weapon when `g_hitMemo` already has (object,
  that enemy) with the same epoch from an earlier tick. For a held object
  this needs a melee-speed body hit (≥ 2.5 m/s): a slow touch does not use up
  the hit, as in `SweepHits`. The weapon still wins in the same tick. Log
  `PROPS: disarm skipped: <mesh> (held|thrown) hit <pawn>'s <weapon>, but
  this swing|throw already hit his body N ms ago`; counted in the 30 s line
  (`N skipped (the body was hit first in that swing or throw)`).
  - `Disarm` also treats an enemy in item 4's 0.6 s melee wait
    (`g_meleeCooldown`) like the 1.5 s knock-down wait: no second stagger.
    The gun can still drop.
  - Dummy case `disarmtest2.py held:bottle_round02 3 --dir=down`: the bottle
    chops down past the dummy's head toward the AK. Result: `PROPS: hit ...
    bone Arm_Right ... 3.8 m/s` and the melee stagger (75 -> 59), then
    nothing. Item 4's melee brake stops the held object on the body, so it
    never reached the gun, and the body-then-gun order was not reproduced.
    The check is verified by reading the code only. The braking also makes
    the held case rarer than the review assumed; the thrown bounce case
    stays a code path without a run.
- **Low fixes (code, no extra run).**
  - A thrown object's counted weapon hit now also marks the throw for item 7
    (`FakeGrenadeOnPawnHit` with the weapon's enemy; `enemyHit` count). A
    bottle that passes through the gun and lands next to him no longer
    raises a fake grenade.
  - `ClearHits` and `WeaponReset` reset the free-hand state
    (`DisarmHandsReset`), and the hand segment counts only if the palm of
    the previous tick is under 0.1 s old. After a pause, a scene or a level
    load, the first segment is no longer metres long.
  - The drop needs the weapon to be the arsenal's current weapon
    (`YArsenalManager.m_currentWeapon` +0x6C4, reflection). In every pawn
    read on 27/10 (AK47, Steyr, SAW, M4) it equals the pawn's
    `m_currentWeapon` and `m_currentActiveWeapon` (+0x6C8). A secondary-mode
    object gives `the weapon stays: not the arsenal's current weapon
    (secondary mode?)`.
  - `EnemyWeaponBox` checks `ClassIsA(YGamePawn)` before it reads +0x1594.
- **Regressions** (final dll `9bf4e719`, once, `f10x/final.sh`):
  - Grab suite `suite.sh f10xs`: grasp power, centre f -2.6 p 5.5 a 6.6 cm,
    turned 46 degrees; drive settle 0.049-0.108 s, sweep 1.6 cm; static grab
    released on the grip (as in items 8 and 9x); glove 1.8 m in 0.41 s,
    caught; bottle knocked 53.6 cm; drop broke at 5.6 m/s; smash broke in
    the hand; shot moved the radio 163 cm; door 60.7 degrees, stop sign 5.4.
  - Hit pipeline `p4t/hitall.sh f10x_hit`: ht, the held contact on the head
    at 1.6 m/s, no effect; hs, swing 1 one hit (Spine1 2.6 m/s), swing 4 two
    lines (2.5 then 3.0 m/s 9 ms later: the existing below-melee-speed
    upgrade, as in item 9x), 0 for press, pull and push; hd, 1 hit (Spine3
    1.8 m/s); rg, 0 hit lines.
  - Unit tests 2868/2868. Build: 0 warnings, 0 errors.
  - `PROPS: tick` 0.170-0.200 ms on average in all sessions (the 11-20 ms
    peaks are `spawnprop`'s test-only rescan). No `SCRIPT: exception`, no
    crash.

**Throwing enemy grenades back (gameplay plan, item 8, 27/10).**
Option `grenadeback on|off` (default on, Basic menu "Throw back grenades", ini
line with comment). It needs `grabprops` (the hands loop), not `propcombat`.
The red corners also need `grabhighlight`. Code: `props.cpp` block "Granate
nemiche rilanciate" (`GrenadeScan`, `GrenadeTick`, `GrenadeNearest`,
`GrenadeGrab`, `GrenadeDrive`, `GrenadeRelease`, `GrenadeGoneInHand`,
`GrenadeBackReport`), `vr::props::IsThrownBackGrenade` (props.h), pure helpers
`GrenadeHoldVel`, `GrenadeThrowVel`, `GrenadeOnGround`, `GrenadePredTime`,
`GrenadeFuseGrabbable` (pure_utils.h, unit tests `TestGrenadeBack`). Test
command `synthleftlost <ms>` (the fake left hand loses tracking for <ms>). hooks.cpp: highlight states 3/4
drawn red, and the projectile redirects (`RedirectThrownGrenade`,
`RedirectNewProjectiles`) skip a thrown-back grenade. Scripts in scratchpad
`f8/` (`gprobe.py`, `gprobe2.py`, `ownertest.py`, `dummytest.py`,
`realtest.py`, `glib.py`), outputs and screenshots in `f8/out/`.
- **What an AI grenade is** (read from memory, the bytecode with `f5/udis.py`,
  and Ghidra, 27/10):
  - `YProj_Physical` subclasses: `YProj_FragGrenadeAI` (fuse
    `m_lifeTimeThrow` 4.0 s; Walker's own `YProj_FragGrenade` has 2.3 s),
    `YProj_StunGrenadeAI` (1.1 s), `YProj_PhosphorGrenadeAI` (0.7 s),
    `YProj_StickyGrenadeAI` (3.0 s), `YProj_PercussionGrenadeAI`
    (`m_explodeOnContact`, never rests on the ground), and Adams' and Lugo's
    `YProj_FragGrenadePlayerSquadAI` (1.7 s). The AI throw speed
    `m_startSpeedThrow` is 1000 units/s (20 m/s).
  - They are not PhysX bodies: `PHYS_Falling` with the projectile's own
    bounces (`HitWall` -> `Bounce`, `m_numBounces` +0x518).
  - **They follow `Actor.Velocity`, not a precomputed curve**
    (`f8/gprobe2.py`). A frag in flight with Velocity rewritten to
    (0, 0, 600) went straight up and fell back under gravity.
    `m_predictedLocation` / `DoPrediction` (vtable +0x3E8 -> 0x00BA2900) only
    feed the AI warnings: they predict the bounces up to the fuse and, at each
    predicted impact, send `SendEvent_GrenadeImpact` and
    `m_causerPawn.GrenadeImpactImminent`.
  - A frag at rest on the ground, with `bPerformGravity` (+0x3E0 bit 0x1)
    cleared and Velocity = (point - Location) / 0.03 written every 30 ms,
    rose 60 units and held there until it exploded.
  - At rest, Velocity keeps growing with gravity (-3000 units/s after 3 s)
    while Location does not move. "On the ground" is therefore read from
    position changes (below 4 m/s), after a bounce or with `m_isSliding`
    (0x100).
  - **The fuse** is the `OnExplode` timer that `TimeExplosion` sets
    (`Actor.Timers` +0xB0, 0x1C-byte entries: FuncName +4, Rate +0xC,
    Count +0x10). It keeps running in the hand: fuse left = Rate - Count.
  - **Who threw it:** `YGrenadeThrow.SpawnGrenade` (static, no script
    callers) -> `YWeapon.StaticSetGrenadeLocAndDir`, then `InitGrenade`
    (Instigator = the thrower, `bBlockedByInstigator` off,
    `m_causer` +0x5B4 = his Controller, `m_causerPawn` +0x5C8,
    `m_causerPawnPRI` +0x5CC), then `InitTracerExact`. Owner and
    `InstigatorController` (+0x224) stay None. Spawned at the tail of
    `GWorld.PersistentLevel.Actors` (UWorld +0x50 -> ULevel +0x3C: index 195
    of 196 in level 1).
  - **Damage:** `YProjectile.Explode` -> `YDamageManager.AddDamageItem` ->
    native `ExecuteInstantDamageItem`. Its dealer from
    `Owner.Instigator.Controller` is None for AI grenades.
    `YDamageManager.ApplyDamage` uses `m_causer` when the damage causer is a
    `YProj_Physical`.
- **Kill attribution, measured** (`f8/ownertest.py`). AI grenades were left
  next to fresh dummies and changed before the blast:

  | Change | Dummy | `m_lastDamageinfo.m_hitByEnemy` | Grenade `m_killCount` |
  |---|---|---|---|
  | none | 75 -> 0 | 0 | 0 |
  | Owner = Walker | 75 -> 0 | 0 | 0 |
  | `m_causer` = Walker's PC | 75 -> 0 | 1 | 1 |

  - `m_hitByEnemy` lives in `YGamePawn.m_lastDamageinfo` (+0x1EA0,
    `YS_LastDamageInfo`: +0xC `m_hitByEnemy`, +0x28 `m_damageType`). It is
    set when the hit comes from the victim's enemy side.
  - `m_killCount` (+0x4EC) is counted by `YProj_Physical.ScoreKill` only when
    `m_causer` is a `YPlayerController`. It drives the "three kills with one
    grenade" achievement.
  - So `m_causer` (`InitCauser`) is what makes a kill Walker's.
    `Pawn.LastHitBy` is not usable: it stayed None in every case, and Walker's
    `PlayerReplicationInfo.Kills` and `m_lastEnemyKills` stay 0 in single
    player.
- **Detection** (`GrenadeScan`, every tick):
  - The actors added at the tail of the persistent level's list since the
    last tick, plus a full pass over that list every 0.5 s, or when it
    shrinks after GC. Level 1 holds about 200 actors there. No GObjects
    passes.
  - A grenade class is `IsA YProj_Physical`, cached per class. Milkor, RPG
    and Mortar are excluded, and so is anything with `m_explodeOnContact`,
    exploded, stuck to a pawn, hidden or deleted.
  - Walker's own grenades (Instigator = Walker) are skipped.
  - **Only an enemy's grenades** (review fix, 27/10): the Instigator must be
    a live pawn that `PawnHostile` calls hostile. Adams' and Lugo's
    `YProj_FragGrenadePlayerSquadAI` and scripted grenades with no
    Instigator are left to the game (`GRENADE: <class> thrown by <pawn>
    (not an enemy): ignored, left to the game`, once per grenade; up to 16
    remembered in `g_grenIgnored`, dropped when they die). Before, any
    `YProj_Physical` not Walker's was offered, and throwing back a squad or
    Kismet grenade made it Walker's and moved a scripted blast.
  - At most 8 are followed. Each one is identified by its FName, because the
    allocator reuses grenade addresses.
  - Log: `GRENADE: <class> thrown by <pawn> (enemy|not an enemy), <d> m from
    Walker, fuse <s>`, then once `... on the ground ... fuse <s> left`.
- **Red corners:** the grenade on the ground nearest to Walker, within 8 m,
  gets `Highlight` state 3: red corners from the bounds of `m_projSkelMesh`
  (+0x240, about 7 x 9 x 7 cm, radius 6.6 units). In the left hand's zone it
  gets state 4, red and thicker. They win over the cyan/gold corners: there is
  one box only, and the danger comes first.
- **Minimum fuse** (review fix): a grenade at rest with under 1.0 s of fuse
  (`kGrenMinFuse`, `GrenadeFuseGrabbable`) gets no corners and cannot be
  grabbed, logged once: `GRENADE: <class> on the ground with <s> s of fuse
  left (under 1.0 s): no red corners, no grab`. Stun (1.1 s) and phosphor
  (0.7 s) grenades are at rest 0.2 s after landing, so they are never
  offered; a frag (4.0 s) is offered for about its first 3 s. An unread fuse
  (no `OnExplode` timer) is offered as before.
- **Grab** (`GrenadeGrab`, left hand only, like weapons):
  - The grip closes (within 0.25 s of the press, once per press) with the
    palm within `grabzone` of the grenade's box. A grenade in the zone wins
    over objects and guns.
  - The zone sets `g_propZone[1]` and holding sets `g_propHeld[1]`, so the
    grip is not LB (Walker's own grenade) and there are no gestures.
  - Never while the left hand is on the gun, two-handing or in the physical
    reload (`GunHandHaptics(1)`): the gun hand never changes. Only the left
    hand gets pulses.
  - On the grab: Instigator = Walker, so his cylinder never blocks it
    (`bBlockedByInstigator` is off). `bPerformGravity` and `bCollideWorld`
    (+0xA0 bit 0x10000000) are turned off. A 50 ms pulse goes to the left
    hand only.
  - `m_causer` stays the enemy's while it is in the hand: a blast there is
    the enemy's grenade.
- **In the hand** (`GrenadeDrive`, every tick):
  - Velocity = (palm - Location) / dt + the hand's velocity over 25 ms, at
    most 40 m/s. The palm is the point 13 cm in front of the IK target, where
    objects sit. Gravity and world collision are cleared again each tick.
    The fingers make a fist.
  - Over the 60 ms window the feed-forward lagged a fake hand that stops:
    the grenade waited 13 cm away for 4 ticks. With 25 ms that lag is gone.
  - `grablog N` writes a `GRENLOG:` line per tick (palm distance, velocity,
    fuse; "blocked" while the path below is blocked).
  - **The grenade's path** (review fix): each tick a zero-extent trace
    (movers, level, level geometry, terrain) from the last point reached
    without crossing the world (`clear`, first the rest position on the
    ground) to the grenade. Clear: that is the new point. Blocked: the hand
    is beyond a wall, the cover or the ground, and the point stays until the
    hand comes back. One trace per tick, only while a grenade is held.
  - **Lost hand** (review fix): when the left hand loses tracking (or the
    arms are released) with a grenade in it, the grenade stays where the palm
    was for up to 0.2 s (`kGrenLostGrace`): `left hand lost with the grenade:
    it stays where the palm was ...`, then `left hand back after <s> s, the
    grenade is still in it`. Past 0.2 s it is let go there ("hand lost").
- **Exploded in the hand** (`GrenadeGoneInHand`, from every path: drive,
  release, a lost hand, Walker's death): `GRENADE: exploded in the left hand
  after <s> s ... Walker's health <a> at the grab, <b> now`, plus a 150 ms
  pulse on the left hand only.
- **Release** (`GrenadeRelease`):
  - **Speed, the props' throw model** (review fix, 27/10). With `throwmode
    peak` (default): `PeakThrowVel` on the left hand's history, relative to
    Walker (the peak of the last ~110 ms, blended with the release average
    for a put-down, the lever of the grenade's centre). With `classic`: the
    60 ms average minus `FrameVelocity`. `throwboost` multiplies only the
    hand part, capped at 1.3 x `m_startSpeedThrow` (26 m/s) relative to
    Walker; Walker's `FrameVelocity` is added back unboosted
    (`GrenadeThrowVel(hand, base, ...)`). Before, the 60 ms world average at
    the release was boosted whole: a throw where the hand slows down before
    the grip opens (every real overhand throw) left at about half speed and
    could land next to Walker, and Walker's run was boosted x1.3.
  - "disabled" (a cinematic, a body-animated special move such as a vault or
    a mantle, `grabprops off`) also lets it go with the hand's velocity, the
    same way. A lost hand past its grace, the watchdog (no props tick for
    0.3 s, so it cannot fly off through walls with gravity off) and the
    option turned off drop it with no speed.
  - If the grenade's path (above) is blocked at the release, it restarts
    6 units before the hit, on the side it came from. Before, the trace ran
    from the head: crouched behind cover with the hand just above its top,
    the head-hand line crossed the cover and the grenade was put back on
    Walker's side, into the cover.
  - `m_numBounces` = 0, gravity and the saved `bCollideWorld` back on, the
    sliding bits cleared.
  - It becomes Walker's: Instigator, `InstigatorController` = his PC,
    `InitCauser(PC)` (script call; checked, written directly if the call
    fails), `m_causerPawn`, `m_causerPawnPRI`.
  - The bounce prediction restarts from the release point:
    `m_predictedLocation` / `m_predictedVelocity` = now,
    `m_currentPredictionTime` = time since the throw (clamped to the fuse,
    `GrenadePredTime`), bounces 0. The intent is that the AI gets the
    warning and the "grenade!" bark, as for Walker's own grenades; the barks
    seen on real AI (below) are observed after the throw, with no A/B run
    without the restart.
  - Line: `GRENADE: left hand threw back <class> (grip) at <v> m/s (hand <h>
    m/s relative to Walker, peak|60 ms average, x throwboost ..., at most 26
    m/s, + Walker <w> m/s); peak <p> m/s <ms> ms before release, <a> m/s at
    release (60 ms average), peak weight <k>, fuse <s> left, held <s>,
    palm-grenade distance <mean> cm on average, <max> at most over <n> ticks;
    now Walker's (InitCauser ok)`.
    The real speed comes 3 ticks later: `thrown-back grenade flies at ...`.
  - 0.5 s after the blast, `GRENADE: the grenade Walker threw back exploded
    at ...` gives `m_killCount` and, for each pawn within 8 m, his health and
    last damage ("from his enemy side" = Walker).
  - 30 s counters: `PROPS: enemy grenades in 30 s: seen, came to rest,
    picked up, thrown back, let go, exploded in the hand; at rest with under
    1.0 s of fuse (not offered); not an enemy's (ignored)`.
- **Measured on the dummy** (level 1 CP04, `f8/dummytest.py`, final dll,
  output `f8/out/dummytest_final.txt`). The thrower was a summoned
  `YPawn_MarauderAssault` with `SpawnDefaultController`. Its grenade came from
  the game's own `YGrenadeThrow.SpawnGrenade`, called with `ucall` on
  `Default__YGrenadeThrow`, so `m_causer` was its controller.

  | Test | Log / measure | Result |
  |---|---|---|
  | Grenade dropped 1.4 m from Walker | `on the ground 0.2 s after it was seen, 1.4 m from Walker, fuse 3.8 s left`; red corners (`f8_red.png`) | marked |
  | Catch | `left hand took YProj_FragGrenadeAI (... fuse 3.0 s left, 0 cm from the palm)`; Instigator = Walker, +0xA0 0xE80B0000 (`bCollideWorld` off), +0x3E0 0xC90FE2 (gravity off) | in the hand |
  | Hold 0.65 s | 76 ticks, palm distance 1.3 cm on average; 0.1 cm per tick once settled (GRENLOG); the 34.8 cm maximum is the fake hand's jump onto the grenade at the grab | follows the palm |
  | Throw at the dummy, 6.6 m (aimed 0.8 m beside him) | `threw back ... at 9.6 m/s (hand 7.3 m/s x 1.30)`, `flies at 9.30 m/s (given 9.55) after 25 ms`, `on the ground ... 8.3 m from Walker, fuse 1.4 s left`, exploded 2.4 s after the throw | lands and explodes there |
  | Kill attribution | dummy 75 -> 0, last damage `YDamageType_Grenade` from his enemy side, `m_killCount 1`, "1 dead, 1 of them last damaged from their enemy side (Walker)" | Walker's kill |
  | Control: the same AI grenade not touched, next to a fresh dummy | dummy 75 -> 0, `m_hitByEnemy` 0, `m_killCount` 0 | the enemy's |
  | Held until it explodes, **god mode off and Walker's health set to 1000** (run of 10:57) | `PlayDeathAnim` (special move 53) 2.5 s after the grab, health 1000 -> 0 | the blast in the hand kills Walker |
  | Held until it explodes, god on (final run) | `exploded in the left hand after 3.50 s (fuse was 3.5 s at the grab); Walker's health 115 at the grab, 115 now` | logged from every path |
  | `grenadeback off`, grip on the grenade | no `took` line, the grenade moved 0.0 cm, no corners (`f8_off.png`) | nothing |
  | Final dll, again (`f8/out/dummytest_finaldll.txt`) | catch, 75 ticks at 1.4 cm on average, `threw back ... at 9.6 m/s`, dummy 75 -> 0 from his enemy side, `m_killCount 1`; `grenadeback off` nothing | same |
  | Lift to chest height, in the arm's reach (final dll) | the frag sits in the closed left fist (`f8_lift.png`); after the grab ticks the palm distance is 0.0-0.1 cm per tick | held in the hand |

  - In the 10:57 run the "exploded in the left hand" line was missing:
    Walker's death reached `GrenadeRelease`, which then forgot the grenade
    silently. Since then every path goes through `GrenadeGoneInHand`.
- **Measured on real AI pawns, with grenades the test spawned**
  (`cpTutorialFight`, realfight.md; `f8/realtest.py`, output
  `f8/out/realtest1.txt`). The AI did not throw these grenades: the script
  created each one with `ucall Default__YGrenadeThrow SpawnGrenade <marauder
  pawn> ...` (the AI's own spawn path, so `m_causer` = his
  `YAICtrl_Humanoid`), aimed 18 cm in front of Walker's palm, after
  teleporting Walker 6 m in front of that marauder. What this proves: a
  grenade credited to a real marauder, thrown back, kills him for Walker.
  It does not prove detection or reach of grenades the AI throws on its own
  (see the review fix below).
  - Try 1: `YPawn_MarauderPistol`. Caught 0.5 s after landing (fuse 3.5 s),
    held 1.64 s (palm distance 0.5 cm on average), thrown at 8.7 m/s. It
    landed 7.1 m away and exploded 1.9 s after the throw. **He went 75 -> 0**,
    last damage `YDamageType_Grenade` from his enemy side, `m_killCount 1`.
    The SMG 5.4 m away was not hurt.
  - **AI warning, observed:** the Pistol ran `IncomingGrenade` (114) 0.50 s
    and 0.56 s after the throw. It is consistent with the restarted
    prediction (`GrenadeImpactImminent` with Walker as `m_causerPawn`), but
    no control run without the restart was made, so the cause is not
    proven.
  - Try 2: `YPawn_MarauderSMG`. Caught, held 0.64 s, thrown at 9.9 m/s; the
    SMG ran 114 at +0.45 s. Lugo's SAW killed him before the blast (last
    damage `YDamageType_SAW`), so `m_killCount` stayed 0. The grenade's blast
    hit no one else: the other SMG was 3.9 m away behind cover, health 75.
  - Natural AI throws: in this fight the Pistol and SMG marauders threw no
    grenade of their own in about 40 s (the only `thrown by` lines are the
    two spawned ones). Which levels' AI really throws grenades, and how
    often, is not measured.
- **Regressions** (scratchpad `f8/suite_out.txt`, `f8/hitall_out.txt`).
  - The grab suite (`suite.sh f8s`) ran on the 10:58 dll. The final dll
    differs only inside the grenade code: the live check of the pawn and
    controller in `GrenadeRelease`, and no grenade zone with the left hand on
    the gun. The suite never meets a grenade.
  - Grab suite results, all as in item 7's fix run:
    - grasp power, centre f -2.6 p 5.5 a 6.6 cm, turned 46 degrees;
    - drive settle 0.058-0.100 s, sweep 2.2 cm (items 6-7: 1.5-2.3);
    - the static grab took and released bottle_quad01;
    - the glove pulled the bottle 1.8 m in 0.41 s and caught it in flight;
    - bottle knocked 51.2 cm, barrel 0.0 cm;
    - drop broke at 5.9 m/s, smash broke in the hand;
    - door 60.0 degrees, stop sign 5.4.
  - **Shot test:** the radio moved 3.6 cm (earlier runs 163-180 cm). The
    aiming shot ("it 1") had already hit the radio and made it physical
    (`became physical` twice, 11:12:36 and 11:12:39), so the measured shot
    pushed a radio already at rest against the rifle case. Run once more
    alone on the final dll: 54.1 cm, again after an aiming shot that hit it.
    The shot path is untouched by item 8. The test's figure depends on
    whether the aiming shots hit the radio first.
  - Hit pipeline (`p4t/hitall.sh f8_hit`, final dll):
    - `hitthrow`: held hit `head (closest body)` at 1.5 m/s, release 5.8 m/s;
    - `heldswing`: Spine1 at 3.4 m/s on both fast swings, nothing in the
      pressed, pull and push phases;
    - `hitdrop2`: Spine3 at 1.8 m/s;
    - `regrab`: 0 hit lines (wrist 0.0-0.1 rad/s), as in item 7.
  - Unit tests 2868/2868 (`TestGrenadeBack` included).
- **Cost:** `PROPS: tick` 0.171-0.184 ms on average in every 30 s window of
  the three test sessions (items 6-7: 0.17-0.20). The peak of about 11 ms is
  in the first window, with the summon and calibration, as before. The scan
  reads the few new actors at the tail each tick and about 200 pointers
  every 0.5 s.
- **Not verified / open (headset checks).**
  - Only grenades at rest on the ground can be caught. In-flight catches and
    the gravity glove (HL:A) are not done: they need the glove's prediction.
  - In novr screenshots the red corners sit higher than the grenade
    (`f8_red.png`). The cyan box of item 6 does the same
    (`f6/out/final/A_reach.png`): it comes from the flat test view, not from
    the grenade, whose mesh origin, bounds and Location coincide (one bone,
    identity). Check it in the headset.
  - How the hand looks holding it: in reach, the fist closes on the grenade
    (`f8_lift.png`). With the fake hand target on the ground, beyond the
    arm's reach, the drawn hand stopped about 20 cm short of the grenade
    (`f8_real_held.png`). In the headset the player crouches or bends down.
  - The grab zone (`grabzone`, 8 cm from a box about 7 x 9 x 7 cm) may be
    tight for a small object on the ground. Throw strength (`throwboost`, at
    most 26 m/s) is tuned on the fake hand only.
  - Left-handed mode: not run. Squad grenades are ignored since the review
    fix (below).
  - Stun and phosphor grenades are never offered since the review fix (fuse
    under 1 s at rest). Sticky AI grenades: same code, not run (3.0 s; stuck
    to something they are not `PHYS_Falling` and are skipped).
- **Review fix (27/10 ~14:00-14:35, scratchpad `f8x/`).** Snapshot
  `snap_f8fix_grenadeback`. Scripts `f8x/fixtest.py` (dummy, level 1 CP04),
  `f8x/realnat.py` (real fight), `f8x/final.sh`; output in `f8x/out/`.
  - **Throw model (medium).** `ramp` scenario: the fake hand rises to 7.0
    m/s in 150 ms, slows to a quarter in 70 ms, then the grip opens
    (`synthleftramp ... release`). Log: `threw back ... at 9.1 m/s (hand
    7.01 m/s relative to Walker, peak, x throwboost 1.30 ...); peak 7.00 m/s
    75 ms before release, 3.79 m/s at release (60 ms average)`. The old
    model would have given 3.79 x 1.3 = 4.9 m/s. Measured in flight 8.85 m/s
    (given 9.11). It landed 7.7 m away and killed the dummy (75 -> 0, from
    his enemy side, `m_killCount 1`). Walker's run is no longer boosted:
    unit tests (a still hand while running at 6 m/s leaves at 6 m/s, not
    7.8; the 26 m/s cap applies to the hand part).
  - **Real AI throwing on its own (medium): not achieved, open.** Two
    approaches. (1) `f8x/realnat.py`: `cpTutorialFight`, god mode on every
    pawn (renewed every 5 s, so the fight cannot end), Walker at the start
    point, 150 s of a live firefight (about 210 enemy shots per 30 s, 87 hits
    on Walker): **no grenade at all** (`natural grenades seen: []`, no
    `GRENADE` line). With item 8's 40 s, about 3 minutes without a grenade
    from `YPawn_MarauderPistol` / `MarauderSMG`. (2) A static look for which
    AI carries grenades: `YGamePawn.m_presetGrenadeTypes` (+0x49E, 8 bytes)
    is zero in every loaded enemy pawn default (only Walker and the horde
    players have it), and `YAIDefinition` has no grenade-use field; the AI
    side (`YAICtrl_Humanoid.m_throwGrenade*`) exists, but what enables it
    per enemy was not found. So detection through the actor-list tail, rest
    within the left hand's reach on real terrain, and how often the AI
    throws stay unproven on a natural throw; the final gameplay regression
    (or the headset) has to find a fight where the AI throws. What is proven:
    detection, catch, throw-back and the kill credit with grenades created by
    the AI's own spawn path (`SpawnGrenade`) for a real marauder.
  - **Bark (medium):** described as observed above; no A/B without the
    prediction restart was run.
  - **Lost hand (low).** `lost` scenario: tracking gap of 120 ms
    (`synthleftlost 120`): `left hand lost with the grenade ...`, `left
    hand back after 0.12 s, the grenade is still in it`, grenade moved 0.0
    cm. Gap of 700 ms: `let go of ... (hand lost)` 0.200 s after the loss,
    at the last palm; it fell to the ground there.
  - **Disabled (low).** `dis` scenario: hand moving at 1.92 m/s, then
    `grabprops off`: `threw back ... (disabled) at 2.5 m/s (hand 1.92 m/s
    ... peak)`. A vault, mantle or cinematic takes the same path; not run on
    a real special move (headset check).
  - **Path trace (low).** `wall` scenario: with the palm pushed 25 units
    under the ground, 38 of 40 `GRENLOG` ticks said `blocked`; at the
    release `... the hand was beyond the world on the grenade's path: it
    starts from there, on its side`, and the grenade rested on the ground
    surface (z 638, as the other grenades at rest there) instead of 617
    under it. The crouched cover-top case of the review was not built in the
    game (no world-geometry cover at eye height near CP04): by construction
    the path never crosses a cover the hand went over. The scenario printed
    "False" for `lost` and `wall` because of two script checks (the word
    "hand lost" is in the new grace line; the still-hand release was worded
    "threw back at 0.0 m/s"); the log lines above are the result. Since then a
    release with under 0.5 m/s of hand speed is logged and counted as "let
    go of" (final dll; wording only).
  - **Minimum fuse (low).** `fuse` scenario: an AI frag left at rest by the
    hand logged `on the ground with 0.99 s of fuse left (under 1.0 s): no red
    corners, no grab` 2.79 s after landing; the grip on it took nothing and
    the screenshot has no corners (`f8x/out/f8x_fuse_nocorners.png`; the red
    arrow mid-screen is the game's own grenade indicator). The thrown-back
    grenade of `ramp` got the same line at 0.99 s.
  - **Not an enemy's (low).** `squad` scenario: a frag spawned with Lugo
    as the thrower: `GRENADE: YProj_FragGrenadeAI thrown by YPawn_Lugo (not
    an enemy): ignored, left to the game`, no grab, no corners
    (`f8x_squad_nocorners.png`). The dummy's grenades still log `(enemy)`.
  - The dummy scenarios ran on the dll before the wording change above; the
    real-fight run, the suites and the unit tests on the final dll.
  - **Regressions on the final dll** (`f8x/out/final_out.txt`): grab suite
    (`suite.sh f8x`) grasp power, centre f -2.6 p 5.5 a 6.6 cm, 46 degrees;
    drive settle 0.025-0.108 s, sweep 1.7 cm; static grab took bottle_quad01
    (released "wedged" after 1.6 s, as in several earlier runs: f2, f4-f7,
    f10, f11); glove pulled the bottle 1.8 m in 0.41 s and caught it; bottle
    knocked 55.2 cm, barrel 0.0; drop broke at 5.9 m/s, smash broke in the
    hand; shot moved the radio 162.4 cm; door 59.9 degrees, sign 5.2. Hit
    pipeline (`p4t/hitall.sh f8x_hit`): `hitthrow` held hit `head (closest
    body)` at 1.6 m/s, release 5.5 m/s; `heldswing` Spine1 on both fast
    swings (2 lines 8 ms apart on the first, as in item 11's run), nothing in
    the pressed, pull and push phases; `hitdrop2` Spine3 at 1.9 m/s;
    `regrab` 0 hits. Unit tests 2894/2894.
  - **Cost:** `PROPS: tick` 0.161-0.170 ms on average in the real fight
    (one trace per tick only while a grenade is held), 0.162-0.186 in the
    hit pipeline session.

**Dragging and pushing dead enemies (gameplay plan, item 11, 27/10).**
Option `corpsedrag on|off` (default on, Basic menu "Drag bodies", ini line
with comment; needs `grabprops`, not `propcombat`). Code: `props.cpp` block
"Cadaveri spinti e trascinati" (`CorpseScan`, `CorpseState`,
`CorpseReanimate`, `CorpseNearest` / `CorpseNearestBody`, `CorpseGrab`,
`CorpseDrive`, `CorpseRelease`, `CorpseNearHand`), the pure parts in
`pure_utils.h` (`BodyShapeDist`, `CorpseImpulseSplit`, `CorpsePullStep`,
unit test `TestCorpseDrag`). Test commands `corpsetest` (the left hand takes
the nearest corpse body within 2 m, no zone) and `corpsetest bones` (logs the
corpse's bodies: bone, position, and the palm's distance from each body's
shapes). Scripts in scratchpad `f11/` (`ctest.py`, `realcorpse.py`,
`cstate.py`, `final.sh`), outputs in `f11/out/`.
- **How the game handles corpses** (UnrealScript bytecode read with
  `f5/udis.py` and `f11/allfuncs.py`, measured on the summoned dummy):
  - A dead enemy is a `YGamePawn` in state `Dying`, `Health <= 0`, `Physics`
    10 with its mesh in ragdoll: `PhysicsAssetInstance` (+0x25C) with 16
    bodies for the marauders (`CH_ENEMY_SK_PHYS`: Hips, Spine1, Spine3, head,
    arms, elbows, hands, legs, knees, ankles), `bSkelCompFixed` (+0x3C4 bit
    0x8000) off. Alive, the same mesh has the 16 bodies fixed (kinematic).
  - **Freeze.** `Dying.Tick`: once `!Mesh.RigidBodyIsAwake()`, more than 3 s
    after `TimeOfDeath` (+0x8BC), with `m_deathDBNOState` (+0x48A) not 2 and
    `MayFreezeMesh` (special move not 36/37), it sets `m_ragdollIsDisabled`
    (+0x5E8 bit 0x20000000), static light environment, `FinishKill` (timer
    `DisableMeshUpdates` 0.1 s: native 0x009B4710 -> vtable +0x7C8, removes the
    `PhysicsAssetInstance` and sets `bNoSkeletonUpdate` +0x38C bit 0x1) and
    `CreateBloodPool` (a blood decal under the pelvis). Measured: the killed
    dummy is frozen 0.4 s after it comes to rest, PAI 0, pose kept.
  - **Wake.** `YGamePawn.ConditionalReanimateRagdoll` (script, iNative 0):
    with `m_deathDBNOState` 1-3 ("death DBNO": the enemy is still crawling)
    it calls `DiedDuringDeathDBNO` / `PlaySimpleRagdollDeath` (it would finish
    him off); otherwise, if `m_ragdollIsDisabled`, it clears it, sets
    `bSyncActorLocationToRootRigidBody`, `SetHasPhysicsAssetInstance(true)`,
    `SetComponentRBFixed(false)`, `WakeRigidBody`, `bNoSkeletonUpdate` off,
    `EnableEffectTriggers(true)`. The game calls it itself on every pawn in
    reach (`YDestructibleObject.CheckSleepingObjectReactivation`,
    `YSeqAct_WakeKActors`, found by scanning all bytecode for the name with
    `f11/whocalls.py`). Measured: the 16 bodies exist in the same tick (0.12-0.20
    ms for the call); left alone, the game freezes it again ~0.4 s after it
    rests (one more blood pool where it lies).
  - **Vanishing corpses:** `bScalingToZero` (+0x5E8 bit 0x80000000) shrinks
    and destroys the corpse; such corpses are skipped.
- **Safety: only corpses, never the dying or the living** (`CorpseState`,
  checked at the scan, before any wake, at the grab and on every tick of the
  drag; the first failure lets go): not Walker, not deleted or hidden, a
  `YGamePawn`, `Health <= 0`, an enemy by the hit pipeline's rule
  (`PawnHostile`: `m_squadMemberType` +0x4BC must be 7, then the team
  standing, and without a team, which is the usual case for the dead, a
  `YPawn_Enemy` class without a friend's name; review fix: never Lugo, Adams,
  Riggs, Gould, `YPawn_LugoCanDie`, civilians or scene pawns),
  `m_deathDBNOState` 0 or 4, not `bScalingToZero`, `Physics` 10, at least
  1 s since `TimeOfDeath`, a visible mesh; then either frozen by the game
  (`m_ragdollIsDisabled` AND the `PhysicsAssetInstance` already removed:
  review fix, see below), or a ragdoll with no special move running (53 is
  the death animation), a `PhysicsAssetInstance` and bodies not fixed. The held body is also re-checked every tick: same mesh, same
  `PhysicsAssetInstance`, same `RB_BodyInstance` at the same index, same
  `NxActor` (the game frees them when it freezes the corpse).
- **Candidates:** every 0.25 s the `WorldInfo.PawnList` (no GObjects scan),
  at most 8 corpses within 4 m of Walker (`g_corpses`).
- **PhysX pose** (`CorpseBodyPose`): `NxActor::getGlobalPose`, vtable slot 9
  (+0x24), `NxMat34` by hidden pointer (order of `NxActor.h`, PhysX 2.8:
  `setMass` is slot 45 as verified for item 6). Verified in the game before
  any other use (`CorpsePoseCheck`, once per session, on the body of the
  first corpse met): the pose must match its bone (`GetBoneLocation` /
  `GetBoneQuaternion`, world space) within 3 units and ~8 degrees. Measured
  on every run: "body at 0.0 cm from the bone, axes match 3.000 as columns"
  (rows 0.30-0.83): the `NxMat33` is stored by rows, the body's axes are its
  columns. The vtable slot must also be in the vtable's module
  (`PhysXCore.dll`). Without the check passing there is no grab.
- **Body shapes** (`BodyShapes`): `RB_BodySetup` (`PhysicsAsset.BodySetup`
  +0x40 indexed by `RB_BodyInstance.BodyIndex` +0x40; `BoneName` +0x84)
  inherits `KMeshProps.AggGeom` +0x48: `TArray`s of `KSphereElem`,
  `KBoxElem`, `KSphylElem` at +0x48, +0x54, +0x60, array stride 80 bytes
  (the `FMatrix TM` is 16-aligned; `KSphylElem` 0x4C, `KBoxElem` 0x50 read
  from the ScriptStructs). `TM` rows are the shape's axes and origin in the
  body (bone) frame; sphere `Radius` +0x40, box `X Y Z` +0x40 (full sizes),
  capsule `Radius` +0x40, `Length` +0x44 (the cylinder, along z). Times the
  mesh scale (`Scale` +0x1D8, `Scale3D` +0x1DC, actor `DrawScale` +0x6C,
  `DrawScale3D` +0x70). Dummy: hips capsule r 6, spine r 8.5-9, upper arms
  r 3.7-3.9 x 9-10, forearms r 2.8-3 x 9, thighs and shins r 4.3-5 x 15-16,
  hands and feet boxes, head three capsules. Cached per `RB_BodySetup`.
- **Push:** the free hand's sphere (stage 3) collides with the ragdoll's
  bodies like with any object (ragdoll `RBChannel` 16 `DeadPawn`, collides
  with `GameplayPhysics`, the sphere's channel). A frozen corpse has no
  bodies: the left hand (not on the gun, nothing held) arriving within 10 cm
  of a limb (the body shapes placed on its bones, review fix; before: of its
  bounds) faster than 0.3 m/s wakes it (`CorpseNearHand`), at most once a
  second per corpse. While the palm stays within 30 cm of a physical
  corpse, `WakeRigidBody` every 0.3 s keeps the game from freezing it under
  the hand.
  - **Weight in the push** (`CorpsePushCap`): the sphere (4 kg, set back to
    the hand's velocity every tick) hits one light ragdoll body at a time
    (0.3-6 kg) and the joints drag the rest along: in the first final run a
    1.5 m/s sweep through the torso moved the dummy (30 kg) by 1 m. Now, for
    every physical corpse not in the hand within 40 cm of the left palm, and
    for 0.5 s after, each body's horizontal speed is held to 0.6 m/s
    (`NxActor::getLinearVelocity`, slot 58 as everywhere; the missing
    velocity change through `AddImpulse` with `bVelChange` on the body's
    bone; vertical untouched, so it still falls). `CORPSE: ... shoved by the
    left hand: N bodies held to 0.6 m/s (fastest X m/s)` (at most one line
    every 0.5 s) and a count in `PROPS: corpses in 30 s`.
- **Grab** (left hand only, grip in the zone, once per grip press, never with
  the left hand on the gun; objects, guns and grenades in the zone win): the
  ragdoll body whose shapes are nearest the palm, within `grabzone` (8 cm)
  of their surface (`CorpseNearestBody`; a frozen corpse is in the zone by
  the same shapes placed on its bones, `FrozenShapes`, review fix, and is
  woken at the grip, then the body must be within the zone too). The grabbed point is the nearest surface point (or the palm itself
  if it is already inside the limb), kept in the body's axes. 40 ms pulse
  on the left hand, cyan corners around the corpse in reach, fist, and
  `g_propHeld[1]` (no grenade or punch gestures). While the hand comes
  slowly (under 1 m/s) towards a body in the zone, the left sphere ignores
  the corpse's bodies, as for objects in the zone: before, the 4 kg sphere
  pushed a 0.4 kg arm away before the grip closed (two misses in the test
  runs). Faster is a push. `CompNxAll` lists every ragdoll body for the
  held corpse (`g_corpseComp`) and for every corpse in `g_corpsePairMeshes`
  (the approached ones and, review fix, every released one: pruned at the
  end of each tick when no pair is left), so the pairs are set
  and restored on all bodies; freed bodies are never touched (`ProxyPair`
  keeps only live ones).
- **Drag** (`CorpseDrive`, every tick), like the doors of stage 5 but with
  the weight rules:
  - wanted velocity of the grabbed point = hand velocity + error / 80 ms, at
    most 1.5 m/s; dv = wanted - measured (displacement per tick);
  - the whole corpse would need total mass x dv; within the limit force x dt
    (`CorpseImpulseSplit`), the grabbed body gets its own share (its mass x
    dv, `AddImpulse` at the point with its bone name) and the rest, only the
    horizontal part, goes to the root body (the pelvis, `AddImpulse` with no
    position: at its centre of mass). That is the pull an arm passes on to
    the trunk. With everything on the grabbed body a 0.4 kg upper arm pulled
    at 0.3 m/s did not move the dummy (1 cm, then it slipped with the force
    at x4): the joints cancelled it. No lift from the pelvis: pulled up by an
    arm, the body raises the arm and drags;
  - force = the hand's weight force (`griptune force`, 200 N) x 1 (`full`),
    x 2 (`light`, default), x 4 (`off`); while the point stays more than
    10 cm from the palm without coming closer it doubles every 0.4 s up to x4,
    and is back to x1 within 6 cm (`CorpsePullStep`): heavy, never immovable;
  - `WakeRigidBody` on the whole mesh every 0.25 s: the game never freezes a
    corpse in the hand;
  - more than 40 cm from the palm for 0.3 s: it slips out of the hand. With a
    3 m/s cap a fast yank had dragged the dummy 58 cm and thrown it 2 m when it
    slipped; at 1.5 m/s it slips sooner and stops nearby;
  - the drawn hand (`solidhands held/full`) sits on the grabbed point
    (`SolidSource` "constrained point", the point of the last tick); the gun
    hand is never involved.
- **Mass:** the dummy's ragdoll is 0.75 PhysX units = 29.9 kg at `masskg`
  40 (Spine1 5.0 kg, Hips 2.9, upper arm 0.4, foot 1.1). `masskg` only
  converts the force limit, the PhysX masses are the game's.
- **Log:** `CORPSE:` lines for each wake (who woke it, bodies, ms), the pose
  check, each grab (bone, body mass, total mass, palm distance, force) and
  release (why, time, how far the pelvis moved, palm-point distance on
  average and at most, highest force); `GRABLOG: L corpse` per tick with
  `grablog N`; `PROPS: corpses in 30 s` (woken, grabbed, let go, slipped,
  lost, largest move). The `GRABLOG: L free` line now ends with the gun and
  corpse in the zone.
- **Measured, summoned dummy, final dll** (`f11/ctest.py f11final2`,
  scratchpad `f11/out/f11final2/`, level 1 CP04, killed with
  `HandleMeleeAttack`):
  - dying: no `CORPSE` line in the 6 s after the kill (ragdoll at 0.5-1 s,
    frozen at ~5 s);
  - push wake: the hand arriving at 0.5 m/s woke the frozen corpse ("the
    left hand pushed it and woke up the ragdoll ... 16 physics bodies,
    0.17 ms"); pose check "0.0 cm from the bone, axes match 3.000 as columns";
  - torso grab and drag: Spine1 (5.0 kg of 29.9), palm 0.0 cm from it; hand
    40 cm back and 20 up at 0.3 m/s, then 50 cm sideways at 0.4 m/s: box
    57.5 + 35.0 cm, pelvis 66 cm, palm-point distance 3.1 cm on average, 4.2
    at most, 400 N (`drag_back.png`, `drag_side.png`);
  - arm grab and drag: Arm_Left (0.4 kg) aimed at the elbow from `corpsetest
    bones`, hand 50 cm back at 0.3 m/s: box 40.1 cm, pelvis 54 cm, distance
    5.1 cm on average, 16.5 at most, 400 N (`arm.png` shows the cyan corners
    around the corpse);
  - yank: Spine1, hand 80 cm in 0.27 s: dragged 31 cm, slipped after 1.47 s
    (67.4 cm at most); the push limit then held 15 bodies (fastest 3.49 m/s)
    to 0.6 m/s;
  - shove: empty hand swept through the torso at 1.5 m/s: box 31.1 cm, "7
    bodies held to 0.6 m/s (fastest 1.27 m/s)" (100.8 cm in the first final
    run, before the limit);
  - living dummy (75 HP): hand in its torso with the grip for 1.5 s: no grab,
    HP 75, moved 0 cm;
  - left alone, the game froze it again 1 s after the hand left; the hand
    then came slowly (0.2 m/s, no push wake) onto the frozen elbow and closed:
    "grip woke up the ragdoll ... 0.12 ms", "took ... by Ellbow_Left (0.3 kg,
    palm 0.0 cm)", dragged 23 cm.
  - Earlier runs (r7, r8, the first final run) gave the same pattern; the arm
    drag once grew the force to x4 (1600 N, 18.9 cm at most).
- **Measured, real enemy, final dll** (`f11/realcorpse.py f11real2`,
  `cpTutorialFight`, see scratchpad `realfight.md`): a `YPawn_MarauderSMG`
  with `YAICtrl_Humanoid` and a `YPlayerReplicationInfo`, killed by the
  game's own death (`Pawn.Suicide` -> `Died`): special move 53 (death
  animation) from 0.15 s, ragdoll at 1.2 s, frozen at 3.5 s, PAI removed at
  3.6 s; no `CORPSE` line while dying. Woken by the hand, grabbed by Hips
  (palm aimed at Spine1 from `corpsetest bones`), dragged 40 cm back and
  40 cm sideways: box 39 + 34 cm, pelvis 60 cm, palm-point distance 4.4 cm on
  average, 11.4 at most, force up to 800 N; `SOLID: constrained point` lines
  while dragging; frozen again 1 s after the hand left. A first real run
  (build before the push limit) grabbed Spine1 and dragged it 51 cm; one run
  in between aimed the palm at the corpse's bounds centre, which on that pose
  was not on any body ("corpse -1" in `GRABLOG`, no grab): the zone is the
  bodies' real shapes, so the test now aims at a bone.
- **Cost:** `PROPS: tick` 0.164-0.176 ms on average, 0.34-0.70 ms at most
  during the corpse runs (the baseline is 0.15-0.18).
- **Earlier suites, final dll:** grab suite (`suite.sh f11b`) and hit
  pipeline (`p4t/hitall.sh f11b_hit`) match the item-10 final run: drive
  settles in 0.066-0.108 s, static grab, glove catch, knocked bottle 53.5 cm,
  barrel 0 cm, drop and smash break, shot radio 164 cm, door 59.5 degrees,
  sign 5.1; `hitthrow` head hit 1.6 m/s (release 5.8 m/s), `heldswing`
  Spine1 at 2.5-3.0 m/s and 24 units, none pressed or slow, `hitdrop2`
  Spine3 2.0 m/s, `regrab` 0 hits. The same on the build before the push
  limit (`f11`, `f11_hit`).
- **Review fixes (27/10, ~14:40, final dll; scratchpad `f11x/`:
  `ctest2.py`, `realgun.py`, `final.sh`, outputs in `f11x/out/`, snapshot
  `snap_f11fix_corpses`):**
  - **Push limit on a corpse just woken.** `CorpseNearHand` wakes a frozen
    corpse, but its `g_corpses` entry stayed "frozen" until the next scan
    (up to 0.25 s), and `CorpsePushCap` skipped frozen entries: the sweep
    that woke it crossed the new bodies with no limit (the "31 cm instead of
    1 m" had been measured on a corpse already awake). Now `CorpseReanimate`
    marks the entry physical in the same tick, and `CorpsePushCap` no longer
    skips frozen entries (`CorpseState` decides). Measured on the dummy,
    frozen with PAI 0 right before the hit: a 1.5 m/s sweep through Spine1,
    started 76 cm to the side, woke it ("the left hand pushed it and woke
    up ... 0.17 ms") and the limit acted 0.058 s after the wake ("2 bodies
    held to 0.6 m/s (fastest 0.64 m/s, 0.058 s after its wake)"); the bounds
    moved 41.3 cm (awake corpse 31.1 cm, no limit 100.8 cm).
  - **Only enemies.** `CorpseState` now applies the hit pipeline's rule
    (`PawnHostile`, above): never a squad member or story character
    (`m_squadMemberType` +0x4BC not 7: Lugo, Adams, Riggs, Gould,
    `YPawn_LugoCanDie`), never a civilian or a scene pawn. Decision: civilians
    do not count (menu, ini and README say so). Dead pawns usually have no
    controller or PRI any more, so the class decides: the real dead
    `YPawn_MarauderPistol` read "squad 7, team 255 (no team, enemy class) ->
    enemy". Measured: a summoned `YPawn_Lugo` (squad byte 1) and a summoned
    `YPawn_Civilian` (squad 7, not a `YPawn_Enemy`), both killed with
    `Suicide` and frozen: "squad member -> not a target" and "no team, not an
    enemy class -> not a target"; a 1.5 m/s sweep and a grip on each: no
    `CORPSE` line, not moved, `GRABLOG` "corpse -1" (only the dummy counted
    as a corpse nearby).
  - **Frozen only once the game has finished.** `m_ragdollIsDisabled` comes
    0.1 s before `FinishKill`'s `DisableMeshUpdates` timer removes the
    `PhysicsAssetInstance` (real run: frozen at 22.54 s, PAI 0 at 22.64 s).
    In that window the corpse is now "being laid to rest by the game" and is
    not touched; it counts as frozen (wakeable) only with the PAI gone, so a
    wake never races the timer.
  - **Frozen corpses by their limbs, not their bounds** (`FrozenShapes` /
    `FrozenDist`): a frozen corpse has no PhysX bodies but keeps its pose
    (`bNoSkeletonUpdate`), so the PhysicsAsset's body shapes are placed on
    its bones (`GetBoneLocation` / `GetBoneQuaternion`, the calls of the pose
    check; bone axes = the body axes, verified by that check), once per
    freeze and cached while it stays frozen ("16 of 16 bodies placed from
    their bones (0.01-0.02 ms)"; every bone within the bounds + 50 cm, or
    the bounds are used as before). The grab zone (cyan corners, grip wake)
    and the push wake (within 10 cm of a limb) use them. Measured: palm in
    the bounds 80 cm from the nearest bone centre: `GRABLOG` "corpse -1", a
    grip there woke nothing (no `CORPSE` line, still frozen, PAI 0); before,
    the whole box was the zone. `corpsetest bones` lists a frozen corpse's
    bodies from its bones.
  - **Pairs restored on every released corpse.** The released corpse used
    to sit in a single slot (`g_corpseComp[1]`); taking a second corpse
    while the first still had ignored pairs with the left sphere made
    `CompNxAll` lose the first one's bodies and its pairs stayed ignored.
    Now `g_corpseComp` is the held mesh only and `CorpseRelease` puts the
    mesh in `g_corpsePairMeshes` (pruned once no pair is left); `CompNx` /
    `CompNxAll` use both (`CorpsePairMesh`).
  - **Real gunfire kill** (`realgun.py`, `cpTutorialFight`, god mode, no
    `Suicide`, no `ucall` on the enemies): Walker stood at the start point
    without shooting and the squad fought the first wave; a
    `YPawn_MarauderPistol` 20 m from Walker died 17.5 s into the fight (the
    squad's fire, most likely gunfire at that range) (special move 53, ragdoll at 20.3 s, frozen at
    22.5 s, PAI removed at 22.6 s), `m_deathDBNOState` 0 throughout; no
    `CORPSE` line during the fight. With `gravitygloves on` (the default;
    the earlier runs had sent `off`): woken by the hand, taken by Spine1
    (5.0 kg of 29.9, palm 0.0 cm), dragged 40 cm back and 15 up: bounds
    42.2 cm, pelvis 43 cm, palm-point distance 2.9 cm on average, 5.7 at
    most, 400 N. Its `HitDamageType` was logged as a class pointer, not
    resolved to a name before the next relaunch. No enemy went into death
    DBNO in this fight (all states 0), so that path is still unmet in the
    game.
  - **`gravitygloves on` on the dummy:** grabbed by Hips (zone grip), dragged
    40.5 cm, no `GLOVE` line.
  - **`corpsedrag off`:** with the dummy held, `corpsedrag off` let it go at
    once ("let go of Hips (corpsedrag off) after 3.50 s", counted as lost);
    then, frozen again, a 1.5 m/s sweep and a grip through it: no `CORPSE`
    line, moved 0.0 cm, still frozen. `corpsedrag on` restored it.
  - **Cost:** `PROPS: tick` 0.163-0.177 ms on average, 0.34-0.71 ms at most
    (one 11.7 ms window right after the level load, as in earlier sessions'
    first windows: 15-19 ms); the bone placement 0.01-0.02 ms once per freeze.
  - **Earlier suites on the final dll** (`f11x/final_out.txt`): grab suite
    (`suite.sh f11x`) grasp power, centre f -2.6 p 5.5 a 6.6 cm, 46 degrees;
    drive settles in 0.025-0.108 s, sweep 1.5 cm; static grab took
    bottle_quad01; glove pulled the bottle 1.8 m in 0.41 s and caught it;
    bottle knocked 44.7 cm, barrel 0.0; drop broke at 5.6 m/s, smash broke in
    the hand; shot moved the radio 143.1 cm; door 61.0 degrees, sign 5.5. Hit
    pipeline (`p4t/hitall.sh f11x_hit`): `hitthrow` head hit 1.6 m/s, release
    5.8 m/s; `heldswing` Spine1 at 2.7 m/s on both fast swings, nothing
    pressed, pulled or pushed slowly; `hitdrop2` Spine3 1.8 m/s; `regrab` 0
    hits. Unit tests 2894/2894.
- **Not verified / open (headset checks):**
  - How it feels and looks: the drawn hand on the limb (the novr screenshots
    show the corpse but the drawn arm cannot reach the ground: in the headset
    the player crouches), corpses dragged under or through Walker (the
    ragdoll does not collide with Walker's capsule in this game: the dragged
    dummy passed under the camera), joint stretching on hard pulls.
  - Push at under 1 m/s: a body in the grab zone is ignored by the sphere
    (to be grabbed), and the corpse's pairs stay ignored while the palm stays
    inside its bounds, so a slow nudge after reaching for a limb does
    nothing until the hand leaves the box or moves faster.
  - Every wake and every rest adds one blood pool decal (the game's
    `CreateBloodPool`), at most one wake per second per corpse.
  - Only marauders were tested (dummy and `YPawn_MarauderSMG`); other enemy
    types use the same `YGamePawn` code, not run (the review fix added a
    `YPawn_MarauderPistol` killed by the squad's gunfire). Death DBNO
    enemies were not met in any run, including the real gunfire fight (the
    code skips them by `m_deathDBNOState`).
  - Dragging a corpse while walking (Walker moving) and over stairs: not run.
  - The numbers are tuned on the fake hand only: the 1.5 m/s point speed,
    the 0.6 m/s push limit, the 40 cm / 0.3 s slip and the x4 force growth.
    `masskg` changes the force limit (the corpse's PhysX masses stay the
    game's).
  - The hint list (final gameplay regression, item 10) has no line for this
    feature yet.

**Final gameplay regression (27/10).** Two agent runs on copy A (14:54-17:43,
then 17:47-19:40). Snapshot before any edit `scratchpad\snap_gameplay_final`
(src, tests, docs, deploy, README, CMakeLists, tools); scripts and outputs in
`scratchpad\gpf\` and `scratchpad\merge_work\`. Final dll `ee9f5475`
(`deploy\Release` and installed); the source has not changed since its 15:41
build, so every result below is on that dll.

- **Copy-B follow-up merged.** These are copy B's four follow-up rounds after
  its final regression:
  - the body comes back under the head (`bodyalign`);
  - the squadmate head-push easing;
  - the M2E2 latch hysteresis;
  - the `necksize` menu limits;
  - the exact LocalToWorld for Walker's bone update and the arm-only
    correction of restored bones (f3/f4).

  The merge was three-way per file, with `merge_work\delta_merge_fu.py`:
  base `scratchpad\B\snap_followup`, "theirs" the snapshot
  `scratchpad\B\snap_followup_end`, never copy B's live tree. control.cpp/h,
  pure_utils.h, vr_menu.cpp, unit_tests.cpp, the ini and README merged
  cleanly. hooks.cpp had 3 conflicts and TECHNICAL.md 1, resolved by hand
  (`merge_work\resolve_fu.py`):
  - ViewportDrawThunk: B's `SnapMeshL2W` block first, then copy A's head
    sampling with the pose time stamp (`g_poseStampMs`, item 4 of the physics
    plan);
  - GUNLOG buffer: copy A's 800 bytes;
  - GUNLOG arguments: B's (`axis`, `lead`, `marker`) for B's format string,
    then copy A's ` | axis ... hiw` tail (item 5 of the physics plan). Readers
    that look for `| axis (` still find copy A's full-precision field;
  - TECHNICAL.md: both new sections kept.

  No configversion change: B added only keys with embedded defaults. Unit
  tests after the merge: 2940 / 0.
- **Copy-B follow-up suites on copy A.** Merged dll `40ed2f50`, built before
  the object hints and the weapon-zone fix; neither touches the head, the
  body or the gun. The tracked-hand body run used the final dll `ee9f5475`.
  - Copy B's fake runtime (`runtime fake openvr 72 45`, `fpscap 72`) went into
    copy A's ini for the series only and was put back byte for byte
    (`merge_work\fu_run.sh`, `fu_run_b.sh`; outputs `merge_work\fu_out\`).
  - B's scripts were copied to `merge_work\fu\` and pointed at copy A's exe and
    folder, with no `SPECOPS_*` variables.
  - `twturn.py` ran with `grabprops on` (copy A's palm spheres, as B's merge
    note asks).

  | Suite | Copy A, merged | Copy B (its last run) |
  |---|---|---|
  | WI-3 crouch | every criterion PASS | PASS |
  | WI-3 seated S1-S9 | 11 / 11 | 11 / 11 |
  | WI-4b two-hand, game path | 19 PASS, 0 FAIL | 18 + the known harness 1b |
  | Body under the head, tracked hands (`fix1_game.py L`) | 1.64 m: 25.59 / 1.76 / 0.48 deg off after 1 / 2 / 3 s; 1.15 m: 25.85 / 1.80 / 0.49; `aim body` and untracked: no turn (42 deg, by design); view still (yaw 0.000, place 0.04 units) | 25.42 / 1.72 / 0.49; 25.71 / 1.76 / 0.48 |
  | Body after a side look, untracked fake hands (`followup_body.py S`) | head stays 42 deg off, no BODY line, view 0.000 | same as B's L4 |
  | Squadmate passing at 30 / 26 / 22 units (`ph_pass_fu.py`) | 0 pull ticks, 0 side-change ticks, push max 4.15 / 8.43 / 10.65 | 0 pull, 4 / 2 / 1 side-change ticks (eased) |
  | WI-2 corner and flat wall | corner gains 0.797 / 0.86, push -0.039 / -0.032, flat 1.000 | 0.797 / 0.863, -0.038 / -0.031, 1.000 |
  | Wall 12-15 units to the side, head 40 deg toward it (`fix2_game.py W`) | `bodyalignwall on`: view 0.000 units, body stays 40 deg off; away from the wall the body turns -39.5 deg in 1.95 s | identical |
  | Turn-with-head, two-handed (`twturn.py on`) | barrel yaw range 0.050 deg, origin 0.04 units, engaged 254 / 254 ticks | 0.055 |
  | Turn-with-head, one-handed + realign (`twturn.py on one` + `spikes4.py`) | barrel 0.057 deg, 0 spikes over 0.1 deg, muzzle max 0.061 units, room-step frames 0.014 / 0.022 units, 0 turning ticks with an invalid snapshot | 0.055, 0, 0.057, 0.014 / 0.014 |
  | Turn-with-head, fpscap 14 (85-ms ticks) | `armslead off` 9.89 deg, `on` 0.046 deg | 9.91, 0.044 |
  | WI-4a gun frame, 5 classes (Gorge02) | 45 / 45 poses with shots, worst first shot 0.058 deg, 0 clipped | 45 / 45, worst 0.132 (f3) |
  | WI-0 aim baseline, headset path | 30 / 30 poses, each within 0.020-0.041 deg of copy B's f3 run, 0 clipped | 30 / 30 |

  No script raised an exception in the series. One difference: the
  restored-bone correction angle (`spikes4` `restore_rot_max`) reached 1.29
  deg on copy A, against 0.0 in B's f4 run, where no restore fell inside the
  turn. The muzzle stayed within 0.061 units on those ticks.
- **Gun-hand rule sweep.** Every `PulseHand` / `HapticSend` and every
  left-hand behaviour added by gameplay items 1-11 was checked against
  `GunHandHaptics`.
  - hooks.cpp and vr_input.cpp gained no pulse from items 1-11. Their new
    pulses are copy B's turret and two-hand pulses.
  - In props.cpp the held-object pulses (`held`, `melee`, `hit`, shield, smash
    in the hand) fire only on the hand that holds the object. That is never
    the gun hand: `GunHandHaptics(0)` means "the right hand holds nothing".
  - The disarm slap and its pulse, and the melee pulse, check
    `GunHandHaptics`.
  - Grenades (item 8) and corpses (item 11) are never offered to the left hand
    while it is on the gun.

  **One violation, fixed:** item 6's weapon zone (`WeaponNearest` for the left
  hand) did not check `GunHandHaptics(1)`. With the left hand on the
  handguard next to a dropped gun, three things went wrong:
  - the zone sent the 15 ms contact pulse to that hand;
  - it set `g_propZone[1]`, which takes LB and the left-grip gesture away
    from the hand on the gun;
  - a fresh grip press there grabbed the dropped gun instead of keeping the
    gun.

  Now `wi` is searched only with `!GunHandHaptics(1)`, as for grenades and
  corpses. The same zone pulse for plain objects is older (stage 1, before
  the gameplay items); it is unchanged and listed as open.
- **Object hints.** The orchestrator made an exception to "no new features"
  for these, because the user asked on 27/10: the tester found features only
  through hints, and `ui_text.h` `kHints` only explained the controls. There
  are six one-time contextual hints in the same toast system.
  - **Texts** (`ui_text.h`, `kObjHint*`): every variant fits the toast (25
    toasts, 0 cut, at most 2 lines; unit test `TestUiTextFits`).
  - **Text per option set:** `ObjHintText(which, options)` returns the text
    for the options that are on, or none.
    - Thrown damage and shield decide the text of the "held" hint.
    - Melee and disarm decide the "enemy near" hint.
    - Gravity gloves decide the second half of the "near" hint.
    - With `grabprops off` there is no hint.
  - **Order:** `kObjHintOrder` puts danger first: grenade, explosive, enemy
    near, gun, held, near.
  - **Step:** the pure function `ObjHintStep(now, gameplay, turret,
    situations, options, shown, lastAny)` shows a hint when all of these
    hold:
    - its situation is on this tick;
    - it has not been shown yet;
    - it has a text;
    - Walker is on foot in gameplay;
    - at least 7 s have passed since the last hint of either series.
  - **Situations** (`props.cpp` `HintSituations()`). They are computed in
    `TickImpl` and reset at its start, so an early return leaves none.
    - `near`: the left hand's grab zone has an object, unless a weapon, a
      grenade or a corpse wins the zone.
    - `gun`: the zone has a dropped gun (item 6's `wi`, so never with the left
      hand on the gun).
    - `grenade`: item 8's red corners are on a grenade.
    - `held`: either hand holds an object.
    - `explosive`: a held object is an explosive carrier (`CarrierOfActor`).
    - `melee`: an object is in hand and a live hostile pawn is within 2.5 m of
      Walker (`GatherPawns` every 0.25 s, only while something is held).
  - **Showing** (`vr_toast.cpp` `HintsTick(gameplay, turret, situations)`;
    hooks.cpp passes `vr::props::HintSituations()`). It runs before the
    controller series, whose "done" check returns early.
    - With `hints auto` the shown bits are the profile's: `objecthints N` in
      the ini, saved with `SaveIniValue` at each new hint, also after the
      series' 3 sessions.
    - With `hints on` they are the session's; with `hints off` there are none.
    - `toasts off` shows none and counts none.
    - A shown hint sets the series' `lastHint`, so the two series stay 7 s
      apart.
    - Log: `HINT: object hint N (situations 0x.., options 0x...)` and the usual
      `TOAST: queued`.
    - `gameplay` is the series' own: no scene, no locked scene, Walker alive,
      not paused. On a turret no hint shows.
  - **Command:** `control.cpp` `objecthints N` (state, like `hintsshown`: not
    in the menu; `vr_menu.cpp` skips it). Text added to the README
    (Interactive objects bullet), to the ini comment of `hints`, and to the
    Basic menu help of "Controller hints".
  - **Unit test `TestObjHints`:**
    - texts per option set;
    - a 72 Hz simulation of both series as in `HintsTick`: near at 2.0 s, held
      at 9.0, explosive at 16.0 and enemy-near at 23.0 while both are on;
      nothing in a scene, on a turret or with `weaponpickup off`; grenade at
      59.0 before gun at 66.0;
    - each object hint shows once, the 7 controller hints are still all
      shown, and the smallest gap between any two hints is 7.00 s;
    - the edge cases of `ObjHintStep` (6.999 s against 7.000 s).
- **The usual set** on `ee9f5475` (`scratchpad\gpf\chain.sh`, output
  `gpf\chain.out`; copy A's own ini, novr). The `objecthints` line was removed
  at the start so the hints could be counted over the whole series.
  - `tools/regress.py`: PASS (max view jump 0.0, fire 0.024 deg, magazine
    inserted, cinema arms back).
  - **Grab suite** (`suite.sh gameplay_final`):
    - grasp identical: power, centre f -2.6 p 5.5 a 6.6 cm, 46 deg;
    - drive settles in 0.058-0.109 s, sweep 1.5 cm;
    - static grab of `bottle_quad01` held 13 s and let go by the grip (no
      "wedged");
    - glove: 1.8 m in 0.41 s, caught;
    - bottle knocked 39.5 cm, barrel not;
    - drop broke at 5.7 m/s, smash broke in the hand;
    - the shot moved the radio 135 cm;
    - door 53.3 deg and stop sign 3.1 deg (see the open item below).
  - **Hit pipeline** (`p4t/hitall.sh gpf_hit`), the same lines as the item
    runs:
    - `hitthrow`: head (closest body) at 1.6 m/s, release 5.8 m/s;
    - `heldswing`: Spine1 at 2.7 and 3.5 m/s; nothing pressed, pulled or
      pushed slowly;
    - `hitdrop2`: Spine3 at 1.8 m/s;
    - `regrab`: 0 hits.

    Haptics: the melee pulses are 70 ms on the left hand, and the held hits
    are muted while settling.
  - Every item's main test once, with its option on and off, on the dummy
    ("on" = the item's own script):

    | Item (option) | On | Off |
    |---|---|---|
    | 1 thrown damage (`propthrowdamage`) | radio 5.6 m/s into the chest (controller + PRI): 80 J, heavy, 40 damage, 75 -> 35, special move 0 -> 9 | the same hit line, no throw damage, health 75 |
    | 2 glass (`propglass`) | bottle onto the coach pane: 76 J -> 40 (light, capped), health 25 -> -15, destroyed | run first on a fresh pane (`gpf/offfix.sh`): the hit line, no glass line, the pane stays; then on, the same pane: destroyed |
    | 3 shield (`propshield`) | 5 dummy shots stopped on the radio (hit 1-5 of 5), knocked out at 1.28 m/s, -0.00 towards the head | 11 shots let through, 0 stopped |
    | 4 melee (`propmelee`) | radio swung at 4.3 m/s: 65 J, stagger, 20 damage, 75 -> 55, knocked down | hit line, health 75, no stagger |
    | 5 explosive (`propexplosive`) | carried propane bottle dropped from 6 m: 16.3 m/s, 51 damage, EXPLODED, dummy 75 -> 0 | the level bottle at the palm, grip closed (`gpf/offfix.sh`): 0 "now carried" lines, nothing taken; on again: carried (13.7 kg) |
    | 6 weapon pickup (`weaponpickup`) | AK grab (timer cleared, 15 s), held 21 s behind the head, hand-over (M4 -> AK, 25+6), fire 5 shots, reload, two hands, M4 regrabbed and dropped, ammo touch 0+19 -> 10+19 | grip on a dropped gun: no `WEAPON:` line, ammo unchanged |
    | 7 fake grenade (`fakegrenade`) | bottle 1.4 m from the dummy: warned (`SendEvent_GrenadeImpact`, reach 6.5 m), special action 114 | 0 fake lines, no bark |
    | 8 grenade back (`grenadeback`) | AI grenade on the ground 1.5 m away, grabbed at 1.08 s, thrown back at 10.1 m/s, exploded 9.1 m away: two dummies 75 -> 0, Walker's kills (`m_killCount 2`) | grip on the grenade: moved 0.0 cm |
    | 9 noise (`propnoise`, default off) | bottle lands 1.1 m from the dummy: `SendHearEvent_Melee`, loudness 1.08, 6.5 m | 0 noise lines |
    | 10 disarm (`disarm`) | left-hand slap 4.4 m/s on the AK: drop (`ServerDropWeapon`), stagger, 75 -> 70, knocked down | the same gesture is a normal melee (`PlayMeleeAttack`, 75 -> 35), gun kept |
    | 11 corpses (`corpsedrag`) | frozen dummy shaped from its bones (16 of 16 bodies), woken by a shove, grabbed and dragged | let go at once (`corpsedrag off`); swipe and grip on the frozen corpse move it 0.0 cm; Lugo and a civilian never targets |

  - **Options from the in-headset menu** (`gpf/options_gp.py`, `vrmenu cycle
    basic:<key> +1`; the menu thread runs only with a VR output, so this used
    the fake runtime). 22 of 22 clicks OK: each of the 11 options went to its
    other state and back, with the VRMENU line, the value written in the ini,
    the game alive and no exception. The ini was put back byte for byte.
  - **Object hints over the series** (fresh profile, `hints auto`): each of the
    six showed exactly once, at its first situation, was saved at once
    (`objecthints` 2 -> 3 -> 7 -> 39 -> 55 -> 63) and did not repeat in later
    sessions.
    - held 16:09:52 (graspdiag's grab; situations near + held, held wins);
    - near 16:10:21;
    - enemy near 16:17:30 (hit pipeline, radio held next to the dummy);
    - explosive 16:28:58 (carried bottle);
    - gun 16:34:55 (item 6, the AK in the left hand's zone);
    - grenade 16:40:00 (item 8, red corners).

    Saving the ini took 1 ms. The controller series was already done in this
    profile (`hintsshown 3`), as for the user. **The series left
    `objecthints 63` in copy A's ini**, which would hide all six hints from the
    user. The second run removed the line at the end, so the ini is again the
    one from before the series.
  - **`PROPS: tick` on average:**
    - level 1: 0.164-0.196 ms per 30 s window over the series, and
      0.161-0.189 ms in the real fights of the second run (peaks of 10-19 ms
      in windows with a `spawnprop`, summon or level-load rescan, as before);
    - Gorge02 (regress): 0.139-0.160 ms;
    - Desert02 (explosive run): 0.152-0.179 ms, at most 0.54 ms;
    - Gorge01 in gameplay: 0.181-0.186 ms, at most 0.65 ms (0.003-0.009 ms
      while the level-start cinematic flag is set).
  - No `SCRIPT: exception` and no crash dump in either run.
- **Exits (must-do 5).** `gpf/exitcrash.py`, output `gpf/exitcrash.out`:
  - 10 normal closes from gameplay: 5 with `grabprops on` (radio held, then
    thrown, closed while flying) and 5 with `grabprops off`;
  - 10 of 10 clean: the "closing" line, exit in 0.8-1.0 s, no dump, no
    WerFault, no CRASH line.

  The single exit crash of 26/10 19:29 (0x006D77D2, FUN_006d77c0, during
  the f2 fix run's relaunch, with a test-only spawned fractured bowl) did
  not come back. No teardown change was made.
- **Real enemies** (second run; level 1 `ce cpTutorialFight`, scratchpad
  `realfight.md`; god mode on; scripts `gpf/realbus.py`, `realthrow.py`,
  `realshield3.py`, `realdrop.py` / `realdrop2.py`; outputs `gpf/rb*`,
  `gpf/rt*.out`, `gpf/rs*.out`, `gpf/realdrop*.out`). All enemies are
  `YAICtrl_Humanoid` with a real `YPlayerReplicationInfo`.

  | Check | Result |
  |---|---|
  | Thrown damage, heavy (item 1) | Radio 6.4 m/s into a `YPawn_MarauderSMG` of the second wave: `105 J -> heavy, 40 damage, health 75 -> 35, special move 0 -> 9`, then 54 with knock bits 0x50000 0.2 s later. He got up after about 10 s. B pressed after that was a normal melee (`PlayMeleeAttack`, 35 -> 1). |
  | Throw knock-down, then B | Radio 6.6 m/s (`Arm_Right`) on a second `MarauderSMG`: 111 J heavy, 40 damage, 75 -> 35, knocked down. B 1.6 s later from 95 units: `execution allowed ... canBeExecutedInDBNO yes`, `SPECIALMOVE: 0 -> 27 (Execute)`, enemy 35 -> 0 in 2.4 s. |
  | Thrown damage, light | Bottle (spawned `bottle_quad01`, 1.7 kg) at 5.7 m/s, `Spine1 (trace)`: `27 J -> light, 7 damage, health 75 -> 68 ... TakeDamage (light)`. The game healed him to 75 within 5 s. An earlier bottle missed twice (cover in the way). |
  | Shield, one isolated real shooter (item 3) | Adams, Lugo and the other marauder held 60 m away (Location rewritten every 0.5 s). Walker 6 m from a `YPawn_MarauderSMG`, suitcase (16.8 kg) at belly height (hand 50 cm below the head, grabbed after the teleport). In 0.32 s, 6 `YWeapon_MicroUzi` bullets hit the suitcase: 4 `stopped by the game`, 2 `redirected from Walker`; at hit 6 of 6 it was `knocked away at 1.04 m/s, direction (0.27, -0.94, -0.22), -0.00 towards the head`. |
  | Weapon pickup from real drops (item 6) | Drops are `YPickup_WeaponSpawnable` (MicroUzi, M9A1), +0x210 = 0x1B45, `m_initTemporarily` 0, no Actor timer, `m_destroyTimer` 15, as item 6 found. A second-wave MicroUzi: `left hand took YWeapon_MicroUzi (... ammo 29+41, 2 physics bodies, mass 1.6 kg ...); no self-destroy timer`, then at 14.6 cm from the right hand `DoPickup done; YWeapon_M4Carbine (31+120) -> YWeapon_MicroUzi (29+41); weapons 2 -> 2`. After 900 ms of fire (29+41 -> 15+41), the left hand touched another Uzi drop: `touch: ammo ... 15+41 -> 15+107 ..., the pickup is empty and gone`. |
  | Sand bus (item 2) | Not done; see below. |

  - **Shield attempts before the one that worked** (all with the shooter
    isolated):
    - suitcase at the chest (25 cm below the head): his 45 hits on Walker all
      landed at belly height (impact z about 127, Walker's centre about 124:
      the crouched marauder's muzzle is low), none on the suitcase;
    - a second try in the same fight: the AI went quiet, 0 shots;
    - two tries with the suitcase grabbed before the 4 s walk at 55 and 80 cm
      below the head: `released (slipped)` during the walk. The heavy
      suitcase carried low slipped out of the hand while Walker walked (hand
      1.1 m/s).
  - **Sand bus with real enemies: not achieved, open.** In
    `cpTutorialFight` the level never armed the two sand-bus panes
    (`m01a_busGlass_warpedWindow_single` and `..._flatWindow_single`,
    `YDestructibleObjectDelayed`, health 60; `m_damagingEnabled` +0x1FC bit
    0x400 stayed off). The runs (4 fight sessions):
    - the first wave alive for up to 180 s;
    - Walker going into cover (A) during the first wave;
    - the second wave alive;
    - 90 s after the second wave died.

    They were not armed at `cpflanking`, `cpCommand`, `cpMelee` or
    `cpGrenade` either. What is damage-enabled is the first coach window
    (`m01a_166_166_single`, health 25), which is armed at every level 1
    checkpoint tried, before any fight. The second wave crouches inside
    that coach.
    - The radio broke that window 3 times of 3 (`157 J -> 196 damage (heavy),
      broken, health 25 -> -171 (Destroyed)`), once with a real
      `YPawn_MarauderSMG` 1.2 m (in plan) under it and three more within 5.6
      m.
    - Nothing happened to them: health 75 for 45 s, no `BusAvalanche`. That
      window is a coach window with no sand, so this is not the avalanche.
    - The avalanche itself was proven by item 2 with the two panes armed by
      hand: both events fired, sand and falling cars, and the next wave
      spawned.

    What arms the panes in normal play (a trigger the scripted Walker never
    reaches, or a tutorial step) was not found. It is a headset check: play
    chapter 1 normally and throw a radio at the sand bus when the game asks
    you to shoot it.
  - **Natural AI grenade (item 8): none seen, open.**
    - Gorge01 `cp02`: a 33rd group (`YPawn_EnemyAssault`,
      `EnemyAssaultAK47`, `EnemySniper`) 53-67 m away, 150 s: no grenade.
    - Gorge01 `CP03` with Walker teleported 15 m from them: 150 s, 0 enemy
      shots (the group stayed idle, special move 37), no grenade.
    - Desert02 `CP1`: no enemy near the checkpoint, and the script's palm
      calibration failed while the level-start cinematic flag was set.
    - Level 1 `cpGrenade`: the level script spawned a
      `YProj_FragGrenadeAINoCollision` with no Instigator. `grenadeback`
      ignores it by design (`GRENADE: ... thrown by nobody (no Instigator)
      (not an enemy): ignored, left to the game`).
    - Together with item 8's earlier 3+ minutes in `cpTutorialFight`, no
      enemy threw a grenade on its own in any run. Natural grenades stay a
      headset check.
- **Explosives in their own level (must-do 6).** `gpf/explvl.py
  SP_Desert02_p - gas_tank`: two `YExpl_PropaneBottle` in `SP_Desert02_A_K`,
  visible. Walker was teleported to the nearest one, which was not moved by
  Python.
  - The left hand took it: `now carried by KActorSpawnable: mass 0.341 units
    ...`, 13.7 kg.
  - It was dropped and shot twice with the M4: `health 50 -> 20, still whole`,
    then `20 -> -10, EXPLODED`; the carrier was gone after the second shot.
  - Scuba tanks (Bridge / ShipsGraveyard): not run (time).
- **Door and stop sign: open, low priority (user, 27/10: "lasciare perdere
  porta e cartello, conta molto di più il gameplay").** The hinge test turns
  less than at the end of the physics plan. The angles, door / stop sign:
  - clean per-session runs today: 43-44 deg / 2.6-2.7 deg;
  - today's grab suite: 53.3 / 3.1;
  - end of the physics plan: 59 / 5.4;
  - the pre-delta dll `92fa3a71`: the same 43.1 / 2.6, so the copy-B
    follow-up merge is not the cause.

  The cause is not bisected (outputs `gpf/hinge*.out`). The investigation was
  stopped by the user's decision.
- **Open / not verified:**
  - **Sand bus:** the avalanche with real marauders (above).
  - **Natural AI grenade:** none seen (above). The throw-back on a natural
    grenade is unproven; on grenades from the AI's own spawn path it works.
  - **Shield:** the 6 real-AI bullets came in one window, from one shooter,
    with the object held at belly height.
    - A heavy object carried low while walking slipped out of the hand
      twice (`released (slipped)`). This is a headset check for the weight
      tuning.
  - **Ammo touch log line:** after taking the reserve, it prints the
    pickup's numbers unchanged (`pickup 27+66 -> 27+66`) next to "the pickup
    is empty and gone". The game's `AddAmmoFromWeaponPickup` took the 66
    reserve rounds, not the 27 in the pickup's magazine, and destroyed the
    pickup. This is cosmetic and the game's rule.
  - **Plain-object zone pulse:** the stage-1 zone pulse for plain objects
    can still reach the left hand on the gun (it is older than the gameplay
    items).
  - **Restored bones:** copy A's `restore_rot_max` reached 1.29 deg
    (muzzle within 0.061 units).
  - **Shoulder seam:** one tick after a room step (copy B's known open
    point).
  - **Door and stop sign:** above.
  - **Not run on real AI in this regression:** item 4 melee, 7 fake grenade,
    9 noise, 10 disarm and 11 corpses. Each item's own run already checked it on
    `cpTutorialFight` (sections above).

## Issue #4 "jitterfest": posed submit when needed (29/10)

Tester report (issue #4, also on Discord; the same tester's issue #2 had "camera
overall is too jittery" and "world scale constantly changes"): constant jitter,
unplayable after a few minutes, lowering the graphics settings did not help. Not
reproducible on the developer's PC.

- **Cause.** With `submitpose off` the eyes are drawn with the head pose the mod
  predicts (`predict auto`, the measured head-to-photons time), but the layer
  declares the pose the runtime predicted for the frame it is submitted in
  (OpenXR: `xrLocateViews` at that frame's `predictedDisplayTime`; OpenVR: the
  pose of `WaitGetPoses`). The compositor reprojects from the declared pose, so
  every difference between the two shows as the world turning by it, and the
  difference changes every frame. While the game holds the headset rate the two
  predictions nearly agree; as soon as it runs below the rate, frames land one
  vsync later than predicted and the error becomes a whole frame of head motion.
  That is why a fast PC does not see it and lowering the graphics does not help
  when the game is CPU bound.
- **Reproduced** on the fake runtime (`runtime fake openxr 90 45` and `openvr 72
  45`, `mode hmd`, `fake preset b2`, `fpscap` to play a slower PC; scratchpad
  `slowpc.py`). World step = frame-to-frame change of the world as displayed:

  | runtime, game fps | `submitpose off`: world step rms / p99, frames over 0.1 deg | `submitpose on` |
  |---|---|---|
  | openxr 90 Hz, 90 fps | 0.069 / 0.242 deg, 3% | 0.004 / 0.007 deg, 0% |
  | openxr 90 Hz, 80 fps | 0.302 / 1.231 deg, 57% | 0.004 / 0.007, 0% |
  | openxr 90 Hz, 60 fps | 0.385 / 0.728 deg, 90% | 0.004 / 0.007, 0% |
  | openxr 90 Hz, 45 fps | 0.558 / 2.257 deg, 55% | 0.004 / 0.007, 0% |
  | openvr 72 Hz, 72 fps | 0.298 / 1.021 deg, 34% | 0.004 / 0.007, 0% |
  | openvr 72 Hz, 60 fps | 0.338 / 1.213 deg, 81% | 0.004 / 0.007, 0% |
  | openvr 72 Hz, 45 fps | 0.459 / 1.173 deg, 96% | 0.374 / 1.821, 4.7% (repeated frames) |

  Render pose vs declared without the pose: 0.34-0.79 deg rms, 0.5-1.1 mm.
  The cost of the posed submit is the reprojection edge: p99 1.1-1.8 deg against
  `xroverscan 1.5` per side on OpenXR (thin black slivers at most, at 45 fps).
- **Fix: `submitpose auto`** (new default; `on` / `off` force it), `configversion
  13`: `submitpose off` -> auto for inis still at the old default
  (`kConfigVersion` is now 13, separate from `kResetConfigVersion` 12). In
  `auto` the output thread estimates, every frame, how much the world would
  move without the pose: the change from the previous frame of the rotation
  between the render pose and the runtime's pose for the frame
  (`vr::pure::SubmitPoseAuto`, unit-tested). A constant lag does not show; its
  changes do. It switches to posed frames when the slow rms (~250 frames) is
  over 0.20 deg, the fast rms (~10 frames) over 0.60 deg or one step over 1 deg,
  and back to plain frames after 5 s with the slow rms under 0.15 deg.
  `g_submitWithPose` stays on in auto, so the OpenXR overscan (and the declared
  field of view) never changes with the switches. `SUBMITPOSE:` lines log each
  switch; `POSEAGE` ends with the state and the estimate.
  - Calibration (fake openxr 90 Hz, `preset b2`): the estimate reads 0.06-0.20
    deg at 90 fps and 0.23-0.39 deg at 60 fps (true world step without the pose:
    0.05 and 0.38 deg rms). Thin margin: thresholds 0.12 and 0.40 kept it on at
    90 fps.
  - Final run: 90 fps -> plain frames after the start-up spike (world step
    0.049-0.050 deg rms, as before); 60 and 45 fps -> posed within a second
    (0.004 deg); back at 90 fps it stayed posed for the 30 s measured (errs on
    the safe side).
  - `posed` was turned off on 19/10 for "black bands and fisheye" on Link; that
    was before `predict auto` (the render pose was then ~53 ms old, so the
    compositor reprojected by a whole latency) and on the same day as the
    inverted projection fix.
- **Headset checks.** On Link and on VD/VDXR: no black bands or warping when
  turning the head fast with posed frames; `SUBMITPOSE` lines in a real
  session. On a fast PC the jitter can be provoked with `fpscap 45` +
  `submitpose off`.

### Same tester, 29/10: menu, pause menu, HUD

- **Mod menu descriptions cut** (one line with an ellipsis; Advanced help also
  cut at 160 characters): the panel is now 1000 px high (was 820, same 1.5 m
  width); under the list a box shows the full description of the row under the
  laser, and that row is highlighted. `vrmenu dump <bmp> basic@N` draws it with
  the laser on row N.
- **Pause menu "too close and to the right"**: the pause screen (Scaleform)
  does not go through the HUD's canvas matrix, so in 3D it was the same pixels
  in both eyes. `WorldInfo.Pauser` set now also asks for the virtual screen
  (log: `PAUSE: game paused.` then `CINEMA: virtual screen`; content rows
  356-1085 of 1440, the 16:9 band). The game's own layout keeps the items on
  the right of that band.
- **Weapon and ammo icon at the edge**: `hud 0.4 100` -> `0.3 100` (default and
  configversion 13 migration); the Basic menu also offers 0.25. Smaller HUD
  text is the trade-off; the Wrist HUD is the other option.

## Headset session fixes (27/10 evening)

The user's headset session after the final gameplay regression (Quest, Virtual
Desktop's VDXR runtime, 120 Hz). Items 1-3 are code changes (item 3 with new
TURRET diagnostics); item 4 is a game finding, 5 a troubleshooting note, 6 a
testing lesson and 7 a test-key cleanup. The code is in hooks.cpp (items 1
and 3), props.cpp (item 2), pure_utils.h (`vr::pure::TurretHandles`),
control.cpp / control.h and the ini (`turretpivot`) and the unit tests. Unit
tests: 3002 passed, 0 failed (`build\Release\unit_tests.exe` of 27/10 22:05,
with the 0.10.0 settings reset: `TestConfigReset` and `kToastSettingsReset` in
`TestUiTextFits`).

- **1. Dynamic resolution: the two eyes match again** (hooks.cpp,
  `constexpr int kGeometryPercent = 100`).
  - **Symptom** (user, VDXR at 120 Hz): double vision and two aim markers.
    At 120 Hz the 8.3 ms frame made dynres step between 85 and 100% all the
    time.
  - **Cause:** in gameplay the game upscales the reduced `ScreenPercentage`
    scene to the whole back buffer. The mod still cropped a centred box of
    that percentage and scaled the aim marker, the HUD canvas and the boxes
    by it. Each step zoomed the image around the buffer centre, which differs
    per eye with the off-centre projection, so each eye was zoomed
    differently.
  - **Fix:** the eye crop (vr_output.cpp), the aim marker, the HUD canvas,
    the highlight and squad-target boxes, the laser, the scope bars and the
    vignette always use `kGeometryPercent` (100). The frame tag carries 100,
    and a Present without a tag right after a change is no longer dropped
    (hooks.cpp passes `cropUnknown = false`; WI-1's `VR-OUTPUT: pair without
    frame tag ... not sent` no longer appears). The real percentage stays in
    `g_screenPercent`, for the GPU queries, the controller and the `GPU:` /
    `DYNRES` lines.
  - **Proof:** back-buffer captures in the headset at 75% and at 100% show
    the same framing.
  - **Why the tests missed it:** `dynres test` compares only the largest
    `DrawIndexedPrimitive` viewport with the percentage, so it never saw the
    game's final full-screen upscale pass. See item 6 and "Rendering" >
    Dynamic resolution.
- **2. Gravity gloves also pull dropped guns** (props.cpp `GloveTick`,
  `WeaponPickUsable`, `WeaponMakePhysical`, `WeaponTryGrabPick`,
  `WeaponGoneWhy`, `GloveTargetUsable`). User: "la raccolta delle armi non
  avviene coi guanti gravitazionali" (guns are not picked up with the gravity
  gloves).
  - **Targets:** dropped guns (`YPickup_WeaponSpawnable`, item 6's
    `g_weapons`). The scan radius around Walker went from 4 m to 8 m (the
    gloves' reach), still at most 32. Not the gun already in the left hand
    (`g_wpnComp[0]`), and only pickups that are still usable
    (`WeaponPickUsable`: live, not hidden or deleted, not `m_pickupHidden`,
    still with the same `m_pickupMesh`). Only with `weaponpickup on` (off: no
    gun is scanned, the gloves pull objects only).
  - **Gun-hand rule:** guns are never targeted while the left hand is on the
    gun, two-handed or in the reload (`GunHandHaptics(1)`), as in item 6's
    zone. The glove is the left hand's only.
  - **Aiming:** the same as for objects: `AimScore` (controller ray or palm
    normal, minus the apparent radius, up to 8 m), the 16-degree cone, kept
    up to 26 degrees unless another target is 5 degrees better, the light
    tick on a change and the same corner highlight (cyan, gold once locked).
  - **Lock (grip press while aimed):** `WeaponMakePhysical` calls
    `ConditionalReactivatePhysics` if `m_physicsDisabled` (+0x210 bit 0x4) is
    set, `StartPhysic` if `Physics` is not `PHYS_RigidBody` or there is no
    root body, then always `SetComponentRBFixed(false)`. The re-created
    PhysicsAssetInstance has fixed bodies that `SetRBLinearVelocity` does not
    move (first try, 27/10: commanded 23-36 m/s, the gun stayed put). Then
    the maximum angular velocity is raised, a 35 ms pulse, and `GLOVE: left
    locked <pickup> (weapon <class>)`. If the gun cannot be made physical:
    `GLOVE: left could not make <pickup> physical`, nothing is locked.
  - **Flick and flight:** the objects' model (hand towards the catch point
    faster than 1 m/s or up faster than 1.2 m/s; flight 0.35-0.8 s on an arc
    recomputed every tick, at most 25 m/s; Walker's bodies ignored). The
    velocity is set with `WeaponSetVel` (angular velocity zero, then
    `SetRBLinearVelocity` added to every body): no spin.
  - **Arrival** (bounds centre within 1.5 x `grabzone` of the catch point in
    front of the palm): with the grip still held it is caught in flight;
    otherwise it floats in front of the hand for 0.6 s and a new grip press
    catches it. The catch is item 6's
    grab (`WeaponTryGrabPick`): if Walker already has that gun and its ammo
    is not full, the ammo is taken; otherwise the gun goes into the left hand
    (`WeaponGrab`); from there item 6's hand-over gives it to the gun hand
    when the player brings it within about 15 cm of that hand for 0.1 s (not
    automatic). Lines
    `GLOVE: left caught it in flight` / `... while floating` / `... could not
    take it ...`.
  - **Not caught:** a gloved gun just drops (`not caught, dropped`; `flight
    timed out` after 2 s). Unlike an object it is not added to the hit
    pipeline's falling list (`FallingAdd`).
  - **Put to rest by the game:** the game sets a resting pickup back to
    `Physics` none a few tenths of a second after it stops. While a gun is
    locked, flying or floating and its `Physics` is not `PHYS_RigidBody`, it
    is made physical again, at most every 0.5 s (`Glove::remakeAt`): `GLOVE:
    left weapon put to rest by the game, made physical again` (on failure
    `weapon could not be made physical again` and the glove lets go).
    `GloveTargetUsable` therefore asks only `WeaponPickUsable` of a gun, not
    a rigid body: in the headset 0.4 s between lock and flick was enough for
    the first flight tick to find the gun at rest ("target gone"), and the
    collision flags changed for the flight were not restored.
  - **Churn fixed the same evening:** the first version re-activated the gun
    whenever `m_physicsDisabled` was set. That bit stays set after the
    reactivation, so `ConditionalReactivatePhysics` ran every tick (120 a
    second) and re-created the gun's PhysX bodies. The code comment of that
    fix links the churn to enemy ragdolls vanishing and to the hand spheres
    being re-created every 2 s; the later check of item 4 found a game cause
    for the vanishing corpses, and the spheres are still open (item 4).
  - **Why a gun target is lost** (`WeaponGoneWhy`, line `GLOVE: left weapon
    gone: ...`): `the pickup was destroyed`, `the pickup was hidden or
    deleted`, `taken by the game (same gun as Walker's: ammo on contact) or
    back in its pool`, `the game reused the pickup for another drop (new
    mesh)`; anything else stays `target gone`. Added after three "target
    gone" lines in one headset fight, one of them without a flight.
  - **Locked targets kept awake:** while locked and waiting for the flick,
    `Wake` is called on the target's component every tick. A
    `KActorFromStatic` that falls asleep reverts to static (the game makes it
    a `StaticMeshActor` again), and the flight then found it gone (headset: a
    monitor locked again after being let go).
  - **Measured in the headset:** an AK47 and a sawed-off shotgun pulled from
    1.7-3.8 m, caught and handed over to the gun hand.
- **3. Ground turrets: two-hand pivot, like a real mounted gun**
  (pure_utils.h `vr::pure::TurretHandles`; hooks.cpp `TurretGripStep`).
  - **Why:** the user held the controllers upright, 8-25 cm apart, without
    the grips. The handlebar (both grips, palms 25-90 cm) never engaged, and
    the one-hand aim from the gun hand's pointing ray swung by tens of
    degrees with wrist roll ("devo fare una X tra i due controller per andare
    a destra": I have to cross the controllers into an X to go right). A
    first fix aimed with the orientation of both hands; the user: "gli assi
    sono invertiti" (the axes are inverted): the user expected a real
    mounted gun, with the handles behind the pivot.
  - **When:** Turret grip resolves to NORMAL, on a ground turret (not the
    helicopter minigun, `g_flightGunHeld`), with the support hand tracked and
    the palms 3-90 cm apart. No grip is needed.
  - **Model:** at engagement a pivot P is placed `turretpivot` cm in front of
    the palms' midpoint, along the aim of that moment, so nothing jumps. The
    aim is then the direction from the palms' midpoint to P: hands left ->
    barrel right, hands down -> barrel up. Only the palm positions count, not
    the wrist rotation. With the midpoint within 5 cm of P the last aim is
    kept. The weight blends linearly in 0.15 s to and from the one-hand aim.
    The mount limits and the yaw latch then apply as before.
  - **Re-anchoring:** on release P is dropped and placed again at the next
    engagement. On an absorbed runtime recenter in which the controllers
    jumped with the space (the test the WI-4b solver and the handlebar use),
    `Reanchor(yaw)` turns the held aim with the yaw reference and places P in
    front of the hands in the new space. Reset when the turret ends or the
    grip leaves NORMAL.
  - **Handlebar:** now only on the helicopter minigun (`in.allowed =
    g_flightGunHeld`), unchanged there (both grips, palms 25-90 cm). The
    near-vertical yaw hold (`TurretYawHold`) is skipped while the pivot has
    weight, as with the handlebar.
  - **Option `turretpivot N`:** cm, default 35, clamped to 10-80; shorter =
    more turn per cm (10 cm of hand travel at 35 cm turns the barrel about 16
    degrees). An ini line with its comment, so it is in the Advanced menu
    automatically. It is not in `MenuStepFor`'s table: the menu steps it by
    1 cm without the 10 / 80 bounds, so a click past a bound writes e.g. 81
    into the ini while the command keeps 80 (the `necksize` case of the
    follow-up).
  - **Log:** the TURRET line's first field reads `grip two-hand handles`;
    `TURRET: two-hand handles on|off (palms N cm apart; pivot N cm in front
    of the hands, aim from it yaw ... pitch ... deg, gun hand alone yaw
    ...)`. With NORMAL every TURRET line now ends with `| hands: gun frame
    ypr (...) gun hand raw ypr (...) support raw ypr (...) palm gun (...)
    support (...) cm`: yaw / pitch / roll in degrees of the gun frame and of both raw
    controller poses, palms in cm in tracking space, `(no)` before the
    support values when that hand is untracked.
  - **Unit test `TestTurretHandles`:** engagement at +10 deg without a jump;
    hands 10 cm left -> barrel 24-28 deg (10 + about 15.7); hands 10 cm down
    -> 14-17 deg up; wrist rotation ignored; release back to the one-hand aim
    within 15 ticks at 90 Hz, steps under 2.5 deg; re-anchor after a
    recenter without a jump; midpoint within 5 cm of P keeps the last aim.
  - **Open:** no headset result for the pivot version is recorded here.
    Engagement is by distance only (no grip), so a free hand resting within
    90 cm of the gun hand can also move a ground turret. The NORMAL turret
    texts that still described gripping and handlebars were changed for
    0.10.0 (item 8): toast "Turret: move both handles to aim", hint "Turret:
    both hands on the handles, move them to aim like a real machine gun. One
    hand alone also aims", the Controls line and the Basic *Turret grip* help.
- **4. Finding, not a mod bug: corpses vanishing in one fight.** In one
  level section corpses and their guns vanished at the moment of death.
  - **The game's limit** (`YGameInfo`):
    `m_maxAllowedDeadRagDollPawnsConfig` +0x540 (25),
    `m_maxAllowedDeadRagDollPawnsCurrent` +0x544,
    `m_ragdollDestructionDistanceThreshold` +0x548,
    `m_minTimeBeforeDeadBeforeDestroyed` +0x54C (3 s).
  - **The native code:** `ManageDeadPawns` at 0x00E08550 (`YGameInfo` vtable
    +0x3EC; exec at 0x005EF3A0; decompiled with `tools/ghq.py`). Current = -1
    means "take Config". It counts the entries of a global array at
    0x01506F48 (count at +4; here 10 `YCheapShooter` background shooters)
    plus the dead ragdoll pawns. When the count reaches Current it destroys
    the oldest eligible entry of that array (virtual +0x3C0), otherwise the
    dead pawn it picked.
  - **What happened:** in that section Current was 1, set by the level, not
    by the mod: no native writer of +0x544 was found, and the mod never
    touches it. So corpses and their guns vanished at death: vanilla
    behaviour there. Writing 25 back by hand (a test, no code change) made
    them stay.
  - **Open:** in that section the mod's hand spheres (`KActorSpawnable`)
    were re-created every 2 s. The cause is probably a game cleanup, not
    confirmed. A back-off is still to do (today a lost sphere is re-created
    at once and a failed creation is retried every 2 s, `ProxyTick`).
- **5. Controller drops (troubleshooting).** VDXR reported the interaction
  profile of both controllers as none for about 10 s, so no stick, buttons or
  poses reached the game. The log shows `OPENXR: right hand profile none.`
  (`RefreshProfiles`, one line per change and hand; a hand without a profile
  counts as disconnected) and `INPUT: hand devices: left none, right none`.
  When a controller comes back the OPENXR line names its profile again. Not
  a mod fault; likely causes: the Quest's automatic switch to hand tracking,
  low batteries, or controllers out of the cameras' view.
- **6. Testing lesson.** The novr and fake-runtime suites never run
  `src/vr_openxr.cpp` (the fake runtime provides the OpenVR interfaces
  itself; SteamVR and OpenXR are not used) and never check what each eye
  shows. The dynres bug passed a full regression and failed in the first
  headset minute. A per-eye check is needed: each eye's submitted image
  compared across ScreenPercentage steps and between the eyes, with the
  marker on the same target in both.
- **7. Test-only keys.** Earlier that evening the line `fpscap 120` (a test
  leftover) was removed from the user's game ini. `fpscap` and the
  `specopsvr_keepfocus` file exist only for tests: see "Developer commands".

- **8. Release 0.10.0 and the in-headset texts.** Version 0.10.0
  (`src/version.h`) resets an older `specopsvr.ini` once to the shipped
  defaults and keeps a copy (see "Settings reset (0.10.0)" under the
  settings migration notes at the top; `configversion 12`). The in-headset
  texts that still described ground turrets as handlebars were changed with
  the pivot:
  - `kToastTurretNormal`, `kHintTurretNormal` and the Controls page line
    (`ui_text.h`);
  - the Basic *Turret grip* help (`vr_menu.cpp`);
  - the `TURRET: on` log line (hooks.cpp: "pivot like a real machine gun" on
    ground turrets, "both grips held = handlebars" on the helicopter
    minigun).

  Also changed:
  - the *Gravity gloves* help mentions dropped guns;
  - the *Reload* help says what `reload simple` really does (left hand on
    the magazine, then pull down; X always reloads);
  - `turretpivot` has its Advanced-menu bounds (`MenuStepFor`, 10-80, step 1).

## Tester VR fixes (25/10)

Tester report (Virtual Desktop with VDXR, and Steam Link): turret aim with
both hands, camera jitter, world scale changing, crouch with no camera drop,
torso always in view when seated, gun angle off, two-handing broken. The work
is split into items WI-0..WI-4; this section grows with each of them.

### WI-0: foundations (no change for players)

Tools to measure the seven bugs without a headset, and the shared math the
fixes need. Nothing a player sees changes, apart from the step of the
Advanced menu.

- **Exact pose angles** (`vr::pure::ExactEuler`, `src/pure_utils.h`):
  pitch = asin(forward.y), yaw = atan2(-m02, m22), roll = atan2(-m10, m11)
  of the OpenVR matrix. With these three angles UE3's `FRotationMatrix`
  rebuilds the pose exactly (unit test: 20000 random head/controller poses,
  max error 0.00007 deg). `YawFromOpenVrPose` (now `vr::pure::BlendedYaw`)
  blends in the head's up axis to stay stable when looking straight down; it
  is not the exact yaw when the head or controller is rolled: 4.46 deg at
  pitch -30 / roll 10, 9.33 deg at -20 / 30 (unit test "yaw baseline"). The
  view and the aim still use it in WI-0; WI-1 (head) and WI-4 (controllers)
  switch to the exact angles.
- **Palm of the Touch controllers** (`vr::pure::PalmOf`): the `openxr_grip`
  component of SteamVR's `oculus_quest2_controller_*` and
  `oculus_quest_plus_controller_*` models (origin (+-0.007, -0.00182941,
  0.1019482) m, x positive for the left hand, `rotate_xyz` 20.6, 0, 0) is the
  palm centre in the raw frame. The same constants (`vr::pure::GripInRaw`)
  now build the OpenXR backend's grip -> raw transform (`GripToRaw`). With
  `openxrgrip off` the OpenXR pose already is the grip, so the offset is zero.
  Unit test: within 0.0003 mm of the JSON values and of the OpenXR chain.
- `vr::pure::OneEuro` (1-euro filter), `WrapPi`, `PoseFromEuler`,
  `UeAxesFromRotator`/`UeAxesFromPose`, `PoseAngleDeg`, `QuatFromPose`.
- **Computed migrations**: `MigrateConfigLines(..., computed, n)` takes, besides
  the fixed "from -> to" lines, `ComputedChange {version, key, fn}`: `fn`
  gets the old line and returns the lines that replace it (several, or none),
  e.g. `aimpitch -24` -> `holdmode gunstock` + `stockangle 15.4` (WI-4a).
  `FormatConfigNumber` writes the numbers. The configversion stays 9: the
  single bump to 10 comes with the first changed default (WI-2..WI-4), all
  in one `kChanges`/`kComputed` list in `control.cpp`.
- **Menu steps** (`vr::pure::MenuStepFor`, `StepConfigNumber`): each numeric
  key has its own -/+ step in the Advanced tab. Before, the step came from
  the value: `aimpitch 0` moved by 0.05 degrees per click and `openxrgrip 20.6`
  by 1 degree, rounded to 21. Angles now move by 1 degree (`aimpitch`,
  `stockangle`, `gunangle`, `gunyaw`, `armrot`), centimetre offsets by 0.5
  (`armoffset`, `gunoffset`, `magwell`), `openxrgrip` 0.5, `turnwithhead` 5,
  `turnspeed` 10, `height` 0.01 m, weights and throw values by their own
  steps. Keys not in the table keep the old rule, step taken from the value
  when the menu is built (0.05 under 1, 0.1 up to 10, 1 above) and kept for
  every click, so `-` then `+` comes back to the start value (taking it
  from the value at each click gave 10 -> 9 -> 9.1).

#### Test commands (fake poses, `mode synth`)

- Synthetic time now comes from QPC (`ProfMs`), not `GetTickCount64` (15.6 ms
  steps): the fake head sway is smooth enough to be a jitter source.
- `synthctrl [left] <yaw> <pitch> <roll>` / `synthctrl [left] off`: the gun
  hand (or the left hand) becomes a raw controller matrix with these angles
  (degrees, relative to the head yaw reference) and goes through the
  headset's aim code (`ApplyGunControllerAim` / `ApplyLeftControllerAngles`,
  `aimpitch` included) instead of `synthaim` / `synthleftrot`. It is the
  physical controller pose, as a runtime reports it: `yawsign`, `pitchsign`
  and `rollsign` apply afterwards, as for a real controller (with `yawsign
  -1`, `synthctrl 20 0 0` aims 20 degrees left). Example with `aimpitch
  -24`: `synthctrl 20 -10 0` gives aim (20, -34) deg; `synthctrl 0 -30 10`
  gives yaw +2.6 deg where the true barrel yaw is -6.8 deg (the 9.5 deg
  sideways error of bug 6).
- `synthheight <m>`: the fake standing head height (`height auto` eye height
  as with the headset; with the fake runtime it moves the fake head). `0`
  switches it off.
- The synthetic branch now runs the turret step (`TurretStep`, since WI-1
  `TurretAimStep`, shared with the headset branch): before, turrets ignored `synthaim`. On a turret the
  PlayerController pitch is pulled to the `synthaim` pitch every tick (0 by
  default, the stick's pitch no longer sticks) and, with `synthcomp off` and a
  `synthaim` yaw other than 0, the turret keeps turning (each body turn is
  not taken off the aim). Turret suites set `synthcomp on` and `synthaim 0 0`
  before the scene. `synthcomp` compensates every body turn, the helicopter's
  own rotation included; the headset path compensates only the turret step
  it performed, so the turret reference baseline runs in `mode hmd` on the
  fake runtime.
- `simevent zeropose`: a runtime recenter. With the fake runtime the runtime
  itself re-zeroes its space under the head and sends
  `VREvent_StandingZeroPoseReset`; with SteamVR/OpenXR only the event is
  simulated (`OnZeroPoseChanged`). Ignored without a VR session. Since WI-2
  the mod absorbs the runtime's pose jump instead of recentering (see WI-2).
- `headtrace <0|1|2>[+4]` (WI-2 review fix 1): the head-collision trace. 1 (default) = one pass without
  0x200, pawns included; 0 = the WI-2 two passes; 2 = both passes with pawns; +4 = the wall pull and low probes
  with TraceLineView's old flags (before/after tests only).
- `fppivot <0|1>` (WI-2 review fix 1): the eye pivot correction (first-person offset on the Pawn's current
  yaw); 0 only for before/after tests.

#### Diagnostic lines

- `viewlog N`: one `VIEW:` line per eye (0, 1) for N stereo ticks, counted on
  the right eye; the game's own non-stereo view calls (eye -1) are not logged
  (before they used N up: `viewlog 700` gave ~500 ticks). `key=value`:
  `tick`, `eye`, `t` (ms), `mode`, final `loc`/`rot`, `body`/`bodyp` (tick body
  yaw and pitch), `bodyofs` (body used to rotate the head offset), `head`
  (yaw, pitch, roll units), `ho` (head offset f/r/u units), `ref` (head yaw
  reference, rad), `posref` (m), `hmd`/`hmdq`/`hmdp` (headset pose of the tick
  as quaternion w,x,y,z and position), `sample` (ms), `base`, `pawn`,
  `pawnyaw`, `fp`, `eyeh` (ViewHeightExtra), `eyeauto`, `wall` (wall pull),
  `headt` (head collision t), `lean`, `eyeoff`, `firm`, `turret`, `crouch`
  (GameCrouchDrop), `seated`, `pc` (PlayerController pitch, yaw), `baseyaw`
  (yaw of the Pawn's Base, e.g. the helicopter), `fpyaw`/`fpfwd` (yaw and
  forward offset ApplyFirstPerson used for the eyes); since WI-1 `bodyhs` (the
  body yaw at this tick's head sample: equal to `body` unless the body changed
  between the head sample and the eye draws) and `forced` (the vignette's
  forced-motion value).
- `bodylog N [bones]`: one `BODY:` line per tick (after the right eye): the
  eyes and Walker's bones in the Pawn frame (forward, right, up), each bone's
  angle below (`v`) and beside (`h`) the gaze and whether it is inside the
  frustum (`in`, tangents of the left eye's projection, or the game FOV
  without a headset), plus mesh translation, `m_meshTranslationOffset`, mesh
  lowering, crouch drop, posture, firm ground. Default bones: `Neck head
  Spine3 Spine2 Shoulder_Right Shoulder_Left Leg_Right Leg_Left Hips`
  (Walker's skeleton has `Root Hips Spine1-3 Neck head Shoulder_* Arm_*
  Ellbow_* Hand_* Leg_* Knee_* Ankle_*`, `armbones` lists them; `head`
  coincides with `Neck` because the head is scaled to 0).
- `twohandinfo`: one `TWOHAND:` line: the drawn support grip as `synthleft`
  centimetres (with `armoffset 0 0 0` the fake left hand then sits on it),
  the left hand, its distance from the grip, the right hand, and the last
  two-hand aim test (cm along the barrel, cm off the line, limit, weight).
  Since WI-4b: `synthleft` puts the left PALM on the barrel grip with the
  current left rotation; the aim test is the solver's (palm from the barrel,
  engage / release), followed by `| solver: ...` (mode, engaged and how, veto,
  correction and two-hand target in degrees, palms and barrel grip in body cm,
  segment, stock mount distance, and the palm's distance from the barrel of the
  drawn gun). Review round 1 appends: the distance weight, `near the gun` (the
  flag the gestures use) with its distance, `fading`, the held cant, the drawn
  left hand shift (cm, gun axes) and `drawn gun grip at synthleft` (the grip of
  the drawn gun, leftover included). With the live left rotation in the model,
  `synthleft` puts the visible left wrist on the drawn grip for any rotation.
  Review round 2 appends `| departure N deg (gun hand N), hands apart N deg, past
  the barrel end N cm, limit <departure|hands apart|barrel end|->, regrip needed
  N`; "distance weight" is now the applied two-hand weight.
- `TURRET:` twice a second, plus a line in every tick where the step or the
  view lead is over 1 deg or `refused` is over 0.1 deg (at most 16 extra
  lines per half second); the line ends with `logged 2 Hz` or `logged on
  change`. (Before, only the 500 ms limit on `GetTickCount64`, the same
  clock and period as the `specopsvr_cmd.txt` poll: in command-driven
  suites both flicks landed on logged ticks by phase lock, while with real
  or scripted motion the step tick was usually missed.) Fields: grip mode
  (one-hand only today), target yaw/pitch from the body, PlayerController
  yaw/pitch, the step of this tick
  (gain, dt), `refused` = the part of the previous step the game took back
  (limits), net of the Base rotation, and `moved back` = how far the
  PlayerController yaw moved back since the last write, Base rotation
  included; the Base yaw and its change (helicopter); `view lead` = how far
  the body yaw (`g_bodyYaw`) moved after this tick's head sample, which the
  view shows before the head reference compensates it on the next tick (the
  b1/b2 lead; 0 since WI-1 writes the turret yaw without bumping the body).
  Since WI-1 the line ends with `executed` (the part of the previous step
  taken off the view), the Base factor read at PC+0xBB0 and `view body vs PC
  at the tick start` (0 = the view body already holds the step).
  The game carries the PlayerController yaw with the Base 1:1: in the
  prologue helicopter, with the turret settled, `moved back` = -`delta`
  exactly (+0.599 / -0.599, +0.055 / -0.055 deg), so `refused` is 0 there
  (WI-1's "base yaw x PC+0xBB0" factor is 1 on this mount). `TURRET: on
  (...)` on entry; `TURRET: left, ...` on exit.
- `SHOT:` (`firelog on`) ends with `| weapon <class> | hit <object> held
  <0|1> | muzzle clip <object|none|n/a> held <0|1> pen <units>`: the weapon
  that fired, the object hit (by name; `held 1` = `props::IsHeldActor`, a
  held object or one of the hand spheres), and the obstacle that stopped the
  shoulder -> muzzle trace of `MuzzleStart` (the shot then starts in front
  of it; `n/a` = MuzzleStart not used for this shot, e.g. a pass-through
  continuation) with its penetration. Diagnostics only: the traces are the
  same as before (`TraceLineActor` is `TraceLine` that also returns the
  actor). The fields are at the end, so the existing parsers (weapontest,
  regress, shotreport) read the line as before.
- `tools/viewcheck.py`: parser for VIEW/JITTER lines: `rendered_vs_hmd`
  (the drawn view brought back to tracking space, Yaw(ref - body) *
  R(rot), against the headset pose of the tick), `world_yaw`, `stats`,
  `parse_jitter`; `python tools/viewcheck.py specopsvr.log --last N` prints a
  summary.
  - Position (bug 3, WI-2 criteria): `pos_residual` = `loc - eyeoff - (pawn
    + fpfwd * dir(fpyaw) + R(bodyofs) * ho)` per tick, in the body frame
    (forward, right, up): constant for a view that is Walker's centre + the
    first-person forward + the head offset (forward/right ~0, up = eye height
    over the Pawn centre); its changes are the view moving without the head or
    the Pawn (wall pull, head collision, eye-height clamps, crouch, eyesmooth,
    base snaps). `pos_report` per axis: median, spread, largest tick step,
    rms of a 2 Hz first-order high-pass (`highpass_rms`) and the gain of the
    view against the head offset (least squares, 1 = 1:1; none when the head
    moved less than 0.5 units rms). `log_counts`: `HITCH:` lines and the
    longest, `DYNRES:` scale changes, `FADE:` blinks.

#### Fake runtime (`runtime fake`, `src/vr_fake.cpp`)

A third backend next to SteamVR and OpenXR, for headless tests of the real
headset path (`mode hmd`: UpdateHeadTracking, prediction, recenters, eye
output and submit, overlays, dissolves). It loads no runtime and talks to no
process: no SteamVR, no Virtual Desktop. It works with the `specopsvr_novr`
file (then only the fake can start).

- Enable: `runtime fake [openvr|openxr] [hz] [ms]` in `specopsvr.ini`, then
  start the game (read before the ini is loaded, like `runtime`). Default
  `openvr 90 45`. Not in the menu's choices.
- Display: 1296x1440 per eye (`fake size w h`; small on purpose, AUTORES
  sets the game window to it), Quest-like tangents (`fake fov l r u d`
  degrees, right eye mirrored), IPD 63 mm (`fake ipd`), `fake presence
  on|off` (activity level UserInteraction / Standby). Controllers report
  `oculus_touch` / `oculus_quest2_controller_*` (raw poses, no correction),
  connected with idle actions (no buttons: `pad fake` drives the game).
- Frame pacing: a QPC vsync grid at `fake hz 72|90|120` (re-based on every
  change). `WaitGetPoses` returns 2 ms before the vsync ("running start") and
  gives the poses predicted for the photons of the next vsync plus
  `fake latency <ms>` (vsync to photons, streaming included). A frame
  submitted later than 1 ms before its vsync is shown one vsync later (late,
  repeat). `fake hitch <ms> <every s>`: WaitGetPoses returns late.
- Submit semantics (`fake semantics openvr|openxr`): the declared pose is the
  pose of the frame's WaitGetPoses (OpenXR: the layer view pose of that
  frame), or the render pose with `Submit_TextureWithPose`.
  `SecondsToPhotons` and `Prop_SecondsFromVsyncToPhotons` follow the real
  runtimes (SteamVR: next vsync + latency; OpenXR backend: predicted display
  time of the frame, property 0).
- Poses: head = `fake head <yaw> <pitch> <roll> <x> <y> <z>` (degrees, metres,
  Standing space) + `fake sway <yawAmp> <Hz> <pitchAmp> <Hz> <rollAmp> <Hz>` +
  `fake swaypos <x cm> <y cm> <z cm> <Hz>` + `fake look <deg/s rms>` (sum of
  incommensurate sinusoids, deterministic) + `fake tremor <deg>` (4-10 Hz) +
  `fake turn <deg/s>`, or `fake script <file> [loop]` (lines `t yaw pitch
  roll x y z`, linear interpolation; relative paths from the game folder).
  `fake height <m>` (also `synthheight`). Presets: `fake preset
  still|sway|look|b2` (`b2` = the bug-2 test profile: pitch -20, yaw +-30 at
  0.3 Hz, pitch +-10, roll +-4 at 0.7 Hz, 40 deg/s look-around, 0.03 deg
  tremor, +-3 cm). Hands: `fake hand left|right <x> <y> <z> <yaw> <pitch>
  <roll>` (raw controller pose) or `on|off`, `fake handsway left|right <deg>
  <cm> <Hz>`. Default hands as held at rest: pitched 20 degrees down and
  rolled 15 inwards (left +15, right -15), positions (-0.20, 1.10, -0.25) and
  (0.20, 1.20, -0.35) m. Prediction: `fake predict linear|exact`, `fake prednoise
  <deg/s> [ms]` (Ornstein-Uhlenbeck noise on the predicted head rate).
  `fake reset`: time and recenters back to zero. `fake status` logs the state.
- `JITTER:` line every `fake log <N>` frames (300; `fake log restart` starts a
  fresh window, `fake log <N> frames` adds `JITTERF:` lines per frame):
  - mismatch declared vs drawn: angle between the declared pose and the view
    the game really drew, brought back to tracking space
    (Yaw(ref - body) * R(rot), from `NoteView` in GetPlayerViewPoint, matched
    by the frame tick from vr_output's `NoteSubmit`);
  - world step: change, from one shown frame to the next, of the world's
    orientation as displayed (drawn view * declared pose^-1): how much the
    world moves without the player moving it (rms, p99, max, % over 0.1 deg);
  - edge: angle between the declared pose and the true head at the photons of
    the vsync the frame is shown on (what the compositor reprojects, the
    black edge it can reveal);
  - repeats (vsyncs without a new frame), late, dropped, hitches, fades to
    black seen by the compositor (`FAKE: compositor fade to black` lines),
    frames/s, frames with no view record, cinema frames;
  - render pose vs declared: the head pose the mod drew the pair with
    (vr_output's `NoteSubmit`) against the declared pose, rotation (deg) and
    position (mm), with the number of frames: the declared-vs-render part,
    position included, that "mismatch" (rotation of the drawn view) does not
    show. 0 with `submitpose on`.
- `WaitGetPoses` always writes its pose arrays: when `fake hz` changes during
  the wait it returns the last poses (before, the arrays stayed
  uninitialised).
- The mod's VR menu does not open by itself on the fake runtime (an open menu
  makes the pad neutral, `pad fake` included: regress's fire and reload
  phases failed). `tools/regress.py` passes with `runtime fake` as with
  `specopsvr_novr` alone.
- Use `mode hmd`: in `mode synth` the fake still paces the frames, but the head
  comes from `synth`, so JITTER compares the drawn view with a head that
  did not draw it.
- What the fake does NOT cover. It is a third backend behind the same OpenVR
  interfaces, so it never runs `vr_openxr.cpp`; `fake semantics openxr` only
  changes `SecondsToPhotons` and `Prop_SecondsFromVsyncToPhotons` (the
  declared pose is the same code path for both semantics). Not testable with
  it:
  - the OpenXR shim's mutex held across the swapchain wait and `xrEndFrame`
    (VDXR stalls; WI-1 criterion, per game-thread tick since review round 2:
    under 1% of ticks waiting over 1 ms);
  - the OpenXR grip -> raw conversion and `openxrgrip` (bugs 6/7): the fake
    hands are already raw poses; `GripToRaw` and `PalmOf` are covered by unit
    tests against SteamVR's controller JSON only;
  - `XrEventDataReferenceSpaceChangePending` recenters (`poseInPreviousSpace`,
    WI-2): `simevent zeropose` exercises the OpenVR event path only;
  - Steam Link's and VDXR's own prediction, reprojection, controller drivers
    and pose conventions.
  These need a shim-level harness (a fake `XrInstance` behind
  `vr_openxr.cpp`'s function table, not built yet; the plan's "SteamVR OpenXR
  null driver" option only when allowed to touch SteamVR) or a headset check.
- Copy B practice (25/10): UE3 throttles an unfocused window to ~25 fps, so the
  suites first brought the game to the foreground (`gamectl.focus()`) for
  each measurement. That took the focus from the other game copy and stalled
  its pad-driven tests, so since 16:35 a second copy (`SPECOPS_EXE` set) with
  `specopsvr_keepfocus` never takes the foreground: the dll answers
  `GetForegroundWindow` / `GetActiveWindow` / `GetFocus` with the game's own
  window (display_spoof), `gamectl.focus()` does nothing for it and
  `gamectl.key/mouse/click` refuse (their input would reach the other copy's
  window). With another game copy on the same GPU, frame times stall for
  seconds at times (seen: 7.5 s "draw to present"), so the suites wait for a
  JITTER window instead of a fixed time.

#### Baselines on the WI-0 build (copy B, novr, 25/10)

Unchanged behaviour, measured with the new tools (WI-0 suites: crouch,
turret, fake runtime smoke); each later item re-runs its suite to show the
change.

- Bug 2/6, yaw blend: unit test 4.464 deg at pitch -30 / roll 10 (9.329 at
  -20 / 30); fake runtime, head still at -30 / 10: JITTER mismatch 4.459 deg
  every frame (same from VIEW lines with viewcheck.py).
- Bug 2, head sway (fake `preset b2`, focused, runs without repeated frames):
  mismatch rms 1.0-1.7, p99 1.6-3.3 deg; world step p99 0.15-1.5 deg, 2-58%
  of the frames over 0.1 deg (72/90/120 Hz, both semantics, five runs; fewer
  at 120 Hz).
  With `submitpose on` the mismatch stays (rms 0.9-1.0 deg): what is left is
  the yaw blend, not timing.
- Bug 4, button crouch (`ucall pc GamePad_ToggleCrouch`, synth, 90 Hz
  `fpscap`, focused): Pawn 20 units down, view 0 (crouchview real); with
  `seated on` the view follows the 20 units through eyesmooth (1.7-1.9 units
  per tick at most over four runs; 1.71 on the last).
- Bug 1/2, turret view lead (prologue minigun SP_RadioMan03_P). The
  reference is the headset path (`turret_hmd_fake.py`: fake runtime at 72 Hz,
  `mode hmd`, head still, fake right hand flicked 30 deg right and 5 down, then
  back): in the flick tick the view, relative to the helicopter, turns with
  the turret's whole step (+11.64 deg, PlayerController +11.64). The head
  reference takes that step off only on the next tick, so each following tick
  shows this tick's turret step minus the previous one (-5.08, -2.18, -1.55,
  -0.48 ... while the turret settles): the view swings out and back over
  about 1 s, 24.2 deg of view motion for a net 0. The same on the way back
  (-11.78, then +5.57, +1.82, +1.61 ...). `refused` stays 0 and the Base
  carry is 1:1 (`moved back` = -`delta`). Stand-in in `mode synth`
  (`turret_baseline.py`, `synthcomp on`, `synthaim 30 -5`, 90 Hz cap): +9.19
  deg in the flick tick (PlayerController +9.10), then -2.59, +6.14 (a 77 ms
  tick, turret +12.84), -12.67: 31.7 deg of view motion in 1 s for a net ~0
  (9.2-12.9 deg in the flick tick over six runs; `synthcomp` also takes
  the helicopter's own turns off the view, the headset path does not).
- Bug 3, runtime recenter (fake, head at yaw 40 off the reference,
  `simevent zeropose`): the view snaps by 40 deg when the mod recenters on the
  event (WI-2: 0.0 deg).
- Aim, the "before" for WI-1/WI-4 (`aim_baseline.py`, SP_Gorge02_P, a fresh
  start per weapon, `spread off`, head pitched -30): pistol M9A1, rifle M4,
  SMG UMP45, shotgun Benelli M1014, LMG SAW, each at `aimpitch` 0 and -24 x
  controller roll 0 / 30 / 60 (controller pitch -15, yaw 0); two paths, `mode
  synth` + `synthctrl` and `mode hmd` on the fake runtime (fake right hand),
  30 of 30 poses with shots on each. Both paths give the same barrel
  direction (within 0.04 deg in yaw, 0.2 in pitch): roll 0 -> yaw 0, pitch
  -15 (aimpitch 0) / -39 (aimpitch -24); roll 30 -> yaw +7.2 / +3.0, pitch
  -15 / -35.2; roll 60 -> yaw +12.6 / -0.5, pitch -15 / -25.7, where the
  barrel should stay at yaw 0 with a pitch that does not depend on the roll
  (bug 6). Shots against the barrel: synth at most 0.07
  deg (start within 0.11 units of the barrel axis); fake runtime mostly under
  0.15 deg, but 4.3 deg (UMP45, -24 / roll 30), 0.64 (SAW, 0 / 30) and 0.40
  (UMP45, 0 / 30) with shot starts 1.5-5.6 units off the axis at some poses
  (cause not identified yet; WI-4 compares against these numbers). The
  impact-to-marker distances (7-290 units) are along the same ray and depend
  on the scene.
  The MP7 and the Mossberg 590A1 are cut content (see "Shooting"): they
  become the current weapon but never fire, at level 1 or in Gorge.
- Pacing: on the fake runtime, a 12-14 ms "mod before draw" stall every 4.0 s
  (`HITCH:` lines; the same 12-13 ms pre-draw time shows in the novr runs'
  hitches); at 120 Hz each one costs 2-3 vsyncs (repeats). Source not
  identified yet (WI-1 splits HITCH by source).

### WI-1: head rotation and frame delivery (bugs 2, 1, 3)

Camera jitter (bug 2), the turret view lead of the first turret scene
(bug 1) and the dynamic-resolution cycling (bug 3). Measured before and after
on copy B with the fake runtime (`runtime fake openvr 72 45`, `mode hmd`) and
VIEW lines; scripts in the scratchpad (`wi1_suite.py`, `dynres_ingame.py`).

- **Exact head yaw for the view.** `UpdateHeadTracking` now takes two yaws
  from the headset matrix: the exact one (`ExactEuler`, atan2(-m02, m22)) for
  the view (`g_deltaYaw`, so `PublishHeadPose` and the per-tick snapshot) and
  for the vignette (`g_hmdYawRaw`, so the forced-motion test compares the
  view with the same yaw), and the stable heading (`YawFromOpenVrPose`, the
  blended one, which does not jump when looking almost straight down) for
  everything that needs a direction: recenter and `cinemaalign` reference,
  turn-with-head, `bodyfacing hands`, the left-stick direction (`movedir
  head`, `g_headYawUnits`), the body placement that moves Walker away from
  the gaze (`bodybacklook`) and the execution assist (new `g_headHeadingYaw`).
  The view yaw, pitch and roll are rounded to Unreal units instead of
  truncated (`vr::pure::RadiansToUnrealRounded`; truncation alone cost up to
  0.0055 deg per angle). The controllers keep the blended yaw until WI-4.
  A recenter done with the head both pitched and rolled leaves the view
  turned by the heading/exact difference of that moment (sub-degree for
  normal recenters; the plan accepts it).
- **Turret (TurretAimStep, TurretCompensation).** Before, the turret step
  went through `AddControllerYaw`, which also bumps `g_bodyYaw`; the eyes of
  the same tick read that body, while the head reference was moved only on
  the next tick: each frame showed the view turned by that tick's turret step.
  Now:
  - the step is written to the PlayerController only (no `g_bodyYaw`, no
    `g_ownYawUnits`). The game carries it into the view body on the next tick
    (the camera copy), and `TurretCompensation`, at the start of that tick
    and before the head pose, moves the head reference by the part the game
    really executed, net of the Base: `refused = k * baseDelta - (pcNow -
    pcAfterWrite)` (`vr::pure::TurretRefused`), `executed = step - refused`
    clamped to [0, step] (`TurretExecuted`). The Base term k follows the
    game's own rule (`vr::pure::TurretBaseTerm`, review round 2):
    `AYGamePawn::UpdateTurretAiming` (0x00f5f8e0) applies the based-rotation
    modifier only when Pawn.Base (+0xAC) has Physics (+0x94) 7
    (PHYS_Interpolating), like the prologue helicopter's InterpActor, which
    carries the PlayerController 1:1 (measured). Any other Base gives k = 0,
    and so does a Base that is the turret actor itself (Pawn
    `m_turretInteraction` +0x1184 -> `m_turretActor` +0x224; YTurretActors
    have Physics 7 too). With Physics 7, k is still 0 when the Base's rotation
    follows the step in the same tick (same direction, within 25% or 2 units:
    `vr::pure::TurretBaseFactor`, round 1). Before round 2 that same-tick test
    was the only guard: a turret Base that followed one tick late or at a
    limited rate swung the view 16-17 deg in the unit model (the reviewer's
    model: 18.8 / 35.8 deg); with the game's rule, 0.000 deg. On the M2E2 the
    Base is `Terrain` (Physics 0, measured 26/10). Body and reference move in
    the same tick (the `g_trackRefYaw`
    contract with props.cpp holds). Nothing is counted when the
    PlayerController or the Base pointer changed between the two ticks; the
    turret state does not matter (review fix: the step written in the last
    turret tick is still taken off the view after leaving the turret, a
    cinema or `turretaim stick`; `TURRET: last step after leaving ...`).
  - The precondition was measured: in every eye draw of the turret runs
    (~1960) the view body equals the body at the head sample (VIEW `bodyhs`):
    the camera copy does not run between the head sample and the eye draws.
    `TURRET` "view body vs PC at the tick start" is 0: the next tick's body
    already contains the step.
  - The yaw error is wrapped to +-180 deg (`vr::pure::TurretYawStep`). Before,
    with the reference and the controller on the two sides of the tracking
    space's 180 deg direction, the turret jumped 133 deg the wrong way in one
    tick (fake runtime: head at yaw 175, hand 10 deg right).
  - Gain from dt with time constant 30 ms, at most 0.5 per tick (0.94 in an
    83 ms tick before); also for the pitch.
  - The Base factor at PC+0xBB0 (`YPlayerController.m_turretYawModifierWhenBased`,
    offset confirmed by reflection 26/10) reads 1.00 in the prologue
    helicopter; it is logged only. The `TURRET` line ends with `base term`
    (the k actually used) and, since review round 2, `base <name> (<class>)
    physics <n>` (plus `, the turret itself`).
- **Fades on view jumps** (`vr::pure::ViewJumpDetector`): speeds, not
  per-tick distances. On foot a jump blinks when the view moved over 80
  units in the tick and either more than the distance Walker can run in that
  tick (1200 units/s x the real tick time + 40, or 1.5 x the previous speed
  if that was already higher: falls, scripted slides; review fix, so a
  150-800 unit jump in a long tick still blinks) or at more than 3 x the
  previous speed + 3600 units/s; on a vehicle (Base moving, `!ViewOnFirmGround`)
  only a teleport over 2000 units blinks. Before,
  every long tick in the helicopter (20-83 ms at ~1100-3000 units/s) was taken
  for a jump: 0.3 s of black up to 7 times a second (tester log), 11 times in
  12 s with the fake runtime. The `FADE:` line now gives the tick, the speed
  and the previous speed.
- **Dynamic resolution** (`vr::pure::DynResController`, unit-tested):
  - GPU samples are the stereo-tick GPU times of the level in use only: each
    GPU query records the ScreenPercentage it was issued with; none for 1 s
    after a change;
  - the p90 of the last 2 s decides: down above 1.0 x the headset frame (after
    2 s at the level) or above 0.92 x for 2 s (after 4 s), by at most two
    levels and rounded towards the current level; up by 5 after 5 s under
    0.65 x (after 4 s at the level). Since the review these are Advanced ini
    keys (`dynresdown 1.0`, `dynresslow 0.92`, `dynresup 0.65`, `dynresdwell
    4`, the over-budget dwell is half of it);
  - a level left because it overran is blocked for 20 s (`dynresbackoff`),
    then 40, 80 ... up to 16 x that (320 s) after each new failure, forgiven
    after 60 s held; a failed probe (up, then over budget within 30 s) goes
    back to the level it came from;
  - a Present without a frame tag within 250 ms of a change is not sent (its
    crop could be the old percentage): the compositor repeats the previous
    frame (`VR-OUTPUT: pair without frame tag ... not sent`). **Since 27/10
    evening** the crop no longer depends on the percentage
    (`kGeometryPercent`, see "Headset session fixes (27/10 evening)"):
    hooks.cpp always passes `cropUnknown = false`, so such a pair is sent and
    the line no longer appears (the check in vr_output.cpp is still there);
  - `DYNRES:` gives the p90, the EMA, how often the level failed and how long
    `scale set` took, plus the length of the tick with the change and of the
    next one. The `GPU:` line shows the controller's p90 and EMA.
  The GPU time is still the D3D9 timestamp span (it includes GPU idle caused
  by CPU stalls, which made 85% look 70% heavier than 80% in the tester's
  log); the memory of failed levels is what stops the cycling.
- **Frame delivery.**
  - OpenVR latency, possibly one frame short (hypothesis, proven only on the
    fake runtime, whose openvr model was written with the same assumption):
    the measured head-to-photons time used "until the next vsync" right after
    Submit, but if SteamVR's WaitGetPoses returns 2-3 ms before the vsync
    ("running start") that frame is shown on the vsync after. New
    `vr::rt::FramePhotonsSeconds()`, read right after a successful
    WaitGetPoses: OpenXR (and the fake runtime with `openxr` semantics) the
    predicted display time of the frame; OpenVR one frame more when the wait
    returned in the second half of the frame, but only with `predictframe on`,
    or with `predictframe auto` (the default) once the `POSEAGE` line has
    shown it twice in a row (review fix: `vr::pure::PredictFrameAuto`; render
    pose more than half a frame behind SteamVR's own frame pose, at least 30
    frames turning per 300-frame window; the next window after a switch is
    skipped while the latency average settles; more than half a frame ahead
    switches it back off). `POSEAGE: predictframe auto -> on|off` logs each
    switch; until then Steam Link keeps the pre-WI-1 prediction.
  - `POSEAGE:` every 300 frames: angle between the render pose and the
    runtime's pose for the frame (WaitGetPoses), and the median of how many
    ms the render pose is predicted later than the runtime's, from the head's
    angular velocity (frames turning > 10 deg/s), and the `predictframe`
    state. Real runtimes included: the headset check of the latency fix. Poses
    are read only after a successful WaitGetPoses (zero-initialised; non-finite
    values skipped: review fix). The OpenXR shim's WaitGetPoses now returns
    the HMD pose predicted for the frame's display time (OpenVR semantics;
    before, the current pose; only this line uses it, so the controllers are
    no longer located there: shorter mutex hold).
  - Prediction cap `predictmax 150` ms (was 100 fixed; VDXR at 72 Hz measures
    ~95 ms).
  - Posed submit (`submitpose`, default still off, now in the shipped ini and
    so in Advanced): never for scope frames (the image follows the barrel, not
    the head); on OpenXR the render field of view is `xroverscan` (1.5) deg
    wider per side while it is on (the mod re-reads the projection when it or
    the key changes) and every projection layer, posed or not, declares
    exactly the field of view the mod rendered with (review fix: before, the
    unposed layers of the scope and of untagged pairs were rendered wide but
    declared narrow, a ~3% scale pop entering and leaving the scope). The
    scope state for the tag also includes the detection made during the
    eye's own draw, so the first scope frame is unposed too. The edge reveal
    is unchanged by it on OpenVR, where no field of view is declared.
  - `HITCH:` per source: head tracking time, whether a ScreenPercentage
    change happened in the tick, the last VR output and monitor Present
    times, and `OpenXR lock wait on the game thread this tick X ms (in head
    tracking Y)`: X is everything the game thread waited for the shim's mutex
    in that tick (input reads in the XInputGetState hook included), Y the
    part inside UpdateHeadTracking (review round 2: before, one atomic total
    of all threads since the previous tick, printed as if it were inside head
    tracking, so it could exceed the head-tracking time and blame the wrong
    source).
  - Every interface call of the shim (poses, roles, controller state, input
    actions, overlays, `SecondsToPhotons`, `GetRecommendedRenderTargetSize`)
    takes the backend mutex through `TimedLock`, which times the wait; the
    output thread, which holds the mutex across the swapchain wait and
    `xrEndFrame`, is excluded. `TakeSpaceChange` checks an atomic flag before
    taking the mutex. Waits are kept per thread (`thread_local`):
    - game thread: summed per tick and closed at the tick boundary (end of
      the stereo draw, `vr::xr::EndGameTickLockWait`, no mutex); every 10 s
      `OPENXR: lock wait, game thread, per tick over 10 s: N ticks, p50 p99
      max, K ticks over 1 ms (P%), over 0.5 ms, mean`;
    - the D3D9 Present thread, the session thread (`vr::rt::NameThread`) and
      any other thread: per call, each with its own line (`OPENXR: lock wait,
      Present thread|session thread|other threads, per call ...`).
    Statistics come from a fixed histogram (`vr::pure::WaitHistogram`, 8 bins
    per octave from 5 us; exact count, max and counts over 0.5 / 1 ms), not a
    vector capped at 4096 samples, which filled in 1-2 s at ~30 calls per tick
    and so saw only the start of each window. Review round 2: per call, a
    2-4 ms wait in 20% of ticks is only ~0.7% of ~30 calls per tick, so the
    per-call p99 stayed at the uncontended value (unit model: per-call p99
    0.020 ms, per-tick p99 3.4 ms, 20% of ticks over 1 ms). The criterion is
    per tick: release the mutex across the swapchain wait and `xrEndFrame`
    (and serve poses from a cache) if more than 1% of game-thread ticks wait
    over 1 ms. That needs VDXR (headset check).

#### Measured (copy B, fake runtime, 25/10; before = the WI-0 build)

| Check | Before | After |
|---|---|---|
| Head still, pitch -30 / roll 10: drawn view vs headset (VIEW) | 4.462 deg | 0.003 deg |
| Head still, pitch -20 / roll 30 | 9.325 deg | 0.002 deg |
| Head roll +-15 deg looking 30 deg down: drawn vs headset rms | 4.82 deg | 0.0034 deg |
| `preset b2` sway, `submitpose on`: mismatch declared vs drawn rms / max | 1.03-1.21 / 2.50 deg | 0.003 / 0.005 deg |
| same: world step p99, frames over 0.1 deg | 0.24 deg, 22% | 0.007 deg, 0% |
| same: edge (reprojection) p99 | 1.67-2.16 deg | 0.78-0.83 deg |
| `preset b2`, `submitpose off`, render pose vs declared rms: openvr / openxr semantics | 0.57-0.85 / 0.21 deg | 0.21-0.24 / 0.15-0.27 deg |
| POSEAGE: render pose predicted vs the runtime's (openvr, openxr) | (line new) | -1.3 to +0.8 ms |
| Forced-motion vignette with head roll while looking down | (not logged; the old view and vignette both used the blended yaw, so it did not fire then either) | 0 frames (kept: the vignette follows the exact view yaw) |
| Turret flick 30 / -5 (prologue minigun, `mode hmd`): view vs helicopter in the flick tick | +10.89 deg | 0.000 |
| same: view range vs helicopter over the run | 18.2 deg | 0.000 deg (turret 30.0 deg) |
| Hand at +-120 deg: view step in the first tick / view lead max | +39.3 / 64.5 deg | 0.0 / 0.0 deg |
| Wrap (head at yaw 175, hand 10 deg right): largest turret step | -133.0 deg | 7.4 deg (right way; WI-1 build). Review-fix build, full window at the start of a fresh flight: 6.5 deg, right way, no pose jump in the window |
| Helicopter flight, ticks 14 <-> 83 ms (`fpscap 72/12` every 0.6 s): blinks | 11 in 12 s | 0 (two runs: 448 and 498 ticks, 77-90 over 40 ms, up to 196 ms) |
| The teleport at the end of the flight (15009-17682 units) | blinks | blinks |
| Dynres on a step model of the tester's curve (unit test, 600 s at 120 Hz; not the recorded curve) | 850 changes | 12 (6 after 60 s: 0.11 per 10 s steady-state average; at most 2 in any 10 s, a probe and its failure) |
| Dynres in game, real 13.9 ms budget, 40 s at 100% | (not provoked) | 0 changes |

- The vignette still sees forced motion around turn-with-head body turns
  during big head sweeps (17 of 21 frames in `preset b2` were next to a body
  turn; 0 with pure head yaw): the one-tick fp pivot lag (bug 2 cause F, WI-2),
  not the yaw change; not A/B-tested on the old build.
- With a 1 ms forced budget, copy A's GPU bursts on the shared GPU (p90 spikes
  to 5-7 ms against a typical 0.4 ms) still made the in-game controller probe
  and fall back; failed probes now return to the previous level, unit test
  with bursts: 0.27 changes per 10 s, at most 2 in any 10 s.
- The prologue minigun is clamped on one side only: at +120 the game took
  back ~27 deg every tick, at -120 the turret reached -175 deg from the
  helicopter (absolute writes not re-clamped on that side). The view stays
  still either way; the limits are WI-4c's.

Earlier suites on the final WI-1 build: `tools/regress.py` PASS (fake
runtime); fake runtime smoke: every functional check passes (tilt mismatch
0.003 deg, posed submit 450 of 450, hitch, fade, zeropose, presence), with
two throughput misses at 90 and 120 Hz (77.6 and 114.4 frames/s in the worst
windows while copy A shared the GPU; 89.9 and 119.7 in the steady PROFILE
windows, mod cost per tick unchanged); WI-0 turret suites "not reproduced"
(headset path: view vs helicopter range 0.000 deg, turret 30.0 deg; synth
stand-in: -0.005 deg in the flick tick); crouch unchanged (WI-3); aim
baseline on the fake runtime: 29 of 30 poses with shots (the SAW lost its
weapon at one pose), barrel yaw within 0.06 deg of the same poses in `mode
synth` (the aim code is unchanged by WI-1).

#### Review fixes (round 1, 25/10)

The adversarial review of WI-1 found six medium and ten low issues; all are
fixed in code except where noted. Copy B, dll `40409330` (22:10), fake
runtime openvr 72 Hz, same session as copy A's tests.

- **OpenXR lock-wait instrument added its own wait** (medium): fixed
  (`TimedLock`, atomic total, see "Frame delivery"). Not runnable without
  VDXR.
- **Unposed OpenXR layers declared the narrow field of view** (medium):
  fixed, every projection layer declares the rendered field of view; the
  scope tag also includes the current draw's detection. Not runnable
  without VDXR.
- **The OpenVR "+1 frame" was unproven** (medium): now `predictframe
  auto|on|off`, default auto = off until POSEAGE confirms. Fake runtime
  (openvr semantics, `preset b2` sway): POSEAGE render pose vs the runtime's
  frame pose -12.0 / -12.1 ms with `off`, -1.5 to +0.7 ms with `on`;
  `auto` switched on after two windows (-8.6, then -12.1 ms; log
  `POSEAGE: predictframe auto -> on, one more frame to photons`) and then
  read -2.5 ms; with openxr semantics the state is `n/a`. The decision on
  SteamVR / Steam Link stays a headset check: either the switch happens or
  the offset is already near 0 ms with `off`.
- **The Base factor 1 was measured on one mount only** (medium):
  `TurretBaseFactor` drops the base term when the Base rotates with the
  step (the Base is the turret). Unit model with the turret as the Base:
  view swing 0.000 deg with the rule, 45 deg with the factor fixed at 1. On
  the prologue helicopter every TURRET line used the base term (k = 1), so
  nothing changed there (flick: view vs helicopter 0.000 deg over 5.8 s,
  turret 30.0 deg). UNVERIFIED on the M2E2: one attempt to board it without
  a checkpoint jump (its streaming level `SP_Desert01_03_K` made visible,
  Walker placed 71 units from the enabled `YTrigger_TurretInteraction`, A /
  X / B pressed through `pad fake`) did not enter the turret; the checkpoint
  jump would overwrite the saves copy A shares. Round 2 boarded it after a
  checkpoint jump; see "Review fixes (round 2)".
- **fake_smoke throughput FAIL on the WI-1 dll** (medium): environmental.
  A/B in one session with `fpscap 0`, copy A running (GPU 49-67%), 3 tries
  per rate: pre-WI-1 dll 90 Hz 3/3 kept up (90.0-90.03 frames/s, 0
  repeats), 120 Hz 3/3 (119.75-120.05, 0-2 repeats); final dll 90 Hz 3/3
  (90.0-90.03, 0 repeats), 120 Hz 3/3 (120.02, 0 repeats). The chain's
  fake_smoke (which caps at the rate) again missed 72 and 90 Hz while copy
  A loaded the GPU, and its hitch check failed (138 repeats for 5 hitches);
  the hitch check with `fpscap 0` right after: PASS, 5 hitches, 25
  repeats in 750 frames.
- **Tuning values hard-coded** (medium): `xroverscan`, `predictmax`,
  `predictframe`, `dynresdown`, `dynresslow`, `dynresup`, `dynresdwell`,
  `dynresbackoff` are ini keys with comments (Advanced, with menu steps);
  defaults unchanged, so no configversion bump. Left as constants, each
  covered by a unit test: the FADE detection thresholds (80 units, 3 x +
  3600 units/s, 1200 units/s walking, 2000-unit teleport: scale facts of the
  game, not comfort trade-offs), the 250 ms untagged-present window (frame
  tag latency) and the dynres sample rules (1 s ignore, 2 s p90, 60 s
  forgiveness).
- Low: POSEAGE reads poses only after a successful wait (zeroed, non-finite
  skipped); the turret's last step is compensated after leaving the turret;
  the on-foot FADE rule; the lock-wait covers every interface call and the
  shim locates only the HMD in WaitGetPoses; `g_headHeadingYaw` is reset with
  `g_deltaYaw` (`mode off`, exit); `submitpose` stays in the retired-key list
  for pre-version-8 inis only, with a comment; `g_lastOutputMs` /
  `g_lastPresentMs2` are atomic; `TurretCompensation` returns 0 on a Base
  change, as documented.
- Unit tests: 381 pass (new: on-foot long-tick jumps 5 of 5 blink, 1 of 5
  with the speed rule alone, running or falling through a long tick 0
  blinks; base factor; turret-as-base model; dynres parameters;
  predictframe auto decision; menu steps).

Measured on the final build (each suite once):

| Check | Result |
|---|---|
| Head still, pitch -30 / roll 10 and -20 / 30: drawn vs headset | 0.0031 / 0.0019 deg |
| `preset b2`, `submitpose on`: mismatch rms / max, posed frames | 0.003 / 0.005 deg, 450 of 450 |
| `preset b2`, `submitpose off`, render pose vs declared rms: openvr (`predictframe auto`, on) / openxr | 0.50 / 0.48 deg (game at 66-70 frames/s with repeats; equal between the semantics) |
| POSEAGE offset, openvr (auto, on) / openxr | -0.7 / -0.5 ms; -0.1 / -0.9 ms |
| Head roll while looking down: forced-motion frames | 0 |
| Turret flick 30 / -5: view vs helicopter in the flick tick, range over the run | 0.000, 0.000 deg (turret 30.0 deg) |
| Hand at +-120: view in the first tick, view range (window cut at the end-of-flight teleport) | 0.0 deg, 0.456 deg (turret 176.9 deg, refused up to 35.2 deg per tick) |
| Wrap, full window at the start of a fresh flight (WI-2 harness rule: wait 1.6 s after the abrupt fake head, then recenter) | largest turret step 6.54 deg, right way; no pose jump in the window |
| Helicopter flight, ticks 14 <-> 83 ms, fresh flight | 0 blinks in 329 ticks (59 over 40 ms, up to 97.5 ms) |
| Dynres in game, real budget (shared GPU, p90 8-15 ms of 13.9) | recovery 65 -> 80% in 5-point steps 5+ s apart, one failed probe back to 75%; second 40 s: 0 changes |

- The wrap window's view moved 4.1 deg against the helicopter. It is not
  the compensation: in the first 0.7 s the mod wrote no step (TURRET step
  0.000, head term 0) and the PlayerController, hence the view, still
  turned 1.7 deg against the Base while the helicopter turned. The game did
  not carry the PlayerController with the Base in that stretch, with the
  turret at its limit (-36 deg from the helicopter). It is game behaviour at
  the limit, for WI-4c's clamp; the same window on the WI-1 build (later in
  the flight) read 0.0.
- The chain's first `regress.py` failed on two render-thread stalls of 5-6 s
  inside the NVIDIA driver (`nvd3dum.dll` -> KERNELBASE wait, during a
  scripted scene); the second run, once: PASS.
- Earlier suites: WI-2 room (zeropose 0.0 deg, clamped height drift 0.001
  units, wall and flat wall as in WI-2's final run), squadmate 0 pull ticks,
  crouch unchanged (WI-3), WI-0 turret suites as in WI-2's final run, aim
  29 of 30 poses (the SAW lost its weapon at one pose, as in WI-1), barrel
  yaw within 0.06 deg of WI-1's run.

#### Review fixes (round 2, 26/10)

The second review found one medium and two low issues; all three are fixed.
Copy B, final dll `97b77bf3` (00:24), fake runtime openvr 72 Hz.

- **The lock-wait line could not decide plan item 6** (medium): fixed, see
  "Frame delivery". The game thread's waits are summed per tick (all its
  shim calls, input reads in the XInputGetState hook included), closed at
  the tick boundary and kept in a fixed histogram. The Present, session and
  other threads get their own per-call lines. HITCH prints the game-thread
  total of that tick and the part in head tracking.
  `GetRecommendedRenderTargetSize` is now timed too. Unit model (30 calls
  per tick, 3 ms in 20% of ticks):
  - per call, as the old line counted: p99 0.020 ms;
  - per tick, as the new line counts: p99 3.4 ms, 20.0% of ticks over 1 ms.

  The criterion is now per tick: release the mutex if more than 1% of
  game-thread ticks wait over 1 ms. Not runnable without VDXR: the fake
  runtime never runs `vr_openxr.cpp`, and its HITCH lines print 0.00.
- **The Base term used a guess where the game's rule is known** (low):
  `vr::pure::TurretBaseTerm` applies the game's rule. k is 1 only when the
  Base has Physics 7 (PHYS_Interpolating); otherwise k is 0, and it is also
  0 when the Base is the turret actor itself. `TurretBaseFactor` stays as
  the extra same-tick guard.
  - Unit model, view swing with the game rule vs `TurretBaseFactor` alone:
    turret Base one tick late 0.000 / 17.0 deg; Base turning at 60 deg/s
    0.000 / 16.2 deg. Helicopter (Physics 7): 0.000 deg with the hand at 30
    and at 120.
  - The TURRET line now names the Base, its class and its Physics.
  - Prologue helicopter: Base `DynamicBlockingVolume`, Physics 7, base term
    1 in 556 lines, so the behaviour there is unchanged.
  - M2E2 (`SP_Desert01_03_K`), boarded once. Setup: `ce cpBusRoof` in
    `SP_Desert01_p`; the AI gunner removed with `ucall <pawn> Suicide` (the
    pawn whose `m_turretInteraction` +0x1184 is the turret's trigger); Walker
    placed on the trigger's grab point at the turret platform's height, then
    pad A. Result: Base `Terrain`, Physics 0, base term 0.
  - M2E2 hand flick to 30 deg on the fake runtime: view against the world
    0.000 deg while the PlayerController turned 30 deg. Hand to +-60: view
    0.000 deg, PlayerController 109 deg, refused up to 7.0 deg.
  - These M2E2 numbers come from dll `793c78b2`, which already had the game
    rule. The final dll only adds the turret-actor guard and the log fields.
  - On the final dll the boarding did not repeat in three tries: twice from
    the ground and once replaying the successful sequence. Pad A no longer
    entered the turret from the grab point, with the trigger enabled and
    empty. The cause was not found within the time box.
- **Word-valued Advanced keys could not be changed in the menu** (low): such
  keys now cycle like the *Basic* choices (`vr::pure::AdvancedWordChoices`,
  12 keys: `predictframe` auto/on/off, `predict` auto/off, `headcollision`
  push/off, `crouchview`, `aim`, `movedir`, `turretaim`, `hand`, `reload`,
  `objectweight`, `hints`, `runtime`). New test command `vrmenu cycle <key>
  [+1|-1]`: it does what a click on the item does (LoadIni + Change, so the
  command runs and the ini is saved).
  - In game: `predictframe` auto -> on -> off -> auto, `predict` auto -> off
    -> auto, `headcollision` push -> off -> push; every step was written to
    the ini.
  - The first build checked the on/off rule first. That turned "predictframe
    on" into a plain on/off toggle and wrote `predict on`, which is not a
    valid value. The word table is now checked first.
- Unit tests: 423 pass. New: the per-tick lock-wait model; the game-rule
  base term with a lagging, a rate-limited and a Physics-7 Base; the
  Advanced word choices (12 shipped keys, each at its default first).

Measured on the final build (each suite once, fake_smoke twice; see below):

| Check | Result |
|---|---|
| Head still, pitch -30 / roll 10 and -20 / 30: drawn vs headset | 0.0031 / 0.0019 deg |
| `preset b2`, `submitpose on`: mismatch rms / max, posed frames | 0.003 / 0.005 deg, 450 of 450 |
| `preset b2`, `submitpose off`, render pose vs declared rms: openvr (`predictframe auto`, on) / openxr | 0.41 / 0.46 deg (game at 65-69 frames/s with repeats) |
| POSEAGE offset, openvr (auto, on) / openxr | -0.9 / +1.1 ms; -0.2 / +0.8 ms |
| `predictframe` off / on / auto, last window | -13.2 / +0.5 / -2.5 ms; auto switched on after two windows at -12.6 ms |
| Turret flick 30 / -5: view vs helicopter in the flick tick, range over the run | 0.000, 0.000 deg (turret 30.0 deg, 5.8 s) |
| Hand at +-120: view range (window cut at the end-of-flight teleport) | 0.549 deg (turret 172.7 deg, refused up to 30.3 deg per tick) |
| Wrap window (cut by the end-of-flight teleport after 1.75 s) | view vs helicopter 0.000 deg, turret 10.0 deg, largest step 3.73 deg |
| Helicopter flight, FADE | 0 blinks in 65 ticks (27 over 40 ms) |
| Dynres in game (shared GPU) | steady 40 s: 3 changes, at most 2 in any 10 s; over budget: 90 -> 65% in 3 steps; recovery 60 s: 65 -> 75% with one failed probe; second steady 40 s: 75 -> 100% in 5-point steps, at most 2 in any 10 s |
| Hitch 50 ms every 2 s, 90 Hz, `fpscap 0` | PASS, 5 hitches, 25 repeats in 900 frames |
| fake_smoke | chain run FAIL (4), environmental; re-run PASS on every check (72 / 90 / 120 Hz kept up, recenter blink 1 fade, zeropose 0.0 deg) |
| Advanced menu cycle (`vrmenu cycle`) | every value in order, ini written, start values back |

- The chain's fake_smoke failed four checks. Two were throughput: the game
  did not keep up at 72 and 90 Hz, as in earlier rounds. The other two came
  from the window losing the foreground from 00:48:18 to 00:48:37, while
  copy A's tooling ran.
  - During that window copy B ran at 86 ms ticks ("outside the draw"). It
    recovered at the log line `DISPLAY: game window brought to the
    foreground`.
  - Recenter blink: the log shows the fade, but no 150-frame JITTER window
    finished in the check's 4 s, so the check counted 0 fades.
  - Zeropose: with 86-114 ms ticks and poses predicted about 114 ms ahead,
    the absorber (`HeadJumpDetector`) caught the jump one tick late and
    measured only 30.6 of the 40 deg, and the view stepped 40 deg. This is
    WI-2's absorber at about 10 frames/s; it is listed as an open edge case.
  - The re-run at 01:18, with the game in the foreground at full rate,
    passed every check. The WI-2 room's zeropose, in the same chain, read
    0.0 deg.
- Earlier suites, all on the final dll:
  - WI-2 room: zeropose 0.0 deg; height drift 0.0 units (clamped 1.20 m and
    free 1.70 m); wall and flat wall as in WI-2's final run. Squadmate: 0
    pull ticks.
  - WI-2 turret, still: drift 0.002 units, largest step 0.003 before the
    end-of-flight teleport. The teleport fell inside the still window, so
    the fire window had no flight left.
  - Crouch: baseline b4 still reproduced (WI-3).
  - WI-0 turret suites: the headset-path and synth view-lead baselines are
    not reproduced (fixed).
  - `regress.py`: PASS.
  - Aim: 30 of 30 poses (29 of 30 in round 1). The shots clipped on the
    muzzle KActorSpawnable are known; copy A has the fix (p2d).

#### Not covered without a headset

The fake runtime never runs `vr_openxr.cpp`: the OpenXR overscan and declared
field of view, the shim's WaitGetPoses poses and the lock-wait line are
checked by reading only. Headset checks: posed submit A/B on VDXR and Steam
Link (edges, warping, scope, HUD, cinema screen); looking 20-40 deg down with
head roll, the world must not swing; the helicopter without strobing; the
`POSEAGE` offset near 0 ms on Steam Link and on VDXR, and on Steam Link
whether `predictframe auto` switches on (mandatory before release: the "+1
frame" is a hypothesis until then); `OPENXR: lock wait, game thread, per
tick`: under 1% of ticks over 1 ms (otherwise release the mutex across the
swapchain wait and `xrEndFrame`), with the Present / session thread lines for
reference; `DYNRES` lines per minute in a fight (the D3D9 timestamp GPU
metric still includes CPU stalls: the controller contains the cycling, the
metric is unchanged); the M2E2 turret's feel in the headset (the view against
the world was measured constant on the fake runtime in review round 2).

### WI-2: view translation, eye height, recenters (bug 3, bug 2 E/F/J)

Bug 3 ("world scale constantly changes") had no single cause on the mod's
side: IPD, field of view, projection, `posscale` and the near plane do not
change from frame to frame (investigation of 25/10). What did change was the
view position, without the head moving:
- the wall pull and head collision moved the eyes forward and back by up to
  25 units;
- a squadmate in front pulled the view back;
- the eye height followed the highest head position;
- each runtime "zero pose" event recentred the view.

WI-2 removes these. Measured on copy B (novr, fake runtime `openvr 72 45`,
`mode hmd`, VIEW lines). The scripts are in the scratchpad: `ph_room.py`,
`ph_pawn.py`, `ph_turret.py`, `wallframe.py`.

- **View traces with the nearest hit** (`TraceViewNearest`, new; the physics
  owner's `TraceLineView` is unchanged).
  - **The old problem.** The view traces carried `TRACE_StopAtAnyHit` (0x200,
    from FastTrace). In MultiLineCheck (0x0071B130), a BSP hit with that flag
    skips the whole actor pass (static meshes, props, pawns). So a farther BSP
    face could win over a nearer crate, and the result flipped as the trace
    moved with the head.
  - **The new trace** drops 0x200. It skips held objects and the hand spheres
    like `TraceLineView` (`props::IsHeldActor`, same step past their bounds).
  - **Wall pull and low probes** (`kViewPull`): no pawns, and no moving actors
    (`Velocity` +0x140 over 10 units/s). Before, a squadmate 30 units in front
    of Walker pulled the view back by 16.4 units in every tick (measured).
  - **Head collision** (`kViewHead`): **replaced by `headtrace 1`** (review
    round 1): one pass without 0x200 and with pawns, the default. The first
    WI-2 version took the nearer of two passes (one without 0x200 and without
    pawns, one with the old flags), on the claim that a zero-extent trace
    without 0x200 never hits a squadmate. That claim was wrong: the pass had
    also dropped TRACE_Pawns (0x1). Pawns stay in head collision (19/10
    squadmate fix).
  - **Hit normal**, read at FCheckResult +0x14 (Location +0x8, Material
    +0x28, PhysMaterial +0x2C, Component +0x30). It is used only if it is a
    unit vector facing the trace. In the checkerboard room it is exactly the
    wall's axis, (-1, 0, 0) / (0, -1, 0).
- **Wall pull** (`vr::pure::WallPullStep`, `LowProbeLatch`):
  - **Direction.** Only the body-forward component of the first-person offset
    is pulled back (direction = the Pawn yaw that `ApplyFirstPerson` used).
    Before, the direction was the base minus Walker's centre, cover lean
    included, so the pull went diagonal and ate the lean.
  - **Speed.** Towards the wall at once, up to the point where the eyes are 4
    units from the surface; the rest of the 12-unit gap at 150 units/s; away
    from the wall with tau 0.2 s. Before, all of it went at 150 units/s, so
    for a few ticks after a turn the eyes were inside the wall.
  - **Low probes** (10 and 22 units under the real eye, the cp01 crate fix)
    now latch: once they hit, the exit test is 4 units lower and the pull is
    held 0.4 s after the last hit. Unit test, head bobbing +-2 units across a
    crate edge for 6 s: 12 toggles before, 1 engage and 0 releases after. The
    latch releases 0.40 s after standing up past the edge.
- **Head collision.**
  - **Option.** `headcollision push|off`: Basic "Head in walls", default
    `push`. configversion 11 (10 in copy B, renumbered at the merge of 27/10) migrates `on` to `push`. `fade` is accepted and
    means push until a fade API exists.
  - **What is removed.** Only the part of the head offset that goes into the
    obstacle. The eye is moved back along the hit plane's normal to 8 units
    from it (`vr::pure::ClampIntoPlane`), at most to Walker's centre.
  - **Check.** A second trace checks the corrected point. At corners and on
    curved pawn bodies, the old behaviour applies (the whole vector from
    Walker's centre scaled by t). Review round 1: up to three planes instead.
  - **Before.** t always scaled the whole vector, first-person forward
    included: the view dollied back, and side-to-side head motion near a wall
    shrank.
  - The correction snaps in and releases with tau 0.1 s.
- **Eye height** (`vr::pure::StableEyeHeight`).
  - **Reference.** The view uses a stable vertical reference, taken at the
    user's recenter, at the scene start (headset put on) or from the
    calibrated `height`. `posRef[1]` still tracks the highest head position
    and decays 1 cm/s, but only for the crouch and cover-peek thresholds.
  - **Limits.** Only the reference is limited: 30..55 units over the Pawn
    centre (30 = Walker's eye until WI-3 measures it; was 25). The head's own
    movement is never limited, so crouching still lowers the view 1:1.
  - **Before.** With the height clamped (head below ~1.4 m with `seated off`),
    the view followed the tracked maximum: raising the head did not raise the
    view, and the floor drifted up afterwards.
  - **One exception** (replaced in review round 1 by a one-off re-capture
    under a blink, see below), which the old max tracking covered: the reference was
    taken low (headset put on while bent down) and the player then stands up.
    The reference then rises at half the head's speed, while the head rises
    and is more than 15 cm above it, until it no longer matters (1.5 m). A few
    cm of head motion never changes it.
- **eyesmooth only on the game's height.** The real head's height above the
  game's fixed eye height (`ViewHeightExtra`) is now added after the 0.12 s
  smoothing, like the head offset. Before, it was inside the smoothing: when
  the reference moved, the two parts moved in opposite directions in the same
  tick, and the view swung for 0.12 s (0.064 units measured with the head
  still).
- **Runtime recenters** (`vr::pure::HeadJumpDetector`, `SpaceShift`).
  - **Events no longer recenter the mod**: neither a runtime "zero pose / play
    area changed" event (OpenVR) nor a stage
    `XrEventDataReferenceSpaceChangePending` (OpenXR). Before, the event put
    the yaw reference and position on the current head, at most once per 10 s
    and silently, so the view snapped sideways by up to 45 deg and 15 cm, and
    the height changed.
  - **What the mod watches instead: the headset pose.** When the runtime
    really moves its space, the pose jumps in one tick against a
    constant-velocity extrapolation of the two previous samples.
    - Threshold: 5 cm or 5 deg, plus the extrapolation error of a real head
      accelerating during the tick (alpha x dt^2, with 30 m/s^2 and 3000
      deg/s^2): +0.6 cm / +0.6 deg at 14 ms, +23 cm / +23 deg at 87 ms.
    - Ticks up to 150 ms count. A first version accepted only ticks up to
      50 ms: with copy A loading the GPU (ticks up to 87 ms), a 40 deg / 36 cm
      runtime recenter went undetected and the view snapped (fake runtime
      smoke test, 25/10).
  - **Absorb and confirm** (review round 1: held one tick, absorbed only when
    the next tick confirms the new space, put back under a blink; see below). The jump is taken off the references at once
    (view, direction and height unchanged). An event between 0.5 s before and
    1 s after the jump confirms it (`RECENTER: runtime recenter absorbed`).
    OpenVR events are polled every 0.5 s on the monitor thread, so they
    arrive after the jump.
  - **OpenXR.** The event's `poseInPreviousSpace`, when valid, replaces the
    measured transform (`vr::xr::TakeSpaceChange`).
  - **Jump without an event** within 1 s: a tracking jump, put back
    (`RECENTER: headset pose jump without a runtime recenter event`).
  - **Event without a jump**: does nothing (`... event without a pose jump:
    ignored`).
  - Recentering is now the player's own (both sticks, menu). After a runtime
    recenter, the view does not turn to face the new forward.
- **Mounts** (`PawnOnStaticGround`, `ViewOnFirmGround`, `vr::pure::ModeBlend`):
  - **Moving Base.** A Base counts as moving only above 20 units/s
    (`BaseMovingFast`). Before: 0.5 units between any two calls, even within
    one tick (e.g. a prop settling under Walker).
  - **Turret on a world base** (none, `bStatic` or `bWorldGeometry`): the view
    uses the ground formula (Pawn centre + eye height + head, eyesmooth, no
    6-unit head limit). The body, room-scale and first-person offset still
    treat every turret as a mount.
  - **Moving vehicle** (helicopter): the neck base is kept (19/10 cabin
    fixes), and its offset from the Pawn is frozen after 1 s on board. Before,
    it followed Walker's animation through a 1 s filter. It is re-aligned only
    if it stays more than 10 units off for more than 1 s.
  - **Found while measuring, not changed (pre-existing).** In the prologue
    minigun the neck base engages in some runs only:
    - WI-0's and WI-1's final runs: eye at the Pawn centre + 32.5 units;
    - WI-1's "before" run and WI-2's runs: Walker's neck, 20 units forward
      and 12 up.
    So the helicopter view height can differ by about 20 units (40 cm) from
    one session to the next. Cause unverified (the bone query or its 90-unit
    sanity test at the first sample are the candidates). Follow-up for WI-3
    (body) and a headset check.
  - **Mode change** (ground / vehicle, neck base, first-person offset 15 <->
    0): eased over 0.3 s. The difference between the last tick's eye and the
    new mode's eye, in the body frame, decays linearly (`VIEWMODE:` line).
    Over 60 units it counts as a teleport and is not eased. Unit test: 15.5
    units eased, at most 0.72 units per tick at 72 Hz.
- **Cutaways.** While the virtual screen is about to take over and the game
  camera is more than 200 units from Walker, the view is held black from the
  first tick (review round 1: from the second far tick, and held until 0.3 s back on Walker) (`FADE: the game camera left Walker`). Before, 150-600 ms of the
  scene camera showed in 3D with the head on it. Close shots of scripted
  scenes stay as before (19/10: continuous black flashes).
- **Pair skew counter.** The `VR-OUTPUT: submit` line ends with `L/R from
  different ticks N (largest M ticks)`: the pairs the worker sent with the two
  eyes from different stereo ticks. This is the vergence error suspected in
  bug 3; headset check.
- **New VIEW fields:**
  - `pullsrc`: 1 eye-level trace, 2 low probes;
  - `latch`;
  - `hc`: head collision correction, units;
  - `vfirm`: view mode ground;
  - `neck`;
  - `blend`: mode easing offset;
  - `vref`: view height reference, m;
  - `movskip`: moving actors skipped by the pull traces.

#### Measured (copy B, 25/10; before = the WI-1 build, rebuilt from the WI-2 snapshot)

| Check | Before | After |
|---|---|---|
| Squadmate 30 / 40 units in front, head still: ticks with wall pull; view pulled back | 360 of 360; 16.2 / 6.3 units | 0; 0.000 |
| Same, 40 units, head swaying (6 cm up, 8 cm forward): forward gain, forward residual spread | 0.985, 1.82 units | 1.000, 0.005 units |
| Clamped eye height (`height auto`, `seated off`, head at 1.20 m after a 1.30 m peak), head still 30 s: drift | 4.49 units (20.5 -> 25.0) | 0.000-0.001 units (30.0, Walker's eye) |
| Unclamped (1.70 m after a 1.80 m peak): drift (eyesmooth on the reference) | 0.064 units | 0.001 units |
| `simevent zeropose`, head 40 deg / 14 cm off the reference: view yaw step, eye step | 40.0 deg, 7.07 units | 0.0 deg, 0.0 units (absorbed; event confirmed 0.5 s later) |
| Fake runtime smoke: the same with 36 cm / 40 deg, ticks up to 87 ms (copy A on the GPU) | 40.0 deg (snap) | 0.0 deg (the first WI-2 version, ticks <= 50 ms only, missed it: 40.0) |
| Checkerboard room, flat BSP wall: head sliding along it +-10 cm (wall frame) | tangential gain 1.000 | 1.000 |
| Same wall: forward / vertical / turn sweeps, forward residual spread | 0.005 units | 0.005 units |
| Room corner, head 14 cm forward, swept sideways (two walls) | t-scaling in 390 of 430 ticks; right gain 0.875 | plane clamp, t never used; right gain 0.79-0.85 (the true 8-unit distance from each plane, not along the line from Walker's centre) |
| Low probes, head bobbing +-2 units across a crate edge, 6 s (unit test) | 12 toggles (pull 15-20 units each) | 1 engage, 0 releases |
| Prologue minigun, head still 5.5 s: eye drift in the Pawn frame | 0.004 units | 0.002-0.005 units |
| Mode change on the ground, first-person offset 15 -> 0 and back (`fp 40 0`, the same change as boarding a turret): largest tick step of the eye in the Pawn frame | 15.0 units in one tick | 0.729 units, eased over 44 ticks |
| Mode change easing, 15.5 units at 72 Hz (unit test) | one tick | <= 0.72 units per tick |

In the prologue, the boarding (`TurretEnter`, offset 15 -> 0) happens right at the end of the settle black hold in both builds, and a 15019-unit teleport, also held black, follows 1 s later. So no mount step can be seen there.

Earlier suites on the final WI-2 build (dll c1fcce0c, 25/10 20:42):
- `tools/regress.py`: PASS (fake runtime).
- WI-1 head suite unchanged:
  - drawn view vs headset 0.002-0.003 deg in every case;
  - posed mismatch 0.003 deg;
  - forced-motion vignette frames 5 / 12 / 13 (was 2 / 34 / 5);
  - the submitpose-off mismatch was 0.52 deg rms against 0.21 at 64.7 frames/s, while copy A loaded the GPU.
- Fake runtime smoke: every functional check passes, including the new zeropose check (absorbed, view yaw step 0.0 deg; the printout said -1 because of a formatting bug in the suite, now fixed). The two throughput checks failed (62.0 and 68.2 frames/s at 72 and 90 Hz), as in WI-1's run.
- Dynres in game: 0 changes.
- Crouch: unchanged (WI-3's baseline still reproduced).
- WI-0 turret suites, second run (the first run's windows hit the end-of-flight teleport and a 25 frames/s stretch):
  - headset path: view vs helicopter 0.000 deg, turret 30.0 deg;
  - synth stand-in: not reproduced.
- Aim baseline on the fake runtime: 30 of 30 poses with shots; the barrel directions are the WI-0 ones (e.g. UMP45 at -24 / roll 30: yaw +3.0).

#### Not covered without a headset

Not measured headless, and why:
- **The real cp01 crate and the M2E2 turret** (`SP_Desert01_03_K`) need checkpoint jumps. Those overwrite the saves that the other game copy shares. The low-probe latch is covered by its unit test; the static-turret view formula by code reading only. Review round 1: the cp01 crate is measured (checkpoint jumps are allowed); the M2E2 boarding failed again.
- **Head penetration into a flat wall.** The checkerboard room's wall has a lower ledge that keeps the head out of the 8-unit margin. The plane clamp is shown at the room corner and in the unit test. Review round 1: measured with Walker teleported closer and the head 25-35 cm into the wall.
- **The OpenXR side** (`poseInPreviousSpace`, the `OPENXR: reference space change pending` line): the fake runtime does not run `vr_openxr.cpp`.

Headset checks:
- **Scale steady** standing, seated (`seated off` and `on`), in cover and in the helicopter.
- **Recenter events** on VDXR and Steam Link:
  - the Meta-button recenter must show `RECENTER: runtime recenter absorbed` with the view still;
  - count the `event without a pose jump` and `pose jump without a runtime recenter event` lines;
  - a held object must not jump during a recenter; hands, barrel and a mounted turret must stay put (review
    round 2: the `RECENTER` line should say `2 moved with the space, 0 did not`; no `GRAB: frame jump` line);
  - hold the recenter button long enough for the system UI to show: a `too late to tell` line must be followed
    by `runtime recenter absorbed`, not by a blink.
- **The pair-skew counter** (`L/R from different ticks`) on both runtimes. The fake runtime logged 10 such pairs in 79200 submits, around level loads.
- **Runtime scale settings**: VD world scale at 100%, SteamVR's per-application world-scale override off.
- **Head in walls** at crates, props, squadmates and door frames: nearest-hit traces on static meshes were not exercised headless.
- **Cutaway black**: chapter 1 helicopter scenes must not flash black continuously. Review round 2: count the
  `CUTSHOT` lines (durations, `then back on Walker` = an isolated shot that went black) and the `cutaway black
  over after` lines over a session; the fake runtime saw 0 of both in the chapter 1 intro and the prologue.
- **The prologue helicopter eye height**: neck base or Pawn centre, see the note under Mounts.

#### Review fixes (round 1, 26/10)

Copy B, final dll 3a1c8947 (26/10 02:22), fake runtime `openvr 72`, `mode hmd`, VIEW and CAMLOG lines. Scripts and
raw logs: scratchpad `B\wi2_fix1` (`ph_room2.py`, `ph_corner.py`, `ph_pawn2.py`, `ph_crate.py`, `ph_turnf2.py`,
`ph_pass.py`, `m2e2_wi2.py`, `m2e2_board2.py`). These replace the matching bullets of the WI-2 list above where
they disagree.

- **Runtime-recenter detector** (`vr::pure::HeadJumpDetector`, rewritten). The first version took the jump off
  the references at once, restarted its history from the jumped sample and put the jump back with a snap 1 s
  later if no event came. A line-by-line replica (review) showed false jumps and 1 s offsets from ordinary
  tracking: a single bad sample doubled (10 deg, then 20 deg for 1 s), a repeated sample in a fast turn, an
  80 ms predicted pose (VDXR) with one long tick, and the exact yaw flipping 180 deg when looking straight down.
  Now:
  - **Input**: the headset pose without prediction (`GetDeviceToAbsoluteTrackingPose(0)`, or the hand query
    when that one is unpredicted), and the full rotation (angle against the extrapolated rotation), not the yaw.
  - **A runtime recenter only turns the space about the vertical**: a jump whose rotation is tilted by more than
    half the angle threshold is not a recenter.
  - **Two steps.** The tick of the jump is held: the head pose of that tick is mapped back into the previous
    space (`vr::pure::ShiftPose`), the references are not touched (`kHold`). The next tick decides between
    three hypotheses: (a) the new space (pose and velocity continue from the jumped sample), (b) the old space
    (the sample was bad or repeated: it is dropped from the history), (c) real head motion (the history
    restarts from the sample). Only (a) takes the jump off the references (`kConfirm`). (b) and (c) change
    nothing (`RECENTER: one-tick headset pose jump not taken as a recenter (...)`, at most every 2 s).
  - **Put-back** (a confirmed jump with no runtime event within 1 s): under a 0.3 s blink, applied the tick
    after the blink request. Before: a one-tick snap of the view, the aim and the props hand frame.
  - **Vignette**: the forced-motion block uses the head yaw minus the absorbed space yaw, so an absorb no
    longer flashes the vignette for a tick.
  - **Unit tests** (old detector replicated in the test for the comparison):

    | Case | Old detector | New |
    |---|---|---|
    | One bad sample, 10 deg yaw, head turning 29 deg/s | 2 jumps (view off 10, then 20 deg for 1 s) | 1 hold, 0 confirms, view error 0.00 deg |
    | One bad sample, 10 deg pitch | 0 jumps (yaw only) | tilted: no hold, 0 confirms; shown for one tick, as before WI-2 |
    | Repeated sample after a 30 ms tick, 300 deg/s turn | 2 jumps | 1 hold, 0 confirms, view error 0.00 deg |
    | Nodding across straight down, 10 s | 20 false jumps | 0 detections |
    | 60 deg / 250 ms flick (peak 450 deg/s), 80 ms prediction, one 30 / 45 / 60 ms tick in 60 positions | false jumps in 3 / 22 / 31 runs | 0 confirms (3 / 22 / 31 holds, each released the next tick) |
    | Same, no prediction (what the detector now gets) | 0 | 0 holds |
    | Runtime recenter 30 deg / 20 cm | 1 jump | 1 hold + 1 confirm, found 29.97 deg (0.150, -0.130) m, view error max 0.033 deg |
    | 20 s of head motion up to 364 deg/s with pitch and roll; 87 ms ticks | 0 | 0 detections; 0 confirms |

  - **In game**, `simevent zeropose` with the head 40 deg / 14 cm off: hold, confirm, event 0.47 s later
    (`runtime recenter absorbed`); view yaw step 0.0 deg, eye step 0.0 units.
  - **Not covered**: vertical space changes (a stage floor re-calibration, `poseInPreviousSpace` y) are still
    not absorbed: the view moves by dy x 50 units in one tick, as with any real head move. Known limit, headset
    check. Before WI-2 the event recentre reset the height instead.
- **Eye-height reference too low** (`vr::pure::ViewHeightRecapture`, replaces the "rise rule"). The rule raised
  the reference by half of every rise per tick and never lowered it, so tracking noise and sway ratcheted it
  (review replica: 6 units in 30 s with 0.2 mm noise; seated players lost 2.6-3 units per stand-up). Now one
  re-capture: the head (filtered, 0.25 s) more than 15 cm above the reference for 3 s in a row (the timer
  restarts only below 13 cm), then the median of the last 1.5 s, applied under a 0.3 s blink when it changes
  the view by 0.5 units or more (`HEIGHT: view reference re-captured`). Never with `seated on` or a fixed
  `height`.
  - Unit tests (0.3 mm white noise, 3 mm sway): headset on at 1.20 m, standing at 1.70 m from t = 5 s: one
    re-capture at 8.1 s, reference 1.702 m, view 40.00 units, drift over the next 30 s 0.000 units. `seated off`
    at 1.20 m rising 25 cm for 2 s, five times: 0 re-captures, seated view 30.00 units. Noise and sway only,
    60 s: 0.
  - In game: head 1.20 -> 1.70 m with 3 mm sway: re-capture after 3.1 s under the blink (view 55.0 -> 40.0
    units), then 40.00 +- 0.15 units (the sway itself) for 30 s. Clamped 1.20 m and free 1.70 m, head still
    30 s: drift 0.000 units.
- **Head collision against squadmates** (`headtrace`, default 1). The WI-2 claim "without 0x200 the trace never
  hits the squadmate" was wrong: that pass also removed TRACE_Pawns (0x1), and the hits seen before were the low
  probes. Measured with Lugo held 18-30 units in front of Walker (level 1 start):

  | Lugo ahead | Head | Mode 0 (WI-2: two passes) | Mode 1 (one pass, no 0x200, with pawns) |
  |---|---|---|---|
  | 18 units | still / leaning 10 cm / looking down 40 deg | head trace hits him (t 0.41-0.55); correction 10.4 / 14.1 / 12.5 units; eye 10.3 / 7.7 / 8.2 units from his centre | same: 10.5 / 15.0 / 12.5 units; 10.4 / 7.8 / 8.2 |
  | 22 units | same | 9.1 / 12.4 / 10.1 units; eye 9.3 / 9.0 / 10.2 | 9.1 / 12.5 / 10.1; 9.9 / 8.9 / 10.1 |
  | 30 units | same | 0 / 1.3 / 0 units | 0 / 1.3 / 0 |

  So one pass without 0x200 with pawns (`kViewTraceFlags`) does hit him, with the same result as the two
  passes, and now there is no pass left whose BSP hit could skip a squadmate standing in front of a wall. Mode
  1 is the default (one SingleLineCheck fewer per tick); `headtrace 0|1|2` stays as a test command (`+4` = the
  wall pull with the old flags, for before/after tests). Not measured: a squadmate with a BSP wall right behind
  him (level 1 has no BSP there).
- **Squadmate walking past** (80 units/s across, head still): 40 units ahead nothing (0 pull ticks, 0 hits);
  30 units ahead 0 pull ticks, the head stopped 3.9 units at most for 31 ticks while he crossed the head
  trace (snap-in by design, release tau 0.1 s). Before WI-2 a squadmate 30 units ahead pulled the view 16.4
  units in every tick.
- **Static meshes and the real cp01 crate** (`ce cp01`, chapter 1; checkpoint jumps allowed since 25/10 23:40).
  Walker spawns behind the crate (a `StaticMeshCollectionActor`): the low probes hit it at t 0.65 in every tick
  with both flag sets (`headtrace 5` = the old flags, `headtrace 1` = `TraceViewNearest`). Pull 12.24 units,
  spread 0.03 units or less while bobbing +-4 cm at three heights (1.55 / 1.43 / 1.31 m), latch engaged the
  whole time (0 toggles), forward / side / vertical gains 1.000. No BSP inside the probes' reach there, so the
  two flag sets give the same numbers. At the level 1 start the probes also hit a `concrete_facade_destroyed`
  StaticMeshActor with both. The no-0x200 pass therefore hits static meshes; the BSP-masking case itself is
  covered by the room's BSP walls only.
- **Head collision at corners** (`vr::pure::ClampIntoPlanes`). Up to three planes: the first hit, then any
  surface the check trace meets closer than the margin along its normal (a plane already used, or one beyond
  the margin, is fine). The traces now reach two margins (16 units) past the eye, so a surface is found before
  the eye enters its margin, also with the trace oblique to it: a first version with the old 2-unit check made
  the eye jump 6.5 units when the second wall came in. Measured in the checkerboard room (BSP, Walker teleported,
  `roomscale off`):
  - **Flat wall, head 25-35 cm into it** (the review asked for a run with the clamp engaged; hc 13-23 units):
    tangential gain 1.000, gain into the wall 0.03, forward residual spread 0.145 units, no oscillation.
  - **Corner, head pushed into both walls**: two planes in all 430 ticks, eye held at the corner (gains -0.04 /
    -0.03), forward residual spread 0.04 units; largest eye step beyond the head's 0.74 units.
  - **Corner, the review's sweep** (head 14 cm forward, +-10 cm sideways): the head touches one wall or the
    other (182 / 147 ticks), never both; along each wall the tangential gain is 1.000. The "right" gain of 0.80
    is the part of the motion that goes into a wall, which is removed on purpose (WI-2 final 0.79, before WI-2
    0.875 from t-scaling the whole vector). Forward residual spread 1.2 units, 0.92 reversals per second, both
    following the 0.5 Hz head sway.
- **Eye pivot lag (bug 2 cause F, `fppivot`)**. ApplyFirstPerson puts the eyes 15 units forward along the
  Pawn yaw at the camera copy, one tick before this tick's body rotation; the pivot compensation already counts
  the new body. Now GetPlayerViewPoint moves the forward offset to the Pawn's current yaw (also in cover: the
  eyes still follow the Pawn, 18/10). The wall pull uses that direction too. Measured with the head held still
  in the room (`fppivot 0` = WI-2, same session):

  | Turn | Eye step beyond the model, max / spread (units), before | After |
  |---|---|---|
  | Body following the head (+-90 deg head sway at 0.25 Hz, 290 deg of body turn) | 0.36 / 0.79 | 0.011 / 0.008 |
  | Stick, 150 deg/s | 0.56 / 0.66 (pops at start and stop) | 0.011 / 0.009 |
  | Snap turns of 30 deg | 7.77 (one tick per snap) | 0.004 / 0.004 |

  Not measured: cover entry and exit (the game's ~50 deg turn); the correction applies there too, headset
  check.
- **Cutaway black**: a single far tick no longer counts (2 ticks in a row), and once black it stays black until
  the camera has been back on Walker for 0.3 s or the virtual screen comes. A series of short shots with quick
  returns to Walker is one black period, never black-VR-black. A line at the end gives the duration and the
  number of far shots (`FADE: cutaway black over after ...`), for the headset session.
- **Trace cost** (VIEW `trus` / `trn`, cumulative; per tick in the room and wall runs): 4-6 SingleLineChecks,
  mean about 5 microseconds, at most 88 microseconds.
- **Naming**: the pull direction comes from `g_fpYawApplied` (atomic; was the diagnostic `g_viewDiagFpYaw`).
- **Not measured** (one attempt each, no repeat on the same level):
  - **M2E2 turret** (`SP_Desert01_p` + `ce cpBusRoof`, gunner killed): boarding failed with the grab-point
    press (three positions) and with the walk-in from 220 / 160 / 110 / 70 units, as in WI-1 fix 2's last
    attempts. Before boarding, Walker is PHYS_Walking on Base `Terrain` (+0x9C 0x3304510a, world bits set), no
    DrivenVehicle. Whether he stays so on the turret, so that `ViewOnFirmGround` is true, is still unverified
    (WI-1 fix 2's successful boarding logged Base `Terrain`, Physics 0 in the TURRET line).
  - **The prologue neck base** engaging in some runs only: pre-existing, see Mounts; WI-3.
- **Runtime recenter UX**: a deliberate Meta-button or SteamVR recenter is absorbed (the view stays still), as
  in WI-2. Whether a confirmed jump plus event should instead do the mod's own recenter under a blink is the
  user's decision.
- **Earlier suites on the final build** (dll 3a1c8947, each once; `B\wi2_fix1\final_chain.sh`):
  - `tools/regress.py`: PASS. WI-1 predictframe: PASS. WI-1 head: drawn view vs headset 0.002-0.003 deg, no
    forced-motion vignette. Dynres: 0 changes steady, 4 in the forced over-budget window (as designed).
  - Fake runtime smoke: every functional check passes (zeropose: view yaw step 0.0 deg); the 120 Hz throughput
    check (112 frames/s) and its hitch-count check fail with copy A on the GPU, as in WI-1; the `fpscap 0` hitch
    suite passes (4 hitches, 20 repeats in 750 frames).
  - Crouch baseline: unchanged (WI-3's bug still reproduced). Prologue minigun: eye drift in the Pawn frame
    0.002-0.003 units. Turret flick and wrap: view vs helicopter 0.000 deg; headset-path and synth turret
    baselines: not reproduced.
  - Turret +-120: view vs helicopter 7.8 deg in the first run and 2.4 deg in a second (0.5 deg in WI-1 fix 1
    and 2), in steps of 0.01-0.2 deg while the turret sits at its limit and the helicopter turns (Base delta
    up to 2.1 deg per tick). No detector, height or recenter line in either window, and none of this round's
    changes touch the view yaw outside those, so this is WI-1's open item (turret at its limit, the game not
    carrying the PlayerController with the Base; WI-4c clamp), now hit in a longer window.
  - Aim on the fake runtime: 30 of 30 poses with shots; barrel directions within 0.08 deg of WI-1 fix 2's
    final run (shots clipped on the hand spheres as before: fixed in copy A, merge item).
  - `simevent zeropose`: no forced-motion vignette at the absorb (WI-2: one tick at 1.0).

#### Review fixes (round 2, 26/10)

Copy B, final dll 453ce542 (26/10 04:13), fake runtime `openvr 72`, `mode hmd`. Scripts and raw logs: scratchpad
`B\wi2_fix2` (`ph_hands.py`, `ph_turret_zp.py`, `ph_declared.py`, `cut_scan.py`, `final_chain.sh`). Before/after
in the same session with the test command `recenterhands 0|1` (0 = round 1).

- **Hands, aim and turret across a runtime recenter.** Round 1 held only the head pose in the tick of the jump
  (`kHold`); the controllers were already in the new space and the references still old. So on every real
  runtime recenter the hands, the barrel, the two-hand aim, the turret and the props hand frame jumped for one
  tick (round 1's first WI-2 version had rebased the references in that same tick, so the hands stayed put).
  Now, in the held tick:
  - **Controllers**: the gun-hand and support-hand poses that moved with the space are mapped back into the old
    space too (`vr::pure::PoseFollowsShift`: mapped back, the pose is closer to the previous tick's than the raw
    one). A jump of the headset alone (bad sample) leaves them as they are. The `RECENTER` line counts them
    (`controllers in the held tick: N moved with the space, M did not`).
  - **Declared pose** (`submitpose`, VIEW `hmdq`): the raw pose, in the runtime's space, when the controllers
    say the space changed. Before: the held pose, which the compositor read in the new space.
  - **Props hand frame** (`g_trackRefYaw` / `g_trackOriginOff`): given in the first space (the references mapped
    back through the absorbed jumps, `spaceAbsorbed`). The hands in the world are the same, but an absorbed
    recenter no longer changes the frame, so props.cpp no longer sees a frame jump and does not carry the held
    objects and hand spheres around the tracking origin. A jump put back under the blink still changes it, like
    the hands.
  - Measured, `simevent zeropose` (head 40 deg / 14 cm off, the fake runtime moves the controllers too), level 1,
    rifle, `aimpitch 0`:

    | Per-tick step around the event | Round 1 (`recenterhands 0`) | Now |
    |---|---|---|
    | Gun-hand offset / support-hand offset | 16.7 / 25.5 units (33 / 51 cm) | 0.000 / 0.000 |
    | Barrel yaw, one hand (`twohand off`) | 40.0 deg | 0.000 |
    | Two-hand aim (engaged): largest excursion | 6.3-6.4 deg | 0.000 |
    | Props frame: yaw / origin; `GRAB: frame jump` lines | 40.0 deg / 7.07 units; 1 (2 hand spheres carried 51 cm) | 0 / 0; 0 |
    | View yaw | 0.0 deg | 0.0 deg |
    | Prologue minigun, hand held 30 deg right: turret step (vs the helicopter) / aim step, two runs | 11.8-14.8 / 51.8-54.8 deg | 0.000-0.005 / 0.000-0.005 deg |
    | `submitpose on`: frames reprojected by 40 deg (fake `JITTERF` edge) | 3 | 2 |

    The 2 frames left with `submitpose on` were drawn before the recenter and shown after it (45 ms of display
    latency on the fake runtime): any frame in flight at a runtime recenter is reprojected across the space
    change by the runtime itself. The one removed is the held tick's.
- **Recenter followed by a long tick** (`HeadJumpDetector::kUnsure`). If the tick after the jump comes after
  more than 150 ms (the Quest system UI appears while the recenter button is held), the three hypotheses can no
  longer be told apart. Round 1 gave up (references untouched: the whole recenter showed as a snap, and the
  event was then logged as "without a pose jump"). Now the jump is taken off like a confirmed one, kept if the
  runtime event comes within the usual window, and otherwise put back under the 0.3 s blink, like any jump
  without an event (`RECENTER: ... next tick N ms later (too late to tell) ...`). Unit test, 30 deg / 20 cm
  recenter then a 200 ms tick: 1 hold, 1 unsure, found 29.97 deg, view error at most 0.033 deg (30.0 deg snap
  before). A tilted jump (no hold) followed by a long tick is rejected as before. In game this needs a long tick
  right after the jump, which the fake runtime cannot place: unit test only, headset check.
- **Cutaway black, measured through scripted scenes** (`cut_scan.py`, old and new dll, no skipping):
  - chapter 1 start (`SP_Desert01_p`, 400 s from the load into gameplay): the scripted intro is on the virtual
    screen from the load to the end (`CINEMA: end, back to VR`); 0 cutaway blacks, 0 far shots outside the
    screen;
  - prologue helicopter (`SP_RadioMan03_P`, 300 s, four flight legs): every leg ends with a teleport (virtual
    screen at once) and starts with the settle black; 0 cutaway blacks, 0 far shots outside the screen.

  So no isolated short far shot occurred in the scenes the tester and the 19/10 report were about, and the round 1
  rule stays (black from the second far tick, held until 0.3 s back on Walker). What such a shot costs is known:
  0.1-0.6 s of shot plus 0.3 s, plus the 0.25 s fade-in. New line for every far shot of 2 ticks or more outside
  the virtual screen: `CUTSHOT: far shot X s (N ticks), then back on Walker | then the virtual screen`. A
  synthetic shot (`ucall pc SetViewTarget CameraActor`, then back to the pawn) crashed copy B's game at the switch
  back (exe+0xA58734, `specopsvr_crash_212851109.dmp`; the mod's code is not on that path); not retried.
- **Docs**: README (squadmates walking past: the head only stops at them within reach); the WI-2 list's head
  collision bullet marked as replaced by `headtrace 1`.
- **Earlier suites on the final build** (dll 453ce542, `B\wi2_fix2\final_chain.sh`, each once; three re-run
  once in `rerun.sh` because the first run was spoiled by the shared machine):
  - unit tests 454 / 0; `simevent zeropose` view yaw step 0.0 deg, eye step 0.0 units; height: one re-capture
    (1.200 -> 1.702 m), then 40.00 +- 0.15 units; clamped / free drift 0.001 units; `fppivot` 0.36-7.77 ->
    0.005-0.012 units (same as round 1); WI-1 predictframe PASS; WI-1 head: drawn view vs headset 0.002-0.005 deg;
    hitch suite (`fpscap 0`) PASS; crouch baseline unchanged (WI-3's bug still reproduced); prologue minigun
    eye drift 0.002 units; turret flick and wrap: view vs helicopter 0.000 deg; pm120 1.9 deg at the turret
    limit (WI-1's open item); aim on the fake runtime 30 of 30 poses, barrels within 0.034 deg of round 1.
  - Fake runtime smoke: every functional check passes (zeropose 0.0 deg); the 72 and 120 Hz throughput checks
    (67.7 / 113.8 frames/s) and the hitch count fail with copy A on the GPU. In that window the HITCH lines put
    the long ticks outside the mod's draw (head tracking at most 0.10 ms), so not this change.
  - Dynres: 0 changes steady and 4 in the over-budget window as in round 1; in the recovery window a step to
    75% failed back to 65% and the climb finished in the next window (GPU shared with copy A).
  - Re-run once: `tools/regress.py` (first run: "move: no gameplay STATS lines", copy B fell to about 16
    frames/s during the 8 s window) PASS; synth turret baseline (first run: tick p90 231 ms) not reproduced;
    headset-path turret baseline (first run: the window ended on the end-of-flight teleport, view and turret
    jumping together by 14.4 deg) not reproduced.
- **Still open** (merge / headset checklist): the M2E2 static-turret formula and the real mount step (boarding
  failed in round 1 with two approaches); cover entry and exit for cause F (`cover_ticks` 0 in round 1); a
  squadmate with a BSP wall right behind him; the recenter UX (user decision); the long-tick recenter in game.

### WI-3: body, seated play, crouch (bugs 5 and 4)

Bug 5: "body too prominent (playing seated), the torso is constantly visible". Bug 4: "crouch technically works but
there is no noticeable change in camera height". The user asked for a body "really ready for full VR". Measured on copy
B: novr, fake runtime `openvr 72 45`, `mode hmd` for the head poses (the fake HMD is moved on a neck model, eyes 8 cm
in front of and 13 cm above a fixed pivot, like a real head) and `mode synth` for the crouch. Scripts in the scratchpad:
`wi3\bodytest.py`, `crouch_wi3.py`, `seated_test.py`, `arms_test.py`, `measure*.py`.

**Measurements first (26/10, bodylog with the head unhidden, standing).**
- Walker's eyes (`FACE_LEYE` / `FACE_REYE`) are 36.25 units above the Pawn centre (81 above the floor, a headset at
  1.62 m), 8.2 in front of and 5.7 above his `Neck` bone.
- `Neck` is at +30.5, the shoulders at +26.2, the skeleton is 94 units tall (Root to Camera_Bone).
- `Pawn.BaseEyeHeight` (+0x2E4) is 30: the game's logical camera, **not** the model's eyes. The investigation had
  taken it for Walker's eyes; the real eyes are 6 units higher. `CrouchHeight` +0x23C = 25.
- Crouched by the game, the eyes drop 29 units and the Pawn centre (capsule 45 -> 25) 20.
- On sand dunes the game's floor adaptation sinks the mesh by up to ~4 units, so a live neck height varies with the
  terrain.

**Causes (confirmed in code and now in screenshots).**
- The body followed the headset position 1:1, 10 units forward (`bodyback -10`), with threshold patches (looking down
  past 30 degrees: 26 units back; head turned past 35 degrees: 26 units away from the gaze; crouch posture). Turning or
  bending the head moves the eyes 3-8 units around the neck, so the chest was dragged under the eyes, and the patches
  then shoved the body away. Screenshot `before 1.20off y60 p-15/-30`: the inside of the jacket collar fills the view.
- A seated player on the default `seated off` had the eye clamped to 30 (WI-2's lower bound), inside Walker's neck
  (+30.5); `seated on` put it at `fp` 40, 3.75 above Walker's eyes. Nothing detected seated play.
- `fpbody off` turned the whole body management off: the game's fade cylinders came back (Walker semi-transparent),
  the head was shown, and with `g_fpPawn` null KeepGoodBones (gun to the hip while walking) and finger posing stopped.
- Crouch: `crouchview real` added the game's 20-unit drop back on every crouch, so the crouch button never moved the
  view (baseline: Pawn -20.5, view -0.47); the body raise that went with it was written to Translation.Z, which the game
  rewrites every tick (dead code); `UpdateMeshLowering` left the +20 out of its eye height.
- Seated in cover, the cover-peek entry clamp (head 25 cm below the standing reference) made a seated player "lean
  out" at once (LT held, Walker left the cover).

**Changes.**
- **Neck anchor** (`vr::pure::NeckAnchorShift`, `NeckAnchorStep`; `UpdateFirstPersonBody`). The player's neck pivot is
  the final eye of the last drawn tick (wall pull and head collision included) minus the head rotation applied to
  `neckmodel 8 13` (cm forward, up). The point of Walker that is the same distance behind and below his own eyes is
  put on it: with the head straight his eyes are on yours; turning or bending the head leaves the body where it is;
  leaning (also in a chair) moves it. Walker's `Neck` is read in mesh space (no feedback from the shift): standing, a
  slow average (3 s), because the game's aim pose moves the live Neck by a few units at every new aim; the live value
  (0.12 s) only in the second after a posture change, so the crouch pose is followed. 0.5-unit deadband, at most 200
  units/s, 60 units away; at most 10 units in cover; nothing on mounts and in animated moves (as before).
  **Reach** (review fix 1; the first version had a constant cap, see "Review fixes (round 1)"): the body moves forward
  only as far as the gun hand needs. After each skeleton update (`MeasureGunReach`, in the `UpdateSkelPose` hook) the
  gun arm's length (upper arm + forearm bones) and the IK target relative to the shoulder give the forward shift that
  keeps the target 2 units inside the arm's reach (`vr::pure::ReachForwardNeed`); the anchor adds it, never past the
  old placement (10 forward), rising at once and released after 0.5 s at 15 units/s. Found by the WI-4a gun-frame
  suite: with the pistol the game's stance puts the Neck ~11 units forward, the anchor moved the body 14 units behind
  the old placement, the gun arm hit its stretch limit and the M9A1 / SAW drew the gun up to 1.4 degrees off. Walker's
  arm is short for VR (24 units, 48 cm, stretch 1.25): a fully registered body would need longer arm stretch; that is
  a headset decision (arms seams).
  **Neck ring** (review fix 1): with head and Neck hidden, the collar keeps an opening around Walker's Neck. Looking
  down past the lower edge of the field of view (pitch minus the lower half-FOV beyond -90 degrees) the anchor moves
  the body back along the gaze just enough for a ring of 4 units around the Neck, 1 above it, to stay 1 unit outside
  the lower edge (`vr::pure::NeckRingClearance`). It acts for tall players (eyes 10-20 units above Walker's neck); for
  the others the neck pivot alone keeps the ring out.
  `bodyback` stays as a fine-tune (units back, now 0); the look/turned heuristics, the crouch-pose correction and the
  dead Translation.Z write are gone.
- **Eyes out of the body** (`UpdateMeshLowering`): unchanged rule (the body only goes down, never for short players
  standing upright), with two fixes. Its eye height is the real final eye of the last tick (the formula missed
  smoothing, collision and the crouch view), taken back to the head-level eye on the neck pivot
  (`vr::pure::NeckPivotEyeZ`, review fix 1: bending the head moves the eye ~10 units down but forward, in front of the
  throat, so the body must follow the pivot, not the eye). Of the crouch-view drop V only the unfinished part of the
  ramp is added back (review fix 1). The standing gap (eye to Walker's neck, upright) is learned only with
  the head within 20 degrees of level: looking down for a few seconds the standing reference decays 1 cm/s toward the
  lowered head, the old condition became true again and the learned gap drifted to zero (1.64 m: 6.6 -> -0.3 units),
  so at -85 degrees the body no longer went down and the eyes ended in the collar.
- **Seated play** (`seated auto|on|off`, default auto, Basic "Seated play"; `vr::pure::SeatedDetector`): the median
  head height over 3 s with the headset worn, after the scene start, every recenter, the headset put back on, or
  "auto" chosen in the menu. Sitting below 1.35 m (review fix 1, was 1.30; the SEATED line logs the margin), or 0.30 m
  below a saved `height`. It decides only at those moments, never by itself mid-game, with one exception (review fix 1):
  detected sitting and then standing more than 0.30 m above the seated reference for 3 s, it measures again
  (`vr::pure::SeatedStandCheck`; only upward: a long real crouch never switches to seated). A change is announced
  (toast) and happens under a blink. Seated, the eye is at Walker's
  eye height (36.25, the measured constant: the live neck height changes with the terrain) plus the real head movement
  from the seated reference, which is the measured median at each decision (also when the mode did not change: a
  recenter taken with the headset off left the eye 27 units low). No physical crouch while seated (as before), and no
  cover-peek entry clamp. `height fixed` is also Walker's eye height now (was `fp`).
- **`fpbody full|arms`** (Basic "Walker's body"; `on` = full, `off` = arms). Body management always runs in first
  person (Walker opaque, head hidden, anchor, KeepGoodBones, finger posing). `arms` hides torso, legs and gear in
  rendering only: a hook on the skeletal mesh's render update (`FSkeletalMeshObject::Update`, MeshObject +0x26C,
  vtable +0x10, called at the end of `USkeletalMeshComponent::UpdateTransform` 0x00C33FD0) collapses those bones'
  `SpaceBases` to Spine3 with scale 0 just for that call and puts them back after it. The first version hooked the whole
  `UpdateTransform` (vtable +0x138): rendering was right, but the same function pushes the bone matrices into the
  physics bodies, and hittest lost the legs and the neck (-40, -16..-4, +28). The shoulders are kept (collapsed onto
  the arm joint they made a cone of the sleeve looking down).
- **Crouch view** (`crouchview auto|real|game`, default auto, Basic "Crouch view"; `crouchtime 0.35`;
  `vr::pure::CrouchView`, `UpdateCrouchView` once per tick before the eyes). The game's drop G (capsule) is always
  cancelled in the same frame (now seated too: before, seated players got a 20-unit step in one tick). A crouch the
  player did not make with the body adds V: button or script V = G minus the real crouch already there at its start;
  low cover V = G seated, 0 standing (standing players duck themselves, 19/10 rule); a real crouch V = 0 (never twice).
  V changes only at state changes, never with the head, along a smoothstep of `crouchtime` (0 = a step under a blink);
  it is applied after the eye-height smoothing (with `eyesmooth` on top it took 0.7 s) and drives the comfort vignette
  (|dV/dt| / 150). "Held": standing up for real while the game keeps Walker down (low ceiling) for 0.5 s makes the
  view join Walker under a blink (it moves against the head). Real-crouch drops are ignored for 0.5 s after a recenter.
  Ownership is exclusive: after a crouch or stand-up the player did not make (button, script, cover),
  `UpdatePhysicalCrouch` neither takes it over nor re-toggles until both the player and Walker are standing (this
  removes the double drop and the stale-reference re-crouch loop). `GameCrouchDrop` handles a Pawn first seen crouched.
  `CROUCHVIEW:` log line per source change (with the cover flags). Classification (review fix 1): a button or script
  crouch is the PlayerController's `m_wantsToCrouch` switched on not by the real crouch (`g_crouchButtonFlag` from
  `UpdatePhysicalCrouch`; cover does not touch it), unless it changes within 0.3 s of a cover state change (the game's
  own); a capsule drop without it is cover (cover state +0x498, cover moves 19/20, aiming from cover +0x5F0 bit
  0x800, the mod's cover peek, or cover seen in the last 1.5 s: dropping back into cover after aiming out of it the
  capsule shrinks before the cover state, found by regress), else after 0.3 s a script crouch (V as the button). A
  button crouch that enters cover keeps its V; leaving cover still crouched keeps V (standing 0, seated G) until Walker
  stands or the button is pressed. The first version reclassified a button crouch as cover (the view rose 20 with
  Walker crouched) and a cover crouch as button 1.8 s after leaving it (the view dropped 20 with the player still).
  The body lowering adds back only the unfinished part of the V ramp (rising from a button crouch, the view ramps up
  after Walker stands and the body dipped 9-11 units; at the end of the ramp the real eye counts).
- **Safety** (found while testing): at the first draw after a level load a `GetBoneLocation` call faulted on a mesh
  that was not usable ("None..SkeletalMeshComponent"), and `CallFunction` then disabled `GetBoneLocation` for the
  whole session (bodylog, armdebug, the arm measurements and the anchor all went "missing"; 3 of 5 level reopens).
  Two changes: the per-tick bone queries first check the component (`MeshOwnedBy`: Owner +0x4C is Walker, attached,
  neither object pending kill, `SpaceBases` posed, `SkeletalMesh` set): in WI-3 the body code (physical crouch, anchor,
  BODY line, arms list); since review fix 1 also `UpdateArms` (the arm rig's mesh, before any bone query of the tick),
  `MuzzleStart`, `UpdateCoverLean`, the neck-base calls in `GetPlayerViewPoint` and the GUNLOG hand lines. In
  `script_call.cpp` a fault in `GetBoneLocation` / `GetBoneName` / `GetBoneQuaternion` disables that call on that object
  only, keyed (review fix 1) by pointer, FName and Outer, and the per-object entries are cleared whenever the
  first-person body management starts again (`vr::script::ClearObjectFaults` where `g_fpPawn` is set: a new Walker,
  but also after every cinematic and every third-person spell, since the body management stops there): the game
  reuses freed blocks, and a new Walker mesh at the address of a faulted one lost its bone queries for the whole level.
  The faulting object, from the new log line, is the previous level's Walker mesh: a stale cached mesh pointer used at
  the first draw of the new level (pre-existing code, probably the arm rig before `UpdateArms` refreshes it; copy A has
  it too). The per-object fault keeps the other bone queries alive but does NOT make that stale call safe: copy A's
  level-change crash fix (26/10, dll d9a95b92) showed that `GetBoneLocation` on the previous level's freed mesh
  corrupts the allocator's free lists (FMallocWindows) and crashes the next level change. A fixed it at the source
  (`CallFunction` skips ProcessEvent when the object's GObjects slot no longer holds the object; `MuzzleStart` uses
  `g_rig.mesh` only if `StillAlive` finds it). At the merge both stay: A's liveness checks plus B's `MeshOwnedBy`
  guards and per-object key. Side effect of the fix: WI-2's "the prologue neck base engages in some runs only" was this session-wide fault
  (no `GetBoneLocation`, no neck base); now it engages every time (prologue minigun: eye at Walker's neck + 12, 11.0
  above the Pawn centre instead of `fp` 32.5; base fixed in the Pawn frame to 0.01 units over 13 s, eye within 0.15).
- configversion 11 (10 in copy B, renumbered at the merge of 27/10; same bump as WI-2/WI-4a): `seated off` -> auto, `crouchview real` -> auto, `fpbody on/off` ->
  full/arms (`vr::pure::kWi3ConfigChanges`). Verified in game on copy B's v9 ini.
- Diagnostics: the BODY line (`bodylog`) gains `anchor`, `target`, `shift`, `neckavg`, `neckexp` (expected Neck),
  `crouchcomp`, `crouchv`, `seatedmode`, `seateddet`, `fpbodymode`, `neckstand`, `eyesrc`, `lowerdiag` (eye, head,
  standing gap, lowering target); `SEATED:` and `CROUCHVIEW:` lines.

**Measured (before = WI-4a dll 36ca1d1f, after = final WI-3 dll; level 1, same machine and day).**
- **Screenshots** (`wi3\shots_before`, `wi3\shots_final2`, contact sheets `wi3\sheet_before_vs_final2_<height>.jpg`;
  heights 1.15 seated auto, 1.20 seated on, 1.20 seated off, 1.45, 1.64, 1.85 m x pitch 0/-15/-30/-60/-85 x head yaw
  0/60/120): before, a seated player (1.15 and 1.20 with the default `seated off`) saw the inside of the jacket collar
  fill the lower view with the head turned 60 degrees at -15 and -30; looking down, the body had been shoved 26 units
  back (only boots in view). After: no torso at 0/-15/-30 at any yaw and height; looking down, chest, vest and legs
  under you. Left for the headset: at -85 (looking straight down) the neck opening of the hidden head shows at the
  bottom edge, and with `seated off` forced at 1.20 m the support sleeve enters the lower left from -30.
- **bodylog matrix** (`bodytest.py`): torso bones (Neck, Spine3, Spine2, shoulders) out of the frustum or 5+ degrees
  below it at 0 and -15: 36/36 before and after (the bone points sit behind the eye plane; the vest surface does not,
  hence the screenshots). Neck behind the eyes along the gaze looking down (-60, -85): 36/36, 28.6-31 units behind
  before (body shoved back) -> 7.6-9.4 after (with the reach cap). Actual Neck vs expected (anchor) |W-N| <= 0.59
  units in 90/90 poses.
  Seated eye: 30 (clamp, `seated off`) or 40 (`seated on`) -> 36.2 (Walker's eyes). At -85 the safety lowering keeps
  Walker's neck 1.0-8.2 units under the eye (before the standing-gap fix the eye was up to 4.5 units under the neck).
- **Reach** (armdebug, hand-to-target units, fake hands mid / far / low): far right 12.6-14.5 -> 9.7-12.0, mid right
  1.6-3.4 -> 0.1-0.3, far left 3.8-7.2 -> 1.0-4.6, low 0.0-1.3 -> 0.0-0.1. No worse anywhere. Gun frame (WI-4a suite,
  M9A1): 9/9 within 0.040 deg, as the pre-WI-3 dll (see the reach cap above).
- **Crouch** (`crouch_wi3.py`, mode synth, 72 Hz): button, `auto`: view drop 0.47 -> 20.56 units (Pawn 20.5) in
  0.333 s (crouchtime 0.35), monotonic, at most 1.3 units per tick, back to the Pawn exactly (0.00). `real` and `game`
  unchanged (0.0 / 20.0). Physical 40 cm: view drop 20.0 (the head only), CROUCHVIEW "real crouch" V 0. Button, then
  real 40 cm, then stand up: Walker stays crouched (button owns it), view = head + 20, largest non-head step 1.19
  units. Seated + button: 20.0 in 0.333 s, monotonic (before: a 20-unit step through eyesmooth); 1.2 units per tick at
  72 frames/s, 4.9 in the final-build run, which dropped to ~36 frames/s with copy A on the GPU (the ramp is timed).
- **Seated** (`seated_test.py`, fake runtime): 1.15 m -> sitting, eye 36.25; 1.70 m -> standing, eye 40.0; headset
  off at the recenter -> no decision until it is back on, then sitting and eye 36.25 (was 8.75 before the reference
  fix); head 10 cm up without a recenter -> no mode change, eye +5.0 (1:1); `simevent zeropose` -> eye unchanged
  (0.00); `seated on` / `off` respected. 7/7.
- **fpbody arms** (`arms_test.py`, `arms_ab.py`): hook on (0x00B17DF0 in this build's game), 65 of 126 bones collapsed;
  screenshots `wi3\sheet_arms_v2.jpg`: torso and legs gone, arms, shoulder pads and gun kept, shadow of arms and gun
  only; one small attached gear piece stays visible at -85. Hittest alternated full / arms / full / arms (3 rounds, 15
  heights): identical hit rates at every height except -16 (full 1/3, arms 2/3: the gap between the knees, pose
  noise). The first hook (whole `UpdateTransform`) lost the legs and the neck: -40, -16..-4 and +28.
- **Menu** (`menu_wi3.py`, `vrmenu cycle`): 12/12 clicks on Seated play, Crouch view, Walker's body, crouchtime,
  neckmodel; values restored. **Migration** on copy B's v9 ini: `fpbody on` -> full, `seated off` -> auto (no
  crouchview line in that ini: the embedded default auto applies), `aimpitch -24.0` -> gunstock 15.4 (WI-4a).
- Unit tests 728/0 (neck anchor invariance over 48 head poses < 0.001 units, step and cap, seated detector incl.
  presence and the median, crouch-view cases 1-9, migration, word choices).

**Not covered here / headset checks.** Seated defaults in a real chair (no torso ahead, natural glances, chest and
legs below without a neck stump; leaning in the chair moves the body); the floor match on VDXR (STAGE) and Steam Link
(the seated decision uses the absolute head height); `crouchtime` 0.2 / 0.35 / 0.5 and the ramp vignette; cover duck
and peek seated and standing; the arm seams and the shoulders in `fpbody arms` (the shadow shows only arms and gun);
the headshot question (the hidden head is not hit: hittest finds nothing above +28, in full and arms alike; changing
that is the user's call); the crouch button on the Touch controllers (the game's binding is in the encrypted
SRInput.ini).

#### WI-3 review fixes (round 1)

The review found that looking down still showed the inside of Walker's body (collar ring, open neck hole) at every
height, that the "reach cap" overrode the anchor's forward axis in 81 of 90 poses, and that the eye floor was still
Walker's neck (30), plus medium findings on the crouch view and seated play. All handled; measured on copy B (dll
from this round, level 1 CP04, fake runtime on the neck-model head, same matrix as above).

- **Looking down (high).** Three causes, three fixes. (1) The safety lowering followed the pitched eye: chinning down
  moves the eye ~10 units down (but forward), and the body went down 5-9 units, bringing the collar under the eye;
  it now uses the head-level eye on the neck pivot (`NeckPivotEyeZ`): lowering at -60/-85 0.0-0.29 units in all 36
  poses (was 5.3-9.1). (2) The constant reach cap held the body 2.3-3.7 units forward exactly at steep pitch; it is
  gone (reach below). (3) The eye floor (next item). For tall players a ring clearance moves the body back along the
  gaze when the lower edge of the view looks behind the eye (`NeckRingClearance`): it acted only for 1.85 m at -85
  (3.9-5.2 units). A 6-unit ring was tried (the scarf edge still touches the bottom of the 1.85 m shots): it pushed the
  body up to 11 units along the gaze, which with the head turned 120 degrees is sideways to the body, and the view
  showed Walker's back and pack, so 4 stays. New mesh-proxy criterion C5 (8 points of a ring around the Neck in the frustum, BODY `ring=`):
  36/36 out at -60/-85 (it does not see the scarf's outer edge, hence the screenshots).
  Screenshots (`wi3_fix1\shots_fix1`, sheets `wi3_fix1\sheet_wi3_vs_fix1_<height>.jpg`): at -60/-85 chest, vest, belt,
  shoulder and legs at every height. Correction (review round 2): the neck hole was gone only up to 1.64 m; at 1.85 m
  and -85 the neck opening with the flesh-coloured inside still showed at the bottom of the view at all three yaws,
  and at 1.15-1.45 m (-85, some -60 poses) the scarf was torn into strands by `HideBone(Neck)`. C5 could not see either
  (it checks the same ring model the clearance enforces). Fixed at the source in round 2 (render-only neck, below).
- **Neck anchor forward axis (high).** The constant cap (never more than 4 units behind +10) is replaced by a
  reach-based limit (see "Reach" above). The BODY line logs the uncapped anchor target (`uncapped=`), the reach push
  (`reach=`, with `reachneed=(need, shoulder-target distance, arm length, fresh)`) and the ring push (`clear=`).
  C3 is now computed against the plan's N (the player's neck pivot from the eye, E - R(gaze)(nf, 0, nu)) and Walker's
  live corresponding point, not against the shift the mesh was given: WI-3 build 58 of 90 poses over 1.5 units (worst
  3.8, recomputed from its BODY lines); now 86/86 unclamped poses within 1.10 (worst 1.10; the 3 others are the
  1.85 m ring clearance, logged); with the M9A1 (1.64 m, 15 poses) 15/15 within 0.68. Smooth head turns (looped fake
  script, yaw 0 -> 120 -> 0 at -30 and -85, with turn-with-head turning the Pawn; 1.15 m seated auto and 1.64 m):
  98.5-99.3 % of 1100 ticks within 1.5; the rest are the 200 u/s rate limit catching up when the Pawn turns (up to
  2.7 deg per tick) and the script start; neck ring in the frustum 0 ticks at -85, lowering at most 0.77.
  Reach (armdebug, far right hand): 6.5-8.2 units from its target (WI-3 9.7-12.0, before WI-3 12.6-14.5); mid and low
  0.0-0.1.
- **Eye floor (high).** `kEyeHeightMin` = Walker's measured eye (36.25, was 30 = his neck), as WI-2 item 4 asked. It
  clamps only the reference, the head moves 1:1. Matrix: 1.20 m `seated off` and 1.45 m now see from 36.25 (were 30,
  learned standing gap 1.2-2.4 units, views from inside the scarf). Unit tests updated (StableEyeHeight,
  ViewHeightRecapture: clamped view 36.25).
- **Crouch view and the body lowering (medium).** Only the unfinished part of the V ramp is added back
  (`max(0, V - target)`, `g_crouchViewTarget`). Button, then a real 40 cm crouch: the steady eye is 9.99 units above
  Walker's Neck point with the body lowered 11.5 (WI-3: -3.3, inside the body, nothing lowered); the -0.79 minimum
  is the one-tick synthpos step of 20 units while the lowering ramps at 120 u/s. Criterion F per steady state
  (`crouch_wi3.py`): standing 9.0-10.3, physical crouch 10.3, button + real 9.99, all >= 4; button crouched 18.5-19.5
  and seated button 18.7 exceed the upper bound 12 because V = G = 20 while Walker's own crouch drops his eyes 29
  (disclosed WI-3 headset decision). Physical crouch C ramped (4 steps of 10 cm): head only, no CROUCHVIEW ramp.
- **Crouch classification (medium).** From `m_wantsToCrouch` instead of time windows (see "Crouch view" above).
  Unit cases: button then cover keeps V (standing and seated); cover left still crouched keeps V for 3 s (then the
  button makes it a button crouch); a capsule drop with no flag and no cover waits 0.3 s, then script; with cover
  arriving in that wait it is cover; a button within 1.5 s of leaving cover is a button at once. In game
  (`cover_wi3.py`, Gorge02): the crouch button gives `CROUCHVIEW: button/script ... wantsToCrouch not by us 1`, V
  20 in 0.35 s; then stick forward + A into cover (`SPECIALMOVE: SnapToCover`): V stayed 20.0, no reclassification.
  Not exercised in that run: Walker had already left the level-start cover (`LeaveCoverCenter`) before the seated-cover
  window, so "seated cover does not hold LT" stays a code guard plus the regress cover phase (standing).
- **Seated auto (medium).** Detected sitting and then standing 0.30 m above the seated reference for 3 s: measured
  again (`SeatedStandCheck`). `seated_test.py` S7: 1.15 m then 1.72 m with no recenter: "measuring again" after 3.0
  s, then standing, mode changed, eye 41.0; S8: rising 25 cm for 6 s: no new measure. Threshold 1.35 m (was 1.30), the
  SEATED line logs the margin. 9/9.
- **fpbody arms hittest (medium).** Controlled A/B in one fixed pose (`arms_ab2.py`, mode synth, full / arms /
  full / arms at every height, BODY per height): identical at all 16 heights -40..+36 (+28..+36 and -36..-16
  included); the mesh shift (7.65, -0.24) and the Pawn yaw constant through the run. The earlier `arms_test` run that
  showed 7 differences took its "full" and "arms" sweeps minutes apart, with head turns to 60 and 120 degrees in
  between (turn-with-head turns the Pawn and the anchor then shifts the mesh sideways by up to 9 units): most likely
  the -36..-16 band (the gap between the knees) and +28 (the edge of the neck) moved with that shift, not with the
  render hook (UNVERIFIED for that run: it logged no BODY lines).
- **Lows.** `MeshOwnedBy` on the remaining per-tick bone queries and the fault key (see "Safety" above); `UpdateSeatedEffective`
  under a mutex (menu thread and game thread); crouch ramp judged by dt over 3 VIEW rows (a 2.6 ms pair of draws
  after a 33 ms hitch read as 440 u/s with a normal 1.2-unit tick step; over 3 rows A 102.5, D 102.8, E 85.3 u/s,
  limit 1.25 x 1.5 x 20 / 0.35 = 107); prologue turret window taken first after a fresh load (below).
- Test command `wi3test`: bit 4 = the WI-3 lowering eye (no pivot), bit 8 = the WI-3 constant cap and no ring push.
- **Earlier suites on this round's build** (once each): regress PASS on the final dll (sweep view jump 0.0, shot
  0.024 deg, magazine inserted, watchdog 0; the two runs before it lost the fire / gesture phases to 4-10 s
  render-thread stalls in the NVIDIA driver that coincided with my own rebuild and unit-test runs next to both game
  copies; no `SCRIPT: exception` over the whole chain: 7 Gorge02 loads, the prologue, 5 level-1 loads); gun frame (WI-4a suite, 5 classes at Gorge02, fresh level
  each) 45/45 poses with shots, worst first-shot barrel error 0.045 deg (WI-3 build 0.052; the pistol stance that the
  constant cap was added for is now covered by the reach push); WI-2 zeropose 0.0 deg / 0.0 units, height rise one
  re-capture then 40.00 +- 0.15, free 1.70 m drift 0.004, clamped 1.20 m at the new floor 36.25 (still Walker: 36.247
  at start and end; that window's max-min of 43 units came from enemy fire at Gorge02 knocking Walker back every ~3 s,
  `SPECIALMOVE: TakeHit`, the Pawn moved ~460 units); fake_smoke all functional checks PASS (72 / 90 Hz "game kept
  up" FAIL with copy A on the GPU, B_BRIEF); prologue turret on a fresh load, fire window first: both windows inside
  the flight (330 ticks each, neck base on), fire drift 1.6 units / still 0.375 (the first valid fire window in any
  run: no baseline; the WI-3 code is off on mounts; the eye on the turret is 2 units higher than in the WI-3 run
  because of the new floor with the 1.55 m fake head).
- **Observed, not changed in round 1 (fixed in round 2, below):** a hit reaction (`TakeHit`, capsule 45 -> 25 for
  ~0.6 s) was classified as a script crouch after the 0.3 s wait and the view dipped ~16-20 units (the WI-3 build
  dipped at once).

#### WI-3 review fixes (round 2)

One high, three medium and three low findings; all handled (copy B, novr, fake runtime, final dll of this round).

- **Hit reactions no longer move the view (high).** Any capsule drop without `m_wantsToCrouch` and without cover used
  to become a "script" crouch after 0.3 s, and `TakeHit` (9), `PlayHitReactionAnim` (58), the sprint moves (38-42) and
  the sand stumbles (63/64) are not cover-like, so under fire the view dipped 16-20 units with the vignette every ~3 s
  (fix 1's Gorge02 log). `UpdateCrouchView` now reads the special move every tick (it was read only when the cover
  state was 0) and passes it to `CrouchView` (`CrouchViewIn::specialMove`): while any special move is active and for
  `kSpecialQuiet` (0.3 s) after it, no script crouch is classified (comp = G alone keeps the view still); a script
  crouch needs the capsule down with no special move for `kScriptHold` (0.5 s, was 0.3). A crouch-button press during
  a special move is still a button crouch (the player made it). New line `CROUCHVIEW: game drop ... during special move
  N (name), view kept still (not a crouch)`, once per drop.
- **Neck under the hidden head, drawn only (medium).** `HideBoneByName("Neck")` (scale 0 in LocalAtoms, which also
  zeroes the physics bodies: the head is not hittable, a user decision kept as is) collapses every vertex weighted to
  the neck into the Neck pivot. Two visible results: at 1.85 m / -85 (and button-crouched, V = 20 against Walker's
  29-unit crouch drop) the collar opening with the pale inside of the jacket and a pinched dark shape at the bottom of
  the view; at 1.64 m / -85 (y0, y120) the scarf pulled into a sheet and strands. The fix keeps HideBone (hits, physics,
  `GetBoneLocation` and IK unchanged) and adds a render-only pass in the existing `FSkeletalMeshObject::Update` hook
  (the one `fpbody arms` uses), for Walker's first-person mesh in both body modes: `Neck` and `FACE_THRO` get back a
  scale (`necksize`, Advanced, default 1) on their real pivot (`SpaceBases` keeps the real rotation and translation,
  only the scale is 0), and `head` plus its 34 `FACE_*` bones are closed into one point at the top of the neck
  (the head's reference-skeleton offset from the Neck, 3.76 units along the neck axis, read from
  `USkeletalMesh.RefSkeleton` +0x88, 80-byte `FMeshBone`, Position +32, ParentIndex +64; checked by name and parent).
  The quaternion convention was verified on Walker's live atoms (Spine3 -> Neck: 3.613/-11.25/0.434 computed vs
  3.618/-11.248/0.438 read) and is unit-tested. The pass is skipped unless the Neck's scale is really 0 (HideBone active)
  and the quaternion is sane; with `fpbody arms` the arms pass runs after it and the atoms are restored in reverse order.
  BODY gains `armposed=`, `necksize=`, `neckrender=` (passes applied).
- **Reach push from the rest length (medium).** `MeasureGunReach` measured the arm length from the posed bones after
  the IK, but the limb controller stretches the arm (`armstretch` 1.25) towards the target, so the posed length was
  the target distance and the need was always at least the 2-unit slack for any forward target: the push could ratchet
  up to the old +10. It now uses `ArmReach`'s rest length (LocalAtoms, the shortest seen; 27.6 units for Walker).
  Decision, from measurements: the stretch does NOT count as reach. Counting it (rest x `armstretch` 1.25, the
  `HandToEffectorDistance` convention; build 2e51acae) left the body at the anchor with the gun arm stretched, and the
  WI-4a gun-frame suite drew the gun off on the first shot: 9 of 41 poses over 0.3 deg, worst 1.63 deg (SAW gunangle
  10 roll 30), 4 SAW poses without shots (weapon recovery); fix 1, whose ratchet in effect pushed until the arm was no
  longer stretched, had 45/45 within 0.045. So the body moves forward just enough that the gun arm does not stretch
  (target within rest length minus `reachslack`, Advanced, default 2 units), never past the old +10 placement, and
  comes back when the hand comes closer. `reachneed=` logs that reach and `armposed=` the drawn arm. Final dll: gun
  frame 45/45 poses with shots, worst first-shot error 0.045 deg (SAW, legacy aimpitch 0), as fix 1.
  A second ratchet showed up in this round's first reach run and is fixed too: the wanted push was "need (>= 0) +
  current push", which with a reachable target equals the current push, so once raised the push never came down
  (M9A1 with the hand near: 5.4 units of push, C3e 5.4). The measurement is now signed (`ReachForwardMargin`: negative
  = how far the mesh could move back and still reach), wanted push = current push + margin, and the existing 0.5 s
  hold / 15 u/s decay brings it down.
- **Seated, standing, seated again (medium).** After seated auto switched to standing because the player stood up
  (`SeatedStandCheck`), nothing detected sitting down again: the eye ended ~28 units under Walker's (in his chest) and
  the real crouch was armed for the rest of the session. Now `SeatedReturnCheck` stays armed after that switch: head
  within 15 cm of the old seated reference (or below) for the stand-up hold (3 s) -> measured again, and the detector
  decides as at the start; disarmed by every new measurement (recenter, headset back on, menu). The toast says "sit down
  for 3 s, or recenter, to go back to seated play". `UpdatePhysicalCrouch` also stands Walker up when seated play turns
  on while our real crouch holds him down (`CROUCH: seated play on, Walker stands (our game crouch undone)`): seated
  play never uses the real crouch, and nothing else would have stood him up.
- **Low: button crouch in low cover, aiming out.** Aiming out of low cover (LT or the body peek) grows the capsule
  (G -> 0) while the cover state stays: with a button crouch the source was reset and the view rose 20 units in 0.35 s,
  then dropped 20 again on the way back in (and with the body peek it came on top of the real head motion). Now a
  button crouch in cover keeps its source and V through the pop-up (`CrouchView::popup`, comp follows G): the view stays
  where it is; the eye with V = 20 is still about 40 cm above Walker's crouched eye, above low cover. Leaving the cover
  standing ends it as before. New line `CROUCHVIEW: button crouch in cover, Walker up to aim out / back down`.
- **Low: headset tuning values are ini keys** (Advanced, shipped ini, `MenuStepFor`): `necksize` (0..1), `neckring`
  (ring radius, 4), `reachslack` (2), `seatedbelow` (1.35 m), `seatedstand <rise m> <hold s>` (0.30 3; the hold also
  times the sit-back check). No default changed, no migration.
- **Low: merge and docs.** The "Safety" bullet above now says when per-object faults are cleared (every restart of the
  first-person body management, i.e. also after cinematics and third-person spells) and that the per-object fault does
  not make the stale `GetBoneLocation` safe (copy A's level-change crash fix); the merge checklist keeps A's liveness
  checks with B's guards; the physics note on how far the body moves forward is corrected (BUGS_PLAN).

**Measured (round 2, copy B).** Final dll 327f8a44. Builds of this round differ only in `MeasureGunReach` / the
reach push: eafae0b8 (neck shots), 2e51acae (signed margin, reach = rest x 1.25: seated, crouch, cover/hits, prologue
turret, and the gun-frame run that rejected that reach), 327f8a44 (reach = rest length: reach, gun frame, aim hmd,
WI-2 room, fake_smoke, regress). The seated, crouch, hit and turret checks do not involve the reach push.
- **Hits under fire** (`wi3_fix2\cover_wi3.py` T1, Gorge02, standing exposed for 30 s, mode synth): 12 `TakeHit`
  starts, 12 `CROUCHVIEW: ... during special move 9 (TakeHit), view kept still` lines, 0 `button/script` lines with a
  hit move (fix 1: one every ~3 s, view down 16-20 units); largest per-tick vertical view step 0.14 units over 2289 VIEW
  rows while the Pawn centre stepped by up to 20.99 (the capsule). STATS still reports a 3D "max view jump" of 21.0 in
  those windows: the vertical part is the 0.14 above, so it is horizontal (the hit knockback moves the Pawn; not
  separately verified). Unit cases: a 0.6 s capsule drop flagged 9 / 58 / 63 / 40, repeated every 3 s for 30 s: V 0
  throughout, comp = G every tick; a move that ends with the capsule still down becomes a script crouch only after
  0.3 + 0.5 s; a button press during a move is a button crouch at once.
- **Neck** (`neckshots.py`, A/B on `necksize` 1 vs 0 in the same session and pose, 1.15 m seated auto, 1.64 m,
  1.85 m, and button-crouched 1.15 m seated / 1.64 m standing, pitch -60/-85 x yaw 0/60/120, 60 shots; sheets
  `wi3_fix2\sheet_neck_fix2_<config>.jpg`, crops `crop_185_p85_y0_y60_new_vs_old.jpg`, `crop_scarf_p85_new_vs_old.jpg`):
  1.85 m at -85, all three yaws: old = the collar opening with the pale inside and a pinched dark shape; new = the
  opening is closed by the neck skin inside the scarf ring (no inside of the body). 1.64 m at -85: old = the scarf
  pulled into a sheet (y0) and torn into strands with dark fold lines (y120); new = the scarf whole with its normal edge.
  Button-crouched (V = 20, eye ~9 above Walker's crouched neck) at -85: the bottom edge shows the closed neck instead of
  the opening. -60: no visible difference anywhere (the neck is out of view). The ring clearance still acts at 1.85 m /
  -85 (2.7-3.9 units) and crouched (1.3-3.3).
  NOT fixed by this: the 1.15 m seated auto y0 / -85 shot of the matrix (vest and shoulder pad torn, long dark strands)
  is identical with the neck restored, so it is not the Neck collapse. In that matrix pose the head is ~45 degrees off
  the body (the poses run yaw 120 -> 0, and turn-with-head had turned the Pawn), so at -85 the eye (26.3 above the Pawn
  centre, Walker's Neck at 27.1) sits over the shoulder pad at shoulder height: the eye is inside Walker's shoulder
  geometry. With head and body aligned (`probe115.py`, sheet `probe_115_vs_164_p85_aligned.jpg`) 1.15 m seated auto,
  1.15 m seated on and 1.64 m at -85 are clean (vest, scarf edge, gun). A real neck cannot put the eye at shoulder
  height over the shoulder (yaw 45 + pitch -85 is outside the human range), so it is a headset check, not changed here.
- **Reach** (`reach_test.py`, 1.64 m standing, fake hands from near (30 cm in front of the eyes) through mid (45),
  far (60), full (72) to beyond (85), left hand on a support point; BODY `reach=`, `reachneed=`, C3e, armdebug hand
  error). Final dll (rest length 27.62): rifle (M4, level 1) push 0.0 near (margin -3.7, C3e 0.90), 3.9-4.0 from mid on
  (the cap: old +10 placement; C3e 3.2-4.5, the anchor's forward axis is overridden there by design), hand error 0.0
  near / mid, 0.1 far, 5.0 full, 12.1 beyond (out of reach even stretched); pistol (M9A1) push 0.0 near (C3e 0.42),
  5.4-5.5 from mid on (cap; the pistol anchor target is +4.6). Earlier builds of this round: rest x 1.25 (2e51acae) gave
  push 0 near and mid (C3e 0.17-0.64) and the cap only from far on, but the gun-frame errors above; before the signed
  margin the pistol kept 5.4 units of push with the hand near (C3e 5.36), the ratchet.
- **Seated** (`seated_test.py`): S1-S9 11/11; S9: seated at 1.15, standing at 1.72 (re-measured standing), back to
  1.15: `back within 15 cm ... measuring again` after 3 s, then sitting (mode changed), eye 36.25; the real crouch that
  crouched Walker while the head was low was undone when seated play turned on. Unit test: re-measure 3.00 s after
  sitting down; 2 s crouches and a head 21 cm above the old reference never fire; not armed -> never.
- **Crouch suite** (`crouch_wi3.py`): all cases pass as in round 1 (button ramp 20.0 in 0.333 s, at most 1.23 units per
  tick; real 40 cm head only; button + real + stand no jump, largest non-head step 1.21; seated button ramp; F steady
  9.9-10.8 after standing up again).
- **Button crouch + low cover + aim out** (`cover_wi3.py` M4): NOT exercised in game: Walker did not get back into the
  cover in M3 (no cover lines, the capsule stayed crouched and LT aimed without a pop-up), and Gorge02's level-start
  cover was already left. Unit case only: two 1 s pop-ups (G 0 with the cover state on) and returns: V 20 throughout,
  comp = G; leaving the cover standing ends it (V 0).
- **Unit tests** 1081/0 (CrouchView 13/13b/14, SeatedReturn, bone-atom rotation on Walker's live values, signed
  reach margin and the push loop).
- **Earlier suites, once each on the final dll 327f8a44:** WI-4a gun frame 45/45 poses with shots, worst 0.045 deg;
  aim hmd 30/30 poses with shots (WI-3 build 26/30), shot vs mesh axis <= 0.045 deg (clipped shots on nearby static
  meshes and the palm spheres as in earlier runs: copy A's p2d fix); WI-2 zeropose 0.0 deg / 0.0 units, one height
  re-capture, clamped 1.20 m at 36.252 start and end (drift 0.001). The WI-2 windows' max-min (rise 21.5, free 1.70 m
  23.7 units) are hit reactions at Gorge02: that metric is view z minus Pawn z, and with this round's fix the world view
  stays still while the Pawn centre drops 20 at each `TakeHit` (85 "view kept still" lines in that log); free 1.70 m
  start / end 39.997 / 40.003. fake_smoke: functional checks PASS (zeropose 0.0 deg, 72 Hz kept up), 90 / 120 Hz throughput FAIL
  with copy A on the GPU (B_BRIEF). Prologue turret (2e51acae, fresh load, fire window first): fire drift 1.23, still
  0.78 (fix 1: 1.60 / 0.375; flight-moment dependent, no baseline; WI-3 code is off on mounts). Regress PASS (max view
  jump 0.0, no `SCRIPT: exception` in the last four logs). Advanced menu (`menu_fix2.py`, `vrmenu cycle`): the five
  new keys appear from the embedded defaults and 10/10 clicks wrote the ini (`necksize` steps past 1 in the menu text;
  the command clamps it to 1).


### WI-4a: gun frame (bug 6, "gun angle is completely off")

**Cause (confirmed).** The gun followed SteamVR's Touch *raw* pose rotated by `aimpitch` (shipped 0). The raw -Z
points 39.4 degrees above the controller's pointing ray (the `openxr_aim` component of SteamVR's `oculus_quest2`
and `oculus_quest_plus` models; VDXR's aim pose is the same ray, unit test below): with a natural hold the barrel,
the shots, the marker, the turret pitch and the two-hand line pointed ~40 degrees high. The developer never saw it
(his ini: `aimpitch -24`). Second cause: the controller yaw came from the head's "heading" yaw (`BlendedYaw`), so a
rolled controller swung the barrel sideways. Third: the left hand mixed frames (pitch from the rotated pose, yaw and
roll from the raw one). Players had no usable correction (Advanced only, 0.05 degrees per click, B+Y wrong when
rolled, `aimpitch` clamped to +-45 on the raw axis).

**Changes.**
- **Gun frame** (`vr::pure::GunFrameFromRaw`, used by `ApplyGunControllerAim`, the gestures in vr_input.cpp and the
  B+Y calibration): pointing ray of the controller (aim in raw: `AimInRaw`) * Rx(hold angle) * Ry(-gunyaw), then
  the EXACT Euler angles (`ExactEuler`; no heading blend) for `g_aimDeltaYaw/Pitch` and `g_handRoll`. The hold angle
  is `gunangle` with `holdmode normal`, `stockangle` with `holdmode gunstock` (degrees of barrel above the ray; 0 =
  along the menu laser). `gunyaw` (+ = outward, mirrored for the left hand) turns the gun about its own up axis.
- **Pointing ray in the raw frame**: OpenVR (SteamVR, Steam Link, fake runtime) = the Touch constant -39.4 (non-Touch
  controllers are already aligned to the Touch tip by `controllerpose auto`). OpenXR: measured
  (vr_openxr.cpp `MeasureAimInRaw`: the runtime's aim pose located in the hand space, 10 samples at 100 ms within
  0.2 degrees, controller profiles only, plausibility window `AimInRawPlausible`: pitch -80..+20, yaw and roll within
  25; log line `OPENXR: <hand> pointing ray (aim pose) measured in the hand pose ...`); before the measurement or if it
  is implausible: `openxrgrip - 60` (-39.4 with the default 20.6, -60 with `openxrgrip off`). So VDXR's personal
  offsets (OffsetsTool registry) and other controllers are followed through the real aim pose. Log line per hand on
  every change: `AIM: gun frame, right hand: normal hold, gun +0.0 deg above the pointing ray ... -> barrel -39.40 deg
  on the raw -Z`.
- **Pivot at the palm**: the frame's origin (the wrist point where Hand_Right is driven, `g_handOffset`) turns with
  the angle about the palm (`PalmOf`, the `openxr_grip` origin) instead of the raw origin. With the angle 0 on the raw
  (the legacy frame, `aimpitch 0` / stockangle 39.4) the position is exactly the old one; at the new default
  (`gunangle 0`, the gun turned 39.4 deg down about the palm) the drawn wrist point moves about 7 cm; for any angle the gun point that sits on the real palm stays
  there, and the barrel line passes through the palm (within the 7 mm lateral offset of the grip). The ADS ray, the
  two-hand line, the props right-hand frame (`g_handWorld[0]`) and the gesture magwell / charging handle / "left hand
  on the gun" tests use this point (vr_input `GestureSample::gunPos`).
- **Left hand**: one rigid frame = the raw controller pose with exact angles, no gun angle (`ApplyLeftControllerAngles`);
  the drawn hand and the props hand axes are the old default's, now exact. Where the hand POINTS (squad orders,
  gravity gloves via `g_supportFwd`, `movedir hand` via `g_leftYawUnits`) is its pointing ray, like the menu laser
  (`g_leftAimYaw/Pitch`, exact; `movedir hand` uses the ray's heading yaw since review round 1, see below); with
  synthetic poses (`synthleftrot`) the pointing direction is the hand's, as before.
- **B+Y** (`stockcal` = the same as a test command): calibrates the current hold, roll-proof, 15-60 cm (see
  "Gunstock calibration").
- **Options**: Basic "Controller hold" (`holdmode normal|gunstock`) and "Gun angle" (`gunangle`, -15..15 step 5;
  provisional default 0, the final default is a headset decision); Advanced `stockangle`, `gunyaw` (ini comments,
  1 degree per click), `holdmode` cycles. Limits: gunangle +-45, stockangle -60..90, gunyaw +-30 (the angle is
  limited, not the raw offset). A Basic choice whose value is not in the list (e.g. 2.5 from B+Y) steps to the next
  choice in the click's direction (`NextNumericChoice`; before, a click went to the second choice).
- **Migration** (configversion 11, 10 in copy B before the merge of 27/10, `vr::pure::MigrateAimpitchLine` + `kAimpitchCommentChanges`): `aimpitch 0` (or
  out of +-45) -> `holdmode normal` + `gunangle 0`; `aimpitch X` -> `holdmode gunstock` + `stockangle X+39.4` (the
  developer's -24 -> 15.4: the same barrel direction, unit-tested to 0.01 degrees). Since the final regression (27/10)
  only |X| >= 5 counts as a stock calibration (`kAimpitchStockMinDeg`): smaller values are Advanced-menu nudges (0.05
  degrees per click), not a B+Y result (B+Y measures -39.4 for a pistol hold, -24 on the developer's stock), and keeping
  them as a stock kept bug 6's 39-degree barrel; they migrate like 0. The old 4-line comment becomes the
  holdmode comment. `aimpitch X` as a command (cmd file, old scripts) still gives exactly the old barrel: gunstock,
  stockangle X+39.4.
- `armrot` is unchanged (still applied to the drawn gun and the shots only).

#### Measured (copy B, novr, 26/10; before = the WI-2 round-2 dll 453ce542, after = the final WI-4a dll)

Scripts and raw results: scratchpad `B\wi4a` (`gunframe.py`, `bycal.py`, `menu_wi4a.py`, `final_chain.sh`).
- **Per weapon class** (`gunframe.py`, `mode synth`, the right controller as a raw Touch matrix whose pointing ray
  is pitched -15 deg and rolled 0/30/60 deg about itself; fresh SP_Gorge02_P per weapon; firelog; first shot of
  each burst for the barrel; `grabprops off` so the props palm spheres, which copy A made invisible to traces,
  do not clip the shots). The drawn gun's mesh X axis in the controller's pointing frame:
  - before, shipped default (`aimpitch 0`), M9A1 / UMP45 / Benelli M1014 / SAW at Gorge02 and M4 at level 1:
    39.39 deg above the pointing ray at roll 0; at roll 30 35.09 deg above and 9.32 deg to the LEFT, at roll 60
    35.1 / 3.4 deg left (the heading-yaw blend; the old code's own target was 39.4 / 0). The developer's -24:
    15.40 / -0.02 at roll 0, 15.78 / +0.67 at roll 30.
  - after, M9A1, AK47, UMP45, Benelli M1014, SAW: 45 of 45 poses (gunangle 0 at roll 0/30/60, gunangle +10 at roll
    30, -10 at roll 60, gunangle 5 + gunyaw 8 at roll 30, gunstock 15.4 at roll 0/30, legacy `aimpitch 0`): worst
    barrel error 0.045 deg against the expected frame (elevation = the hold angle, lateral = gunyaw), shot vs
    mesh axis <= 0.043 deg, shot vs marker <= 0.073 deg, 0 clipped shots; the wrist point (TWOHAND right hand) within
    0.06 cm of the palm-pivot prediction; the legacy `aimpitch 0` pose gives the old 39.39-39.40 deg.
- **B+Y** (`bycal.py`, fake runtime `mode hmd`, controller placed so the barrel looks along the line of sight):
  6 of 6 accepted with 0.000 deg error (normal 7.5 at roll 0/30/60, -12 at roll 30; gunstock 22 at roll 45,
  15.5 at roll 0) and the value saved; the old formula would have given 11.75 / 22.92 / -4.53 / 26.96 at the rolled
  cases. Rejected: palm 13 cm and 70 cm (ignored), line of sight 39 deg to the side (REJECTED).
- **Migration** in game: copy B's v9 ini (`aimpitch -24.0`) became `holdmode gunstock` + `gunangle 0` +
  `stockangle 15.4` with the new comments; AIM line `gunstock hold, gun +15.4 deg above the pointing ray ...
  -> barrel -24.00 deg on the raw -Z` (the same barrel as before).
- **Headset path** (`aim_baseline.py --hmd`, fake runtime, the old `aimpitch 0 / -24` poses, which the legacy
  command maps to the same barrels): all five classes give the exact barrel of the raw pose rotated by the old
  angle within 0.03 deg (e.g. -24 at roll 30: yaw -14.40..-14.42 / pitch -35.24, exact -14.41 / -35.22); the old
  heading-yaw error at roll 30 / 60 is gone (M9A1 `aimpitch 0` roll 30: barrel yaw 7.21 -> -0.01 deg).
- **Open:** the M4 at level 1 after a second console open of SP_Desert01_p drew the gun 3.1 deg low and 1.7 deg
  left in every pose, the legacy `aimpitch 0` one included (the old frame exactly); the before run at level 1 (old
  dll, opened from Gorge02) had none, and at Gorge02 every class, the AK47 rifle included, is within 0.045 deg.
  In the relaunchB level-1 state `armdebug` shows the mesh axis equal to the commanded aim forward within 0.01
  (the M4 does not fire there). Not the gun-frame math; cause UNVERIFIED. Review round 1: not reproduced by either
  dll in the same session and paths (below).
- Unit tests (507): SteamVR raw in aim 39.400 deg; VDXR aim in the shim raw -39.400 / 0 / 0; VDXR grip-in-aim vs
  SteamVR 0.0000 deg / 0.001 mm; level and +-30 aim at roll 0/30/60 x gunangle -10/0/5/15: 0.0000 deg (old chain:
  up to 43.0 deg above and 16.2 deg sideways); palm kept 0.0003 mm; B+Y 0.0000 deg (old formula up to 24.9 deg);
  migrations (0, -24, comma, out of range, missing, idempotent).

**Not covered here / headset checks.** The OpenXR measurement path (the fake runtime never runs vr_openxr.cpp: code
read + unit-tested math only); Touch Plus outward yaw (`gunyaw` default 0); the final `gunangle` default (pistol
geometry suggests slightly below the ray: test -10..+10); the developer's stock after the migration (same direction;
the drawn gun moves ~4 cm because it now turns about the palm); left-hand drawn pose and gravity-glove ray; magwell
and ADS thresholds with the new frame (the magwell point is fixed to the drawn gun, as before).

#### WI-4a review fixes (round 1, 26/10)

Copy B, final dll f3ee2e77 (13:07), novr, fake runtime; scripts and raw results in scratchpad `B\wi4a_fix1`
(`fix_chain.sh`, `final_extra.sh`; `prev\m4_fire_diag.py`, `aim_hmd_new.py`, `turret_hold.py`, `palm_grip.py`,
`menu_fix1.py`). Unit tests 755 / 0.
- **M4 at level 1: not reproduced, not WI-4a.** One session, three runs. (1) The new dll on the path of the WI-4a
  final run (relaunchB, then a second console open of SP_Desert01_p): 9 of 9 poses within 0.057 deg; legacy
  `aimpitch 0`: 39.395 / -0.057. (2) The pre-WI-4a dll (rebuilt from the WI-2 round-2 source, `oldtree`, 7152de28)
  on the same path: `aimpitch 0` roll 0: 39.395 / -0.057, the same numbers; roll 30 / 60: the known 35.08 / -9.32
  and 35.08 / -3.38. (3) The new dll on the WI-4a "before" path (fresh SP_Gorge02_P, then console open level 1):
  9 of 9 within 0.057. WI-3's chain (build ae10978f) had already given 0.057 on the relaunchB path.
  - The suspected carrier is ruled out. The hand/weapon relation (`ARMS: Hand_Right in the frame of weapon mesh:
    X=(1.00 0.06 -0.03)`, about 3.4 / 1.7 deg off identity) is the same idle and while firing (armdebug 0.45 s into a
    1.6 s trigger pull), on both dlls and both paths, and the shots are on the commanded frame. It is the M4's grip
    relation, compensated by `g_handInWeapon` as designed.
  - The 3.1 / 1.7 deg offset of the WI-4a final run (06:13) was a state of that session that neither dll
    reproduces. Cause UNVERIFIED. The final regression keeps one M4-at-level-1 run (`m4_level1.py` + `gunframe.py
    --noload YWeapon_M4Carbine`).
- **Heading yaw near vertical.** The exact yaw of the pointing ray is ill-conditioned when the ray is near vertical
  (a relaxed hand points it 80-90 deg down).
  - `movedir hand` (`g_leftYawUnits`) now uses the ray's heading (`vr::pure::PointingHeadingYaw` = `BlendedYaw` of
    the pointing frame, the WI-1 rule for directions). The drawn hand, `g_leftAimYaw/Pitch` and `g_supportFwd` keep
    the exact angles. Unit test, 2 deg wobble about 26 axes, ray at -80 / -85 / -88 deg: heading swing 2.31 / 2.50 /
    2.65 deg; the exact yaw swings 11.4 / 23.4 / 82.5 deg.
  - The one-hand turret holds its yaw while the barrel is more than 70 deg from level (`vr::pure::TurretYawHeld`;
    no game turret reaches that pitch). Prologue minigun, `synthaim 30 -75`: `TURRET: yaw held, barrel near
    vertical`, 6 TURRET lines with step yaw 0.000. Then `synthaim 30 -5`: `free again`, steps up to 12.9 deg toward
    the target. WI-1 flick: view vs the helicopter 0.000 deg, turret turned 30.0, unchanged.
- **Headset path with the new frames** (`aim_hmd_new.py`, fake runtime, `mode hmd`, M9A1, `grabprops off`):
  holdmode normal + gunangle 0 at roll 0 / 30, +10 at roll 30, -10 at roll 60. The first-shot barrel is within
  0.036 deg of the hold angle, 0 clipped shots, and shot vs mesh axis <= 0.037 deg. The wrist point on the HMD
  branch (`g_gunWristPos - posRef`, TWOHAND right hand) is within 0.05 cm of the palm-pivot prediction.
- **Migration notice and the Gun angle row.**
  - A migrated calibration (`aimpitch X` -> `holdmode gunstock`) now shows two toasts once, after 10 s of gameplay:
    "Your old gun calibration was kept: gunstock hold, stock angle 15.4 deg" and "Playing without a stock? Menu:
    Basic > Controller hold > normal". `control::g_holdMigrationNotice`, `HoldMigrationNoticeTick`; it is called
    before `UpdateGunInHand`'s arms early return. In game the v9 ini gave both toasts 10 s into level 1.
  - Basic "Gun angle" shows and changes the angle of the ACTIVE hold (`SyncHoldAngleRow`): `gunangle` (-15..15)
    with the normal hold, `stockangle` (0..40) with gunstock, and it switches as soon as "Controller hold" is clicked.
    `menu_fix1.py`: 9 of 9 checks (row key, choices, ini write, AIM line, the other key untouched). WI-4a menu 9 of 9
    and WI-3 menu 12 of 12, values restored.
- **B + Y in the normal hold** is circular when the drawn gun is lined up (see "Gunstock calibration"). It is no
  longer offered there: Basic help, Controls page ("B + Y 1 s: gunstock calibration"), README, the rejection toast,
  and the holdmode / stockangle ini comments. The command itself is unchanged.
- **Steam Link evidence for the pointing ray.** On OpenVR, Touch-family models now log their `openxr_aim` and `tip`
  components in the raw pose on the INPUT line (log only; the gun keeps the -39.4 constant). The line is retried
  for about 5 s while the render models load. On the fake runtime, which has no render models, it logs "not
  available". On Steam Link the headset session can read the constant from it.
- **OpenXR aim measurement** (`vr::pure::AimRayMeasure`):
  - the last valid measurement is kept per interaction profile, so the same profile coming back is re-used at once
    (no second of `openxrgrip - 60`, no double snap);
  - it re-measures every 30 s with 10 samples and takes a result over 0.3 deg different (log line only on a change);
  - an implausible re-measure never replaces a good value;
  - a new hand space (`openxrgrip`) clears the cache.
  Unit-tested; the fake runtime does not run vr_openxr.cpp. Headset: VDXR with hand-tracking controller emulation.
- **Palm vs drawn grip (investigation H5, test plan E;** `palm_grip.py`, synth, gunangle 0, five classes). The drawn
  wrist (Hand_Right) is 10.2-10.4 cm ahead of the real palm (the `openxr_grip` origin) along the barrel, by
  construction: the wrist target is the controller's raw origin turned about the palm. The weapon mesh origin
  (Weapon_Right, the grip bone in Walker's palm) is 13.2-13.3 cm ahead and 3.6-3.7 cm across, the same for all five
  classes. That is over the plan's 4 cm. An `armoffset` default (about -13 0 0) is a headset decision with the
  final gunangle; the default is unchanged here.
- **Docs.** WI-3 and WI-4a moved under "Tester VR fixes (25/10)". The stale aimpitch comment in vr_input.cpp is
  gone. The openxrgrip ini comment now says the gun follows the measured aim pose. The README marks VDXR / SteamVR
  as headset check pending.
- **Pre-existing, not changed:** `vr::toast::HintsTick` (R-U2 controller hints) sits after the `g_armsActive` early
  return of `UpdateGunInHand`, so the hints never run while the mod drives Walker's arms (the normal case).
  (Fixed in the WI-4c review round 2: the hints run before that return, on foot; on turrets they wait.)

### WI-4b: two-handed grip (bug 7, "2 handing weapons doesn't work correctly")

**Causes (confirmed, investigation b7 + verification).**
- Two tests decided whether the left hand held the gun, and they disagreed. The aim
  (`ApplyTwoHandAim`): the left RAW origin within 12 cm (8 vertically) of the right controller's
  line, 12-70 cm ahead; it alone set `g_twoHandActive` (spread). The drawn hand (`UpdateArms`):
  the left target within `leftgrip` 20 cm of the animation's grip. The logged hand/weapon matrix
  puts that grip 32 cm forward, 11 cm left and 6 cm up of the aim line: a hand on the drawn
  handguard was drawn gripping with a one-handed aim and 1.5x spread.
- When the aim engaged it pointed along the line between the RAW origins (10 cm in front of the
  palms; 21.6 deg off the barrel for the game's grip) and the engagement offset decayed in 0.7 s:
  the gun turned by itself ~20 deg after grabbing the barrel. Wrist rotations moved the line
  through the 10 cm lever (left wrist yaw 30 deg: 7.3 deg, unit test). 80 ms EMA (7 deg of lag at
  90 deg/s); release snapped 18-40 deg in 0.1 s toward a frozen direction.
- No gunstock mode (a stock lost its B + Y calibration once two hands engaged), no vetoes (props,
  reload, grenade, turrets: the turret aim reads `g_aimDelta*`).

**Change: one solver** (`vr::twohand::Solver`, `src/twohand_math.h`, header-only, unit-tested) runs once
per tick in `UpdateHeadTracking` (`TwoHandStep`, headset branch and `mode synth`, which builds the
same raw matrices) and its single engaged flag drives the aim, the drawn left hand (`UpdateArms`:
`on = canGrip && (no left controller || g_twoHandActive)`), the spread (`g_twoHandActive`), the
haptics and the gesture vetoes (`control::g_twoHandEngaged`, `g_leftOnGun`). The two modes follow
Basic > Controller hold (`holdmode`), as the user asked for the turrets too:
- **Barrel model ("guard")**: where the left PALM sits, relative to the right palm, in the gun
  frame, when the drawn left hand is on the game's grip: the drawn grip (`SupportGripRelation`,
  through `g_handInWeapon` and `armrot` into the controller gun axes) converted from wrists to palms
  (`PalmTargetFromDrawnGripRot`: the right wrist is 10.2 cm along the barrel from the palm by the
  WI-4a pivot, the left wrist is the left raw origin, the left palm sits `R_L * palm-in-raw` from it
  with the left controller's LIVE rotation (review round 1; before: assumed parallel to the right
  one); `armoffset` included). Out of the grip the model follows the live left rotation (it is
  where the visible free hand's wrist meets the drawn grip); at the grab it is frozen
  (`Solver::held`), so later left-wrist rotations do not enter. M4 at gunangle 0, parallel
  controllers: 35.5 cm along, 8.2 cm left, 1.8 cm down; usable segment from 15 cm before to 25 cm
  after it (never under 10 cm). Pistols / Uzi: the cup (`cuprel`) as a point. Left-handed players
  (no drawn grip): the typical model mirrored, aim only. `twohandguardfwd` / `twohandguardright` /
  `twohandguardup` shift it (cm; headset tuning; one key per number because the Advanced menu
  steps only the first number of a line; `twohandguard f r u` stays as a command).
- **The relation is stable**: before WI-4b an accepted relation followed every change under 3
  units, and a burst walked it 4 units (7 cm) back. Only a relation still for 0.3 s (1.5 units) is
  taken, never while the grip holds the gun (firing does not move the barrel model), the first one
  per weapon not during a game special move (`m_specialMove`), and after the first only within 3
  units of the FIRST accepted one (review round 1: before, any pose still for 0.3 s replaced it, so
  a reload pose with the left hand at the magwell or a cover pose could become the barrel model).
  A far pose still for 3 s is logged (`ARMS: grip relation ... the first stays until the weapon
  changes`).
- **NORMAL** (`holdmode normal`): the left palm engages within `twohandgrip` 8 cm of the segment
  with the grip held (at once) or after `twohanddwell` 0.15 s there without the grip (the 22/10
  "engages by proximity" kept); it lets go when the departure weight below reaches 0, or when the
  grip is released if the grip holds it (then no re-grab by resting until the hand leaves). The
  engage test uses the model of the DRAWN gun (the one-hand frame turned by any leftover
  correction, built with the live left rotation in that frame: `Solver::DrawnRot`); review round 2:
  round 1 engaged on either model but released on the one-hand one, so with 16-20 deg left a palm
  put on the gun 59 cm along engaged and released every tick, and the one-hand-frame left rotation
  put the model 1.7-3.3 cm off the visible grip. After a departure release with the grip still
  closed, the grip must be opened and closed again (`needRegrip`). Aim: the gun turns about the RIGHT PALM by the minimal
  rotation that makes the barrel model pass through the left palm (`SolveForward`, fixed point).
  Out of the grip the model's sideways/up offset is in the gun's own frame (current cant: the drawn
  handguard); at the grab the cant relative to gravity is latched and the offset stays in
  `GuardFrameCant` (`GuardFrame`: up = world vertical orthogonal to the barrel, the gun's own up
  past ~70 deg of pitch, turned by the latched cant), so the aim itself ignores later wrist roll
  (the gun-frame "anchor" design moved it 1.8-7.2 deg per 5-20 deg of roll; correction, review
  round 2: in round 1 the distance weight below brought it back, see there) and grabbing a canted
  gun does not turn it (review round 1: the model was always level, so a palm on the visible
  handguard at 20/30/45 deg of cant was 2.9/4.3/6.4 cm off and the grab turned the gun 4.7/7.0/10.3
  deg by itself). While two-handed the drawn left hand is shifted onto the model point
  (`drawShift`, `g_thDrawShift`): with the right wrist rolled after the grab the drawn grip turns
  around the barrel, the model and the real palm do not. Only palms enter, so wrist rotations do
  not either. The gun's roll comes from the right controller, carried by the rotation. No decay
  and no captured offset: with the palm on the model the rotation is 0. 1-euro filter on the
  palm-to-palm line (`twohandsmooth` 1 Hz at rest, beta 9 Hz per m/s). **Departure weight**
  (review rounds 1 and 2): a hand leaving the barrel hands the gun back to the gun hand on the way
  out; before round 1 the barrel followed the palm until 15 cm and the correction then stayed (~21
  deg, the old "gun turns by itself" in a new form). Round 1 weighted the correction by the palm's
  distance (cm) from the ONE-HAND model at the palm's own place along the barrel. That brought the
  right wrist back in (review round 2): a forearm roll swings the one-hand model, more cm further
  along, so +-20 deg of roll moved the two-handed barrel 0.6-10.6 deg at 40-60 cm (+-30 deg at the
  drawn grip: 11.0 deg), sliding the hand from 22 to 60 cm with the hands' line 10-15 deg off left
  the barrel 2.7-15 deg off it, and 0.2 deg of right-controller rotation noise reached the barrel
  as 0.28-0.46 deg rms. Round 2 (`Solver::DepartStep`): the disagreement between the two-handed
  barrel (from the palm line) and the one-hand barrel (filtered, `dirBeta` 2) changes for two
  separate reasons, the left palm moving (the two-handed barrel turns: `dep`) and the gun hand
  turning (the one-hand barrel turns: `turn`). Aiming with both hands moves both and they cancel
  (opposite parts cancel even a few ticks apart, so a right hand that lags or leads does not
  count); a hand leaving moves only `dep`; a wrist roll or controller noise only `turn`. Because
  opposite parts cancel, |dep| never exceeds the change of the disagreement since the grab (noise
  cannot accumulate). The weight is 1 up to |dep| = the angle of `twohandgrip` at the drawn grip
  (atan(8 / 35.5) = 12.7 deg for the M4) and falls (smoothstep) to 0 at the angle of
  `twohandrelease` (22.9 deg): the same anywhere along the barrel. Two more factors: the hands
  apart (one-hand vs two-handed barrel) beyond twice those angles (25.4-45.8 deg: a forearm roll
  of +-30 deg moves the one-hand barrel ~19 deg), and the palm past the end of the handguard
  segment by `twohandgrip`..`twohandrelease` cm (along the two-handed barrel). In the band the
  barrel is a slerp between the FILTERED one-hand barrel and the two-handed one. Rate limits: the
  barrel follows `dep` (the palm's own unmatched motion) at most 150 deg/s (`pend`), and the
  applied weight changes the barrel by at most 150 deg/s; the correction itself is not limited
  while two-handed (round 1 limited it, and a fast wrist roll then moved the barrel for a few ticks
  until the correction caught up: up to 16.8 deg mid-step with a 20 deg roll in one tick). The
  palm-line filter restarts from the palm at the grab (a hand arriving fast dragged the filter
  from 20-25 cm away and its settling counted as departure). Engage blend: smoothstep over max(0.15 s, angle /
  100 deg/s), from the current correction. Release: by distance, what is left (a few degrees at
  most) eases to 0 over max(0.15 s, angle / 100 deg/s); by the grip (or a soft veto) the correction
  stays and shrinks by 0.5 deg per degree the gun hand turns (no time decay, no snap); hard vetoes
  drop it. Review round 2: the shrink goes into a goal (`leftGoal`) that the correction follows at
  most 150 deg/s; before, a gun-hand flick of 20 deg in one tick removed 2.1 deg instead of 10 (the
  per-tick cap cut it and the rest was lost: in game 15.7 deg became 7.2 after 120 deg of motion). **Runtime recenters** (review round 1): the solver keeps the filter state and the last
  gun-hand rotation in tracking space; on a WI-2 absorbed runtime recenter (`kConfirm` /
  `kUnsure`) that state is turned with the space (`Solver::ShiftSpace`; the WI-4c handlebar's
  filtered yaw too, `Handlebar::ShiftYaw`). Before, the filter chased the jump (a 40 deg recenter
  swung the barrel 19.3 / 8.5 / 3.8 deg over the first ticks at 72 Hz in the reviewer's model) and
  the leftover counted the jump as gun-hand motion (0.5 x 40 = 20 deg in one tick). Test switch
  `twohandshift 0|1`.
- **GUNSTOCK** (`holdmode gunstock`): the aim is always the right controller with `stockangle`
  (WI-4a), never the hand line. Engaged = the left controller within `stocktol` 6 cm / `stocktoldeg`
  25 deg of `stockmount` (released at 1.5x); B + Y saves it. Without a saved mount the barrel test
  above decides the flag only. Review round 1: players migrated to gunstock (and anyone on the stock
  without a saved mount) were not told; the migration notice now says how to save it, and with the
  gunstock hold, the two-handed grip on, the left controller tracked and no mount saved a hint
  appears once per session after 20 s of play (`AIM: stock hint shown ...`). Review round 2: a toast
  is one line of ~38 characters (720 px, Segoe UI Semibold 40 px, `DT_END_ELLIPSIS`), so round 1's
  texts showed "On the stock: hold B + Y 1 s with bot..."; the notice is now four short toasts ("Old
  aim kept: gunstock, 15.4 deg", "No stock? Set Controller hold: normal", "Stock: look along it at a
  far target,", "then hold B + Y 1 s, left hand on it"; since the WI-4c review round 2 toasts wrap, and these
  two are one toast: "Stock: look along it at a far target, then hold B + Y 1 s with your left hand on
  it"), the hint the last two (now that one toast), the B + Y results
  "Gunstock calibrated: N degrees" / "Angle kept: look along the gun, far" / "Left hand on the stock
  saved" (all measured with the renderer's own GDI call). B + Y in gunstock saves `stockmount` also
  when the angle is rejected (the old stock angle stays; before, the mount was not saved either).
  Not done: saving the mount automatically (a still pose for 3 s can be any pose).
- **Vetoes and left-grip precedence** (`th::Veto`): prop held in the left hand > grip closed in a
  prop / gravity-glove zone (not while already two-handed) > physical reload in progress
  (`control::g_leftHandBusy` bit 1 from `vr_input`: magazine out / in hand, magazine grab, latch,
  action rack) > grenade grip (bit 2: LB decided at the press away from the gun, or the trajectory
  held) > two-handed grip > magazine grab (blocked while on the gun, as before). The grenade test at
  the press uses the solver's geometry too (review round 1): `control::g_leftNearBarrel` (palm within
  `twohandrelease` of the barrel model, or of the drawn gun's model with a leftover, or near the
  stock mount) is ORed into `vr_input`'s `nearGun` / `nearGunNow`. Before, a grab-and-squeeze before
  the 150 ms dwell at the far end of the segment, 6.6-8 cm outward or with the support hand rotated
  was classified by the raw-origin line test alone: LB went to the game and the grenade veto
  blocked the grip for the whole press. Hard vetoes, which
  also drop the leftover correction at once: stationary weapon (turret / minigun: WI-4c), cinematic
  or locked scene, no gun in hand (arms not driven for two ticks, weapon switch). Dev: `twohand off`,
  `leftgrip off`.
- **Logs**: `AIM: two-handed grip (<rifle|short weapon|stock|stock (no mount saved)>, <why>): left
  palm N cm along the barrel, N cm from the grip line of the drawn gun (N from the one-hand one); gun
  turned N deg toward the left hand.`, `AIM: two-handed grip released (<why>): gun N deg off the gun
  hand ... (left hand moved N deg from the grip, hands N deg apart, N cm past the barrel end)` (why:
  `left hand off the barrel`, `gun hand turned away from the left hand`, `left hand past the end of
  the barrel`, `grip released`, a veto), a near-miss line every
  2 s, `STOCK: left controller on the stock saved (stockmount ...)`, the `TWOHAND` line (see
  "Diagnostic lines"). VIEW `thb` = the solver's blend weight.
- **Options**: no new Basic row (the two modes are Basic > Controller hold; its help text says
  so); Advanced (ini, with comments): `twohandgrip 8`, `twohandrelease 15`, `twohanddwell 0.15`,
  `twohandsmooth 1`, `twohandguardfwd 0`, `twohandguardright 0`, `twohandguardup 0`, `stockmount
  none`, `stocktol 6`, `stocktoldeg 25` (review round 1: one key per number). No default changed for
  a key in the users' inis (the hidden `twohand` radius 12 -> 8 is a retired key). Review round 1:
  hand-written `twohand on <cm> [s]` / `leftgrip on <cm>` lines (the old AIM line suggested them)
  survive in inis from version 8 on and would silently change meaning (cm = palm radius, seconds
  read as Hz); the version-10 migration reduces them to `twohand on|off` / `leftgrip on|off`
  (`vr::pure::MigrateTwoHandLine`, computed change).

#### Measured (copy B, novr, 26/10; scripts and raw output in scratchpad `B\wi4b`)

- **Unit tests** (`TestTwoHand*`, 2205 / 0 in all): palm target exact for a parallel left controller
  (< 1e-5 m, rolled 35 deg, gunangle 12); palm on the segment at 3 positions x roll 0/30: engaged,
  rotation < 0.05 deg; palm 10 cm up (0.2 s ramp): barrel 19.1 deg up, model through the palm
  0.000 cm 0.4 s later; right wrist roll +-20 deg about the palm: one-hand barrel 12.7 deg, two-hand
  barrel 0.000 deg; left wrist yaw +-30 deg with the palm fixed: old raw-origin line 7.3 deg, new
  0.000; canted 30 deg: roll kept within 0.00 deg; hysteresis (7 cm grip at once, 7 cm dwell 153 ms,
  10 cm never, 14 cm held, 16 cm released, outside the segment never, grip release + no dwell
  re-grab until the hand leaves); engage steps 1.57 / 2.08 deg per tick at 72 Hz for 11 / 15 deg
  (1.26 / 1.66 at 90 Hz); release step 0.000 and leftover unchanged for 2 s still, -5.00 deg after 10
  deg of gun-hand motion; filter: 180 deg/s sweep lag 3.43 / 3.21 deg (72 / 90 Hz; the EMA had 13.4),
  0.47 deg rms line noise -> 0.111 / 0.103 deg rms; gunstock: 1000 left poses, barrel vs the right
  controller 0.0000 deg, engaged inside 6 cm / 25 deg 500 of 500 and outside 0; every veto: never
  engaged, aim untouched, hard veto drops the leftover; left controller lost one tick: released
  without a step, re-grab only by the rules; pistol cup: engaged, direction unchanged.
- **In game, `mode synth`** (`twohand_game.py`, level 1 CP04, M4, both controllers as `synthctrl` raw
  matrices, pointing ray -10 deg, left parallel): palm on the drawn grip -> engaged by resting,
  correction 0.06 deg, `AIM: two-handed grip` and `ARMS: left hand on the weapon` in the same tick
  (same ms), drawn left hand 0.0 cm from the drawn grip; palm 10 cm up -> barrel 16.35 deg up, model
  of the drawn gun through the palm 0.00 cm; burst while two-handed: shot vs marker 0.000 deg, shot
  vs barrel axis <= 0.025 deg, first-shot barrel = commanded aim within 0.02 deg, barrel model and
  aim unchanged by the burst; left wrist yaw +-30 / pitch +25 / roll 80 with the palm fixed: barrel
  <= 0.06 deg; right wrist roll +-20 deg about the palm: two-hand barrel 0.055 deg (one-hand 14.9);
  release with the hand away: largest aim step per tick 0.000 deg over 90 ticks, leftover 14.74
  deg kept, unchanged after 2 s still, 9.74 after 10 deg of gun-hand motion; grip held 6 cm off:
  engaged by the grip, largest engage step 0.97 deg per tick; grip released: let go, no re-grab;
  gunstock without a mount: engaged, aim = right controller (0.000 deg); with a mount: on (0.0 cm /
  0.0 deg), off 10 cm away and rotated 40 deg, back on, aim unchanged; `twohand off` / `leftgrip
  off` release the grip and the drawn hand; spread: two-handed / one-handed mean shot deviation
  0.33-0.39 (factors 0.5 / 1.5 = 0.33); pistol (M9A1): engaged on the cup, aim unchanged.
- **Headset path, fake runtime `mode hmd`** (`twohand_hmd.py`): palm on the drawn grip -> engaged,
  correction 0.06 deg, drawn hand on the grip, same tick; palm 10 cm up -> barrel 16.35 deg (the same
  geometry as the synth check, same result), model through the palm 0.00 cm; hand away -> released, largest step 0.000 deg, leftover kept;
  gunstock B + Y (`stockcal`) saved `stockmount` and the grip was then recognised on it; Advanced
  menu `vrmenu cycle twohandgrip/twohanddwell` stepped and saved.
- **Earlier suites on the final dll** (c12bb69d, `final_chain.sh`, once each; the code WI-4b touched): regress PASS;
  B + Y (`bycal.py`) 6 of 6 accepted at 0.000 deg, 3 of 3 rejections; menu (`menu_fix1.py`) 9 of 9; headset-path
  aim (`aim_hmd_new.py`, M9A1) 4 of 4, barrel within 0.026 deg, wrist within 0.05 cm; gun frame M9A1 (`gunframe.py`,
  fresh Gorge02) 9 of 9 within 0.058 deg (a first run gave 0.196 deg in two poses with one burst off and the
  impacts 2-13 units from the marker instead of 160-180: scene, no two-handed grip engaged in that run); WI-1
  turret flick: turret 30.0 deg, view vs the helicopter 0.000 deg.

**Not covered without a headset / open.**
- The real feel: 8 / 15 cm and 150 ms under streaming jitter (VDXR, Steam Link), the 1 Hz / 9 Hz/m/s
  filter, the 0.5 deg-per-degree leftover after a release (a gun that stays off the right
  controller until it moves), and whether players reach for the drawn grip.
- (Fixed in review round 1: the palm model used to assume the left controller parallel to the
  right one; it now uses the live left rotation.) The `twohandguard*` keys remain the tuning keys
  for the palm constants on real controllers.
- Palm constants on VDXR and Steam Link (the `openxr_grip` model; WI-4a open item).
- A real physical stock (`stockmount`, `stocktol`); left-handed play (aim only, no drawn grip).
- Props interplay (physics owner, merge): props.cpp grabs with the left grip even while the left
  hand holds the gun (a prop in reach gets grabbed and the two-handed grip lets go: props win);
  `g_leftOnGun` (finger curl) is the solver's flag now.

#### Review fixes (round 1), measured (copy B, novr, 26/10; scratchpad `B\wi4b_fix1`)

The changes are described in the bullets above (distance weight, rate limit, fade after a distance
release, re-grab on the drawn gun, `ShiftSpace`, shared "near the gun" flag, relation anchored to the
first one, latched cant + drawn-hand shift, live left rotation, stock hint, split keys, `twohand` line
migration). Final dll e07ba0b7.
- **Unit tests** 2385 / 0 (new: `TestTwoHandDeparture`, `TestTwoHandRegrab`, `TestTwoHandRecenter`,
  `TestTwoHandCant`, `TestTwoHandLiveLeft`, `TestTwoHandNear`, `TestTwoHandMigration`):
  - palm leaving straight down at 0.3 / 0.6 / 1.2 m/s (dwell hold): barrel dragged at most 12.4 / 11.8 /
    9.8 deg (the reviewer's port of the old solver: 21.9 / 21.5 / 20.6 and kept), largest step 1.33 /
    2.08 / 2.08 deg per tick at 72 Hz (1.07 / 1.67 / 1.67 at 90 Hz), 0.00 / 0.00 / 3.50 deg at the
    release, 0.000 after;
  - grip released at 9.5 cm (13.3 deg left), palm back on the drawn grip: 8.2 cm from the one-hand
    model (the old test could not take it), engaged by the grip or by resting after leaving, gun turned
    0.38 / 0.04 deg;
  - 40 deg runtime recenter: barrel vs the same barrel in the new space 0.000 deg and correction
    unchanged with `ShiftSpace` (without: 2.1 deg first tick, 4.2 worst, leftover 8.16 -> 6.08 with the
    150 deg/s cap already in); handlebar yaw 0.000 deg (without: 16.0 deg);
  - cant 0 / 15 / 30 / 45 deg, palm on the drawn grip: gun turned 0.000 deg (level model: 0 / 3.54 /
    7.00 / 10.33 deg); right wrist roll +-20 after: barrel 0.000 deg, drawn left hand (drawn grip +
    shift) 0.00 cm from the real palm;
  - left controller yaw 30 / 45 / 60, roll 90, pitch 30, combined 45/30/30 vs the right, drawn wrist on
    the drawn grip: palm 0.00 cm from the live model, gun turned 0.000 deg (parallel model: 5.2 / 7.6 /
    9.6 / 1.0 / 3.0 / 5.3 cm off); left wrist rotated after the grab: barrel < 0.05 deg (frozen model);
  - right wrist roll +-20 deg with the palm on the model: 0.000 deg (palm 7.0-7.3 cm from the one-hand
    model, weight 1); with the palm already 3.6 cm off the model the -20 deg roll takes it to 9.1 cm
    (weight 0.94): barrel 1.02 deg (the distance rule, by design);
  - palm +6 cm: model through the palm 0.000 cm; +10 cm (past `twohandgrip`): weight 0.81, partial follow;
  - "near the gun" at the far end of the segment / 3 cm past it / 7 cm outward / 7 cm under: 0.0 / 3.0 /
    7.0 / 7.0 cm (< `twohandrelease`); `twohand on 12 0.3` -> `twohand on`, `leftgrip on 20` ->
    `leftgrip on`, already at version 11 (10 in copy B before the merge): untouched.
- **In game, `mode synth`, level 1, M4** (`twohand_game2.py`, the WI-4b suite with checks 2/4/5 updated
  for the distance rule): 20 / 20 PASS. Palm on the drawn grip: engaged by resting, 0.06 deg, aim and
  drawn hand in the same ms, drawn hand 0.0 cm from the grip; palm +6 cm: barrel 9.76 deg, model through
  the palm 0.00 cm (+10 cm: 13.47 deg of the 16.39 target, weight ~0.82); burst: shot vs marker 0.002
  deg, model unchanged; left wrist rotations 0.06 deg; right wrist roll +-20 deg (palm on the grip)
  0.06 deg; hand away in one step: largest aim step 1.37 deg per tick, back to the gun hand (0.00 deg
  left); grip released: no step (0.000), leftover 9.79 kept, unchanged 2 s still, 4.79 after 10 deg of
  gun-hand motion; grip 6 cm off: engage steps <= 1.37 deg; gunstock (no mount / mount) as WI-4b; vetoes;
  spread two / one 0.435 (factors 0.33, loose check); pistol cup. New checks (`twohand_fix1.py`):
  - N1 palm leaving at 0.6 / 1.2 m/s (`synthleftvel`): largest aim step 2.11 / 2.12 deg per tick, peak
    drag 11.4 / 9.8 deg, released at 15.7 / 16.4 cm with 0.0 / 3.5 deg left, 0.00 deg after;
  - N2 grip released at 9.5 cm (14.0 deg left), palm on the drawn gun's grip (9.4 cm from the one-hand
    model, 0.9 cm from the drawn one) + grip: engaged, gun turned 0.35 deg, largest step 0.06 deg;
  - N3 right controller rolled 30 deg (latched cant -26.0): palm on the drawn grip -> turn 0.023 deg;
    wrist roll +-20 about the palm after: barrel 0.057 deg, drawn left hand shifted 2.3 cm onto the model
    (the real left wrist was 2.8 cm from the unshifted drawn grip);
  - N4 left controller yaw 45 / roll 90 / pitch 30 (placed with the grip vetoed, then grip): free drawn
    wrist 0.0 cm from the drawn grip, palm 0.0 cm from the model, engaged by the grip, turn 0.059 /
    0.059 / 0.051 deg;
  - N5 palm at the far end of the segment / 7 cm outward with the grip in the same console poll
    (gesturesim, before the dwell): `GESTURE: left grip on the gun, not a grenade`, engaged by the
    grip; control 60 cm away: no such line, no grip;
  - N6 one-handed reload (X) after a burst: the drawn grip identical after the reload (0.0 cm; during
    the animation it moved up to 2.8 cm with the gun in Walker's hand, `g_handInWeapon`, not a relation
    swap), grab right after: engaged, turn 0.059 deg;
  - N7 gunstock, no stockmount, left controller tracked, session without the migration: `AIM: stock
    hint shown` + toast after 20 s, once in 51 s; in the migration session the notice carried the new
    B + Y stock toast (`AIM: migration notice shown (... B + Y saves the left hand on the stock)`).
- **Headset path, fake runtime** (`twohand_hmd2.py`): palm on the grip 0.06 deg, same tick; +6 cm 9.76 deg
  through the palm; hand away: step 1.35, back to the gun hand; B + Y `stockmount` saved and recognised;
  Advanced menu `twohandgrip` / `twohanddwell` and the new `twohandguardright` 0 -> 0.5 -> 0.0 and
  `stocktoldeg` 25 -> 26 -> 25 stepped and saved (the script's exact-text check wanted "0" and saw "0.0").
- **Runtime recenter** (`ph_hands_wi4b.py`, fake runtime `simevent zeropose`, head 40 deg / 14 cm off,
  `holdmode normal`, palm 5 cm off the model, correction 8.14 deg): engaged: aim step 0.000 deg
  (`twohandshift 0`: 2.10 deg yaw), correction 8.14 kept; grip-released leftover: aim step 0.000, leftover
  8.14 kept (`twohandshift 0`: 2.08 deg step, leftover 8.14 -> 6.07); view 0.0 deg in all four runs.
- **Migration in game**: copy B's v9 ini plus `twohand on 12 0.3` -> CONFIG `"twohand on 12 0.3" ->
  "twohand on"`. regress PASS.

#### Review fixes (round 2), measured (copy B, novr, 26/10; scratchpad `B\wi4b_fix2`)

The changes are in the bullets above (departure weight from the left palm's own motion, drawn-gun engage
test and live guard, re-press after a departure release with the grip closed, leftover goal, short toasts,
B + Y mount without the angle). Also: the drawn left hand keeps its shift onto the model while it leaves
the gun (`gripBlend` fading; before, the shift dropped on the first tick and the hand jumped ~96% of it);
`ShiftSpace` / `ShiftYaw` run only when the held tick showed the controllers moving with the space
(`heldHandsMoved > 0 && heldHandsStill == 0`, as `declareRaw`); the first grip relation of a weapon is
provisional when taken in cover (+0x498), within 1 s of the draw, within 0.5 s of a special move or
during a physical reload, and a relation still for 1 s outside those moments replaces it once
(`ARMS: grip relation ... taken as provisional (...)`, `ARMS: provisional grip relation ... replaced by
...`). New VIEW field `lrel=(x,y,z)` (drawn left hand minus drawn right hand, units). Final dll 86c495c3.
- **Unit tests** 2454 / 0 (new: `TestTwoHandRollAlong`, `TestTwoHandSlide`, `TestTwoHandJitter`,
  `TestTwoHandRegrabAlong`, `TestTwoHandSteerLag`, `TestTwoHandNewReleases`, `TestTwoHandLeftoverFlick`;
  `TestTwoHandFollowsLeft` now expects the angle weight). The reviewer's harnesses rebuilt on the new header
  (`wi4b_fix2\rev`) agree:
  - forearm roll +-20 / 30 deg about the raw -Z through the palm, palm on the model at 36 / 45 / 50 / 60 cm
    and 3.6 cm off it, grip and rest, as a one-tick step and as a 0.5 s ramp: barrel 0.0000 deg, 0
    releases (round 1: up to 10.6 deg at +-20, 20.3 at +-35);
  - hand slid 21.5 -> 59.5 cm in 1 s with the hands' line 10 / 12 deg off the one-hand barrel (up and
    sideways): weight 1.000 throughout, barrel 0.000-0.147 deg off the line after, model through the palm
    0.00 cm (round 1: 2.65-15.0 deg off, drawn hand 2.8-15.5 cm from the palm);
  - right-controller rotation jitter 0.2 / 0.4 deg per axis, palm still on the hands' line at 45 / 50 cm:
    0.0001 deg rms (round 1: 0.28-0.92); in the band (palm 18.4 deg away, weight 0.40): 0.089 deg rms;
  - leftover 12 / 16 / 20 deg, palm on the DRAWN gun 59 cm along + grip: 1 event, engaged 72 / 72, gun
    turned 0.000 deg (round 1: 7.8 deg, engage / release every tick at 16 and 20); visible left wrist on
    the drawn grip of the drawn gun after 9.6 / 15.1 / 18.8 deg: 0.000 deg (with the one-hand-frame left
    rotation the model was 1.7 / 2.7 / 3.3 cm off and the grab turned 2.3 / 3.8 / 4.8 deg);
  - two-handed 60 deg sweep at 120 deg/s with the right hand 3 ticks late / 3 ticks early / following only
    90 %: weight 1.000 throughout (departure at most 5.6 / 0.03 / 5.1 deg);
  - gun hand turned 60 deg away with the palm still: released at 45.6 deg apart ("gun hand turned away from
    the left hand"), 0.000 after; palm slid 25 cm past the end of the handguard: released ("left hand past
    the end of the barrel"); grip held through a departure release, hand back on the gun: no grab until the
    grip is pressed again;
  - departure (palm leaving down, resting hold) 0.3 / 0.6 / 1.2 m/s: largest step 1.36 / 1.76 / 2.09 deg
    per tick at 72 Hz (1.09 / 1.39 / 1.67 at 90 Hz), drag 13.4 / 13.4 / 10.5 deg (round 1: 12.4 / 11.8 /
    9.8), 0.000 after;
  - palm +10 cm at 30 cm (past `twohandgrip`): 19.1 deg of departure, weight 0.316 (round 1: 0.81 in cm at
    the palm; at the drawn grip the two rules agree);
  - leftover 15.1 deg, gun hand flicked 20 deg in one tick 4 times: 5.06 left after the first flick, 0.000
    after the fourth, correction step <= 2.08 deg per tick (before the leftover goal: 2.1 deg per flick);
  - 40 deg runtime recenter: with `ShiftSpace` 0.000 deg, correction unchanged. Without it (the A/B arm)
    the first tick now swings 23.3 deg instead of 2.1: the correction is no longer capped, and only
    `ShiftSpace` keeps the palm-line filter out of the jump.
- **Toasts**: measured with the renderer's own GDI call (Segoe UI -40 semibold, 720 px, DT_END_ELLIPSIS |
  DT_MODIFYSTRING, `wi4b_fix2\toastgdi.py`): all new texts fit (537-701 px); round 1's showed "Your old gun
  calibration was kept: g..." and "On the stock: hold B + Y 1 s with bot...". In game (migration session)
  the four were queued and shown (`TOAST: "..."`, 3.5 s apart).
- **In game, `mode synth`, level 1, M4** (first chain on dll c77aeba0, which differs from the final only in
  the leftover goal; the checks on that path were re-run on 86c495c3):
  - `twohand_game2.py` (rounds 0/1 suite) 20 / 20 PASS: palm on the grip 0.06 deg; +6 cm 9.76 deg, model
    through the palm 0.00 cm; +10 cm 11.54 of 16.39 deg; burst: shot vs marker 0.002 deg; left wrist 0.06
    deg; right wrist roll +-20 deg 0.06 deg (one-hand 12.9); release by distance: largest step 1.47 deg per
    tick, 0.0 after; grip release: no step, leftover 9.79 kept, 4.79 after 10 deg of motion (5 / 5b / 5c
    again on 86c495c3: 1.35 deg per tick, 9.79 kept, 4.79); spread two / one 0.21; pistol cup.
  - `twohand_fix1.py` N1 / N2 / N4 / N5 PASS (N1 steps 1.88 / 2.12 deg per tick, released at 22.0 / 21.3 deg
    of departure, 0.0 after; N2 turn 0.083 deg; N4 0.051-0.059 deg; N5 not a grenade at the far end / 7 cm
    out). N3 / N6 failed on c77aeba0 because the leftover of the previous check was not cleared by the
    harness's one-tick gun-hand flicks (7.19 / 2.73 deg left: the cap defect above); on 86c495c3 both PASS:
    N3 grab turn 0.023 deg, wrist roll +-20 after 0.057 deg, shift 2.3 cm; N6 drawn grip identical after the
    reload (0.0 cm, no leftover before the grab), grab turn 0.059 deg. N7 (stock hint) not re-run: it needs a session without the
    migration; the hint's two toasts are the migration's last two, which were shown.
  - `twohand_fix2.py`: R1 forearm roll +-30 deg with the palm on the model 55.5 cm along, resting and with
    the grip: barrel 0.038 deg, engaged throughout, though the palm was 17.6-18.2 cm from the one-hand model
    (round 1 would have released at 15 cm); R2 grip 7.5 cm up (hands' line 12.3 deg off the one-hand
    barrel), palm slid 34.7 -> 58.7 cm along the barrel: 0.127 deg at most during the slide, 0.023 after,
    weight 1.0, no release (round 1 model: 12.5 cm off at the end, weight ~0.3); R3 leftover 16.5 deg, palm
    on the drawn gun 58 cm along + grip: one engage line, no release, turn 0.028 deg (86c495c3: 16.4 deg,
    0.033); R4 gun hand rolled 30 then 50 deg (shift 2.3 cm), grip released: the first release tick moves
    the drawn left hand 0.227 units (0.45 cm), the 0.12 s blend to the real controller pose follows over 9
    ticks; a pop would add 0.96 x the shift = 1.10 units on that first tick; R5 Y, Y (M4 -> M9A1 -> M4): the
    first relation after the draw was provisional, then confirmed (no replacement line), grab turn 0.059 deg.
- **Headset path, fake runtime** (`twohand_hmd2.py`): H2 / H3 / H4 / H5 PASS (+6 cm 9.76 deg, release step
  1.35, B + Y mount saved and recognised, Advanced menu). H1 (palm on the grip: corr 0.06, drawn hand 0.0 cm,
  AIM and ARMS lines) failed only on the harness's same-millisecond test (17.181 vs 17.182 ms, one 13.9 ms
  tick); H5b is round 1's exact-text artifact ("0.0" for 0). `hmd_by_mount.py` H6 PASS: B + Y with the line
  of sight 40 deg off the gun -> `STOCK: REJECTED ... kept the old value` and `STOCK: left controller on the
  stock saved (..., stock angle not changed)`, `stockangle 15.0` unchanged in the ini, the mount recognised
  (0.0 cm), toasts "Angle kept: look along the gun, far" + "Left hand on the stock saved"; accepted case:
  both saved, "Gunstock calibrated: 15.0 degrees".
- **Runtime recenter** (`ph_hands_wi4b.py`, `simevent zeropose`): `twohandshift 1`: engaged and leftover aim
  steps 0.000 deg, corrections unchanged (8.20 / 8.14; 8.14 / 8.14 on 86c495c3); `twohandshift 0`: 22.4 deg
  yaw step engaged (see above), leftover 8.14 -> 6.07 (86c495c3: -> 0.0, the jump now counts in full as 40
  deg of gun-hand motion through the leftover goal; with the shift it counts as none). The script's setup jumps the fake head alone: those confirms logged
  `controllers in the held tick: 0 moved with the space, 2 did not` and no longer turn the solver state; the
  zeropose events logged `2 moved, 0 did not` and were shifted.
- **Earlier suites touched**: B + Y (`bycal.py`) 6 / 6 accepted at 0.000 deg, 3 / 3 rejections; regress PASS
  (c77aeba0 and 86c495c3); no `SCRIPT: exception` in either chain. Copy B left at level 1 with 86c495c3,
  ini restored byte-identical to its start state.

### WI-4c: turret grip modes (bug 1, "both hands controlled the turret")

Tester: "1st scene with turret would feel more natural if both hands controlled the turret. It wasn't until I saw
the hint to use right hand only that I was able to aim." The user: "for two-handed turrets create a GUNSTOCK mode and
a NORMAL mode, because the aiming systems are different".

**Causes (before):**
- The turret followed only the gun hand's frame (since WI-4a: pointing ray + Gun angle). The other hand did
  nothing, and moving both handles did nothing.
- The rifle two-hand grip cannot engage with the hands side by side (WI-4b vetoes it on stationary weapons anyway).
- No turret guidance, and hint 0 said "right hand".
- The mod wrote absolute PlayerController rotations past the turret's limits. The game takes them back on some
  sides only: at +120 it took back ~27 deg per tick, while at -120 the turret reached -175 from the helicopter (WI-1),
  and the M2E2 turned 109 deg (WI-1 fix 2).
- The marker showed the hand's direction, not where the turret fires.

**Changes:**
- **`turretgrip auto|normal|gunstock`** (Basic "Turret grip", default auto = follows *Controller hold*). It applies
  on turrets with `turretaim hand`. On a turret the effective hold (`vr::control::EffectiveHoldMode()`,
  `g_turretHoldNow`, written by `TurretGripBegin()` at the start of `UpdateHeadTracking`) picks the gun frame's
  angle: `gunangle` with NORMAL, `stockangle` with GUNSTOCK, whatever *Controller hold* says. The AIM gun-frame line
  names the effective hold.
- **NORMAL = handlebars** (`vr::pure::Handlebar`, `TurretGripStep()` after the gun frame in both branches):
  - **changed 27/10 evening:** the handlebar now engages only on the helicopter minigun (`in.allowed =
    g_flightGunHeld`); ground turrets in NORMAL use the two-hand pivot (`vr::pure::TurretHandles`, `turretpivot`).
    See "Headset session fixes (27/10 evening)". The rest of this bullet describes the handlebar itself;
  - yaw = the perpendicular of the horizontal line between the PHYSICAL palms (`HandlebarYawFromPalms`): positions
    only, so it holds for any pivot (the turret ahead, or a torso twist behind) and does not depend on the
    controller pose convention (VDXR / Steam Link);
  - engage: both grips held, palms 25-90 cm apart, the line within 35 deg of the controllers' mean forward (mean of
    unit vectors, left hand on the left), slope under 40 deg. Hold: 20-100 cm, 50 deg, 50 deg (hysteresis);
  - pitch = the mean of the two hands' exact frame pitches (pointing ray + `gunangle`, no stock angle);
  - 1-euro filter (1 Hz, beta 4 /s) on yaw (unwrapped around the last value, state kept in the same turn) and pitch;
  - 100 ms smoothstep blend to and from the one-hand aim (`turretbarblend`). A step counts at most 1/30 s of blend,
    and after a gap (the step not run: another turret, a GUNSTOCK spell, a lost controller, a hitch) the first step
    uses a nominal 11 ms (review round 1: before, a gap gave the whole blend in one tick). It lets go when a grip
    opens or the pose fails. One hand alone aims as before;
  - with a tracked controller, the grips on a NORMAL turret send no RB / LB, and no grenade gesture, to the game
    (`g_turretGripsAreHandles`, `ApplyGestures`; a `TURRET: grips are the handles ... kept from the game until
    released` line when it keeps one back). Review round 2: decided at the PRESS and kept until the release
    (`vr::pure::GripStripLatch`), so a grip still closed when the turret ends (TurretLeave), the game pauses, the
    dashboard opens, a scene starts, the controllers drop out or the grip mode changes never reaches the game as a
    fresh press. A press that starts paused or in the dashboard goes to the game's menu as before, and the grip that
    used an interaction (A, e.g. boarding) also stays away from the game until it opens;
  - the thresholds are Advanced keys (review round 1): `turretbarmin` / `turretbarmax` (cm, held 5 cm closer / 10 cm
    further), `turretbaralign` (held +15), `turretbarslope` (held +10), `turretbarblend`, `turretbarsmooth` /
    `turretbarbeta` (1-euro). The engage and release lines give the handlebar vs gun-hand offset (yaw, pitch), for
    the headset session (if it is usually above ~5 deg, keep it at release and shrink it with hand motion, as WI-4b).
- **GUNSTOCK** = the gun hand's frame with `stockangle`: the aim comes from the stock, as with WI-4b's gunstock.
  Never handlebars.
- **Limits in the mount's frame** (`vr::pure::ClampAimInMount`, `turretlimits on|off` in Advanced):
  - the aim target is clamped in the frame of the turret actor (Pawn `m_turretInteraction` +0x1184 ->
    `m_turretActor` +0x224, its Rotation), roll included. That actor is the mount: it does not turn with the aim;
  - helicopter minigun: +50 right / -65 left / +18 up / -25 down (measured below; `turretflightright/left/up/down`).
    Ground turrets: +45 / -55 / +21 / -23 (CMN_TurretCam, not measured; `turretgroundright/left/up/down`, so the
    headset session can correct them without a rebuild);
  - one 30 ms buzz on both hands when the aim reaches a limit, re-armed only after 0.5 s at least 2 deg inside the
    limits (`vr::pure::TurretLimitBuzz`; review round 1: with the mount banking it repeated up to every 0.6 s).
- **Limit latch** (`vr::pure::TurretLimitLatch`, yaw):
  - if the game still takes back at least 70% of a push twice in a row (a limit narrower than the mod's), the mod
    stops pushing that way, with a 40 ms buzz;
  - one probe step every 0.5 s; released when the aim comes back or a probe is accepted.
- **Yaw hold hysteresis** (`vr::pure::TurretYawHold`, WI-4a review round 2): held above 70 deg of barrel pitch,
  free again below 65 deg. Not used with the handlebars (yaw from positions), nor, since 27/10 evening, while the
  ground-turret two-hand pivot has weight.
- **Marker** on turrets: the game's aim (`YGamePawn` +0x14A8, what `UpdateTurretAiming` fires along; equal to the
  PlayerController rotation in steady state, one game tick behind it in large steps: 11.4 deg in the synth run's
  step to -60).
- **Guidance** (texts in `src/ui_text.h`, measured by the unit tests with the renderer's own GDI calls):
  - a toast at the first turret of the session (not with `hints off`), one line at 40 px: NORMAL "Turret: grab both
    handles and steer", GUNSTOCK "Turret: aim along your stock";
  - toasts wrap (review round 1): 40 px on one or two lines, else 36 / 32 px (up to three), shown 3.5-7 s by length.
    Before they were one line cut with "..." after ~38 characters: hint 0 read "Aim with the gun in your right
    hand; l..." (the tester's "hint to use right hand only"), and no turret guidance was ever visible;
  - hint 0: "Aim with your gun hand. Other hand on the barrel (grip or rest it) = two-handed";
  - review round 2: the seven controller hints are about the gun in your hand, so on a turret they wait
    (`vr::ui::HintSequence`) and one turret hint shows instead: NORMAL "Turret: grip both handles and turn them like
    handlebars. One hand alone also aims", GUNSTOCK "Turret: aim along your stock with your gun hand, as with your
    guns". `HintsTick` now runs before `UpdateGunInHand`'s arms-driven early return (WI-4a's pre-existing low): before,
    with the arms driven (the default) the hints ran only on turrets, where the arms are released, and never on foot;
  - a TURRET line on the Controls tab; the tab's rows close up (52 -> 47 px) under the Virtual Desktop notice, so all
    13 lines fit the 820 px panel (before, the 13th was drawn at y 816-864), and every line fits the 1040 px.
- **TURRET line:**
  - the first field is `grip one-hand|handlebar` (old parsers match the one-hand lines as before; since 27/10
    evening also `two-hand handles`);
  - new tail fields: `turretgrip`, handlebar weight, reason, palms / alignment / slope, bar yaw / pitch, mount
    yaw / pitch / roll, the aim in the mount, limits, clamped, clamped target, latch, game aim;
  - new lines: `TURRET: handlebar grip on|off`, `TURRET: both grips held, no handlebar: <why>`,
    `TURRET: limit latch on|off`.

**The game's own limits** (26/10, prologue minigun, `turretaim stick` + `turnspeed 0`, full stick, memory sampled at
25 Hz, `scratchpad\B\wi4c\probe_limits.py`):
- yaw: +50.0 / -65.0 deg relative to the turret actor's yaw;
- pitch up: +18.0 in the turret actor's frame with the helicopter's roll included (CMN_HeliGunCam's values);
- pitch down: the stick reached -32.3 and was still creeping (soft region). With the mod's absolute writes, the
  game's aim stops at -25.0 in the mount frame. The first WI-4c run had the down limit at 32, and there the game took
  back about 2 deg of every push: the down limit is 25.

**Measured** (copy B, final dll `fdc40d2e`, prologue minigun `SP_RadioMan03_P`, 72 Hz, each a fresh prologue
load; `scratchpad\B\wi4c\turret_wi4c.py`, `final_chain.sh`):

| Check | Before (WI-4b dll / first WI-4c run) | WI-4c final |
|---|---|---|
| `synthaim 30 -5`, one hand: turret turn, time to within 1 deg | WI-4b: same gain, no limit | 30.03 deg, 111 ms (90% in 70 ms); pitch -4.98 for -5 |
| Shots vs the game's aim (= the marker on turrets), 12 rounds | marker on the hand's direction, not the aim | mean 0.76 deg (yaw 0.12, pitch 0.75); spread sd 2.1 deg (the minigun's own), so the mean is only good to about +-0.6 deg; the deterministic proof is the SHOT line's `aimat` (review round 1, below) |
| Hand at +120 / -120 | WI-1: +120 refused ~27 deg per tick, -120 reached -175 deg from the helicopter | +49.99 / -65.01 deg in the mount, refused <= 0.066 deg, latch never needed |
| Hand at pitch +40 / -60 | first WI-4c run (down limit 32): ~2 deg taken back every tick | +18.02 / -24.98 in the mount, refused 0.000 |
| Handlebars, headset path (fake runtime, fake hands on two handles, both grips): palms turned, controllers facing forward (palm line +20.79) | one-hand aim only: 0 | turret +20.88 vs the helicopter |
| same: rigid torso twist 20 deg about a pivot 30 cm behind the head | - | +20.012 |
| same: hands back; grips released | - | 0.000; back to the gun hand |
| View vs the helicopter over that whole run (helicopter turned 15.2 deg) | - | 0.000 deg |
| Handlebars, synth (`synthhand` places the gun's palm pivot, `synthleft` the raw origin: palms 4 deg / 11 deg slope apart at the start) | - | bar +20.45 -> turret +20.61; 10 cm sideways: -0.14; grip released: back to the gun hand in 190 ms; `turretgrip gunstock`: never handlebars |
| Yaw hold, barrel at -75 (WI-4a suite) | WI-4a: held, steps 0.000 | held, 6/6 steps 0.000 (the suite's verdict read "false" on the one line logged before the command, target -0.06); free again at -5 |
| WI-1 turret flick (headset path) | 0.000 deg | view vs the helicopter 0.000 deg (turret 42.2, helicopter 23.1 deg) |
| Exit (end of the flight) | "TURRET: left, PlayerController pitch reset to 0." | same line at every flight end |


Earlier suites on the final dll (once each): `tools/regress.py` PASS (fake runtime); menu: *Turret grip*
auto -> normal -> gunstock -> auto and `turretlimits` on -> off -> on, 5/5 with the ini writes. Copy B left at
level 1 with the final dll.

**Unit tests** (2272 / 0; the WI-4c tests are the plan's b1 tests 1-9):
- handlebars:
  - pivot independence: worst 0.0000 deg (pivot at the body, 35 cm ahead, 30 cm behind; +-20 / 40 deg);
  - translation: 0.00 deg;
  - rejections: crossed hands, inline 40 cm, 10 / 100 cm apart, slope 50 deg, one grip, not NORMAL;
  - hysteresis: 94 and 22 cm held but not engaged; a 40 deg controller twist held but not engaged;
  - jitter +-2 mm: yaw sd 0.056 deg (raw 0.246);
  - one controller lost: back to one hand in 111 ms, steps <= 0.33 deg;
  - through +-180: largest step 0.11 deg;
- yaw hold: 1 state change in 500 ticks with the barrel trembling at 69-71 deg (without hysteresis: 249);
- clamp:
  - mount at yaw 30: +100 -> +80, -60 -> -35;
  - pitch: +30 -> +18, -40 -> -25;
  - the PlayerController's accumulated turns are kept;
  - the prologue stick sample reads -64.55 / +18.02 in the mount frame;
- limits model (helicopter turning 20 deg/s):
  - hand at +-120: turret at +50.0 / -65.0, view swing 0.000 / 0.000 deg;
  - a game limit at +45: 546 push ticks without the latch, 19 with it, view 0.000.

**Not done / open:**
- **The second turret (M2E2, `SP_Desert01_03_K`) was not boarded.** The time box ran out after two approaches and
  their variants, so the ground limits (+45 / -55 / +21 / -23) and the handlebars on a static mount are unverified
  in game. The level sweep shows the prologue as the only map that starts on a turret.
  - Walking Walker in with the stick from 150 units behind the gun fires the trigger's Touch:
    `YGamePawn.m_turretTriggers` (+0x1118) gets the trigger. A at the grab point then starts `TurretEnter`
    (special move 59) and sets `m_turretInteraction`.
  - The move then stalls: Walker stays still and the weapon stays the M4 for 15+ s, also with `mode off`.
  - `ucall pawn EnterTurret`, the trigger's `UsedBy` (returns false) and `Touch` through ucall (no registration)
    do not board either.
  - Helper: `scratchpad\B\m2e2_board.py`. Next step: what `YSpecialMove_TurretEnter` waits for (Ghidra: an anim
    notify or a reach test). Review round 1: with A pressed where the walk-in ends (no teleport) TurretEnter starts
    and ends after exactly 2.0 s without boarding (below).
- **Pitch has no latch.** On a turret whose game pitch limit is narrower than the mod's, the mod would keep pushing
  about 2 deg per tick (seen on the helicopter before the limit fix).
- **Headset checks:**
  - handlebar feel and thresholds (25-90 cm, 35 / 50 deg alignment, 1-euro 1 Hz / beta 4);
  - pitch from the mean wrist angle vs pushing the handles down or up (lever cue);
  - the limit buzz;
  - no RB / LB from the real grips on a NORMAL turret (review round 1 proved the strip on the headset path with
    the fake pad; the controller grip -> pad path itself needs real controllers);
  - the toast wording;
  - GUNSTOCK on a real stock;
  - palm positions on VDXR and Steam Link (the handlebar yaw uses positions only).
- **WI-4a lows "if touched"** (`movedir hand`, the INPUT line on VDXR): not touched.

#### WI-4c review fixes (round 1), measured

Copy B, novr, fake runtime. Tested dll `21ac7ec2` (one launch, one pass); final dll `1040a102` = the same plus a code
comment and the Basic *Turret grip* help text, used for the M2E2, hmd2b, bycal and Basic-menu runs. Scripts and
outputs: `scratchpad\B\wi4c_fix1` (`fix1_chain.sh`, `fix1_chain2.sh`, `turret_fix1.py`, `m2e2_try.py`, `renders\`).

- **Guidance you can see (high).** Texts moved to `src/ui_text.h`; the toast renderer wraps (above). Rendered by the
  game's own code (`toastdump`, `vrmenu dump <file> <tab> vd|novd`, new test commands) and by the unit tests:
  - 9 toasts (the 7 hints, the 2 turret toasts): 0 cut; the turret toasts on one line at 40 px (664 / 523 of 720 px);
    hint 0 at 36 px on two lines. Before: one line, "...", hint 0 shown as "Aim with the gun in your right hand; l...";
  - Controls tab with the VD notice: 13 lines, rows 47 px, last row ends at 799 of 820 px (before: 864); widest line
    968 of 1040 px (before: TURRET 1197, CROUCH 1109); without the notice rows stay 52 px;
  - Basic *Turret grip* help now starts with what matters ("normal = grip both handles and steer; gunstock = aim
    along the stock; ..."): the row shows one 610 px line;
  - the grips hint (grenade / squad orders) is held back while on a NORMAL turret (code-read: in the prologue it came
    ~36 s in, on the minigun).
- **Blend after a gap (medium).** `HandlebarParams::maxDtSec` 1/30 s, nominal 11 ms after a gap in
  `TurretGripStep`. Unit: bar 24.66 deg from the gun hand, first step after a gap weight 1.000 (aim jump 24.66 deg)
  -> 0.259 with dt 0.1, 0.034 with the nominal dt (aim steps <= 5.07 deg per tick, smoothstep over 100 ms). Game
  (`bar` session, `turretgrip gunstock` -> `normal` with both grips held, 24.5 deg offset): largest turret step
  9.108 -> 3.966 deg per tick, settled in 180 ms; first engage 0.648; bar +20 -> turret +20.43.
- **RB / LB on the handles (medium).** Gated by `GestureDetector::Gameplay()`. Headset path (fake hands tracked,
  `pad fake`, `inputlog` prints what the game receives): NORMAL turret LB -> 0x0000 and RB -> 0x0000 with
  `TURRET: grips are the handles ... kept from the game`; GUNSTOCK turret RB -> 0x0200; NORMAL turret with the
  game paused (PlayerController frozen 3 s) RB -> 0x0200, no strip. (The pad's LB is always held back on the
  headset path while the left grip is open, by the gesture detector: LB comes from the left grip; so the "passes"
  side was shown with RB.)
- **Tuning keys (medium).** Advanced keys with ini comments: `turretbarmin 25`, `turretbarmax 90`,
  `turretbaralign 35`, `turretbarslope 40`, `turretbarblend 0.1`, `turretbarsmooth 1`, `turretbarbeta 4`,
  `turretgroundright/left/up/down 45/55/21/23`, `turretflightright/left/up/down 50/65/18/25` (one key per number;
  `turretlimitsground|flight r l u d` stays as a command). Defaults give the old values exactly (unit test).
  Menu: 11/11 clicks (*Turret grip*, `turretlimits`, `turretbarmin` +-1, `turretgroundright` +-1,
  `turretbarblend` +-0.02, written to an ini that did not have them). No configversion change.
- **M2E2 (high).** Helper docstring corrected (the trigger registers, boarding stalls; variants `grab`, `immediate`,
  `walking`). One proper try of the reviewer's avenue (`m2e2_try.py`: the cutscene after `ce cpBusRoof` needed
  ~190 s; the two chain tries ran inside it and never walked): walk-in registers the trigger (`m_turretTriggers` 1),
  A where the walk-in ends (140 / 130 / 110 units from the gun, no teleport) starts TurretEnter (special move 59),
  which ends after exactly 2.0 s each time without boarding (`m_turretInteraction` 0). With the teleport to 60 units
  (WI-4c) it stalled 15+ s instead. Open for the final regression (orchestrator 20:15: a ground turret reached in
  normal play); the ground limits can now be corrected from the Advanced menu.
- **Lows.**
  - Limit buzz: one buzz, re-armed after 0.5 s at least 2 deg inside (`TurretLimitBuzz`). Unit: hand still at the
    limit while the mount banks (margin -0.5..+1.5 deg, 10 s): 1 buzz (old rule 7). Game (synth session):
    1 `TURRET: limit buzz` line over the whole +120 / -120 / +40 / -60 sequence (the aim stayed clamped throughout:
    by design, a jump straight from one limit to the other does not buzz again).
  - Handlebar vs gun-hand offset in the engage / release lines (bar session: +4.0 at engage, +24.5 at release;
    release swing 3.99 deg per tick, 181 ms). Carrying the offset is a headset decision (logged for it).
  - Left-handed handlebar (headset path, `hand left`, AIM line "left hand"): palm line +20.79 -> turret +20.80;
    right-handed +20.85 in the same run; view vs the helicopter 0.000.
  - GUNSTOCK frame on a turret: `turretgrip gunstock` + `stockangle 15`, one hand level: AIM "gunstock hold, gun
    +15.0", turret target pitch +15.00.
  - B + Y on a turret uses the effective hold: `turretgrip normal` with `holdmode gunstock` (copy B's migrated
    hold): "STOCK: accepted calibration (normal hold, turret grip) ... -> gunangle" (before it wrote stockangle).
    Off turrets unchanged: bycal 6/6 + 3/3 at 0.000 deg (level 1, final dll).
  - Evidence: the SHOT line's `aimat` lies within 0.45 deg of the logged game aim on all 14 rounds of the synth fire
    phase (mean of the spread rounds 0.79 deg, sd 1.9 / 2.4); the game aim (+0x14A8) equals the PlayerController
    rotation in steady state (<= 0.04 deg) and lags it by one game tick in large steps (15.9 deg in the step to
    +40, 7.4 to -60).
- **Same session, unchanged:** `synthaim 30 -5` 30.06 deg in 111.7 ms, pitch -5.01; limits +50.00 / -64.99 / +17.81
  / -24.96 in the mount, refused <= 0.005 deg, latch never needed; WI-4a turret hold 0.000 steps held, free again
  (verdict true); WI-1 turret flick view vs the helicopter 0.000; regress PASS; unit tests 2488 / 0.

#### WI-4c review fixes (round 2), measured

Copy B, novr, fake runtime, headset path (`mode hmd`, fake hands). Part 1 on dll `06822f7e` (one launch, one pass:
`scratchpad\B\wi4c_fix2\fix2_chain.sh`, `fix2_game.py`); part 2 on the final dll `9ec05e0a` = part 1 + `ReleaseArms`
clearing `g_leftOnGun` (`fix2_chain2.sh`, `fix2_lb.py`, regress). Unit tests 2516 / 0 (2488 before).

- **Hints on turrets and on foot (medium).** The seven controller hints are about the gun in your hand. Before: with
  the arms driven (the default) `HintsTick` ran only on turrets (it sat after `UpdateGunInHand`'s arms return, and the
  arms are released on stationary weapons), so a first session saw hint 0 "Other hand on the barrel ... =
  two-handed" on the prologue minigun, where the two-hand grip is vetoed and a hand on the barrel fails the handlebar
  pose, then the ADS, hand-reload and gunstock hints there; the grips hint deferred "until you get off the turret"
  never came. Now `HintsTick(gameplay, turret)` runs before that return (not while paused), and
  `vr::ui::HintSequence` holds the weapon hints while on a turret and shows one turret hint per grip instead (NORMAL
  "Turret: grip both handles and turn them like handlebars. One hand alone also aims"; GUNSTOCK "Turret: aim along
  your stock with your gun hand, as with your guns"); on foot the sequence resumes where it was. `hints on` now takes
  effect in a running session (before, auto with three sessions counted latched "done" at the first tick).
  - Unit (`TestHintSequence`, 72 Hz): prologue 60 s on a NORMAL turret -> 1 toast, the turret hint; then on foot
    60 s -> the 7 weapon hints in order; a GUNSTOCK turret mid-sequence -> its hint only, then the rest.
  - Game (`hints on` with a fresh prologue load, default hold): on the turret (boarded 23:58:17.7, left 23:58:50.4,
    3 s of it paused) the TOAST lines were the first-turret toast, "Recentered", the migration notice and the turret
    hint (queued 15.0 s after boarding): no weapon hint. Level 1 on foot, arms driven (GUNLOG `arms=1`): hints 0..6
    in order, queued 15.0 s after gameplay and every 7.0 s.
- **RB / LB on the handles, latched per press (medium).** `vr::pure::GripStripLatch` in `ApplyGestures`: a press made
  while the grips are handles (NORMAL turret, a controller tracked, not paused, not in the dashboard) stays away from
  the game until it is released, whatever happens to the turret, the grip mode, the pause, the dashboard, a scene or
  tracking; a press the game has not seen yet that becomes a handle press is held too; a press the game has seen
  (e.g. started in the pause menu) is never cut mid-press (that would be a release: a grenade); the grip that used an
  interaction (A) also stays off RB / LB until it opens (boarding with the grip). Unit (`TestGripStripLatch`, the
  game's view of the button): handles off mid-press 1 -> 0 new presses; boarding with the use grip 1 -> 0; a press
  hidden by another rule then a handle 1 -> 0; a press started in the pause: 1 press, 0 releases. Game (`inputlog`,
  which now logs every change of the asked / received pair and the first read after each `pad fake`):
  - RB held on the NORMAL turret across a pause and a resume: asked 0x0200 -> 0x0000, 0x0210 -> 0x0010 (paused,
    resumed: the new `PAUSE: game paused|resumed` lines), never 0x0200, no `SQUAD: button pressed`;
  - RB held, `turretgrip gunstock` mid-press, released: 0x0200 -> 0x0000 throughout, no SQUAD line, then
    `TURRET: handle grips released (handles now 0)`; a new press on GUNSTOCK: 0x0200 and `SQUAD: button pressed`;
  - game paused on the NORMAL turret, then RB pressed: 0x0200 to the game's menu (round 1's exemption, now only for
    presses that start paused or in the dashboard).
- **LB from the real left grip (low).** Part 1 found why round 1's LB case was confounded: on a turret the gesture
  detector answered "left grip on the gun, not a grenade" to every left grip, on NORMAL and GUNSTOCK alike, because
  `ReleaseArms()` kept `g_leftOnGun` at its last on-foot value (copy B came from level 1 with the left hand on the
  gun). `ReleaseArms` now clears it (arms released: turret, scene, death). Part 2 (`gesturesim grab` = left grip
  closed in gameplay + `pad fake` LB): on the NORMAL turret LB stayed 0x0000 for the whole press while the strip line
  went from "allowed by the gestures: 0" to "1" (the simulated left grip closed) and back; on the GUNSTOCK turret LB
  went 0x0000 -> 0x0100 (the gestures allowed it) -> 0x0000; no "left grip on the gun" line in either. Part 1 (before
  the fix): "left grip on the gun, not a grenade" in both, LB 0x0000 on GUNSTOCK too.
- **Leftovers (low).** The stale one-line comments in hooks.cpp (B + Y result, migration notice) updated; the stock
  hint is one wrapped toast ("Stock: look along it at a far target, then hold B + Y 1 s with your left hand on it",
  36 px, 2 lines) instead of two half sentences, in the migration notice too (now three toasts), texts in
  `ui_text.h`; `toastdump` and `TestUiTextFits` cover 14 toasts (the 7 hints, the 2 turret toasts, the 2 turret
  hints, the 3 stock toasts): 0 cut, at most 2 lines. `m2e2_board.py`'s docstring gives each variant's outcome
  (`immediate`, the default, and `walking`: TurretEnter ends after exactly 2.0 s, not boarded; `grab`: stalls 15+ s).
- **Suites (once each).** Part 1 (`06822f7e`): `fix2_game.py` 8 of 10 verdicts (the 2 LB verdicts failed on the stale
  `g_leftOnGun`, fixed above), `toastdump` 14 toasts 0 cut. Final dll `9ec05e0a`: `fix2_lb.py` 3 of 3, regress PASS
  (max view jump 0.0, reload gesture, cinema with the arms re-acquired), unit 2516 / 0, no `SCRIPT: exception`. Not
  re-run (code not touched): the turret aim, limits and handlebar sessions, bycal, menus. Copy B left at level 1 with
  the final dll, ini byte-identical to the start state.
- **Headset checks added:** the turret hint wording; a real dismount (TurretLeave) with the grips still closed (the
  game test used the flight end, a pause / resume and a grip-mode flip); LB from the real left grip on a GUNSTOCK
  turret.

### Final regression of copy B (27/10)

Every earlier suite once on the final copy-B dll (`5d2d4c90`, built 27/10 00:15), plus the checks only this round does:
weapontest per weapon class, a 10-level load pass, every new option in each of its states, the seated-shoulder pose
measurement and the ground-turret search. Copy B only (novr, fake runtime `openvr 72`, `fpscap 72`, copy A running on the
same GPU). Scripts and outputs: `scratchpad\B\final` (`chain1..7.sh`, `master.sh`, `c<chain>_*.txt`).

- **Code changes (small, assigned to this round by the WI-4a round-2 review):**
  - Migration: `MigrateAimpitchLine` treats only |aimpitch| >= 5 (`kAimpitchStockMinDeg`) as a stock calibration.
    Smaller values are Advanced-menu nudges (0.05 degrees per click) and migrate like 0 (`holdmode normal` +
    `gunangle 0`, no migration notice); before, `aimpitch 0.35` became gunstock with `stockangle 39.8`, i.e. bug 6's
    39-degree barrel kept. Unit tests +19 checks (2535/0). In game: v9 ini with `aimpitch 0.35` -> `holdmode normal`,
    `gunangle 0`, AIM `normal hold, gun +0.0`, 0 migration-notice lines; `aimpitch -24.0` -> `holdmode gunstock` +
    `stockangle 15.4` as before.
  - hooks.cpp: the stray `comp =(` formatting edit (merge noise) reverted.
  - Docs: this section; the WI-4c section moved under "Tester VR fixes (25/10)" (it sat under "Tester fixes (0.9.11)");
    README migration sentence.
- **Results (final dll, once each; the same metrics as the item sections above):**

| Suite | Result |
|---|---|
| Unit tests | 2535 / 0 |
| regress.py (copy B tools, identity `SpecOpsTheLineB.exe`, dummy saves, fake runtime) | PASS (max view jump 0.0, shot 0.024 deg, magazine gesture, cinema) |
| WI-0 fake_smoke | every functional check PASS (vsync accounting, level head 0.000 deg, tilt 0.003, posed submit 600/600, recenter blink, zeropose 0.0 deg, presence); throughput and hitch-count FAIL with copy A on the GPU (B_BRIEF 19:20); `fpscap 0` hitch suite PASS (4 hitches, 20 repeats) |
| WI-0 crouch / turret baselines | crouch: view drop 20.0, smooth (its REPRODUCED verdict reads the fix, as since WI-3); turret_hmd_fake and turret_baseline: not reproduced |
| WI-0 aim baseline, headset path | 30 / 30 poses, every first shot on the expected frame (old `aimpitch 0 / -24` poses), 0 clipped |
| WI-0 aim baseline, synth path | 29 / 30 (SAW `aimpitch -24` roll 60: no SHOT lines after the recovery); M9A1 roll 30: one of 3 bursts drawn 6.5 deg off in yaw, shots on the drawn barrel (0.026 deg), starts 5.2 units off the axis (the WI-0 "unknown cause" outlier, now on the synth path); clipped shots on the palm spheres (known, fixed in A) |
| WI-1 head, predictframe, dynres, hitch | drawn vs headset 0.002-0.003 deg; predictframe PASS (off -15.0, on -0.2, auto switched on after 2 windows); dynres: 6 changes in one stage where the GPU p90 stayed at 14-16 ms over the 13.9 ms budget (copy A on the GPU: the 2-s over-budget dwell), 0 in the steady stages |
| WI-1 turret flick / wrap / +-120 | view vs helicopter 0.000 / 0.000 deg; +-120 2.31 deg at the turret limit (WI-1's open item; WI-4a 1.87); FADE: 1 blink at the 17876-unit teleport |
| WI-2 zeropose, height, fp pivot, hands, turret zeropose, cutaway | 0.0 deg / 0.0 units; recapture 1.20 -> 1.70 m then 40.00 +- 0.15; clamped 36.25, drift 0.0; fp pivot 0.006-0.011 units (0.43-7.77 with `fppivot 0`); hands, barrel, props frame 0.000 (`recenterhands 0`: 16.7 / 25.5 units, 40 deg); prologue turret 0.005 deg; cutaway scan 0 starts |
| WI-2 head collision | corner: 2 planes, eye held; flat wall: tangential gain 1.000; squadmate 18-30 units ahead: hit by the head trace, 0 pull ticks; passing squadmate: 0 pull ticks (see "found" below); ground mode step 0.934 units max over 0.3 s; real cp01 crate (`ce cp01`): low probes hit the StaticMeshCollectionActor at t 0.65, pull 12.21-12.29, latch 0 toggles, gains 1.000 |
| WI-2 prologue turret drift | fire 1.99 / still 1.21 units in the Pawn frame: the neck-base eye, fixed in the cabin, seen from the turret body frame while the turret turned 12 deg (vertical 0.001); not view motion (step <= 0.08 units) |
| WI-3 | seated S1-S9 11 / 11; crouch suite all PASS (F 9.9-10.7 units); reach identical to fix 2 (rifle / pistol near 0.0); body matrix C1 36/36, C2 34/34, C3 88/88 (0.43), C3e 85/85 (0.87), C5 34/34; neck A/B: the closed neck at 1.85 m / -85 as in fix 2; arms hittest identical (second run; the first run differed at 3 edge heights, with flips inside one mode); menus 12/12 and 10/10; cover L1, M1-M3 PASS, hits under fire: 2 "view kept still", 0 script crouches (the T1 "step < 1" flag tripped on Walker standing up from the phase's own crouch toggle: Pawn +20, view ramp 3.2 / 7.2 / 4.8 / 3.8 units between VIEW rows 88 ms apart) |
| WI-4a | gun frame M4 at level 1 9/9 within 0.057 deg; 5 classes 45/45 within 0.058 deg; B+Y 6/6 (0.000) + 3/3; new-frame hmd M9A1 4/4 (0.026 deg, wrist 0.05 cm); turret yaw hold PASS; menus 7/9 + 7/9 in that chain (two VRMENU lines read late, no AIM line in `mode synth`); the same rows pass in the options check below |
| weapontest per class (tools/weapontest.py) | pistol, rifle, SMG, shotgun: shot vs barrel 0.04-0.075 deg, start 0.1 units off the axis (impact vs marker 32-106 units: weapontest's own marker metric, as in copy A's reports); grenade launcher and RPG: projectile 0.01 / 0.0 deg sideways; SAW not equipped by weapontest's give (it fires in the gun frame and aim suites); M110 did not fire (as in copy A's earlier report) |
| WI-4b | twohand_game2 19/20 (1b: the aim and drawn-hand lines 1 ms apart in one tick, the known same-ms harness test); N1-N6 PASS; R1-R5 PASS; hmd2 5/6 (H5b: "0.0" vs "0" text, known); H6 PASS; zeropose `twohandshift 1` 0.000 deg, engaged and leftover |
| WI-4c, prologue | synth: limits +49.98 / -65.00, refused <= 0.022, 14 shots 0.94 deg from the game aim (the session started near the right limit: settle clamped at 18.5 of 30; the pitch phases cut by the end of the flight); bar +20.29 (palm line 20.79); hmd2: RB / LB kept on NORMAL, gunstock pitch 15, B+Y writes gunangle; hmd3 right / left-handed 20.81 / 20.79, view 0.000; handlebars on the headset path +20.80, twist 20.01, view vs helicopter 0.000; fix2_game 10/10 (after a relaunch; the in-chain run started on the prologue turret and read the old state) and fix2_lb 3/3; menu 11/11 |
| Every new option in each state (`options_check.py`) | 110 / 111 states over 49 keys (7 Basic choices through every value, 3 Advanced words, 39 numbers +1 / -1): VRMENU line, ini write, game alive, no exception; effect seen in the log for seated, fpbody, holdmode, gunangle, stockangle, gunyaw, neckmodel, necksize. FAIL: `necksize` 1 -> 1.10 written, the value stays 1.0 (clamped to 0..1; the menu step has no upper bound) |
| Migration of a small nudge | v9 `aimpitch 0.35` -> `holdmode normal` + `gunangle 0`, no notice |
| 10-level load pass | 10 / 10 loaded one after the other in one session (Desert02, Gorge01, CityGate02, Road, ShipsGraveyard, StealingWater02, Atrium01, RadioMan02, Bridge, BurjDubai): gameplay reached, headset path 6 s each (72 fps), 0 script exceptions, 0 watchdog stalls, game alive |
| Ground turret (orchestrator 20:15) | M2E2 HMG in SP_DollsHouse_P, checkpoint `cp5` (SP_DollsHouse_06_K, the turret battle): no gunner, boarded the normal way (walk in, A) at the first try, twice. `synthaim 30 -5` 29.99 deg in 111 ms; limits +44.99 / -55.00 / +20.97 / -23.01 in the mount (the ground values), refused 0.000; 13 shots 1.22 deg from the game aim; handlebars on the headset path +20.80 (palm line 20.79), twist 20.01, view vs world 0.000; left-handed 20.80; RB / LB kept on NORMAL, gunstock pitch 15; base `YBlockingVolume`, physics 0, base term 0; eye drift 0.002 (fire) / 0.000 (still); A leaves ("TURRET: left, PlayerController pitch reset to 0") |

- **Found, not fixed (not small, or not clearly a regression; in the headset checklist):**
  - **Head turned off the body, looking down (the WI-3 open point).** After a look to the side and back without the
    stick, the head stays exactly 45 degrees off Walker's body (`turnwithhead 45` turns the body only above 45, and
    nothing turns it back: 17 samples over 20 s, at 1.15 m and at 1.64 m). Looking down from there puts the eye 8.5-8.9
    units from the shoulder bone: the shoulder pad and torn vest edges fill the view at -85 from 40 degrees off the body,
    at -60 from 60; at 30 degrees or less it is clean (contact sheets `final\wi3\sheet_shoulder_final_*.jpg`). The same
    at 1.64 m standing: not a seated-only case. Reachable in normal play, for as long as the head stays there.
    Mitigation without code: `turnwithhead 30`. Fix options at the merge: let the body follow the head yaw slowly while it
    is held off (or while looking down), or the render-only body push of the WI-3 note.
  - **Squadmate walking through the head** (`ph_pass`, 30 units ahead): when his capsule crosses the eye, the head
    collision's push flips side in one tick (7.0 units sideways, then released with tau 0.2 s); WI-2 fix 1 measured a
    3.9-unit one-sided push. Comfort item; Basic > Head in walls `off` removes it.
  - **M2E2 limit latch during bursts:** the yaw latch switched on and off 3 times in 1.2 s while firing at +22 deg (the
    recoil taken for the game refusing the push, 0.08-0.19 deg). Harmless headless; a feel item.
  - `necksize` menu step above 1 (the value is clamped, the ini says 1.10).
- **Copy B left** at level 1 CP04 with the final dll, ini byte-identical to its start state (configversion 9,
  `aimpitch -24.0`, `runtime fake openvr 72 45`, `fpscap 72`).

### Merge of the tester fixes (27/10)

Copy B's tester fixes (WI-0 to WI-4c above, tree `SpecOpsVR_B`) were merged into copy A (the Boneworks
physics plan, the level-change crash fix and gameplay items 1-6) on 27/10. There is no repository: the merge is
file by file, and copy A's tree before it is in the scratchpad (`snap_merge_before`: src, tests, docs, deploy,
README, CMakeLists, tools).

**Method.**
- Three-way merge per file with `git merge-file --diff3` (a standalone tool, no repository), base = copy A's
  `src` on 25/10 10:31, when copy B was forked (`scratchpad\B\base_src`).
- Outside `src` there is no copy from the fork. `tests/unit_tests.cpp`, `deploy/specopsvr.ini` and
  `CMakeLists.txt` used copy A's project as of 24/10 22:03 (`scratchpad\p1m\snapproj`): an older base is safe,
  because what changed before the fork is the same on both sides. `README.md` and `docs/TECHNICAL.md` used a
  synthetic base (copy A's lines that copy B's 26/10 00:29 copy of the docs also had). For TECHNICAL.md copy B's
  section "Tester VR fixes" (WI-0 to WI-4c and its final regression) was taken out first and put back as one block
  after "Interactive world"; the rest merged as above.
- Copy B's tree was merged from a copy taken at 01:58, while its final regression still ran, and the result was
  tested from 02:16. Copy B's final round then changed only `docs/TECHNICAL.md` (its final-regression section and
  the WI-4c move), merged at the end; `src`, the tests, the ini and README of copy B's final tree are byte-identical
  to that copy, so every result below is for the final merged code.
- Not in this merge: the copy-B follow-up started at 04:31 (shoulder pose, head push side, M2E2 limit latch,
  `necksize` step). Its start snapshot `scratchpad\B\snap_followup` is byte-identical to what was merged here, so it
  can come in later as a delta (`scratchpad\merge_work\delta_merge.py`, base = that snapshot).
- `tools/`: copy B's `gamectl.py`, `levelsweep.py` and `regress.py` are copy A's plus B's additions
  (`no_foreground()` and the save-folder rule, both inert for copy A's default exe; `g.focus()` before the
  regress gesture phase), so B's versions were taken; `viewcheck.py` is new; copy A's `heapcheck.py` stays.
- Line endings: each file keeps copy A's style; new files keep copy B's (`twohand_math.h`, `vr_fake.*` CRLF,
  `ui_text.h` LF). Copy B had turned `vr_menu.cpp` into LF and `vr_openxr.h` / `vr_runtime.*` into CRLF, and
  copy A's `control.cpp` had become CRLF after the fork (B's is LF): copy A's style was kept in all of these.
- Every file was merged with line endings normalised, and every conflict was resolved by hand.

**Conflicts and how each was resolved** (copy A = A, copy B = B).
- **configversion** (`control.cpp`, `pure_utils.h`, `deploy/specopsvr.ini`). Both trees used `kConfigVersion 10`
  with different changes: A's 10 is `solidhands off` -> `held`; B's 10 was `headcollision on` -> `push`, the WI-3
  list (`seated off` -> auto, `crouchview real` -> auto, `fpbody on/off` -> full/arms), the aimpitch comment lines
  and the computed changes (`aimpitch X` -> `holdmode` + `gunangle` / `stockangle`, `twohand`/`leftgrip` with
  numbers -> on/off). A's stays 10; B's are now 11 (`vr::pure::kTesterFixesConfigVersion`, used by
  `kWi3ConfigChanges`, `kAimpitchCommentChanges`, the headcollision change and `kComputed`), and `kConfigVersion`
  is 11 (a `static_assert` keeps it there). So the user's ini, already at 10 from A, still gets B's changes, and an
  ini at 9 gets both. The shipped ini says `configversion 11`. B's MigrateConfig structure (a vector of fixed
  changes plus the computed list) was kept, with A's `{10, solidhands}` in the fixed list.
- **hooks.cpp, UpdateHeadTracking call in the Draw detour.** A stamps the pose time (`g_poseStampMs`, throw
  velocity); B measures the OpenXR lock wait around the same call (HITCH line). Both kept: B's wait measurement
  around A's block.
- **hooks.cpp, ReleaseArms.** A clears the drawn support hand (`RenderHandNone(1)`, `g_supportHandSepCm`); B clears
  `g_leftOnGun` (WI-4c review 2). Both kept.
- **hooks.cpp, UpdateArms left-hand WriteHand.** A draws the free left hand at the solid-hands pose (`wPos`); B
  adds the `lrel` diagnostic (drawn left minus drawn right). Both kept; `g_diagLeftRel` now uses `wPos`, the hand
  that is actually drawn (identical to B's `lPos` while the hand is on the gun, where solid hands is off).
- **hooks.cpp, MuzzleStart.** A's guard (`StillAlive(g_rig.mesh)`, the level-change crash fix, with its ARMS log
  line) and B's (`MeshOwnedBy(g_rig.mesh, pawn)`, the first draw after a load, and `g_muzzleClipActor = nullptr`)
  are both there, A's first: `MeshOwnedBy` reads the mesh's fields, so it runs only on a live mesh. A's guard in
  UpdateArms and B's `MeshOwnedBy` check after it merged without a conflict. The gun frame of B goes through them:
  the shot start, the marker and the muzzle-clip trace all come from `MuzzleStart`, and every bone query of B's
  code is a `CallFunction`, which skips objects that are no longer in GObjects (A's `LiveObject`).
- **script_call.cpp, CallFunction.** A disables a faulting function per UFunction (`std::set<void*>`, item 2: an
  exception in `YDestructibleObject.TakeDamage` no longer disables `YGamePawn.TakeDamage`) and skips stale objects
  (`LiveObject`); B disables `GetBoneLocation` / `GetBoneName` / `GetBoneQuaternion` per object (`name@ptr:FName:
  Outer` key, cleared by `ClearObjectFaults()` when the Pawn changes). Both kept: per-UFunction set for everything
  else, a separate per-object set for the bone queries (cleared whole by `ClearObjectFaults`, which before erased
  only the `@` keys). The SCRIPT line is B's (object pointer, "on this object").
- **vr_input.cpp, ApplyGestures with no sample.** A zeroes the analog grips, B clears `g_leftHandBusy`: both.
- **pure_utils.h.** Both appended sections at the end of the namespace (A: impact pulses, analog grip, throw,
  melee/shield/glass maths; B: pose angles, gun frame, two hands, turret, body): A's block then B's.
- **tests/unit_tests.cpp.** Union of includes, test functions and `main()` calls. New `TestMergedConfigVersions`
  (the merged table: a v9 ini gets both groups, the user's v10 ini only B's and keeps a `solidhands off` chosen
  after A's 10, a B ini at 10 is not changed again, v11 untouched).
- **deploy/specopsvr.ini.** A's weight / analog grip / solid hands block is a superset of the fork's (B had the
  fork's weight lines): A's taken; B's new keys and comments (holdmode, gunangle, stockangle, gunyaw, two hands,
  stock, seated, crouch view, body, neck, turret grip and limits, predictframe, dynres, ...) merged cleanly.
- **README.md / TECHNICAL.md.** 8 and 9 small conflicts, each between an edit on one side and the fork text on the
  other: the edited side was taken (B: head in walls / room-scale / seated paragraph, turrets, recentering; A:
  throw style, analog grip, solid hands, impact haptics, the gameplay items, wedged objects and the hand
  spheres in the shot path). B's text "configversion 10" in its sections now says 11.
- **Stray formatting edit near hooks.cpp 6602 (`comp =(`)**: already reverted in copy B's final tree; not
  present in the merge.

**Semantic checks** (no textual conflict, read by hand).
- Symbols copy B removed or renamed (`g_aimPitchOffsetDeg`, `ApplyTwoHandAim`, `g_thBlend`, `g_leftGripCm`,
  `g_twoHandSmoothSec`, `g_turretYawUnits`, `TakeLockWaitMs`, the old `HintsTick`): copy A's code did not use them
  (the build proves it).
- Copy A's readers of B-changed state: `GunHandHaptics` (props.cpp) still reads `g_leftOnGun || g_twoHandEngaged`,
  both now set by B's two-hand solver (`TwoHandStep`); the left finger curl reads `g_leftOnGun`, which `ReleaseArms`
  now clears (turret, scene, death). Solid hands moves the drawn left hand only with `gripBlend == 0`, and B's grip
  now engages only when the solver holds the gun, so the hand on the gun is never offset. B's solver gives way to
  copy A's props (`kVetoProp` while the left hand holds an object, `kVetoPropGrip` for a grip in a prop zone), and
  the grenade-gesture guard of item 4 (`g_propHeld[1]` / `g_propReleasedMs[1]`) is in the merged vr_input.cpp.
- Copy A's synthetic-hand test commands (`synthleftvel`, `synthleftramp`, `synthleftjump`, `synthleftrotvel`) sit
  in B's rewritten synth branch before `TwoHandStep`, so the physics and gameplay harnesses drive the same hands.

**configversion on the user's ini.** The first launch of the merged dll on copy A (its ini is the user's, at 10)
logged `CONFIG: specopsvr.ini updated to version 11: "headcollision on" -> "headcollision push"; ... "fpbody on" ->
"fpbody full"; "seated off" -> "seated auto"; "aimpitch -24.0" -> "holdmode gunstock + ... stockangle 15.4"` (the
user's stock calibration kept as the same barrel). The ini stays at 11 (the copy from before the merge is
`scratchpad\merge_work\ini_A_before_merge.ini`); the fake-runtime lines of copy B's suites were only in it for
those runs and were taken out byte for byte.

**Build and unit tests.** Clean build (`--clean-first`): 0 warnings, 0 errors. Unit tests 2815 passed, 0 failed
(copy A had 334 and copy B's final 2535, the tests older than the fork counted once, plus the 11 checks of
`TestMergedConfigVersions`).

**Regression on copy A** (merged dll d413b7ce, copy A's game and ini; one run per suite).

| Suite | Result on the merged dll | Reference |
|---|---|---|
| `tools/regress.py` | PASS: stand / sweep / move / checkpoint / cinema pairing, fire 0.024 deg, reload gesture | PASS before |
| Physics grab suite (`suite.sh merge`) | grasp identical; drive settle 0.058-0.100 s, sweep error 1.7 cm; static grab, glove catch, knock 53 cm, drop and smash break, shot aim within 0.1 deg (radio moved 126 cm), door 61 deg, sign 5.1 deg | item 5 fix run: 1.6 cm, 52 cm, 164 cm, 59.5 / 5.5 deg |
| Hit pipeline (`p4t/hitall.sh merge_hit`) | the same lines as before: throw head hit under 2.5 m/s, swing 2 hits and 0 while pressed or slow, dropped hit 1.9 m/s, regrab trace | item 5 fix run |
| Item 1, thrown radio at the dummy | 5.6 m/s, 80 J, heavy: 40 damage, 75 -> 35, stagger (`HandleMeleeAttack`) | item 4 final: the same |
| Item 2, bottle on the coach pane | 7.9 m/s, 52 J: 40 damage, 25 -> -15, destroyed | item 2 fix: 76 J, destroyed |
| Item 3, radio held in front of the face | 5 of 5 AK shots stopped by the radio, Walker 115 -> 115, knocked out at the 5th away from the head | item 3 final: the same |
| Item 4, radio swung at a new dummy | 4.3 m/s, 65 J: stagger, 20 damage, 75 -> 55 | item 4 final: the same |
| Item 5, carried propane dropped from 6 m | impact 16.2 m/s, EXPLODED, dummy 75 -> 0 | item 5: 16.3 m/s, 75 -> 0 |
| Level change RadioMan02 -> CityGate01 (the 26/10 crash) | alive; copy A's guard logged the stale arm mesh at both loads; no SCRIPT exception | 3 of 3 crashed before A's fix |
| props tick | 0.170-0.199 ms on average | 0.17-0.20 ms in the item runs |

Copy B's key suites on copy A, with copy B's fake runtime (`runtime fake openvr 72 45`, `fpscap 72`) written into
copy A's ini only for these runs (put back byte for byte, `ini restored: identical`) and a god-mode keeper for copy A
(`merge_work\godkeeperA.py`, a copy of B's). Suites copied to `scratchpad\merge_work\bsuites` and pointed at copy A
(no SPECOPS_* variables). Copy B's numbers are its final regression of the same night.

| Suite | Merged, copy A | Copy B final |
|---|---|---|
| WI-1 head (`wi1_suite.py head`) | tilt -30/10 and -20/30 drawn vs headset 0.003 / 0.002 deg; sway jitter step p99 0.68 (openvr) / 0.96 (openxr) deg | 0.003 / 0.002; 1.85 / 1.45 |
| WI-1 predictframe | PASS (auto switches on) | PASS |
| WI-2 hands across a runtime recenter | fix on: 0 hand / aim / grab-frame steps, 0 frame jumps; fix off: the old 40 deg step | identical |
| WI-2 zeropose + height | zeropose view step 0.0 deg, eye step 0; re-capture 1.20 -> 1.70 m after 3 s; clamped 36.25 and free 40.0 with 0 drift | identical (see note 1) |
| WI-4a gun frame, 5 classes at Gorge02, grabprops ON, two hands ON | 45 of 45 poses, 0 clipped, worst first-shot barrel error 0.058 deg, shot vs marker 0.037 deg | 45 of 45 (grabprops off), 7 clipped (shotgun, one pose), 0.058, 0.370 |
| WI-0 aim baseline, headset path, grabprops ON | 30 of 30 poses, 0 clipped, start off the barrel axis at most 0.14 deg | 30 of 30, 0 clipped, up to 5.3 deg off |
| WI-4b two hands in game (`twohand_game2.py`, grabprops ON) | all 19 checks PASS (2b two-handed shots: 0 clipped, shot vs marker 0.003 deg; 9 spread ratio 0.33) | 18 PASS, 1b FAIL (the same 1 ms flake as note 2) |
| WI-4b headset path (`twohand_hmd2.py`) | H2-H5 PASS; H1 and H5b FAIL (see note 2) | H1 PASS, H5b FAIL |
| WI-2 prologue turret | fire and still windows on the turret, step 0.058 / 0.011 deg | 0.076 / 0.020 |
| WI-4c handlebar session | turn +20 -> turret 20.36 deg, settle 139 ms, release and gunstock as B | 20.29 deg, 139 ms |
| Ground turret, M2E2 at SP_DollsHouse_P cp5 (copy B's merge note), boarded the normal way | `synthaim 30 -5` in 111 ms; mount limits as copy B (+45 / -55 / +21 / -23); 13 shots, all on the world (`held 0`, no hand); handlebars on the headset path 20.80, twist 20.01, view against the world 0.000; drift 0.002 units; leaving resets the pitch | the same |
| WI-3 seated S1-S9 | all PASS | PASS |
| WI-3 crouch A-F | all PASS | PASS |
| WI-3 body matrix (no shots) | C1 36/36, C2 36/36, C3 90/90, C3e 87/87, C5 36/36 | 36/36, 34/34, 88/88, 85/85, 34/34 |

Notes. (1) `height rise` `eye_z_first` read 61.25 here and 36.25 on copy B: here `viewlog` and the head rise landed in
the same command poll, so the first logged tick already has the 0.5 m rise on top of the 36.25 eye floor
(`ho z = 25`, the head really stood up); copy B's first tick was before the rise. The re-captured reference and every
later value are the same. Re-run with the first ticks saved (`bs_height2.txt`): 61.25 for the 3 s until the re-capture,
then 40.0, as the rule says. (2) H1 checks that the AIM and ARMS lines carry the same millisecond; they were 1 ms
apart (03:30:37.783 / .784) with the engage itself correct (correction 0.06 deg, palm 0.0 cm from the grip): the same
flake as copy B's WI-4b fix 2 run. H5b compares the ini text `twohandguardright 0` with the menu's `0.0`: copy B's
final fails it the same way (harness).

Note added 27/10 evening: the "handlebars on the headset path" results on the M2E2 (the ground-turret row above and
copy B's final regression) are for the code of that night. Since the headset session the handlebar engages only on
the helicopter minigun, and on a ground turret NORMAL aims with the two-hand pivot (`turretpivot`), so a re-run of
that check on the M2E2 would measure the pivot instead (see "Headset session fixes (27/10 evening)"). The prologue
minigun rows (WI-4c handlebar session) still apply.

**Open (not merge regressions; for the physics owner and the headset).**
- Copy B's merge notes for the physics owner still stand: fold `TraceViewNearest` into `TraceLineView`; a filter
  for moving props on the view traces; keep the crouch-view drop V (`g_crouchViewDrop`, the crouch button now lowers
  the view, and the hands with it, over `crouchtime`) and absorbed-recenter put-backs out of the throw velocity
  (a release during the 0.35 s ramp adds up to ~1.1 m/s downwards); objects held in the right hand turn with B's gun
  frame (39.4 deg about the palm for the old default `aimpitch 0`).
- Copy B's "Found, not fixed" items above are still open on the merged build (none comes from the merge; the copy-B
  follow-up is working on them): the head
  left 45 degrees off the body showing the shoulder pad when looking down (mitigation `turnwithhead 30`), the head
  push flipping side when a squadmate walks through the head, the M2E2 limit latch toggling during bursts, and the
  `necksize` menu step above 1.
- Headset: copy B's `HEADSET_CHECKLIST.md` (item 0.4: the migration line now says version 11), plus two-hand ON
  with the left palm on the handguard and the wrist rolled 30-60 deg on VDXR and Steam Link (no shot may stop on the
  hand: 0 clipped shots in the 75 gun-frame and aim poses here, fake runtime).

### Follow-up of the final regression (27/10)

The four points the final regression left open, fixed on copy B only (novr, fake runtime `openvr 72`, `fpscap 72`, copy A on
the same GPU). Scripts and outputs: `scratchpad\B\followup` (`followup_body.py`, `followup_misc.py`, `ph_pass_fu.py`,
`chainA.sh`, `chainA2.sh`, `chainB.sh`, `c_*.txt`, contact sheets `sheet_followup_fu2_*.jpg`). Final dll `fb50d579` (= `f736a883`, on
which the gun frame and aim suites ran, plus the latch line's wording: "... 3 ticks in a row, the aim 1 deg or more past").

- **1. Walker's body comes back under the head** (`bodyalign`, `bodyalignspeed`, Advanced; `vr::pure::BodyFollow`; hooks.cpp
  `UpdateHeadTracking`, turn-with-head block).
  - Cause (final regression): `turnwithhead` turns the body only by the part of the head yaw beyond 45 degrees, and nothing
    turned it back, so after any look to the side and back the head stayed ~45 degrees off the body for good; looking down
    from there the eye was 8.4-8.9 units from the shoulder bone (shoulder pad and torn vest in view).
  - Change: once the head's yaw has been still (filtered speed under 25 deg/s) for `bodyalign` seconds (0.4), the body turns
    toward the head: it starts at 3 degrees off and stops under 0.5, speed ramped (acceleration `bodyalignspeed` / 0.3 s, no
    jerk), at most `bodyalignspeed` (40 deg/s), offset / 0.35 s near the end, never past the head. Same path as the
    turn-with-head turn: `AddControllerYaw` + the head reference shifted by the same units + `compensatePivot` (eyes kept in
    place), fractional units carried to the next tick. Not in cover, on turrets, in scenes / locked scenes, during special
    moves or stick turns; off with `bodyalign 0` or `turnwithhead 0`. Log: `BODY: turning under the head (...)` and
    `BODY: under the head again (turned X deg in T s, R deg left)`. New key, no default changed: no configversion change
    (existing inis get the two lines from the embedded defaults).
  - Found while measuring it (fixed): with the controllers untracked the view is the game's camera plus the head, and the
    game made that camera with the body before the mod turned it in the same tick, while the head reference had already
    moved: the view led by one tick's turn (0.57-0.66 deg range while the body turned). GetPlayerViewPoint now adds the mod's
    turn since the camera (`g_bodyYawAtCamera`; test switch `meshyaw off`): 0.0165 deg range (= 3 units of rounding).
  - Measured (`followup_body.py`, scripted head on the neck model: 0 -> 90 in 1 s, held 2 s, back in 1 s, then held; A/B in
    one session with `bodyalign 0` = the old behaviour):

| | 1.15 m seated (auto) | 1.64 m standing |
|---|---|---|
| head off the body, 1 / 2 / 3 s after coming back | before -43.4 / -43.4 / -43.4 (for good) -> after -25.3 / -1.7 / -0.5 | -42.4 / -42.4 / -42.4 -> -25.5 / -1.8 / -0.5 |
| head held at 90 for 2 s | body 43 deg behind -> 2.0 deg behind | same |
| eye to the nearer shoulder / upper arm at pitch -85 | 8.7 / 9.3 units -> 9.4 / 13.4 (symmetric, as aligned) | 8.7 / 9.8 -> 9.9 / 13.7 |
| contact sheet (pitch 0 / -15 / -30 / -45 / -60 / -85) | before: shoulder pad and torn vest from -45 down; after: clean, the -85 shot equals the aligned pose of the final regression | same |
| view while the body turns (head still) | yaw range 0.0165 deg, location range 0.09 units, step <= 0.057 units | same (0.0165 deg, 0.084 units) |
| torso criterion C1 (pitch 0 / -15), neck behind the eyes (C2) | pass in both rows; neck 10.3-11.2 units behind the eyes looking down | pass |

  - Gun and two hands (1.64 m, gun hand still, head turned 40 deg and held, the body turning 39.5 deg under it): world aim
    (body + aim: the controller's aim, NOT the shot direction, see review 2 below) range 0.0055 deg one-handed and 0.000 deg two-handed, two-hand engaged in
    every tick. **Open**: the drawn gun (`GUNLOG` barrel axis, new `axis` field) is one tick's body step off while the body
    turns: barrel yaw range 0.62 deg one-handed / 1.69 deg two-handed during the 2-s turn (0.0004 / 0.000 with the body still).
    This is not new to this item: UpdateArms converts the hand targets with the Pawn yaw of the draw and the arm pose is drawn
    one tick later with the next yaw, so every body turn of the mod (turn-with-head, stick, executions) has it. A first fix
    (convert with the yaw before UpdatePawnFacing's write) doubled it (1.32 deg; A/B with `meshyaw off` on the same dll) and was
    reverted; the data say the conversion yaw must be one step AHEAD (the current yaw plus the last step). Left for the merge
    (UpdateArms is a conflict zone with copy A): convert with the predicted yaw, check with the GUNLOG axis.
  - Vignette: (corrected in the review fixes below) every still-head window with `bodyalign 0.4` had `forced` 1.0 (2 ticks
    per room-scale step that the turn caused), at both heights and in both runs; the same windows with `bodyalign 0` had 0.0.
    The earlier wording here ("once in some alignment runs") was wrong. Cause and fix: "Review fixes of the follow-up".
- **2. Squadmate walking through the head** (`vr::pure::HeadPushStep`; hooks.cpp GetPlayerViewPoint head-collision
  smoothing, VIEW `hcside`).
  - Cause: a push that grew was applied at once (never see through a wall), one that shrank eased out (tau 0.1 s). When a
    squadmate's capsule crosses the line from Walker's centre to the eye, the plane normal flips side in one tick; the new
    push was "bigger", so the eye jumped 7.03 units sideways.
  - Change: only a push that grows on the SAME side (within 60 degrees of the applied one: walls, edges, room corners) is
    applied at once. A side change opens a transition that moves toward the new push with tau 0.1 s and at most 40 units/s
    until it is reached (within 0.25 units); without the latched transition the half-way push counted as "same side" after a
    few ticks and the rest arrived at once (4.26 units, unit test). Release unchanged.
  - Measured: unit test with the final regression's values: 7.03 units in one tick -> at most 0.556 units per tick, reached
    in 0.36 s; normal turning 180 deg in 6 ticks: <= 0.556. In game (`ph_pass_fu.py`, Lugo walking across at 80 units/s):
    30 units ahead no head contact this session (hit at t 0.90-0.94); the largest eye step (2.3 / 3.25 units at 26 / 22
    units) is the first contact of the capsule, applied at once as for a wall (by design). Correction (review): the
    "11 / 20 limited side-change ticks" at 26 / 22 units were not side flips. `hcside` also counted the release ticks while the
    transition was latched; the per-tick pushes show a plain decay (26) and a decay with the direction turning (22), not the
    final regression's 7-unit flip. The flip fix rested on the unit test alone; see the review fixes for one real flip.
    Earlier suites on this code: corner 2 planes eye held (gains -0.04 / -0.03), flat wall tangential gain 1.000,
    squadmate in front 18-30 units unchanged (`ph_pawn2`: hit by the head trace, 0 pull ticks), regress PASS.
- **3. M2E2 limit latch toggling during bursts** (`vr::pure::TurretLimitLatch`; hooks.cpp `TurretBook::err`, TurretAimStep).
  - Cause: the latch engaged on 2 ticks with >= 70 % of a step >= 0.1 deg taken back; the recoil of a burst shoves the PC
    0.1-0.3 deg, so with the aim 22 degrees inside the limits it switched on and off 3 times in 1.2 s.
  - Change (hysteresis): it engages only with the aim at least 1 degree beyond the PlayerController (`kMinErr`, a real limit:
    the hand keeps pushing past it) for 3 ticks (`kTicks`), and the aim coming back releases it only from 0.25 degrees on
    (`kReleaseErr`; before: any sign change). A probe step still releases it when the game accepts it.
  - Measured: unit tests: the final regression's recoil sequence x20 -> 0 latches (was 3); a game limit at +40 with the hand
    at +42 +-0.3: 1 latch, 0 releases in 8 s, 18 push ticks at the limit (probes every 0.5 s); release hysteresis.
    In game on the DollsHouse cp5 M2E2 (boarded the normal way with `final\turret_hunt.py`; `wi4c\ground_turret.py fix1:synth`):
    13 shots in bursts at +30 (22 degrees inside the limits): 0 latch lines (final regression: on / off 3 times in 1.2 s),
    0.86 deg from the game aim; latch 0 in every phase; settle 29.99 deg in 111 ms; limits +44.99 / -55.00 / +20.97 / -23.01
    in the mount, refused <= 0.005 deg. Real game limit (one representative case, `followup_misc.py latch`): with the mod's
    right limit opened to 80 and the hand at +120 the game took back the whole 12.84-deg step: the latch engaged once and
    stayed on (0 releases, latch 1 to the end); back at 45: no latch, refused 0.000.
- **4. `necksize` menu step above 1**: `MenuStep` has value limits (`lo` / `hi`, the command's own clamp) and
  `StepConfigNumber` stops at them: `necksize` 0-1, `neckring` / `reachslack` 0-10, `seatedbelow` 0.8-1.7, `seatedstand` 0.1-0.8,
  `crouchtime` 0-1.5, `turnwithhead` 0-170, `bodyalign` 0-5, `bodyalignspeed` 5-90. In game (`vrmenu cycle`): necksize
  1 + -> "1.00" (was 1.10), - -> 0.90, + -> 1.00, + -> 1.00; bodyalign 0.4 -> 0.5 -> 0.4; bodyalignspeed 40 -> 45 -> 40.
- **Unit tests** 2563 / 0 (new: TestBodyAlign, TestHeadPushStep, TestTurretLatchHysteresis, TestMenuStepRanges; the latch
  model and checks updated to the 3-argument `Count`).
- **Earlier suites** (once each): WI-3 seated S1-S9 11 / 11; crouch all PASS (F 9.2-9.95 units first thing after a relaunch;
  6.5 when run after `ph_corner`, which leaves Walker in the checkerboard room); regress PASS (dll 7cbd7163, which differs from
  the final one only in the reverted UpdateArms change, inactive with the body still); WI-2 corner / flat wall / squadmate in
  front as above; on the final dll: WI-4a gun frame 5 classes 45 / 45 within 0.045 deg (final regression 0.058), WI-0 aim
  baseline headset path 30 / 30, per pose within 0.020 deg (first-shot barrel pitch) and 0.027 deg (barrel yaw) of the final
  regression's run (one M4 shot clipped on the palm sphere: known, fixed in copy A by p2d); no script exceptions.
- **Merge notes**: see the follow-up paragraph in `scratchpad\B\BUGS_PLAN.md` (every function and file touched).

#### Review fixes of the follow-up (27/10)

Review of the follow-up: 2 high, 3 medium and 5 low findings (one low truncated in the task text). Copy B only (novr, fake
runtime `openvr 72`, `fpscap 72`, copy A on the same GPU). Scripts and outputs: `scratchpad\B\followup_fix1` (`fix1_game.py`,
`chain.sh`, `chain2.sh`, `c_*.txt`, `look_*.txt`, `fix1_game_LEG.json` / `fix1_game_L.json`). Final dll `01b672b8`. The first
chain ran on `1c8238eb` (= final minus the second vignette-metric step and the `bodyalign` ini comment); every number below says
which dll it comes from.

- **`bodyalign` moved the aim with `aim body` and with untracked controllers (high).** There the game's aim is the
  PlayerController yaw, and BodyFollow turns the body by writing it, so after a glance the aim swung toward the gaze at up
  to 40 deg/s with no stick input (the follow-up's S runs had `body_turn_deg` -42 / +40 with untracked controllers: the aim
  turned 42 degrees too). Fix: BodyFollow runs only when the aim does not follow the body
  (`!(g_aimFollowsBody && !(g_aimHand && g_aimValid))`); a turn in progress stops with
  `BODY: turn under the head stopped (the game aim is the body; ...)`. Measured (`fix1_game.py`, look 90 and back, 1.64 m,
  dll 01b672b8): `aim body`: head 42.3 deg off for good (as before the follow-up), PlayerController yaw range 0.000 deg
  while the head is still, 0 BODY lines; `aim hand` with untracked controllers: 42.5 deg off, PC yaw range 0.000, 0 BODY
  lines; `aim hand` with tracked controllers: 25.4 / 1.72 / 0.49 deg off 1 / 2 / 3 s after the return at 1.64 m, 25.7 / 1.76 /
  0.48 at 1.15 m seated (same as the follow-up). So the shoulder fix needs tracked controllers and `aim hand` (the default);
  with `aim body` the old pose stays (mitigation `turnwithhead 30`).
- **Drawn gun one tick's turn off while the body turns, during steady aiming (high).** BodyFollow now treats a gun being
  aimed like a moving head (`vr::pure::BodyFollow::Step(..., busy)`): two hands engaged, ADS (gun at the eye), or the
  right trigger in the last second. It does not start; a turn already running ramps down within the same acceleration
  (no snap of the drawn gun) and resumes `bodyalign` s after the aiming ends (log `BODY: turn under the head paused, gun
  aimed (...)`). Measured (part G, 1.64 m, head 40 deg off and held, dll 1c8238eb): two-handed with `bodyalign 0.4`: body
  turn 0.0 deg, barrel yaw range 0.000 deg, world aim 0.000 (follow-up: body turned 39.5, barrel 1.69 deg); one-handed,
  gun at the hip (not aimed): body turned 39.5 deg, world aim range 0.0055 deg, drawn barrel 0.70 deg (the known one-tick
  lag; correction in review 2: with `muzzleaim` the shots and the marker follow that barrel, so this was an aim drift, not
  cosmetic; fixed there with the predicted yaw). Unit: busy from the start -> 0 rotation; busy half-way (13.5
  deg left) -> rate 0.00 at the end of a 1-s aim, acceleration within 133 deg/s^2, 0.50 deg left 3.8 s later. The trigger
  rule was not exercised in game (it reads `g_rightTriggerHeld`).
- **Vignette at full strength on every realign (medium): cause found and fixed.** Per-tick VIEW lines through one alignment
  (new fields `forcedold` = the old calculation, `rstep` = room-scale steps so far) show it: while the body turns about the
  eyes, `compensatePivot` grows the head offset (ho -4.78 / -5.70 units), the room-scale rule moves the Pawn under it (pawn
  -2.67 / -4.24 = 5.0 units in one tick, ho down to -1.60 / -1.92, eye unchanged to 0.004 units), and the metric read that as
  base motion twice: in the step's tick (the head offset dropped by S) and in the next one (the game's POV caught up by S).
  The metric's `outLoc` is the game's POV, not the final eye (the eye is written later): so it also counted fast real head
  motion as forced. New metric: game POV without the eye offset, plus this tick's room step (as the view's base, L8), plus
  the head-offset changes the mod made (`g_headShiftWorld`: room steps and `compensatePivot`); the tracked head no longer
  enters. Measured, look 90 and back, still windows (side hold and after the return), dll 01b672b8: `forced` max 0.000 at
  1.64 m and 1.15 m (old calculation in the same ticks: 1.000; 2 room steps per window); whole run including the scripted
  head turns 0.144 / 0.000 (old 1.0). The pawn still moves under the head during a realign (room scale, view unchanged).
- **Head push easing too broad (medium).** `HeadPushStep(..., params, easeSides)`: the side-change easing applies only when the
  head trace hit a Pawn alone (`vr::script::IsA(actor, "Pawn")`, one plane); walls, edges and props grow at once from any side,
  as before the follow-up. An applied push under 0.5 units (`minPush`) counts as none (a released contact's residual no longer
  eases a new wall). In every case the eye stays at most `maxInside` = margin 8 - 2 = 6 units behind the new push (2 units
  outside the surface). Unit (the review's cases): residual 0.01 units then a push on the other side growing at 70 units/s or
  arriving at once at 10: eye behind the push 0.00 (was 9.5 / 9.4, through the surface), as Pawn and as wall; wall at 3 units
  then 6 on the other side: at once; Pawn at 3 then 10 at once on the other side: at most 6.00 behind, 26 eased ticks. The
  final regression's flip stays 0.556 units per tick; the synthetic normal turning 180 deg in 6 ticks now steps up to 1.62
  units per tick (the 6-unit limit). In game (dll 01b672b8, Lugo 26 units ahead): one real side change on a pawn, logged
  `VIEW: head push changes side on a pawn (70 deg, 3.29 -> 4.51 units), eased`, step 0.56 units in that tick; first contact
  2.95 units at once (by design). Walls on dll 1c8238eb (`ph_corner`): corner 2 planes, eye held (gains -0.039 / -0.031),
  flat wall tangential gain 1.000, as in the follow-up.
- **Turret latch held after a slow return (medium).** `TurretLimitLatch::Filter` also releases after `kTicks` (3) ticks in a row
  with the aim not past the PlayerController on the latched side (`err * dir <= 0`). Unit (the review's model: game limit 40,
  hand back to 30 at 3 / 6 / 15 deg/s, then right at 2 / 5 / 10 deg/s): turret at most 0.24 deg behind the hand, 0 runs
  still latched (was 2.5-5 deg); the real limit case still 1 latch, 0 releases; recoil x20 still 0 latches. Not re-run in
  game (pure logic).
- **Lows:**
  - `hcside` counts only ticks with a push on the other side (not the release while the transition is latched), and the
    side change is logged once with its angle (`VIEW: head push changes side on a pawn ...`).
  - Advanced menu ranges: `lo` / `hi` from the command's clamp on every clamped key in the table (vignette, eyesmooth,
    armstretch, outputscale, turnspeed, snapturn, throwboost, grabzone, masskg, xroverscan, predictmax, dynres*,
    twohandgrip / release / dwell / smooth, twohandguardfwd / right / up, stocktol, stocktoldeg, turretbar*, turretground* /
    turretflight*, gunangle, stockangle, gunyaw) besides the follow-up's nine. Unit: vignette 1.0 + -> 1.0, turnspeed 0 - -> 0,
    snapturn 0 - -> 0, armstretch 1.6 + -> 1.60, gunangle 45 + -> 45.0.
  - Untracked-controller view: the mod's own turn since the camera is limited to +-10 deg (was: dropped above 30, a jump) and
    is applied to the location path's body yaw too. Switch renamed `camownyaw` (`meshyaw` still accepted). Measured (dll
    1c8238eb, untracked, turn-with-head 0 -> +120 -> -120 -> 0, 243 deg of head yaw, 262 deg of body turns): view minus head
    yaw range 0.022 deg, largest step 0.020 deg (the WI-1 check on the touched path).
  - BODY lines: a turn interrupted by cover, a turret, a scene, a special move, the stick or the aim rule now logs
    `BODY: turn under the head stopped (<why>; ...)`, not "under the head again".
  - Which dll: item-1 numbers of the follow-up came from 7cbd7163 and 79f339ce (see above); this round's numbers name theirs.
- **Unit tests** 2579 / 0 (new checks in TestBodyAlign, TestHeadPushStep, TestTurretLatchHysteresis, TestMenuStepRanges).
- **regress.py**: on 1c8238eb FAIL (the simulated reload's 4-s window: no log line for 3.35 s after "magazine out", the
  magazine taken at 31.18 s and the window closed at 31.5 s; gesture code untouched); on the final 01b672b8 PASS (max view
  jump 0.0, shot 0.024 deg, magazine inserted, cinema arms back).


#### Review 2 fixes of the follow-up (27/10)

Second review of the follow-up: 2 high, 4 low findings (the last low, on the vignette metric for stick turns, was truncated
in the task text and is not addressed). Copy B only (novr, fake runtime `openvr 72`, `fpscap 72`, copy A on the same GPU:
frame times in the gun runs had a median of 14 ms and a 95th percentile of 73-87 ms). Scripts and outputs:
`scratchpad\B\followup_fix2` (`fix2_game.py`, `chain.sh`, `c_game.txt`, `gun_lead_*.txt`, `wall_*.txt`; the first pass on
dll `96bd9287` in `pass1\`). Final dll `baef2539`.

- **Realign near a wall moved the view (high).** The view's wall pull (GetPlayerViewPoint, `wallPull` / `LowProbeLatch`)
  pulls the eye back along the BODY's forward. BodyFollow turns the body under a still head and `compensatePivot` keeps
  only the unpulled eye in place, so an active pull slid sideways (2 W sin(d/2)) and a body turned toward a near wall or
  crate edge switched the pull on. Fix: the view (left eye) publishes `g_bodyAlignBlock`: bit 1 pull active (> 0.5 units),
  bit 2 low traces latched, bit 4 / 8 the same eye-height (fl + 12) / chin traces (fl + 20, 10 and 22 under the eye) cast
  along the HEAD's yaw (the body's yaw at the end of a realign) with 4 units of margin, only when the head is >= 0.5 deg
  off the body and `bodyalign` is on (1.3 us per trace). Any bit = BodyFollow not allowed (stops at once; the body stays
  off the head as before the follow-up). VIEW line field `ablk`; test switch `bodyalignwall off`. Measured (`fix2_game.py
  W`, ph_corner's flat wall turned to the side, ~12 units to the right of the Pawn centre, head 40 deg toward it and held,
  1.64 m, A/B in one session):

| | `bodyalignwall off` (= before) | `bodyalignwall on` (fix) |
|---|---|---|
| head 40 deg toward the wall | body turned 39.5 deg, wall pull 0 -> 8.25 units, view location range 6.29 units (step <= 0.89) | body turn 0.0, pull 0, view location range 0.000, `ablk` 4 in all 333 ticks, head stays 40 deg off |
| head 40 deg away from it (control) | body turned 39.5, view location range 0.042 | body turned 39.5, view location range 0.046 |

  (A first pass with the wall ~15 units away: pull 3.61, view range 2.75 units before.) Not run: the cp01 crate (chin
  traces, bit 8: same code path as the eye-height probe).
- **Drawn gun one turn step off while the body turns: an aim drift, not cosmetic (high).** With `muzzleaim` (default) the
  shot (`Hooked_CalcWeaponFire`, along `GunBarrel`) and the green marker / camera aim (`UpdateAimMarker`) follow the drawn
  barrel, so the 0.62-0.70 deg was a drift of shots and marker for ~2 s after every still glance (the earlier "world aim
  0.0055 deg" measured the controller, not the shot; wording corrected above and in the checklist). Cause, from per-tick
  GUNLOG lines (pawn yaw, barrel axis, new `lead`): the mod turns the Pawn itself in the Draw (`UpdatePawnFacing`: Pawn yaw
  = PlayerController yaw when they differ by >= 32 units), and the barrel at Draw N+1 is off by exactly the Pawn step of
  Draw N minus the lead used at Draw N (fits every tick, also with a wrong lead). A first fix (convert with the
  PlayerController yaw after this tick's turn) was wrong: 0.97 deg (pass 1). Fix: `UpdatePawnFacing` publishes the step it
  wrote (`g_pawnFacingStep`), and `UpdateArms` converts both hands with the Pawn yaw plus that step ("current yaw + last
  step"; steps over 5 deg are jumps, not used). Test switch `armslead off`. GUNLOG now ends with `lead <units> marker <valid>
  (x, y, z)` (the marker's world point). Measured (`fix2_game.py G`, one-handed, gun hand still, head 40 deg off and held,
  body turning 39 deg under it, A/B in one session):

| | `armslead off` (= before) | `armslead on` (fix) |
|---|---|---|
| barrel yaw range while the body turns (= shot direction with `muzzleaim`) | 0.616 deg (97 ticks over 0.03 deg off) | 0.044 deg (0 ticks over 0.03) |
| green marker point range (wall ~355 units away) | 17.4 units | 1.4 units |
| view (yaw / location range) | 0.000 deg / 0.049 units | 0.000 / 0.031 |

  The lead ranged 40-618 units per Draw (618 = a 3.4-deg step in an 86-ms hitch): the fix holds with long frames too. It
  covers every mod body turn that goes through UpdatePawnFacing (turn-with-head, the realign, mod stick turns); game-made
  Pawn turns (walking, cover) are not led. Two-handed: not re-run (the realign is paused with two hands).
- **Gun aimed: stop at once.** `BodyFollow::Step(..., busy)` now stops the turn in the first aimed tick (rate 0) instead
  of ramping down for up to 0.3 s; it resumes after `bodyalign` s of still head without aiming. Unit: busy half-way (14.1
  deg left) -> 0.0000 deg turned during the 1-s aim, 0.50 deg left 3.8 s later, restart acceleration within 133 deg/s^2.
  The trigger and ADS conditions are still not exercised in game (headset check).
- **Lows.**
  - BODY lines throttled: one line per realign, at its end (`BODY: turned under the head: X deg in T s (head Y deg off at
    the start, R left[; paused by the gun])` or `BODY: turn stopped under the head: ... : <why>`), for the first 20
    realigns of a session; then one summary every 30 s with activity (`BODY: turns under the head in the last 30 s: n (k
    stopped, p paused by the gun), D deg in all, longest ...; held off by a wall or low cover in front for S s.`). The
    start and "paused" lines are gone. Game: 1 line per realign in every run above.
  - `bodyalign` is in the Basic menu (Body under the head: 0 / 0.4 / 1): it changes what the body does after every
    glance. The ini comment says it needs `turnwithhead` above 0 and stops near walls.
  - `aim view` excluded: BodyFollow runs only with `aim hand` and tracked controllers (`g_aimHand && g_aimFollowsBody &&
    g_aimValid`), as the ini and README say. Reason given in the BODY line: `the game aim is not the hand`.
- **Unit tests** 2579 / 0 (TestBodyAlign busy check rewritten for the stop). **regress.py** PASS on the final dll
  `baef2539` (and on the first pass's `96bd9287`). Not re-run (TEST ECONOMY: not this round's code): seated, crouch, WI-2
  corner / squadmate, gun frame and aim suites; the view code touched here only adds traces and a flag next to the wall
  pull, whose own values are unchanged (`wall_off` runs: pull 8.25 as predicted).
- **Merge notes**: `scratchpad\B\BUGS_PLAN.md`, review 2 paragraph.

#### Verification of the follow-up (27/10)

Every earlier suite run once on the follow-up's final dll `baef2539` (the last fix round changed UpdateArms, the wall-pull
reporting and the bodyalign gating and had not re-run them). Copy B only, novr, fake runtime `openvr 72`, `fpscap 72`, copy A
on the same GPU. Scripts and outputs: `scratchpad\B\verify_followup` (`chain.sh`, `chain2.sh`, `chain3.sh`, `twturn.py`,
`c_*.txt`).

| Suite | This run (baef2539) | Follow-up | Final regression |
|---|---|---|---|
| WI-3 seated S1-S9 | 11/11 | 11/11 | 11/11 |
| WI-3 crouch (first after the relaunch) | all PASS, F 9.15-10.0 | all PASS, F 9.18-9.95 | all PASS, F 9.92-10.74 |
| WI-2 corner / flat wall | 2 planes, eye held, gains -0.036/-0.028; tangential 1.000 | -0.039/-0.031; 1.000 | -0.038/-0.031; 1.000 |
| WI-2 squadmate 30/26/22 (`ph_pass_fu`) | 0 pull ticks; first contact 2.96 / 2.50 / 5.01 units; one side change eased (d22, 65 deg); one NOT eased (d26, 5.14 units, see below) | first contact 2.3-3.25; eased change 0.56/tick | flip 7.04 units (d30) |
| WI-4a gun frame, 5 classes | 45/45, worst 0.058 deg, every pose within 0.029 of the follow-up | 45/45, 0.045 | 45/45, 0.058 |
| WI-0 aim baseline `--hmd` | 30/30; first-shot barrel pitch within 0.02, yaw within 0.059 of the follow-up | 30/30 | 30/30 |
| WI-4b two-hand, game path | 20/20 (1b passed this time) | - | 19/20 (1b, 1-ms harness check) |
| WI-4b two-hand, headset path | 5/6 (H5b, the known "0.0" text check) | - | 5/6 (H5b) |
| WI-1 turret flick (prologue) | view vs helicopter 0.000, steps 0, turret 30.01 deg, refused 0.0 | - | 0.000, 31.37 deg, refused 0.0 |

Aim notes: M4 `aimpitch -24 roll 0` had 3 shots starting on a StaticMeshActor (level geometry at the muzzle, start off axis
0.04-0.06 deg: the direction is right); the informative SAW "legacy hand pose" did not run (the game had switched to the
sawed-off shotgun). Fade phase (not part of this check): the prologue load jump held black 2.12 s "on a vehicle".

- **Two-handed hold during a turn-with-head turn** (`twturn.py`, new; head 0 -> 90 deg in 1 s, body turns ~47 deg with it,
  hands still in the tracking space, left palm on the handguard, 1.64 m, A/B `armslead` in one session): barrel yaw range
  5.71 deg with `armslead off` (= before review 2; one to three steps behind on almost every turning tick) and 1.87 deg with
  `armslead on`. With the lead the barrel stays within 0.04 deg on most ticks, but about 1 tick in 6 is still one step
  (1.0-1.8 deg) off for a single tick, then back. The same single-tick spikes show one-handed during the turn and during the
  realign after it (0.55 deg = one realign step), so they are a lead mismatch on some frames, not a two-hand fault. Cause
  not found (the game's mesh update is sometimes two steps behind the Pawn yaw instead of one). The barrel does not stay
  put; this is open for the merge (with `muzzleaim` the shots follow the barrel in those ticks). **Fixed in f3 (next
  subsection): the spikes were KeepGoodBones restoring the previous tick's bones.**
- **Fixed: a long tick gave one large realign step.** In a natural 209-ms hitch during a one-handed realign, BodyFollow
  turned the body 8.8 deg in one tick (40 deg/s x 0.209 s). That is over the hand lead's 5-deg cap, so the lead was 0 and the
  drawn barrel (= shots and marker with `muzzleaim`) was 8.8-9.5 deg off for 3 ticks. `BodyFollow::Step` now uses at most
  `kMaxStepDt` (0.05 s) for the ramp and the step: a long tick turns at most 2 deg (the view does not depend on this step;
  the realign just takes longer through a hitch). Unit: one 209-ms tick at full speed -> 2.00 deg (8.4 before). Game (dll
  `edab0d16`, `fake hitch 200 0.7`, frames 38-92 ms and one of 458 ms): realign steps <= 2.00 deg, the lead applied on every
  step (<= 364 units), barrel within 0.02 deg except two single-tick spikes (0.66 / 1.32 deg, the spikes above).
- **Open: turn-with-head steps in long frames.** The turn-with-head step is limited to 180 deg/s x dt (dt < 0.1 s), so in
  the same hitch run it turned 6.1 and 8.7 deg in 59-85-ms frames; over the 5-deg cap the lead is 0 and the barrel was 6.1 /
  8.7 deg off for 2 ticks. Not changed here (limiting it lets the head go further past the threshold; raising the cap would
  also lead the game's own yaw snaps). Merge / headset decision. **Fixed in f3 (next subsection): the lead is gone, the
  bone update is exact for any step.**
- **Open: a squadmate side flip with two planes is not eased.** At d26 the head trace's control trace found a second plane
  on Lugo's capsule (`planes=2`), so `tickHitPawn` (`planes == 1 && Pawn`) was false and the side change (89 deg) was
  applied at once: 5.14 units in one tick (final regression 7.04). The follow-up's run had the change on one plane (eased).
  Possible fix at the merge: pass the hit actor of the control traces and ease when every plane is on the same Pawn.
  **Fixed in f3 (next subsection).**
- Unit tests 2580 / 0. Copy B is left at level 1 with `edab0d16` (= `baef2539` + the BodyFollow step limit); its ini is
  the same as at the start of this run.

#### Open points of the verification fixed (f3, 27/10)

The three points the verification left open. Copy B only (novr, fake runtime `openvr 72`, `fpscap 72` unless stated, copy A
on the same GPU). Scripts and outputs: `scratchpad\B\f3` (`twturn.py` = the verification's script with `TW_TAG` /
`TW_FPSCAP`, `spikes.py` (spikes per tick from GUNLOG + SKELDIAG), `passana.py` (head-push side changes), `chain_diag.sh`,
`chain_final.sh`; first pass on dll `a4e5884a` in `pass1\`). Final dll `31aeeaab` (rebuilt after comment-only edits as
`c74fb086`: 4 bytes of the PE header differ). With `muzzleaim` (the default) shots and the green marker follow the drawn
barrel, so every barrel number below is an aim number.

- **Cause of the single-tick barrel spikes (measured).** New diagnostic line `SKELDIAG` after each GUNLOG (only with
  `gunlog`): the drawn mesh yaw, the yaw UpdateArms converted with, the mesh yaw at that moment, the UpdatePawnFacing and
  UpdateArms calls since the last line, and every UpdateSkelPose call on Walker's mesh (return address, mesh LocalToWorld
  yaw before / after, Pawn yaw, the LocalToWorld correction, KeepGoodBones restores and their turn). On the review-2 dll
  (lead on) the bookkeeping is the same on every tick, spike or not: one UpdateSkelPose per tick, always from the mesh's
  own tick (`FUN_00D35E30`, call at 0x00D36121), one UpdatePawnFacing and one UpdateArms per Draw, no eye-order effect.
  The game composes the bones with the mesh LocalToWorld of the tick BEFORE (the LocalToWorld is updated after
  UpdateSkelPose, from the Pawn rotation of now), so an actor-space IK target comes out one Pawn step ahead: that is what
  the review-2 lead compensated. The spikes are the ticks in which `KeepGoodBones` (SKELFIX, 19/10: in about 1 tick in 5
  the game's animation tree ignores the IK and the hand lands ~30 units off) puts back the previous tick's good bones:
  they are in the mesh's space, which has turned one step with the Pawn since, so the barrel is one step ahead for that
  tick (diag run, two-handed, tick 83: lead 1.91 deg applied as usual, barrel +1.890 deg = the previous step). In a still
  pose the restore is invisible, which is why it never showed before. Long frames add the second cause: the lead was 0
  above 5 deg.
- **Fix (no lead any more).** (a) `UpdateSkelPoseThunk`: before the game composes Walker's bones, the mesh LocalToWorld is
  replaced by the one the game computes right after it (`MeshL2WSnap`: the LocalToWorld in the actor's frame, taken at the
  start of the Draw before the mod turns or moves the Pawn and valid only when two snapshots in a row agree (axes within
  ~0.1 deg, origin within 0.5 units net of the mesh Translation); then the actor of now and the Translation of now), and it
  is put back after the call (after KeepGoodBones, MeasureGunReach and PoseFingers, which then see the true pose). Not
  applied when equal, or when the change would be over 60 deg / 100 units; a game pose that turns the mesh against the
  actor makes the snapshots disagree, and then nothing is changed (measured: a TakeHit reaction, corrections 0.005 deg).
  The actor-space targets then come out exactly, for any step size and whoever turned the Pawn. UpdateArms converts with
  the Pawn yaw of now (lead 0); GUNLOG `lead` is now the yaw correction of the last bone update (units). (b)
  `KeepGoodBones`: the restored bones are turned about the Pawn's vertical axis by the change of the hand frame relative
  to the Pawn since those bones (`g_armsFrameYaw` = g_bodyYaw - g_trackRefYaw - conversion yaw, saved with every good
  update): a mod turn under a still head (turn-with-head, realign, cover compensation) turns them back, a stick turn (the
  tracking space turns with the body) not at all. All SpaceBases, and the root of LocalAtoms (the others are relative).
  Math in `vr::pure::ue3m` (UE3 row matrices and FQuat). `armslead off` = neither (test switch, as before). Unit
  `TestUe3BoneMath`: quaternion round trip 6e-8, predicted LocalToWorld 1.5e-5 (relative taken at 30 deg, actor now at 39
  deg and moved, Translation changed), bone turned 7 deg about the Pawn 3.6e-5.
- **Measured, item 1** (`twturn.py`, head 0 -> 90 deg in 1 s, 1.64 m; spike = a GUNLOG tick with the barrel more than 0.1
  deg off its still value):

| | review 2 (verification / diag run of this session) | f3 (final dll) |
|---|---|---|
| two-handed, turn-with-head (body ~47 deg) | barrel range 1.87 / 1.93 deg, 7 / 1 spikes up to 1.82 / 1.89 deg | 0.055 deg, 0 spikes; 72 restores (5 turned, up to 1.86 deg), barrel within 0.037 deg on them |
| one-handed, turn-with-head + realign (body ~88 deg) | 1.54 (during the turn) / 1.34 deg, 23 / 22 spikes up to 1.32 deg | 0.056 deg, 0 spikes; 34 restores (23 turned, up to 1.32 deg), within 0.027 deg on them |

  LocalToWorld corrected on every turning tick (32 / 131 ticks, up to 2.07 deg). First pass (`a4e5884a`, same code but
  for the snapshot rule): 0.056 / 0.073 deg, 0 spikes.
- **Item 2: long-frame steps. Chosen: the bone update covers any step; the turn-with-head rate is unchanged.** Bounding the
  turn-with-head step per tick would let the head go further past the threshold in exactly the frames in which the
  player is already turning fast, while (a) makes every step exact (a game-made Pawn turn too, which the 5-deg cap was
  meant to keep out). `fake hitch` does not lengthen the game's ticks here (the fake compositor's wait runs on the VR
  output thread: with `fake hitch 200 0.7` the ticks stayed under 32 ms), so the long frames are real ones from `fpscap
  14` (ticks up to 85 ms, as the 59-85 ms of the report), same session:

| `fpscap 14`, body steps up to 9.9-10.6 deg | `armslead off` (= the review-2 lead over its 5-deg cap) | f3 |
|---|---|---|
| one-handed, turn-with-head + realign | barrel range 9.91 deg, 32 ticks over 0.1 deg | 0.044 deg, 0 ticks (correction up to 9.93 deg) |
| two-handed, turn-with-head | (not run) | 0.022 deg, 0 ticks (correction up to 10.6 deg) |

  `BodyFollow::kMaxStepDt` (verification) is kept: the realign still turns at most 2 deg per tick.
- **Item 3: squadmate side change on several capsule planes.** The control traces of the head collision now return their
  hit actor; the side-change easing applies when every plane used in the tick comes from a Pawn (`allPawn`: the first hit
  and each added plane; 2-entry IsA cache), so two or three planes on a squadmate's capsule ease, while any wall, object or
  BSP plane among them (null or non-Pawn actor) keeps the at-once rule. New VIEW field `ppawn`. ph_pass_fu 30 / 26 / 22
  on the final dll: every side change eased (largest step in a side-change tick 0.555-0.560 units: 40 units/s at 72 Hz),
  but every side-change tick of that pass had ONE plane (`passana`: SIDE ... planes [1]), so the target case was not
  exercised there; the 2-3-plane ticks on Lugo were same-side ticks, now `ppawn=1`. (Correction, review of f3.) The
  multi-plane side change was seen once, on the intermediate dll `a4e5884a` (first pass, same head-collision code): the
  d26 side change on 2 planes eased (0.547 units in that tick, was 5.14 at once), d30 on 3 planes (0.386). Its raw VIEW
  logs were overwritten by the final run (same `f3` tag); only summaries without per-tick planes / `ppawn` remain
  (`f3\pass1\c_pass.txt`). From f4 on every pass has its own output tag. First contact and same-side growth still at once
  by design (2.7-6.7 units).
  Walls (`ph_corner`, after the pass): corner 2 planes, eye held, gains -0.038 / -0.031, flat wall tangential 1.000
  (follow-up -0.039 / -0.031, 1.000).
- **Suites on the final dll (once).** WI-3 crouch PASS (F 9.13-10.0) and seated 11/11; WI-4b two-hand game 19/20 (1b, the
  known harness check: AIM and ARMS lines 1 ms apart, as in the final regression); WI-4a gun frame 45/45, worst 0.132 deg
  (one UMP45 pose, `gunangle 0 roll 60`: one of its two bursts started 0.24 deg off the other; that class run again: 9/9,
  the same pose 0.008 deg, worst 0.045); WI-0 aim `--hmd` 30/30, first-shot barrel pitch within 0.024 and yaw within
  0.059 deg of the verification; regress.py PASS (max view jump 0.0, magazine inserted). (A first chain had run
  ph_corner before the two-hand suite: Walker then stood against the corner wall and 1c / 2b failed on the wall push; in
  the verification's order they pass. A stale test process of that aborted chain also restarted the game once; those runs
  are discarded, `f3\aborted`.)
- **Unit tests** 2581 / 0 (TestUe3BoneMath new).

#### Review fixes of f3 (f4, 27/10)

Review of f3: 2 medium, 4 low findings (the last low, on process, was truncated in the task text; this round ran one
build, one launch and one pass, plus a diagnostic/guard-only rebuild, see the end). Copy B only (novr, fake runtime
`openvr 72`, `fpscap 72`, copy A on the same GPU). Scripts and outputs: `scratchpad\B\f4` (`twturn.py` = f3's,
`spikes4.py`, `chain_f4.sh`, `twturn_on_one_f4.txt`, `c_spikes.txt`, `c_regress.txt`). Game pass on dll `60a0855b`,
final dll `8cd390a4`.

- **The restored bones were corrected for yaw only (medium).** f3 turned KeepGoodBones' restored bones about the Pawn's
  vertical axis by the hand frame's yaw change. Between the good tick and the restore two other things move the mesh
  against the hands: the mesh Translation (the neck anchor moves it every Draw as the body turns about the eyes) and the
  Pawn Location (room-scale steps). f3's own log (`twturn on one`, view fixed): muzzle (= shot origin with `muzzleaim`)
  0.65 units off on a rotated restore, 2.8 units on restores after a room step, 5.0 units on the room-step frames
  themselves; `barrel_origin_range_units` 6.97. Fix (`vr::pure::ue3m::RestoreCorrection`, `TransformBoneAtom`):
  - KeepGoodBones saves, with the good bones, the LocalToWorld they were composed with (the predicted one when f3's
    correction applied) and the hand frame in the world (`g_armsFrameW`: tracking origin = centre + Rz(bodyYaw) x
    `g_trackOriginOff`, yaw = bodyYaw - `g_trackRefYaw`, as HandTarget and props' SetHandFrame).
  - On a restore the bones get the full rigid correction M = L2W_good * D * L2W_now^-1 (row vectors; D = the world
    move of the hand frame since then: a stick turn turns it, a room step or a turn under the head does not).
  - The room-step frame: MoveSmooth updates the mesh, its physics bodies and the attached gun at once, with bones
    composed for the old location. `RoomStepMove` now shifts all of Walker's SpaceBases by -step (component space)
    before MoveSmooth (and back by the part the Pawn did not move): for that frame the whole skeleton stays where it was
    in the world, the next tick composes it at the new place. The body's 5-unit step shows one frame later; the view
    does not move either way. Only with the mod's arms active; `armslead off` = as before (test switch).
- **The whole body turned on a restore (medium).** f3 turned ALL SpaceBases (and the root LocalAtom): on every rotated
  restore the drawn body (trunk, legs, gear) stood at the previous tick's yaw, then caught up (f3 logs: 23 / 34 restores
  rotated one-handed, up to 1.32 deg; 5 / 72 two-handed, up to 1.86; up to 3.86 at `fpscap 14`). Now the correction
  applies only to the arm subtrees: bones under `Shoulder_*` / `Arm_*`, plus `Weapon_*` / `IK_Hand_*` outside them
  (skeleton parents from RefSkeleton; on Walker 54 of 126 bones: the subtrees of Shoulder_Left, Shoulder_Right and
  IK_Hand_Root, logged once per mesh as `SKELFIX: restored bones corrected on ...`). The trunk keeps the good bones in
  mesh space, so it moves with the mesh. LocalAtoms are no longer touched.
- **Measured** (`twturn.py on one`: head 0 -> 90 deg in 1 s, body 89 deg with it and back under it, gun hand still in
  the tracking space, 1.64 m; f3 = its final run's log `f3\twturn_on_one_fix.txt` read with the same `spikes4.py`):

| | f3 | f4 |
|---|---|---|
| muzzle position (world, view still): max off / ticks over 0.3 units / over 1 unit | 5.07 units / 5 / 4 | 0.057 / 0 / 0 |
| muzzle on the 2 room-step frames | 5.02 / 5.07 units | 0.014 / 0.014 (bones shifted in both) |
| muzzle on restore ticks (max) | 5.07 units | 0.050 (27 restores, all corrected; the correction moved the hand up to 4.80 units) |
| `barrel_origin_range_units` (twturn) | 6.97 | 0.07 |
| barrel yaw: max off / ticks over 0.1 deg | 0.040 deg / 0 | 0.037 / 0 |
| trunk (Spine3 yaw relative to the mesh) on restore ticks | turned with the restore, up to 1.32 deg (by construction) | 0.000 change (moves with the mesh); the animation's own change is held for the tick: the next tick catches up 0.26-0.73 deg (0.73 after 3 restores in a row; normal ticks <= 0.19) |
| shoulder seam (arm root moved against the trunk) on restore ticks | - (whole body moved) | <= 0.46 units while turning; once 4.97 units, see open |
| turning ticks with an invalid LocalToWorld snapshot | 0 | 0 (153 turning ticks) |

- **Open: the shoulder seam on a restore right after a room step.** When the game's bad tick falls right after a room
  step, the good bones predate the step: the arms go back to the hands (correct for the gun) and the trunk stays with
  the moved Pawn, so the arms are off the shoulders by the step for that one tick (headless once in the run: 4.97 units,
  about 10 cm). Possible fix at the merge: translate the trunk by the mean shoulder displacement of M in that case (the
  body would then show its room step one more tick later, as on the room-step frame). Headset check A3.3 (6).
- **Patch gating (low).** The LocalToWorld patch in UpdateSkelPoseThunk now also requires `PawnAlive` and `MeshOwnedBy`,
  as KeepGoodBones, MeasureGunReach and PoseFingers (no patched prediction on the first ragdoll tick after a death).
- **Snapshot rejected while turning (low).** UpdateSkelPoseThunk counts ticks with a mod turn (`g_pawnFacingStep` != 0)
  and an invalid snapshot; every 30 s with any, `SKELFIX: bone update not corrected in N of M turning ticks in the last
  30 s ...`. In those ticks UpdateArms falls back to the review-2 lead (Pawn yaw + this Draw's step, not above 5 deg),
  so a game pose that moves the mesh against the actor leaves the gun as in review 2, not one step behind. SKELDIAG
  `snap` shows the snapshot state per tick. Not exercised in game (0 invalid turning ticks in the run).
- **Item 3 of f3 (low, docs):** the multi-plane squadmate case was seen once, on the intermediate dll, logs not kept;
  wording corrected in the f3 subsection above and in the checklist.
- **Diagnostics.** SKELDIAG gains `hmove` (how far the correction moved the gun-hand bone, units), `seam` (largest
  displacement of a `Shoulder_*` subtree root), `trunk` (Spine3 yaw relative to the mesh), `snap` and `rshift` (room-step
  shifts); `rot` is now the correction's rotation angle (unsigned). `spikes4.py` counts muzzle position spikes.
- **Rebuild after the pass (no behaviour change in the pass).** The pass showed that the first `rot` field measured the
  angle about the component's Z axis, which is not the vertical (Walker's mesh space is X forward, -Y up), so it read
  0.000 and the 60-deg sanity limit of the correction used that angle (the 100-unit hand limit was right). The final
  dll uses the rotation angle of M (`XfAngle`) for both, and `seam` counts only the `Shoulder_*` roots (the pass's
  numbers above included IK_Hand_Root; on the room-step restore both are the same translation). In the pass every
  restore was far inside both limits (27 / 27 corrected, hand moved <= 4.80 units), so the pass stands for the final dll.
- **Unit tests** 2581 / 0 (`TestUe3BoneMath` rewritten: a bone composed with one LocalToWorld and hand frame, restored
  with the mesh turned 9 deg, moved and its Translation changed, frame still or turned 7 deg and moved, scale 1 / 1.15:
  back in the hand frame within 2e-5; the correction's angle with the frame still = the mesh turn, 9.00 deg).
  **regress.py** PASS on `60a0855b` (max view jump 0.0, shot 0.024 deg, magazine inserted, cinema arms back). Final dll
  `8cd390a4` (the rebuild above); copy B's game folder keeps `60a0855b`, the dll of the pass. No other suite re-run
  (TEST ECONOMY: the change is in the restore path and the room-step frame, both exercised by `twturn on one`).

## Tester fixes (0.9.11)

From the first tester reports on 0.9.9/0.9.10 (Quest 3, Virtual Desktop, 120 Hz):

- Auto resolution keeps the per-eye aspect when SteamVR asks for more than
  4096 (the back buffer became 4096x4096 square, window clipped to 4067).
- Wall pull also traces 10 and 22 units under the eye: behind a low crate
  (chapter 1, cp01) the view sat over the lid.
- Full-screen canvas effects (blood, flashes) use the HUD's per-eye mapping at
  scale 1 and infinite distance: before, identical pixels in both eyes had a
  large disparity with the off-centre projection ("cross-eyed", blood
  covering the aim). Headset check pending.
- Defaults: `reload simple`, `hud 0.4 100` (configversion 8); `commandaim`
  retired: squad orders follow the head only.
- Squad orders: the class default YCheapShooter is skipped (it sat at the map
  origin and was always the "closest cheap shooter"); 10 degree tolerance on
  cheap shooters; the game's control restrictions (PlayerController+0xC30,
  YS_ControlRestrictions, bit set = forbidden; m_squadControl 0x8000,
  m_squadCommandAttack 0x200000) give a toast when orders are forbidden and
  leave the target to the game when only attack is forbidden.
- Window activated without keyboard focus: focus taken (WM_ACTIVATE minimized
  flag dropped + periodic check).
- Body moves away from the gaze from 35 degrees of head turn.
- Toast overlay recreated after SetOverlayRaw error 23.
- Open: the order before the banner sniper (restrictions now logged on each
  press) and the first downed enemy after the container in chapter 1 (B logs
  the pawns within 4 m when no downed enemy is recognised).

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
  (WI-4c review round 2: they run on foot with the arms driven, wait on
  turrets, where a turret hint shows instead, and `hints on` takes effect
  in the running session.)
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
- **Updates (R-R4):** the boot sentinel (rollback after two early deaths,
  version skipped while `specopsvr_skip.txt` existed) was removed on 24/10
  together with `autoupdate off` and the line `rollback.bat` wrote into the
  ini: updates are always on, the users who crash are the ones who need them.
  A bad release is withdrawn for everyone through `updates.json`; the old
  sentinel files are deleted at start-up. Companion files: `package.py` writes
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
  `off`; players who want it turn it back on; configversion 9, 22/10, turned it
  back **on** by default at the user's request, `marker off` in the menu) and a new **`laser on`** draws
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
  show|hide` from commands, and `vrmenu cycle <key> [+1|-1]` for tests (the
  same change as a click on that item, logged as `VRMENU: test cycle`);
  while open the game pad is neutral. A Virtual
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
- **Executions (22/10, corrected the same day):** `YPlayerController.CheckExecute`
  (script, bytecode disassembled) returns false when
  `m_playerCam.Rotation.Pitch > m_executionMaxCamPitchAngle` (+0xB90, -1000 =
  -5.5 degrees): the game executes only when looking down. A first fix wrote
  -16384 (-90 degrees), which made executions practically impossible (only
  punches, tester report); the assist now writes INT_MAX (no pitch condition).
  `YGamePawn.CheckExecute` also refuses in cover, on a turret or during a
  special move, and needs a target in the detection box with
  `m_deathDBNOState` 1..3, `m_canBeExecutedInDBNO` on its PRI, or a melee
  take-hit move that is executable ("Meleed enemies will stand up again").
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
  when looking down past 30 degrees); a physical crouch lowers the mesh
  (both heights do not show: see "Mesh height" below).
  `height auto`: eye height = standing HMD height x posscale - 45 (25..55);
  `seated on` uses `fp`. *(WI-3, 26/10: the mesh raise was dead code and is gone;
  the default is now `crouchview auto`, seated play uses Walker's eye height 36.25:
  see "WI-3: body, seated play, crouch".)*
- `physicalcrouch on` (23/10): crouching 30 cm below the standing height
  (back up above 20 cm) calls `YAnimtreeManager.PlayPosture(YPTID_Crouch=2)`
  (`YGamePawn.m_animtreeManager` +0xB30, `m_currentPosture` +0x98, Stand = 1):
  Walker plays the game's own `Crc_*` crouch set (hips -23, head -28 units,
  feet on the ground) and keeps it through fire, reload, aim, sprint, weapon
  swap and grenades; the game camera and capsule do not change. Before, the
  view went down while Walker's skeleton stayed standing.
  If the game re-crouches Walker within 2 s of being stood up, it is left
  alone until the next real crouch.
  The crouched pose brings the head 14 units forward (and 29 down) in mesh
  space (X = actor forward, -Y = up, Z = right; measured on the head bone in
  component space, so no feedback with the mesh translation): with head and
  neck hidden, the neck stump ended up in front of the eyes ("decapitated
  body", user 23/10). The mesh now moves back by the head's forward shift
  measured each tick against the standing average (at most 80 units/s, so
  entering cover does not snap): the neck stays 4 units behind the eyes as
  when standing.
  Engine crouch (`bCanCrouch` + `bWantsToCrouch`, +0x228 bits 0x20/0x4) only
  shrinks the capsule 45 -> 25, the skeleton stays standing: not used.
  24/10 (user: "you look crouched but the game doesn't think so, low
  passages stay blocked"): the real crouch now calls the game's own
  `YPlayerController.GamePad_ToggleCrouch` (what the crouch button does), which
  flips `m_wantsToCrouch` (PC +0x5E4 bit 0x20000) and gives the whole game
  crouch: `bIsCrouched`, capsule 45 -> 25, `YGamePawn.m_crouched`, posture 2
  and crouch speed. Standing up toggles it back only if the mod crouched him;
  a crouch from the button or a script is left alone until you crouch and
  stand for real. If the game ignores the toggle there (0.3 s), the old
  posture-only crouch is the fallback. The view is unchanged: `crouchview
  real` already removes the game's 20-unit drop. *(WI-3: a real crouch no longer
  takes over a button crouch; with `crouchview auto` the button lowers the view.)*
  Hits: instant-hit traces hit Walker's mesh through its physics asset
  (`CH_MC_SK_PHYS_new`, mesh `BlockZeroExtent`); the capsule only blocks
  extents (`BlockNonZeroExtent`). `hittest`, standing: hit from -10 to +28
  units above the centre; crouched 45 cm with the hands down: only at 0 and
  below, +5..+28 passes over him (hands held up: the forearms are hit). So a
  real crouch behind a low wall protects like the game's cover.
  *Needs a headset check: body seen from above while crouching, low cover,
  crouch then duck-into-cover.*
- Mesh height (measured 23/10): `YGamePawn`'s mesh translation system (floor
  traces, smoothing, `m_meshTranslationOffset` +0x1574) rewrites
  `Translation.Z` every tick before `LocalToWorld` is built, so the heights
  the mod writes to `Translation.Z` (`crouchview real` raise in cover, mesh
  lowered with a real crouch) do not show; X/Y do. Writing
  `m_meshTranslationOffset` moves the mesh at once (-10 -> `LocalToWorld`
  -51 -> -61, within one tick, no smoothing).
- Eyes out of the body (23/10, user: kneeling or crouching too low showed the
  inside of the body): with head and neck hidden, the eyes reached the neck
  and the chest when lowering the head before Walker crouches (at 30 cm the
  head bone was 2 units above the eyes) or going below his crouch pose (90 cm:
  3.5 above). `UpdateMeshLowering` keeps Walker's head bone at least 10 units
  below the eyes (or as far as when standing upright, if less: short
  players) by lowering the body through `m_meshTranslationOffset`, owning
  only its own delta (a value rewritten by the game becomes the new base) and
  at most 120 units/s; gap = eye height above the Pawn (`fp` +
  `ViewHeightExtra` + head offset) minus head bone height (`LocalToWorld` + our
  delta + head in mesh space). Measured: 10-30 cm before the crouch pose
  lowers 2-12 units, 70/90 cm crouched 4/13, upright 0. The game's foot IK
  keeps the feet on the floor (ankles unchanged, legs bend). Not in cover, on
  mounts, in animated moves, never on a dead Walker; restored when leaving
  first person.
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
  (Stale since 22/10, when the offset started to decay; replaced by WI-4b, see
  "WI-4b: two-handed grip".)
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

## OpenXR backend

The mod was written against OpenVR. Instead of rewriting every call site,
`vr_openxr.cpp` implements the OpenVR interfaces the mod actually uses
(`IVRSystem`, `IVRCompositor`, `IVROverlay`, `IVRInput`, plus stubs for
`IVRRenderModels`, `IVRSettings`, `IVRApplications`) on top of an OpenXR
session; `vr_runtime.cpp` hands out either the real OpenVR interfaces or these
shims (`vr::rt::System()`, `vr::rt::Compositor()`, ...). The game logic, the
output worker, the overlays and the gesture code are unchanged.
`tools/gen_openvr_stubs.py` regenerates `src/xr_shim_ifaces.h` from
`vendor/openvr/headers/openvr.h` (every pure virtual gets an override; the
ones listed in the script are implemented by hand, the rest return defaults).

- **Runtime loading**: no `openxr_loader.dll` is shipped. The backend reads
  the active runtime json (`XR_RUNTIME_JSON`, else
  `HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime`, which a 32-bit process sees
  through `WOW6432Node`, i.e. the 32-bit runtime), loads its `library_path`
  and negotiates through `xrNegotiateLoaderRuntimeInterface`. API layers are
  therefore not applied. `openxrjson <path>` overrides the json (tests:
  Oculus `oculus_openxr_32.json`, Virtual Desktop `virtualdesktop-openxr-32.json`,
  SteamVR `steamxr_win32.json`).
- **Selection**: `runtime auto` (default since configversion 9), `steamvr`,
  or `openxr`. `auto`: with `VirtualDesktop.Streamer.exe` running it tries VDXR
  (`virtualdesktop-openxr-32.json` from `AvailableRuntimes`, whatever the active
  runtime is), otherwise SteamVR. The Oculus runtime (Link) is deliberately NOT
  picked by `auto` until the backend is verified on Link in the headset (the
  migration moves every Link player to `auto`). Until one Init succeeds every
  retry (5 s) tries VDXR again, so a headset connected to VD after the game
  started still gets VDXR. `auto` with an explicit `openxrjson` keeps the old
  rule (OpenXR unless that runtime is SteamVR's). Read directly from the ini
  by `vr_runtime.cpp` because the session starts from `CreateDevice`, before
  the hooks load the ini. OpenXR has no Background applications: `Init`
  always gives a full session and `vr::session` treats it as Scene.
- **Graphics**: D3D11 device created by the backend on the runtime's LUID
  adapter (`xrGetD3D11GraphicsRequirementsKHR`), shared with `vr_output`
  (`vr::rt::SharedD3D11()`); the immediate context gets
  `ID3D11Multithread` protection because the output worker and the backend's
  keepalive thread both use it. Eye swapchains are `B8G8R8A8_UNORM_SRGB`
  (the game output is gamma encoded; the mod's BGRA textures are copied with
  `CopySubresourceRegion`, same typeless family); overlays use
  `R8G8B8A8_UNORM_SRGB` from the RGBA pixels of `SetOverlayRaw`.
- **Frame loop**: `WaitGetPoses` = `xrWaitFrame` + `xrBeginFrame` (+
  `xrLocateViews` at the predicted display time, stage and view space);
  `Submit(Left)` records the texture, `Submit(Right)` copies both eyes into
  the swapchains and calls `xrEndFrame`. The projection layer uses the
  render pose when the mod submits with pose (`submitpose on`), otherwise the
  views located at `xrWaitFrame`, which is what OpenVR does without a pose. A
  keepalive thread closes a frame with the last layers whenever 100 ms pass
  without one (menu of the mod without game frames, window without focus,
  start-up): OpenXR shows nothing without `xrEndFrame`. A frame left open by
  the caller (pair dropped by the worker) is closed at the next
  `WaitGetPoses`.
- **Overlays** are `XrCompositionLayerQuad`s sorted by `SetOverlaySortOrder`:
  absolute = stage space, HMD-relative = view space, controller-relative =
  the hand's action space; `SideBySide_Parallel` becomes two quads with
  `eyeVisibility` LEFT/RIGHT; `IgnoreTextureAlpha` disables alpha blending
  (else unpremultiplied source alpha). `FadeToColor` is a black 8 m quad in
  view space whose 2x2 texture alpha follows the fade. Mouse events for the
  interactive menu overlay (`VROverlayInputMethod_Mouse`) are generated by
  the backend from the controller ray (right hand first): `MouseMove` in GL
  coordinates (bottom-left origin, like SteamVR), trigger = button, stick up/
  down = `ScrollDiscrete`; a 2 cm dot (internal overlay) marks the hit point.
  The ray comes from the runtime's `aim/pose` (the pointing pose SteamVR's own
  laser uses; the grip/raw pose made players hold the controller flat, 23/10)
  and a 4 mm beam quad (internal overlay, turned toward the head) is drawn
  from the controller to the hit point, or 1 m ahead of the last pointing
  hand while a menu is open but not hit.
  `ComputeOverlayIntersection` is the same ray/plane test (cinema laser).
- **Input**: one action set with per-hand stick/trigger/grip/primary/
  secondary/stickclick/menu/haptic/grip-pose actions and suggested bindings
  for Touch (and Pro/Plus), Index, Vive, WMR, HP Reverb G2, Cosmos, Focus 3,
  Pico; the mod's SteamVR Input manifest is accepted and ignored, its action
  handles map onto these actions, `UpdateActionState` = `xrSyncActions` (also
  from `GetControllerState` and from the frame loop, at most every 2 ms). The
  legacy `GetControllerState` returns the same Touch layout the mod expects.
  `IsInputAvailable`/dashboard = session FOCUSED vs VISIBLE.
- **Poses**: device 0 = HMD (view space in stage), 1/2 = left/right hand
  action spaces. The action space pose is `inverse(openxr_grip)` of SteamVR's
  Touch render model (origin x -0.007/+0.007, y -0.0018, z 0.1019 m, pitch
  20.6 degrees, `openxrgrip`), so the mod receives the "raw Touch" pose it is
  calibrated for from any controller; the shim reports Touch render model
  names so `vr_input` applies no further correction. Predicted poses use
  `xrConvertWin32PerformanceCounterToTimeKHR` (now + seconds).
  `vr::rt::SecondsToPhotons()` (predicted display time - now) replaces the
  OpenVR vsync arithmetic in `PredictedSecondsToPhotons` and in the photon
  latency measurement of `vr_output`.
- **Other mappings**: `GetRecommendedRenderTargetSize` = view configuration;
  projection matrix/raw from the located FOV; hidden-area mesh from
  `XR_KHR_visibility_mask` (tangent-plane vertices to UV, v=0 top); display
  frequency from `XR_FB_display_refresh_rate` or the predicted period; headset
  worn from `XR_EXT_user_presence` when available, else the session state;
  `TrackingSystemName` = "oculus" / "virtualdesktop" / runtime name (the ASW
  and Virtual Desktop notices keep working); reference space change =
  `VREvent_StandingZeroPoseReset`; `GetMirrorTextureD3D11` fails (the mod's
  own mirror window remains); `IVRSettings` (Motion Smoothing override) is a
  no-op.
- **Not covered**: SteamVR's per-game resolution slider (use the runtime's
  own: Oculus app pixel density, Virtual Desktop graphics preset); SteamVR
  binding UI (bindings are the suggested ones); Vive wand trackpad zones.
- **Headless test**: SteamVR's own OpenXR runtime with the null driver
  (`runtime openxr` + `openxrjson ...\steamxr_win32.json`) exercises the
  whole backend without a headset; Oculus and VDXR need the headset.

## Level-change crash: stale arm mesh in the aim trace (26/10)

Found by the final physics regression: SP_RadioMan02_P for about 2 minutes,
then `console open` of SP_CityGate01_P, crashed 3 times out of 3. It also
crashed with `grabprops off`.

**What the crash is.**
- The dumps show an access violation in MSVCR90 (+0x3B6DF), 1-2 s into the
  load. The write goes to a garbage address (0xA2EDC060, 0x986EFBC0,
  0x367ED4E0, 0x94F9C680). It always comes from the same call:
  - the engine constructs an object (InitProperties, `0x00D12E40`);
  - it copies an array property (`0x0099A040`);
  - `appRealloc` (`0x00E17990`) returns the new 0x48-byte buffer;
  - `memset` on that buffer faults.
- The engine's own crash handler then faults in the same page. Its
  allocation got 0xA2EDC010: 0x50 lower, exactly one block of that size
  class.
- **Allocator.** `GMalloc` (`[0x1451E38]`) is a thread-safe proxy around
  FMallocWindows (`[GMalloc+4]`, Malloc at `0x004D12A0`), which returns
  `FirstMem + (--NumFreeBlocks) * BlockSize`. So a free block of the 0x50
  table had a garbage `NumFreeBlocks`: something wrote into freed memory
  before the load.
- `tools/heapcheck.py` walks these free lists from outside the game, to see
  that kind of corruption without waiting for the crash.

**Root cause (mod).**
- At the first tick of a new level, `UpdateAimMarker` runs before
  `UpdateArms` (hooks.cpp, Draw detour). Through `MuzzleStart` it called
  `GetBoneLocation` on `g_rig.mesh`, which still pointed to Walker's mesh
  from the previous level, already freed.
- ProcessEvent on a freed object goes through a vtable slot that the
  allocator has overwritten (`FFreeMem.Next`). That executes whatever is
  there.
- The marker in the log is the line
  `SCRIPT: exception calling SkeletalMeshComponent.GetBoneLocation on None..SkeletalMeshComponent`
  (or `None.DumpAnimChannels..`), at the entry of RadioMan02. The garbage
  name is the freed object.
- **Evidence.** Both crashes whose logs survived (physfin X4, and R2 here)
  had that line. None of the RadioMan02 -> CityGate01 runs without it
  crashed: R1, R3, R4, the physfin short stays X3, and two hops with
  `gpushare off` (E2).
- **Partial link only.** The exception does not always lead to the crash.
  R5 (`gpushare off`) had it and loaded CityGate01 fine, and S1 had it
  before RadioMan03. This matches a write through garbage, whose effect
  depends on the heap layout.

**Fix.**
- `MuzzleStart` uses `g_rig.mesh` only if `StillAlive` still finds it in
  GObjects. Otherwise it logs once
  `ARMS: arm mesh ... of the previous level is gone ...` and skips the
  shoulder trace for that tick. `UpdateArms` takes the new mesh right after.
- Central guard: `vr::script::CallFunction` no longer calls ProcessEvent on
  an object whose GObjects slot (index at +0x04) is not the object itself.
  It logs `SCRIPT: <Function> skipped on <ptr>: not a live object (stale pointer)`
  instead, for this path and for any other cached pointer that a level
  change leaves behind.

**Checks done (26/10, without a headset, `fpscap 120`).**

| Run | Build | gpushare | RadioMan02 | Exception at entry | Next level |
|---|---|---|---|---|---|
| physfin E, E2, X4 | 11:14 | on | ~2.5 min + hand moving | X4 yes (others not logged) | CityGate01 **crash** x3 |
| physfin X1, X3 | 11:14 | on | 14-20 s | X3 no | CityGate01 ok |
| S1 | 11:14 | on | 200 s, idle | yes | **RadioMan03 ok** (natural transition) |
| E2 (2 hops) | 11:14 | off | 150 s, idle | no | CityGate01 ok x2 |
| E3 | 11:14 | on | 150 s, idle | (log lost) | CityGate01 ok |
| R1 | diag | off | as physfin X4 | no | CityGate01 ok |
| R2 | diag | on | as physfin X4 | yes | CityGate01 **crash** |
| R3, R4 | diag | on | as physfin X4 | no | CityGate01 ok |
| R5 | diag | off | as physfin X4 | yes | CityGate01 ok |
| F1 | fix (+lock diagnostics) | on | as physfin E | no; stale mesh blocked at CityGate01 entry | CityGate01 ok |
| F2 | fix | on | as physfin E | **blocked** (`ARMS: arm mesh ... is gone`) | CityGate01 ok |
| F3 | fix | on, `grabprops off` | as physfin X4 | **blocked** | CityGate01 ok |
| F4 | fix | on | 200 s, idle | no | RadioMan03 ok, alive 62 s later (natural transition) |

- **With the fix.**
  - F2 and F3 hit the old trigger: the stale mesh at RadioMan02 entry,
    blocked this time. F1-F3 also hit it at CityGate01 entry.
  - The guard in `CallFunction` never fired: no `not a live object` line,
    so no false positives.
  - `tools/heapcheck.py` ran through F1-F4 (1562 checks, about one per
    second): no corrupted free block.
  - `PROPS: tick` stayed at 0.19-0.20 ms, and the test grabs still worked
    (bar stool, vase).

- **No vanilla run.** The game without the mod has no console:
  `Console.ConsoleKey` and `TypeKey` are `None` on the live instance, and
  the user ini files in Documents are encrypted, so no key binding can be
  added. Instead, `gpushare off` removed the D3D9Ex device and the MANAGED
  conversion, the mod's most invasive part of the load path. It did not
  crash, but it did not remove the exception either (R5).
- **Frame cap without the mod's `fpscap`.** The engine's own smoothing can
  be switched at runtime, without touching any ini:
  `console set GameEngine bSmoothFrameRate True` and
  `console set GameEngine MaxSmoothedFrameRate 50` (measured: 50.0 fps).
  Send them one at a time: the mod queues only the last `console` command
  of a batch.
- **D3D9Ex diagnostics.** The conversion now logs any MANAGED texture that
  had to be created without DYNAMIC: such a texture cannot be locked. None
  appeared in these runs. Lock hooks on textures, surfaces and vertex/index
  buffers saw no failed lock, and no texture released while locked or
  unlocked late, so they were removed again.

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
  simply work or experiments): `twohand on|off [cm] [Hz]` (WI-4b: grip radius and
  filter cutoff; the old third value in seconds is gone), `leftgrip on|off` (the cm is ignored since WI-4b),
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
- Headless VR tests (25/10, see "Tester VR fixes (25/10)"): `runtime fake`
  and `fake ...` (fake VR runtime), `synthctrl`, `synthheight`, `simevent
  zeropose`, `viewlog N`, `bodylog N [bones]`, `twohandinfo`; `TURRET:` lines.
- Test-only, never for players (not in the shipped ini nor in the menu):
  - `fpscap <hz>`: at most <hz> game ticks per second (props.cpp `TickImpl`
    waits once per game tick, so only while Walker is the view target; 0 or
    no value = free), for 90 / 72 / 45 Hz runs without a headset. It is a
    command, so a line in `specopsvr.ini` applies it at every start and the
    Advanced menu then lists it like any ini line. Test runs that write it
    into a player's ini must take it out again (27/10 evening: a leftover
    `fpscap 120` was removed from the user's ini).
  - `specopsvr_keepfocus`: a file (any content) next to the dll, not an ini
    key. With it the window keeps the focus without a headset, and the game
    always sees its own window in the foreground (`GetForegroundWindow` /
    `GetActiveWindow` / `GetFocus` hooks; log `DISPLAY: specopsvr_keepfocus
    present ...`). Made for two game copies running side by side (copy B,
    25/10); `gamectl.py` never brings a second copy (`SPECOPS_EXE` other than
    `SpecOpsTheLine.exe`) with this file to the front. The player-facing
    behaviour is the `keepfocus` setting above (focus loss ignored while the
    headset is in use).

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
- `viewcheck.py` - parses `VIEW:` / `JITTER:` lines (drawn view against the
  headset pose, view yaw per tick, fake-runtime summaries) for the suites.
- `deepsweep.py` - jumps through every checkpoint of every level.
- `loadouttest.py` / `weapontest.py` - fire the weapons and measure the shot
  direction against the barrel.
- `ue3.py` - engine object browser.
- `heapcheck.py [seconds] [interval]` - walks the free lists of the game's
  allocator (FMallocWindows) from outside and prints `ANOMALY` with the
  block contents when one is corrupted (see "Level-change crash").
- `FindImm.java` (Ghidra scripts folder, next to `Decomp.java`) lists the
  functions that use an immediate, e.g. a field offset (`0xE98` -> the squad
  target validation); `ghq.py` (below) needs `JAVA_HOME`.
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
  mode (kills the game; never run two game tests at once). The gesture phase
  brings the game to the foreground first: the simulated gestures advance on
  the game's pad polls, and an unfocused window stops polling the pad (25/10,
  two game copies: "pad polls per tick" 1.00 -> 0.00 and the simulated reload
  never reached "magazine inserted").
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
