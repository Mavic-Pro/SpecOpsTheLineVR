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
  # A trailing backslash before the closing quote ("C:\Games\Spec Ops\") reaches
  # PowerShell as a stray quote; the folder, Binaries\Win32 or the exe all work.
  if ($game) {
    $game = $game.Trim().Trim('"').TrimEnd('\')
    if ($game -like '*.exe') { $game = Split-Path $game -Parent }
    if (Test-Path (Join-Path $game 'SpecOpsTheLine.exe')) { return $game }
  }
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
if (Get-Process SpecOpsTheLine -ErrorAction SilentlyContinue) { Write-Host 'Close the game first.'; exit 1 }
# Another d3d9.dll (ReShade, dgVoodoo...) is kept aside, uninstall puts it back.
$cur = Join-Path $dst 'd3d9.dll'
if ((Test-Path $cur) -and -not (Select-String -Path $cur -Pattern 'specopsvr' -SimpleMatch -Quiet)) {
  Copy-Item -Force $cur (Join-Path $dst 'd3d9.dll.other')
  Write-Host 'Another d3d9.dll was found and saved as d3d9.dll.other (uninstall.bat restores it).'
}
try {
  Copy-Item -Force -ErrorAction Stop (Join-Path $PSScriptRoot 'd3d9.dll'), (Join-Path $PSScriptRoot 'openvr_api.dll') $dst
} catch {
  Write-Host "Copy failed: $($_.Exception.Message)"
  exit 1
}
$ini = Join-Path $dst 'specopsvr.ini'
if (Test-Path $ini) {
  Copy-Item -Force (Join-Path $PSScriptRoot 'specopsvr.ini') "$ini.new"
  Write-Host 'Your specopsvr.ini was kept; the new defaults are in specopsvr.ini.new'
} else {
  Copy-Item (Join-Path $PSScriptRoot 'specopsvr.ini') $ini
}
Write-Host "SpecOpsTheLineVR installed in $dst"
Write-Host 'Connect the headset (Virtual Desktop, Link or SteamVR), then start the game.'
"""

UNINSTALL_PS1 = r"""
. (Join-Path $PSScriptRoot 'find_game.ps1')
$dst = Find-Game $args[0]
if (Get-Process SpecOpsTheLine -ErrorAction SilentlyContinue) { Write-Host 'Close the game first.'; exit 1 }
# d3d9.dll only if it is the mod's (a ReShade or dgVoodoo installed later stays).
$cur = Join-Path $dst 'd3d9.dll'
if ((Test-Path $cur) -and (Select-String -Path $cur -Pattern 'specopsvr' -SimpleMatch -Quiet)) {
  Remove-Item -Force -ErrorAction SilentlyContinue $cur
} elseif (Test-Path $cur) {
  Write-Host 'd3d9.dll is not the mod (another tool?): left in place.'
}
foreach ($f in 'openvr_api.dll', 'specopsvr_protube.exe', 'specopsvr_protube.exe.new', 'd3d9.dll.old', 'd3d9.dll.new', 'd3d9.dll.prev', 'd3d9.dll.rolledback') {
  Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $dst $f)
}
$other = Join-Path $dst 'd3d9.dll.other'
if ((Test-Path $other) -and -not (Test-Path (Join-Path $dst 'd3d9.dll'))) {
  Move-Item -Force $other (Join-Path $dst 'd3d9.dll')
  Write-Host 'Your previous d3d9.dll was put back.'
}
# Controller bindings written by the mod (SteamVR Input).
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue (Join-Path $dst 'specopsvr_input')
Write-Host "SpecOpsTheLineVR removed from $dst (specopsvr.ini and the logs were kept)."
Write-Host 'Remember to remove "-windowed ResX=2400 ResY=2500" from the Steam launch options.'

