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
- **First person** with Walker's body: his arms follow your controllers
  (inverse kinematics on the game's own skeleton), the gun stays in his hand,
  and the body turns with you when you turn around.
- **Aim with the gun**: shots, rockets and grenade-launcher rounds leave the
  barrel of the gun you hold and land on the green marker (verified for every
  campaign weapon, within 0.15 degrees). Two-handed aiming with the left hand
  on the barrel. **Aim down the sights** by bringing the gun to your eye.
  Gamepad aim assist is disabled while aiming by hand.
- **Gunstock support**: one-button calibration of the gun angle.
- **VR cover**: the world does not spin when Walker takes cover; lean out
  with your body to pop out and aim.
- **Mounted guns and the helicopter minigun** aim with your controller.
- **Squad orders** target the enemy you point your gun at.
- **Gestures**: throw grenades, punch (melee), physical two-handed reload (pull the magazine, take a new one from your belt, push it in),
  duck into cover, grab the next weapon from your shoulder.
- **Haptics**: controller rumble on the gun hand, **ProTubeVR**
  (ForceTube / ProVolver) recoil and **bHaptics** vest and sleeves.
- **Virtual cinema screen** for videos, menus and loading screens; scripted
  in-engine scenes stay in 3D.
- **Comfort options**: smooth turning at a fixed speed or snap turning,
  movement where you look, vignette, recentering (also follows the
  headset's own recenter), left-handed mode.
- **Sniper scopes** work as a lens along the barrel.
- Fast GPU-to-GPU frame copy (D3D9Ex shared surfaces to Direct3D 11 / OpenVR).
- **Performance**: each eye drawn with its exact field of view (no wasted
  pixels), resolution taken from SteamVR (use the SteamVR resolution slider),
  dynamic resolution that keeps the headset frame rate, a VR graphics preset
  and low-latency frame submission. Works with canted-display headsets
  (Pimax and similar).
- **Automatic updates** from the GitHub releases (stable or beta channel,
  withdrawn versions replaced automatically, `rollback.bat`).
- Detailed **logs and crash dumps** for bug reports.

## Requirements

- Windows 10/11, a recent NVIDIA/AMD GPU.
- *Spec Ops: The Line* (Steam version; the mod checks the executable and stays
  inactive on unsupported versions).
- SteamVR and a SteamVR headset with two motion controllers. Developed and
  tested with a Meta Quest 3 over Link; Virtual Desktop works too. Default
  bindings for Quest/Rift Touch, Valve Index, Vive, Vive Cosmos, HP Reverb G2
  and Windows Mixed Reality controllers.

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
4. Start SteamVR, put the headset on, start the game.

The game files are never modified: the mod only adds `d3d9.dll` and
`openvr_api.dll` next to `SpecOpsTheLine.exe`. `uninstall.bat` removes them.

## Updates

The mod checks GitHub at start-up and installs new versions for the next
launch. New versions can first come out as test builds (GitHub
pre-releases): add `updatechannel beta` to `specopsvr.ini` to get them too.
If an update causes problems, close the game and run `rollback.bat` to go
back to the previous version (it also turns automatic updates off).
`autoupdate off` disables updates.

## Controls

| Input | Action |
|---|---|
| Left stick | Move |
| Right stick | Turn (smooth or snap) |
| Right trigger / left trigger | Fire / aim |
| Left grip | Grenade (hold: trajectory, release: throw); on the gun it just holds the gun |
| Right grip (hold, point the gun at an enemy, release) | Squad attack order |
| A / B / X / Y | Action-cover / melee / reload / next weapon |
| B + Y held for 1 s | Gunstock calibration |
| Left menu button | Pause (hold 0.6 s: Back / objectives) |
| Right stick held + direction | D-pad (secondary fire, grenade type) |
| Both sticks held for 1 s | Recenter (stand straight and look ahead) |

On **Valve Index** controllers A and B on the right hand are A and B, on the
left hand X and Y; the left trackpad click pauses. Every action can be
rebound in SteamVR: *Settings > Controllers > Manage controller bindings >
Spec Ops: The Line*. Aiming is aligned to each controller's pointing
direction, so it feels the same on every controller.

| Gesture | Action |
|---|---|
| Overhand throw with the left hand | Grenade (flies where you throw) |
| Straight punch with the left hand (chest height) | Melee |
| Left grip on the magazine (under the gun) and pull it out, grip at your belt for a new one, push it into the gun | Reload (each step vibrates; `reload simple` = touch the magazine and pull down) |
| Duck quickly next to cover | Take cover |
| Lean out sideways (or stand up) while in cover | Pop out and aim |
| Gun up to your eye | Aim down the sights |
| Right hand over the right shoulder + grip | Next weapon |
| Left hand on the barrel | The hand grips the gun; two-handed aiming |
| Turn your head past 45 degrees | Walker turns with you |

## Gunstock

Aim the stock at something far away, looking at it through the sights in the
middle of your view, and hold **B + Y** for one second. The angle between the stock and the controller is
measured, applied and saved (`aimpitch` in `specopsvr.ini`). Repeat if you
move the controllers on the stock.

## Haptic devices (optional)

- **ProTubeVR ForceTube / ProVolver**: copy `ForceTubeVR_API_x32.dll` (from
  the ProTubeVR software/SDK) next to `SpecOpsTheLine.exe`, pair the device,
  start the game. Recoil on every shot, scaled by weapon.
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

## Configuration

`specopsvr.ini` in the game's `Binaries\Win32` folder; every option is
documented in the file (aiming, arms, cover, comfort, cinema screen, haptics,
performance, updates). The same commands can be sent while the game runs by
writing them to `specopsvr_cmd.txt` in the same folder. If something looks
wrong, most features can be switched off one by one (`submitpose off`,
`ads off`, `coverpeek off`, `coversnap off`, `turretaim stick`,
`cinemawalker screen`, `gesture melee off`, `vrinput legacy`).

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
