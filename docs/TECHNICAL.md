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
| `updater.cpp` | Automatic update from the GitHub releases |

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
  (`[0x014DF734]`).

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
- Aim assist (`YWeapon` friction / snap-on flags at +0x260,
  `YPlayerController.m_aimSnapOnActiveByUser` +0x734) is disabled while
  aiming by hand.
- Verification (`firelog on` + `tools/weapontest.py` + `tools/shotreport.py`):
  the impact point of every shot is compared with the marker as an angle at
  the muzzle. Measured with spread off, median error 0.03-0.3 degrees (AA12,
  AK47, Barrett M99, Benelli M1014, FAMAS, P90, HK417, UMP45, M4, M9,
  Micro Uzi, Python); RPG rocket direction equal to the barrel within 0.01.
  Weapons whose assets are not loaded in the test level cannot fire there
  and still need testing in the levels where they appear.

## Input

- Touch controllers are read through the OpenVR legacy API and returned to
  the game from hooked `XInputGetState` / `XInputSetState` (IAT `0x0110974C` /
  `0x01109748`). The game's actions come from `YGamepadInputScheme`.
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