"""

ROLLBACK_PS1 = r"""
. (Join-Path $PSScriptRoot 'find_game.ps1')
$dst = Find-Game $args[0]
# A dll that never started leaves the version it replaced as d3d9.dll.old: that
# is the one to go back to (.prev can be two updates old).
$prev = Join-Path $dst 'd3d9.dll.old'
if (-not (Test-Path $prev)) { $prev = Join-Path $dst 'd3d9.dll.prev' }
if (-not (Test-Path $prev)) { Write-Host 'No previous version found (d3d9.dll.prev / .old).'; exit 1 }
if (Get-Process SpecOpsTheLine -ErrorAction SilentlyContinue) { Write-Host 'Close the game first.'; exit 1 }
Copy-Item -Force (Join-Path $dst 'd3d9.dll') (Join-Path $dst 'd3d9.dll.rolledback')
Copy-Item -Force $prev (Join-Path $dst 'd3d9.dll')
Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $dst 'specopsvr_update.pending'), (Join-Path $dst 'specopsvr_boot.count')
Add-Content -Path (Join-Path $dst 'specopsvr.ini') -Value "`r`nautoupdate off  # set by rollback.bat; remove to get updates again"
Write-Host 'The previous version is back. Automatic updates were turned off in specopsvr.ini.'
"""

RESET_PS1 = r"""
. (Join-Path $PSScriptRoot 'find_game.ps1')
$dst = Find-Game $args[0]
if (Get-Process SpecOpsTheLine -ErrorAction SilentlyContinue) { Write-Host 'Close the game first.'; exit 1 }
# The old settings are kept next to the new ones, nothing else is touched.
$ini = Join-Path $dst 'specopsvr.ini'
if (Test-Path $ini) {
  $bak = Join-Path $dst ('specopsvr.ini.bak-' + (Get-Date -Format 'yyyyMMdd_HHmmss'))
  Move-Item -Force $ini $bak
  Write-Host "Your settings were saved as $bak"
}
$shipped = Join-Path $PSScriptRoot 'specopsvr.ini'
if (Test-Path $shipped) { Copy-Item -Force $shipped $ini; Write-Host 'The shipped specopsvr.ini is in place.' }
else { Write-Host 'specopsvr.ini removed: the mod starts with its built-in defaults and recreates the file when you change a setting.' }
"""

COLLECT_PS1 = r"""
. (Join-Path $PSScriptRoot 'find_game.ps1')
$dst = Find-Game $args[0]
$files = Get-ChildItem -LiteralPath $dst -File | Where-Object {
  $_.Name -like 'specopsvr*.log' -or $_.Name -like 'specopsvr*.ini' -or $_.Name -like 'specopsvr_crash_*.dmp' }
