# SpecOpsTheLineVR

An unofficial **full VR mod for Spec Ops: The Line** (2012, Unreal Engine 3,
Direct3D 9): stereoscopic 6DOF first person, motion-controller aiming with
Walker's own arms, VR cover, gestures, external haptics, and cutscenes on a
virtual cinema screen.

> Status: **beta**. The whole campaign loads and plays; please report bugs
> (see [Reporting bugs](#reporting-bugs)).

## Features

- **Stereoscopic 3D** at the headset's resolution, field of view and IPD,
  synchronous stereo (both eyes rendered from the same game tick), head pose
  predicted to the moment the image reaches your eyes (measured on your PC).
- **6DOF head tracking**: look, lean, crouch and side-step; the head stops at
  walls instead of clipping through them. **Room-scale**: when you step,
  Walker walks with you. Your real height sets the eye height (seated mode
  available). The game's recoil, camera shakes and crouching never move your
  view; cuts, checkpoints and death fade from black.
- **Audio in the headset**: the game's sound follows the Windows default
  playback device, so it reaches the headset even when Virtual Desktop or
  Link starts streaming after the game is already running.
- **Wrist HUD and laser**: ammo, weapon and health on your support-hand
  wrist like a watch (`wristhud`); on the virtual screen the gun controller
  is a laser pointer for the game's menus, trigger to click (`cinemalaser`).
- **First person** with Walker's body: his arms follow your controllers
  (inverse kinematics on the game's own skeleton), the gun stays in his hand,
  and the body turns with you when you turn around (`fpbody off` in
  `specopsvr.ini` if you prefer floating hands).
- **Aim with the gun**: shots, rockets and grenade-launcher rounds leave the
  barrel of the gun you hold, with the game's own spread scaled by your
  stance (sights at the eye = practically none, two hands = half, long gun in
  one hand = more). A green marker shows where the barrel points (on by
  default, `marker off` or the in-headset menu to use only the weapon's
  sights); `laser on` adds a visible beam. Two-handed aiming engages
  by itself: left hand on the barrel of a rifle, or wrapped around the gun
  hand with a pistol. **Aim down the sights** by bringing the gun to your eye.
  Gamepad aim assist is disabled while aiming by hand.
- **Gunstock support**: one-button calibration of the gun angle.
- **VR cover**: the world does not spin when Walker takes cover; lean out
  with your body to pop out and aim.
- **Mounted guns and the helicopter minigun** aim with your controller.
- **Squad orders** target the enemy you look at; the
  target stays locked until you release the button.
- **Executions**: press B next to a downed enemy and Walker turns to him.
- **Gestures**: throw grenades (harder throw = faster grenade), punch or hit with the gun (melee), reload by touching the magazine with the left hand and pulling down (`reload physical` in the settings: pull the magazine, take a new one from your belt, push it in; `manualaction on` also asks you to rack the gun),
  duck into cover, grab the next weapon from your shoulder.
- **Haptics**: controller rumble on the gun hand, **ProTubeVR**
  (ForceTube / ProVolver) recoil and **bHaptics** vest and sleeves.
- **Virtual cinema screen** for videos, menus and loading screens; scripted
  in-engine scenes stay in 3D.
- **Comfort options**: smooth turning at a fixed speed or snap turning,
  movement where you look, vignette, recentering (also follows the
  headset's own recenter), left-handed mode (gun buttons on the left
  controller and Walker holds the gun in his left hand).
- **Sniper scopes** work as a lens along the barrel.
- Fast GPU-to-GPU frame copy (D3D9Ex shared surfaces to Direct3D 11 / OpenVR).
- **Performance**: each eye drawn with its exact field of view (no wasted
  pixels), resolution taken from SteamVR (use the SteamVR resolution slider),
  dynamic resolution that keeps the headset frame rate, a VR graphics preset
  and low-latency frame submission. Works with canted-display headsets
  (Pimax and similar).
- **Automatic updates** from the GitHub releases (stable or beta channel,
  withdrawn versions replaced automatically, automatic rollback of a version
  that fails to start, `rollback.bat`, `reset_settings.bat`).
- Detailed **logs and crash dumps** for bug reports.

## Requirements

- Windows 10/11, a recent NVIDIA/AMD GPU.
- *Spec Ops: The Line*. Developed and tested on the Steam version; other
  copies (non-Steam executables) are untested: the mod still starts and shows
  a notice in the headset, but it may not work on them.
- A PC VR headset with two motion controllers. With the default
  `runtime auto` the mod picks by itself: **Virtual Desktop (VDXR)** when the
  Virtual Desktop Streamer is running, otherwise **SteamVR**. `runtime openxr`
  (in `specopsvr.ini` or the in-headset menu) uses the OpenXR runtime active
  on your PC instead (Meta Quest Link, Pimax...; experimental on Link). Developed and tested
  with a Meta Quest 3 over Link; Virtual Desktop works too. Default bindings
  for Quest/Rift Touch, Valve Index, Vive, Vive Cosmos, HP Reverb G2, Pico and
  Windows Mixed Reality controllers.

## Install

1. Download `SpecOpsTheLineVR.zip` from the
   [latest release](https://github.com/Mavic-Pro/SpecOpsTheLineVR/releases/latest)
   and extract it anywhere.
2. Run `install.bat`. It finds the game through your Steam libraries; if it
   cannot, pass the folder:
   `install.bat "D:\SteamLibrary\steamapps\common\SpecOps_TheLine"`.
3. In Steam, *Spec Ops: The Line > Properties > Launch options*, add
   `-windowed ResX=2400 ResY=2500`. Once the headset is on, the mod switches
   the game to the resolution SteamVR recommends for your headset: to change
   sharpness, use the SteamVR resolution slider.
4. Put the headset on (connect it to Virtual Desktop or Link, or start
   SteamVR) and start the game. Through OpenXR (VDXR, Link) SteamVR is not
   needed and the resolution follows that runtime's own setting (VD's
   Graphics Quality, up to Godlike at 120 Hz) instead of the SteamVR slider.

The game files are never modified: the mod only adds `d3d9.dll` and
`openvr_api.dll` next to `SpecOpsTheLine.exe`. `uninstall.bat` removes them.

## Updates

The mod checks GitHub at start-up and installs new versions for the next
launch. New versions can first come out as test builds (GitHub
pre-releases): add `updatechannel beta` to `specopsvr.ini` to get them too.
If an update causes problems, close the game and run `rollback.bat` to go
back to the previous version (it also turns automatic updates off). A new
version that dies twice right after starting is put back automatically
(`d3d9.dll.bad` is kept, the version is skipped while `specopsvr_skip.txt`
exists). Updates also refresh the helper scripts and `openvr_api.dll` when
the release lists them; your `specopsvr.ini` is migrated, never replaced
(`reset_settings.bat` restores the defaults and keeps a copy of yours).
`autoupdate off` disables updates.

## Controls

| Input | Action |
|---|---|
| Left stick | Move |
| Right stick | Turn (smooth or snap) |
| Right trigger / left trigger | Fire / aim |
| Left grip | Grenade (hold: trajectory, release: throw); on the gun it just holds the gun |
| Right grip (hold, look at an enemy, release) | Squad attack order |
| A / B / X / Y | Action-cover / melee / reload / next weapon |
| B + Y held for 1 s | Gunstock calibration |
| Left menu button | Pause (hold 0.6 s: Back / objectives) |
| Right stick held + direction | D-pad (secondary fire, grenade type) |
| Both sticks held for 1 s | Recenter (stand straight and look ahead) |
| Both sticks held for 3 s | Also set and save your standing height (`height` in the ini) |

When the game says **RB** it means the **right grip**. Chapter 1, sniper on the
billboard ("While holding RB target the enemy, then release"): hold the right
grip, point the gun **or just look** at the sniper (the red triangle follows
your view) until it is highlighted, then release. The target stays locked
until you release, even if the gun drifts off it.

On **Valve Index** controllers A and B on the right hand are A and B, on the
left hand X and Y; the left trackpad click pauses. Every action can be
rebound in SteamVR: *Settings > Controllers > Manage controller bindings >
Spec Ops: The Line*. Aiming is aligned to each controller's pointing
direction, so it feels the same on every controller.

| Gesture | Action |
|---|---|
| Overhand throw with the left hand | Grenade (flies where you throw) |
| Straight punch with the left hand (chest height) | Melee |
| Left hand on the magazine (under the gun), then pull down | Reload (`reload physical`: grip the magazine and pull it out, grip at your belt for a new one, push it into the gun; each step vibrates) |
| Duck quickly next to cover | Take cover |
| Lean out sideways (or stand up) while in cover | Pop out and aim |
| Gun up to your eye | Aim down the sights |
| Right hand over the right shoulder (or at the hip) + grip | Next weapon |
| Left hand on the barrel | The hand grips the gun; two-handed aiming |
| Turn your head past 45 degrees | Walker turns with you |

## Gunstock

Aim the stock at something far away, looking at it through the sights in the
middle of your view, and hold **B + Y** for one second. The angle between the stock and the controller is
measured, applied and saved (`aimpitch` in `specopsvr.ini`). Repeat if you
move the controllers on the stock.

## Haptic devices (optional)

- **ProTubeVR ForceTube / ProVolver**: install the free **ProTubeVR
  Companion App** (Steam; it provides the up-to-date ProTube driver), pair the
  devices in Windows Bluetooth settings, close the Companion App and start the
  game. Recoil on every shot, scaled by weapon; with two ProVolvers the second
  one kicks in the support hand while aiming two-handed (`protube swap` in
  `specopsvr.ini` if they come out the wrong way round). The mod starts a
  small 64-bit helper, `specopsvr_protube.exe`, because the game is 32-bit;
  its log is `specopsvr_protube.log`.
- **bHaptics** vest and arm sleeves: start the bHaptics Player before the
  game. Recoil on the arms, hits taken on the vest, grenade / melee / reload
  on the left arm.

Both are off automatically when no device is found; `protube off` /
`bhaptics off` in `specopsvr.ini` disable them.

## Frame rate

The mod keeps the game at the headset refresh rate with dynamic resolution.
The headset software can still lock the game at half the refresh rate (for
example 36 fps on a 72 Hz Quest) with its frame-synthesis feature, even when
there is plenty of headroom:

- **Quest over Link**: Meta ASW is turned off automatically while the game
  runs and set back to Auto when it closes (`linkasw keep` to leave it).
- **Index, Vive, Pimax, WMR**: SteamVR Motion Smoothing is forced off for
  this game only (`motionsmoothing keep` to leave it).
- **Virtual Desktop**: set *Synchronous Spacewarp* to *Disabled* in the
  Virtual Desktop Streamer app if the game stays at half rate (the mod cannot
  change it; it writes a reminder in the log when it detects VD).
  For the lowest latency use the H.264+ or AV1 codec and the *High* quality
  preset (higher presets add encoding/decoding time). When the game starts,
  SteamVR may show its dashboard: close it with the menu button.

## Configuration

The **in-headset settings menu** opens by itself on the game's main menu
(point the controller laser and pull the trigger; PLAY closes it): *Basic*
has the main options with a short explanation, *Advanced* every option of the
ini, *Controls* the full control list. Changes apply at once and are saved.

`specopsvr.ini` in the game's `Binaries\Win32` folder; every option is
documented in the file (aiming, arms, cover, comfort, cinema screen, haptics,
performance, updates). The same commands can be sent while the game runs by
writing them to `specopsvr_cmd.txt` in the same folder. If something looks
wrong, most features can be switched off one by one (`ads off`,
`coverpeek off`, `turretaim stick`, `cinemawalker screen`, `gesture melee off`,
`vrinput legacy`).

## Reporting bugs

1. Run `collect_logs.bat`: it creates `SpecOpsTheLineVR_report_<date>.zip` on
   your Desktop with the logs of the last four sessions, your configuration
   and any crash dump.
2. [Open an issue](https://github.com/Mavic-Pro/SpecOpsTheLineVR/issues/new),
   describe what happened (level, what you were doing, what you expected) and
   attach the zip.

## Building from source

Requirements: Visual Studio 2022 (C++ desktop workload, 32-bit toolset), CMake
3.16+.

```
cmake -S . -B build -A Win32
cmake --build build --config Release
python tools/package.py        # dist/SpecOpsTheLineVR.zip
python tools/regress.py        # release gate (runs the game, no headset needed)
```

How it works, the reverse-engineered engine addresses and the test tools are
described in [docs/TECHNICAL.md](docs/TECHNICAL.md).

## Credits and license

- [MinHook](https://github.com/TsudaKageyu/minhook) (BSD 2-Clause) and the
  [OpenVR SDK](https://github.com/ValveSoftware/openvr) (BSD 3-Clause) are
  included under `vendor/`.
- ProTubeVR and bHaptics integrations follow the public APIs of those
  devices; their software is not included.
- Spec Ops: The Line is © 2K Games / Yager. This project is not affiliated
  with them and ships no game files.
- Released under the MIT License (see [LICENSE](LICENSE)).
