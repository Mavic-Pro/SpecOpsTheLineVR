"""Builds the release package in dist/0-SpecOpsTheLineVR.zip.

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
ZIP_NAME = "0-" + NAME + ".zip"


def version():
    with open(os.path.join(ROOT, "src", "version.h"), encoding="utf-8") as f:
        return re.search(r'SPECOPSVR_VERSION "([^"]+)"', f.read()).group(1)


# Shared by the scripts: finds the folder of SpecOpsTheLine.exe (Steam or any
# other copy) from the argument, the running game, Steam libraries, GOG/Epic/
# uninstall entries, the usual game folders and, last, a search of every drive.
FIND_GAME_PS1 = r"""
$ExeName = 'SpecOpsTheLine.exe'

# Folder containing the exe for a path: the exe itself, its folder, or a folder
# above it (searched down to $depth levels).
function Resolve-GameDir([string]$p, [int]$depth = 4) {
  if (-not $p) { return $null }
  # A trailing backslash before the closing quote ("C:\Games\Spec Ops\") reaches
  # PowerShell as a stray quote.
  $p = $p.Trim().Trim('"').TrimEnd('\')
  if (-not $p) { return $null }
  if ($p -like '*.exe') {
    if ((Split-Path $p -Leaf) -eq $ExeName -and (Test-Path -LiteralPath $p -PathType Leaf)) { return (Split-Path $p -Parent) }
    $p = Split-Path $p -Parent
  }
  if (-not $p -or -not (Test-Path -LiteralPath $p -PathType Container)) { return $null }
  if (Test-Path -LiteralPath (Join-Path $p $ExeName) -PathType Leaf) { return $p }
  if ($depth -le 0) { return $null }
  $hit = Get-ChildItem -LiteralPath $p -Filter $ExeName -File -Recurse -Depth $depth -ErrorAction SilentlyContinue |
    Select-Object -First 1
  if ($hit) { return $hit.DirectoryName }
  return $null
}

function Add-Candidate([System.Collections.ArrayList]$list, [string]$dir) {
  if (-not $dir) { return }
  $full = (Resolve-Path -LiteralPath $dir -ErrorAction SilentlyContinue).Path
  if (-not $full) { return }
  foreach ($c in $list) { if ($c -ieq $full) { return } }
  [void]$list.Add($full)
}

function Find-Candidates {
  $list = New-Object System.Collections.ArrayList
  # The game running right now.
  foreach ($pr in (Get-Process SpecOpsTheLine -ErrorAction SilentlyContinue)) {
    if ($pr.Path) { Add-Candidate $list (Split-Path $pr.Path -Parent) }
  }
  # Steam libraries (any Spec Ops folder under steamapps\common).
  $steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
  if ($steam) {
    $libs = @($steam)
    $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
    if (Test-Path $vdf) {
      foreach ($m in (Select-String -Path $vdf -Pattern '"path"\s+"([^"]+)"' -AllMatches).Matches) {
        $libs += ($m.Groups[1].Value -replace '\\+', '\')
      }
    }
    foreach ($l in $libs) {
      $common = Join-Path $l 'steamapps\common'
      if (Test-Path -LiteralPath $common) {
        foreach ($d in (Get-ChildItem -LiteralPath $common -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -like '*Spec*Ops*' })) {
          Add-Candidate $list (Resolve-GameDir $d.FullName 3)
        }
      }
    }
  }
  # Uninstall entries (GOG, retail, other launchers) and GOG's own keys.
  $keys = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*',
          'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*',
          'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*'
  foreach ($e in (Get-ItemProperty $keys -ErrorAction SilentlyContinue | Where-Object { $_.DisplayName -like '*Spec Ops*' })) {
    Add-Candidate $list (Resolve-GameDir $e.InstallLocation 3)
    if ($e.DisplayIcon) { Add-Candidate $list (Resolve-GameDir (($e.DisplayIcon -split ',')[0]) 0) }
  }
  foreach ($e in (Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\GOG.com\Games\*', 'HKLM:\SOFTWARE\GOG.com\Games\*' -ErrorAction SilentlyContinue |
                  Where-Object { $_.gameName -like '*Spec Ops*' })) {
    Add-Candidate $list (Resolve-GameDir $e.path 3)
  }
  # Epic Games Launcher manifests.
  $epic = Join-Path $env:ProgramData 'Epic\EpicGamesLauncher\Data\Manifests'
  if (Test-Path -LiteralPath $epic) {
    foreach ($f in (Get-ChildItem -LiteralPath $epic -Filter '*.item' -ErrorAction SilentlyContinue)) {
      try { $j = Get-Content -Raw -LiteralPath $f.FullName | ConvertFrom-Json } catch { continue }
      if ($j.DisplayName -like '*Spec Ops*') { Add-Candidate $list (Resolve-GameDir $j.InstallLocation 3) }
    }
  }
  if ($list.Count) { return ,$list }
  # The usual game folders on every fixed drive.
  $drives = @(Get-CimInstance Win32_LogicalDisk -Filter 'DriveType=3' -ErrorAction SilentlyContinue | ForEach-Object { $_.DeviceID + '\' })
  if (-not $drives) { $drives = @('C:\') }
  $roots = 'Games', 'Giochi', 'Program Files', 'Program Files (x86)', 'SteamLibrary', 'GOG Games', 'Epic Games', 'XboxGames'
  foreach ($d in $drives) {
    foreach ($r in $roots) {
      $p = Join-Path $d $r
      if (Test-Path -LiteralPath $p) {
        foreach ($h in (Get-ChildItem -LiteralPath $p -Filter $ExeName -File -Recurse -Depth 5 -ErrorAction SilentlyContinue)) {
          Add-Candidate $list $h.DirectoryName
        }
      }
    }
  }
  if ($list.Count) { return ,$list }
  # Last resort: every fixed drive.
  foreach ($d in $drives) {
    Write-Host "Searching $d for $ExeName ..."
    foreach ($h in (cmd /c dir /s /b /a-d ($d + $ExeName) 2>$null)) {
      if ($h -and (Test-Path -LiteralPath $h -PathType Leaf)) { Add-Candidate $list (Split-Path $h -Parent) }
    }
  }
  return ,$list
}

function Find-Game([string]$game) {
  if ($game) {
    $dir = Resolve-GameDir $game 6
    if ($dir) { return $dir }
    Write-Host "$ExeName was not found in: $game"
    Write-Host 'Pass the folder that contains the game, e.g.  install.bat "D:\Games\Spec Ops The Line"'
    exit 1
  }
  Write-Host "Looking for $ExeName ..."
  $list = Find-Candidates
  if (-not $list -or $list.Count -eq 0) {
    Write-Host ('Spec Ops: The Line (' + $ExeName + ') was not found. Run:  install.bat "<game folder>"')
    Write-Host 'e.g.  install.bat "D:\SteamLibrary\steamapps\common\SpecOps_TheLine"'
    exit 1
  }
  if ($list.Count -eq 1) { Write-Host "Game found: $($list[0])"; return $list[0] }
  Write-Host 'More than one copy of the game was found:'
  for ($i = 0; $i -lt $list.Count; $i++) {
    $mod = if (Test-Path -LiteralPath (Join-Path $list[$i] 'specopsvr.ini')) { '  (mod installed)' } else { '' }
    Write-Host ("  {0}) {1}{2}" -f ($i + 1), $list[$i], $mod)
  }
  $n = 1
  try {
    $a = Read-Host "Which one? [1-$($list.Count), Enter = 1]"
    if ($a -match '^\d+$' -and [int]$a -ge 1 -and [int]$a -le $list.Count) { $n = [int]$a }
  } catch {}
  Write-Host "Using: $($list[$n - 1])"
  return $list[$n - 1]
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

Requirements: Windows 10/11, Spec Ops: The Line (Steam; other copies work
but are untested) and a PC VR headset
with two motion controllers, through Virtual Desktop (VDXR) or SteamVR:
"runtime auto" (default) picks Virtual Desktop when its Streamer is running,
otherwise SteamVR. Developed and tested with a Meta Quest 3.

INSTALL
  1. Run install.bat. It looks for SpecOpsTheLine.exe (Steam libraries,
     GOG/Epic/other installs, usual game folders, then every drive); if it
     is not found, or to pick a copy, pass its folder:
     install.bat "D:\\Games\\Spec Ops The Line"
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
        "install.bat": bat("install.ps1", "Installs SpecOpsTheLineVR into the game folder (finds SpecOpsTheLine.exe)."),
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
    # GitHub lists release assets by name: the "0-" prefix puts the zip first,
    # above the updater's loose files (before, players had to expand the list).
    zip_path = os.path.join(ROOT, "dist", ZIP_NAME)
    if os.path.exists(os.path.join(ROOT, "dist", NAME + ".zip")):
        os.remove(os.path.join(ROOT, "dist", NAME + ".zip"))
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for name in sorted(os.listdir(DIST)):
            z.write(os.path.join(DIST, name), os.path.join(NAME, name))
    print(zip_path, os.path.getsize(zip_path), "bytes, version", version())
    assets = " ".join('"%s"' % os.path.join(DIST, n) for n in ["d3d9.dll", "companions.json"] + companions)
    print("release assets:  gh release create v%s \"%s\" %s" % (version(), zip_path, assets))


if __name__ == "__main__":
    main()