if (-not $files) { Write-Host 'No SpecOpsTheLineVR logs found.'; exit 1 }
# Copied to a temporary folder first: the log of a running game is open and
# Compress-Archive would fail on it.
$tmp = Join-Path $env:TEMP ('SpecOpsTheLineVR_report_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $tmp | Out-Null
foreach ($f in $files) {
  try { [IO.File]::Copy($f.FullName, (Join-Path $tmp $f.Name), $true) } catch { Write-Host "Skipped $($f.Name): $($_.Exception.Message)" }
}
$inputDir = Join-Path $dst 'specopsvr_input'
if (Test-Path $inputDir) { Copy-Item -Recurse -Force $inputDir (Join-Path $tmp 'specopsvr_input') }
# SteamVR logs (input and headset problems).
$steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
if ($steam) {
  $vrlogs = Join-Path $steam 'logs'
  $vrdst = Join-Path $tmp 'steamvr_logs'
  New-Item -ItemType Directory -Force $vrdst | Out-Null
  foreach ($n in 'vrserver.txt', 'vrcompositor.txt', 'vrmonitor.txt', 'vrclient_SpecOpsTheLine.txt') {
    $src = Join-Path $vrlogs $n
    if (Test-Path $src) { try { [IO.File]::Copy($src, (Join-Path $vrdst $n), $true) } catch {} }
  }
}
$zip = Join-Path ([Environment]::GetFolderPath('Desktop')) ('SpecOpsTheLineVR_report_' + (Get-Date -Format 'yyyyMMdd_HHmmss') + '.zip')
Compress-Archive -Path (Join-Path $tmp '*') -DestinationPath $zip
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $tmp
Write-Host "Created $zip"
Write-Host 'Attach it to your issue on GitHub.'
"""


def bat(script, what):
    return ("@echo off\r\nrem " + what + "\r\npowershell -NoProfile -ExecutionPolicy Bypass -File \"%~dp0" + script +
            "\" %*\r\npause\r\n")


README_TXT = """SpecOpsTheLineVR {version} - unofficial VR mod for Spec Ops: The Line
==========================================================================

https://github.com/Mavic-Pro/SpecOpsTheLineVR

Requirements: Windows 10/11, Spec Ops: The Line (Steam) and a PC VR headset
with two motion controllers, through Virtual Desktop (VDXR) or SteamVR:
"runtime auto" (default) picks Virtual Desktop when its Streamer is running,
otherwise SteamVR. Developed and tested with a Meta Quest 3.

INSTALL
  1. Run install.bat (it finds the game through Steam; otherwise
     install.bat "D:\\SteamLibrary\\steamapps\\common\\SpecOps_TheLine").
  2. In Steam, game Properties > Launch options, add:
       -windowed ResX=2400 ResY=2500
     (the mod then uses the resolution the VR runtime recommends: change
     sharpness in Virtual Desktop / the Oculus app / the SteamVR slider).
  3. Connect the headset (Virtual Desktop, Link or SteamVR) and start the game.

UNINSTALL
  Run uninstall.bat. The game files are never modified: the mod only adds
  d3d9.dll and openvr_api.dll next to the game executable.

UPDATES
  The mod checks GitHub at start-up and installs new versions by itself for
  the next launch ("autoupdate off" in specopsvr.ini disables it). New
  versions first come out as test (pre-release) builds: "updatechannel beta"
  gets them too. If an update causes problems, close the game and run
  rollback.bat to go back to the previous version. A version that dies twice
  right after starting is put back automatically. The helper scripts and
  openvr_api.dll are refreshed with the updates; your specopsvr.ini is only
  migrated, never replaced (reset_settings.bat puts the defaults back,
  keeping a copy of yours).

CONTROLS (Touch layout; Index: A/B on each hand = A/B and X/Y, left trackpad
click = pause; every button can be changed in SteamVR > Settings >
Controllers > Manage controller bindings > Spec Ops: The Line)
  Left stick: move                 Right stick: turn
  Right trigger: fire              Left trigger: aim
  Left grip: grenade (LB), or hold the gun when your left hand is on it
  Right grip: squad orders (RB)
  A: action / cover                B: melee
  X: reload                        Y: next weapon
  Stick clicks: LS / RS            Left menu button: Start (pause)
  Right stick held + direction: D-pad (secondary fire, grenade type)
  Both sticks held for 1 second: recenter (stand straight, look ahead);
    keep holding for 3 seconds to also set and save your standing height
  You aim with the gun in your right hand: shots leave the barrel and land
  on the green marker. Put your left hand on the barrel to aim two-handed.
  Bring the gun up to your eye to aim down the sights.
  Gunstock users: aim the stock at something far away, looking at it in the
  middle of your view, and hold B + Y for 1 second (calibrates the gun angle).

GESTURES
  Grenade: quick forward throw with the left hand.
  Reload: grip the magazine under the gun with your left hand and pull it out,
    grip at your belt to take a new one, push it into the gun.
    ("reload simple" in specopsvr.ini: left hand on the magazine, pull down.)
  Cover: duck quickly next to cover (not while already in cover).
  Weapon: right hand over your right shoulder (or at your hip) + right grip
    = next weapon.
  Ropes, ziplines, turrets: the controller ticks when your hand is on the
    grab point; close the grip to use it.
  Melee: punch forward with the left hand at chest height, or jab/strike
    with the gun in your right hand.
  Cover: lean out sideways (or stand up) to pop out and aim.

HAPTIC DEVICES (optional)
  ProTubeVR ForceTube / ProVolver: install the free ProTubeVR Companion App
  (Steam), pair the devices in Windows, close the Companion App and start the
  game. bHaptics vest and sleeves: start the bHaptics Player before the
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
    # "python tools/package.py v0.9.7": the tag must match version.h, or the
    # released dll would report an older version and update itself forever.
    import sys
    if len(sys.argv) > 1 and sys.argv[1].lstrip("v") != version():
        raise SystemExit("tag %s does not match version.h %s" % (sys.argv[1], version()))
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
        "reset_settings.ps1": RESET_PS1,
        "reset_settings.bat": bat("reset_settings.ps1", "Puts the default specopsvr.ini back (your settings are kept as a .bak copy)."),
        "README.txt": README_TXT.format(version=version()),
    }
    for name, text in files.items():
        with open(os.path.join(DIST, name), "w", encoding="utf-8-sig" if name.endswith(".ps1") else "utf-8",
                  newline="" if name.endswith(".bat") else "\r\n") as f:
            f.write(text.lstrip("\n"))
    # Manifesto dei file compagni per l'aggiornamento automatico (R-R4): la
    # release deve avere come allegati, oltre al zip e a d3d9.dll, anche
    # companions.json e ogni file elencato (vedi il comando stampato in fondo).
    import hashlib
    companions = [n for n in sorted(os.listdir(DIST)) if n not in ("d3d9.dll", "specopsvr.ini")]
    entries = []
    for n in companions:
        data = open(os.path.join(DIST, n), "rb").read()
        entries.append({"name": n, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()})
    import json
    with open(os.path.join(DIST, "companions.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump({"version": version(), "files": entries}, f, indent=1)
    zip_path = os.path.join(ROOT, "dist", NAME + ".zip")
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for name in sorted(os.listdir(DIST)):
            z.write(os.path.join(DIST, name), os.path.join(NAME, name))
    print(zip_path, os.path.getsize(zip_path), "bytes, version", version())
    assets = " ".join('"%s"' % os.path.join(DIST, n) for n in ["d3d9.dll", "companions.json"] + companions)
    print("release assets:  gh release create v%s \"%s\" %s" % (version(), zip_path, assets))


if __name__ == "__main__":
    main()
