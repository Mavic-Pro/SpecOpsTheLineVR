# SpecOpsTheLineVR

An unofficial **full VR mod for Spec Ops: The Line** (2012, Unreal Engine 3,
Direct3D 9): stereoscopic 6DOF first person, motion-controller aiming with
Walker's own arms, gestures, and cutscenes on a virtual cinema screen.

> Status: **beta**. The whole campaign loads and plays; please report bugs
> (see [Reporting bugs](#reporting-bugs)).

## Features

- **Stereoscopic 3D** at the headset's resolution and field of view, synchronous
  stereo (both eyes rendered from the same game tick).
- **6DOF head tracking**: look, lean, crouch and side-step; the head stops at
  walls instead of clipping through them.
- **First person** with Walker's body: his arms follow your controllers
  (inverse kinematics on the game's own skeleton), the gun stays in his hand,
  the body follows you and stays opaque.
- **Aim with the gun**: shots leave the barrel of the gun you hold and land on
  the green marker. Two-handed aiming when your left hand is on the barrel.
  Gamepad aim assist is disabled while aiming by hand.
- **Gestures**: throw grenades, reload by pulling the magazine, duck into
  cover, grab the next weapon from your shoulder.
- **Virtual cinema screen** for cutscenes, menus and loading screens.
- **Comfort options**: smooth or snap turning, vignette, recentering.
- **Sniper scopes** zoom like in the game.
- Fast GPU-to-GPU frame copy (D3D9Ex shared surfaces to Direct3D 11 / OpenVR).
- **Automatic updates** from the GitHub releases.
- Detailed **logs and crash dumps** for bug reports.

## Requirements

- Windows 10/11, a recent NVIDIA/AMD GPU.
- *Spec Ops: The Line* (Steam version; the mod checks the executable and stays
  inactive on unsupported versions).
- SteamVR and a SteamVR headset with two motion controllers. Developed and
  tested with a Meta Quest 3 over Link.

## Install

1. Download `SpecOpsTheLineVR.zip` from the
   [latest release](https://github.com/Mavic-Pro/SpecOpsTheLineVR/releases/latest)
   and extract it anywhere.
2. Run `install.bat`. It finds the game through your Steam libraries; if it
   cannot, pass the folder:
   `install.bat "D:\SteamLibrary\steamapps\common\SpecOps_TheLine"`.
3. In Steam, *Spec Ops: The Line > Properties > Launch options*, add
   `-windowed ResX=2400 ResY=2500` (a nearly square window gives the sharpest
   image in the headset).
4. Start SteamVR, put the headset on, start the game.

The game files are never modified: the mod only adds `d3d9.dll` and
`openvr_api.dll` next to `SpecOpsTheLine.exe`. `uninstall.bat` removes them.

## Controls

| Input | Action |
|---|---|
| Left stick | Move |
| Right stick | Turn (smooth or snap) |
| Right trigger / left trigger | Fire / aim |
| Left grip | Grenade (hold: trajectory, release: throw) |
| Right grip | Squad orders |
| A / B / X / Y | Action-cover / melee / reload / next weapon |
| Left menu button | Pause |
| Right stick held + direction | D-pad (secondary fire, grenade type) |
| Both sticks held for 1 s | Recenter (stand straight and look ahead) |

| Gesture | Action |
|---|---|
| Quick forward throw with the left hand | Grenade |
| Left hand on the magazine, then pull down | Reload |
| Duck quickly next to cover | Take cover |
| Right hand over the right shoulder + grip | Next weapon |
| Left hand on the barrel | Two-handed aiming |

## Configuration

`specopsvr.ini` in the game's `Binaries\Win32` folder; every option is
documented in the file (aiming, arms, comfort, cinema screen, performance,
updates). The same commands can be sent while the game runs by writing them
to `specopsvr_cmd.txt` in the same folder.

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
```

How it works, the reverse-engineered engine addresses and the test tools are
described in [docs/TECHNICAL.md](docs/TECHNICAL.md).

## Credits and license

- [MinHook](https://github.com/TsudaKageyu/minhook) (BSD 2-Clause) and the
  [OpenVR SDK](https://github.com/ValveSoftware/openvr) (BSD 3-Clause) are
  included under `vendor/`.
- Spec Ops: The Line is © 2K Games / Yager. This project is not affiliated
  with them and ships no game files.
- Released under the MIT License (see [LICENSE](LICENSE)).
