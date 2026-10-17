"""Builds the release package in dist/SpecOpsTheLineVR.zip.

Contents: d3d9.dll (proxy), openvr_api.dll, specopsvr.ini, install/uninstall/
collect_logs (.bat launching the .ps1 next to it) and README.txt. No Windows files are shipped
(the system d3d9.dll is loaded from the Windows folder).

    python tools/package.py
"""
import os
import re
import shutil
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NAME = "SpecOpsTheLineVR"
DIST = os.path.join(ROOT, "dist", NAME)


def version():
    with open(os.path.join(ROOT, "src", "version.h"), encoding="utf-8") as f:
        return re.search(r'SPECOPSVR_VERSION "([^"]+)"', f.read()).group(1)


# Shared by the scripts: finds the game folder from the argument or from every
# Steam library listed in libraryfolders.vdf (Steam path from the registry).
FIND_GAME_PS1 = r"""
function Find-Game([string]$game) {
  if (-not $game) {
    $steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
    $libs = @()
    if ($steam) {
      $libs += $steam
      $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
      if (Test-Path $vdf) {
        foreach ($m in (Select-String -Path $vdf -Pattern '"path"\s+"([^"]+)"' -AllMatches).Matches) {
          $libs += ($m.Groups[1].Value -replace '\\+', '\')
        }
      }
    }
    foreach ($l in $libs) {
      $p = Join-Path $l 'steamapps\common\SpecOps_TheLine'
      if (Test-Path (Join-Path $p 'Binaries\Win32\SpecOpsTheLine.exe')) { $game = $p; break }
    }
  }
  if (-not $game -or -not (Test-Path (Join-Path $game 'Binaries\Win32\SpecOpsTheLine.exe'))) {
    Write-Host 'Spec Ops: The Line was not found. Run:  install.bat "<game folder>"'
    Write-Host 'e.g.  install.bat "D:\SteamLibrary\steamapps\common\SpecOps_TheLine"'
    exit 1
  }
  return (Join-Path $game 'Binaries\Win32')
}
"""

INSTALL_PS1 = r"""
. (Join-Path $PSScriptRoot 'find_game.ps1')
$dst = Find-Game $args[0]
Copy-Item -Force (Join-Path $PSScriptRoot 'd3d9.dll'), (Join-Path $PSScriptRoot 'openvr_api.dll') $dst
$ini = Join-Path $dst 'specopsvr.ini'
if (Test-Path $ini) {
  Copy-Item -Force (Join-Path $PSScriptRoot 'specopsvr.ini') "$ini.new"
  Write-Host 'Your specopsvr.ini was kept; the new defaults are in specopsvr.ini.new'
} else {
  Copy-Item (Join-Path $PSScriptRoot 'specopsvr.ini') $ini
}
Write-Host "SpecOpsTheLineVR installed in $dst"
Write-Host 'Start SteamVR, then the game.'
"""

UNINSTALL_PS1 = r"""
. (Join-Path $PSScriptRoot 'find_game.ps1')
$dst = Find-Game $args[0]
foreach ($f in 'd3d9.dll', 'openvr_api.dll', 'd3d9.dll.old', 'd3d9.dll.new', 'd3d9.dll.prev', 'd3d9.dll.rolledback') {
  Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $dst $f)
}
Write-Host "SpecOpsTheLineVR removed from $dst (specopsvr.ini and the logs were kept)."
"""

ROLLBACK_PS1 = r"""
. (Join-Path $PSScriptRoot 'find_game.ps1')
$dst = Find-Game $args[0]
$prev = Join-Path $dst 'd3d9.dll.prev'
if (-not (Test-Path $prev)) { Write-Host 'No previous version found (d3d9.dll.prev).'; exit 1 }
if (Get-Process SpecOpsTheLine -ErrorAction SilentlyContinue) { Write-Host 'Close the game first.'; exit 1 }
Copy-Item -Force (Join-Path $dst 'd3d9.dll') (Join-Path $dst 'd3d9.dll.rolledback')
Copy-Item -Force $prev (Join-Path $dst 'd3d9.dll')
Add-Content -Path (Join-Path $dst 'specopsvr.ini') -Value "`r`nautoupdate off  # set by rollback.bat; remove to get updates again"
Write-Host 'The previous version is back. Automatic updates were turned off in specopsvr.ini.'
"""

COLLECT_PS1 = r"""
. (Join-Path $PSScriptRoot 'find_game.ps1')
$dst = Find-Game $args[0]
$files = Get-ChildItem $dst -File | Where-Object {
  $_.Name -like 'specopsvr*.log' -or $_.Name -like 'specopsvr*.ini' -or $_.Name -like 'specopsvr_crash_*.dmp' }
if (-not $files) { Write-Host 'No SpecOpsTheLineVR logs found.'; exit 1 }
$zip = Join-Path ([Environment]::GetFolderPath('Desktop')) ('SpecOpsTheLineVR_report_' + (Get-Date -Format 'yyyyMMdd_HHmmss') + '.zip')
Compress-Archive -Path $files.FullName -DestinationPath $zip
Write-Host "Created $zip"
Write-Host 'Attach it to your issue on GitHub.'
"""


def bat(script, what):
    return ("@echo off\r\nrem " + what + "\r\npowershell -NoProfile -ExecutionPolicy Bypass -File \"%~dp0" + script +
            "\" %*\r\npause\r\n")


README_TXT = """SpecOpsTheLineVR {version} - unofficial VR mod for Spec Ops: The Line
==========================================================================

https://github.com/Mavic-Pro/SpecOpsTheLineVR

Requirements: Windows 10/11, Spec Ops: The Line (Steam), SteamVR and a
SteamVR-compatible headset with two motion controllers. Developed and tested
with a Meta Quest 3 over Link.

INSTALL
  1. Run install.bat (it finds the game through Steam; otherwise
     install.bat "D:\\SteamLibrary\\steamapps\\common\\SpecOps_TheLine").
  2. In Steam, game Properties > Launch options, add:
       -windowed ResX=2400 ResY=2500
     (a nearly square window gives the sharpest image in the headset).
  3. Start SteamVR, put the headset on and start the game.

UNINSTALL
  Run uninstall.bat. The game files are never modified: the mod only adds
  d3d9.dll and openvr_api.dll next to the game executable.

UPDATES
  The mod checks GitHub at start-up and installs new versions by itself for
  the next launch ("autoupdate off" in specopsvr.ini disables it). New
  versions first come out as test (pre-release) builds: "updatechannel beta"
  gets them too. If an update causes problems, close the game and run
  rollback.bat to go back to the previous version.

CONTROLS (Touch controllers, shown to the game as an Xbox pad)
  Left stick: move                 Right stick: turn
  Right trigger: fire              Left trigger: aim
  Left grip: grenade (LB)          Right grip: squad orders (RB)
  A: action / cover                B: melee
  X: reload                        Y: next weapon
  Stick clicks: LS / RS            Left menu button: Start (pause)
  Right stick held + direction: D-pad (secondary fire, grenade type)
  Both sticks held for 1 second: recenter (stand straight, look ahead)
  You aim with the gun in your right hand: shots leave the barrel and land
  on the green marker. Put your left hand on the barrel to aim two-handed.
  Bring the gun up to your eye to aim down the sights.
  Gunstock users: hold the stock as when aiming, look along it at something
  far away and hold B + Y for 1 second (calibrates the gun angle).

GESTURES
  Grenade: quick forward throw with the left hand.
  Reload: left hand on the magazine (below the right hand), then pull down.
  Cover: duck quickly next to cover.
  Weapon: right hand over your right shoulder + right grip = next weapon.
  Melee: punch forward with the left hand at chest height.
  Cover: lean out sideways (or stand up) to pop out and aim.

HAPTIC DEVICES (optional)
  ProTubeVR ForceTube / ProVolver: copy ForceTubeVR_API_x32.dll (from the
  ProTubeVR software/SDK) next to SpecOpsTheLine.exe, pair the device, start
  the game. bHaptics vest and sleeves: start the bHaptics Player before the
  game. Both can be turned off in specopsvr.ini (protube off / bhaptics off).

CONFIGURATION
  specopsvr.ini in the game's Binaries\\Win32 folder (every option is
  described in the file).

REPORTING BUGS
  Run collect_logs.bat: it creates a zip on your Desktop with the logs,
  your configuration and crash dumps. Open an issue on GitHub, describe what
  happened (level, what you were doing) and attach the zip.
"""


def main():
    if os.path.exists(DIST):
        shutil.rmtree(DIST)
    os.makedirs(DIST)
    shutil.copy2(os.path.join(ROOT, "deploy", "Release", "d3d9.dll"), DIST)
    shutil.copy2(os.path.join(ROOT, "vendor", "openvr", "bin", "win32", "openvr_api.dll"), DIST)
    shutil.copy2(os.path.join(ROOT, "deploy", "specopsvr.ini"), DIST)
    files = {
        "find_game.ps1": FIND_GAME_PS1,
        "install.ps1": INSTALL_PS1,
        "uninstall.ps1": UNINSTALL_PS1,
        "collect_logs.ps1": COLLECT_PS1,
        "install.bat": bat("install.ps1", "Installs SpecOpsTheLineVR into the game folder (found through Steam)."),
        "uninstall.bat": bat("uninstall.ps1", "Removes SpecOpsTheLineVR (the game files are never modified)."),
        "collect_logs.bat": bat("collect_logs.ps1", "Zips logs, configuration and crash dumps for a bug report."),
        "rollback.ps1": ROLLBACK_PS1,
        "rollback.bat": bat("rollback.ps1", "Puts back the version installed before the last automatic update."),
        "README.txt": README_TXT.format(version=version()),
    }
    for name, text in files.items():
        with open(os.path.join(DIST, name), "w", encoding="utf-8-sig" if name.endswith(".ps1") else "utf-8",
                  newline="" if name.endswith(".bat") else "\r\n") as f:
            f.write(text.lstrip("\n"))
    zip_path = os.path.join(ROOT, "dist", NAME + ".zip")
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for name in sorted(os.listdir(DIST)):
            z.write(os.path.join(DIST, name), os.path.join(NAME, name))
    print(zip_path, os.path.getsize(zip_path), "bytes, version", version())


if __name__ == "__main__":
    main()
