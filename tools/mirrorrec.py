"""Records what the headset shows over Meta Link: OculusMirror (right eye,
undistorted, with the system UI) captured by ffmpeg into an mp4.

    python tools/mirrorrec.py start [out.mp4]   # opens the mirror and starts recording
    python tools/mirrorrec.py stop              # stops the recording and closes the mirror
    python tools/mirrorrec.py status

The pids are kept in %TEMP%\\specopsvr_mirrorrec.json. Requires the Oculus
software (OculusMirror.exe) and ffmpeg (winget Gyan.FFmpeg).
"""
import glob
import json
import os
import subprocess
import sys
import time

MIRROR = r'C:\Program Files\Oculus\Support\oculus-diagnostics\OculusMirror.exe'
STATE = os.path.join(os.environ.get('TEMP', '.'), 'specopsvr_mirrorrec.json')


def ffmpeg_exe():
    for c in ['ffmpeg'] + glob.glob(os.path.expandvars(
            r'%LOCALAPPDATA%\Microsoft\WinGet\Packages\Gyan.FFmpeg*\ffmpeg-*\bin\ffmpeg.exe')):
        try:
            subprocess.run([c, '-version'], capture_output=True, check=True)
            return c
        except (OSError, subprocess.CalledProcessError):
            continue
    sys.exit('ffmpeg not found')


def load():
    try:
        return json.load(open(STATE))
    except (OSError, ValueError):
        return {}


def start(out):
    if load():
        print('already recording:', load())
        return
    mirror = subprocess.Popen([MIRROR, '--RightEyeOnly', '--Size', '1600', '900', '--IncludeSystemGui',
                               '--IncludeNotifications', '--IncludeGuardian'])
    time.sleep(4)
    if mirror.poll() is not None:
        sys.exit('OculusMirror closed at once (headset not connected over Link?)')
    cmd = [ffmpeg_exe(), '-y', '-loglevel', 'error', '-f', 'gdigrab', '-framerate', '30', '-i', 'title=Oculus Mirror',
           '-c:v', 'libx264', '-preset', 'ultrafast', '-pix_fmt', 'yuv420p', '-vf', 'pad=ceil(iw/2)*2:ceil(ih/2)*2', out]
    rec = subprocess.Popen(cmd, stdin=subprocess.PIPE)
    time.sleep(3)
    if rec.poll() is not None:
        mirror.terminate()
        sys.exit('ffmpeg could not capture the mirror window')
    json.dump({'mirror': mirror.pid, 'ffmpeg': rec.pid, 'out': os.path.abspath(out)}, open(STATE, 'w'))
    print('recording', os.path.abspath(out))


def stop():
    st = load()
    if not st:
        print('not recording')
        return
    # "q" on stdin closes the file cleanly; ffmpeg was started by another
    # process, so a CTRL_BREAK is the next best (the moov atom is written on
    # a clean exit only).
    subprocess.run(['taskkill', '/PID', str(st['ffmpeg'])], capture_output=True)
    for _ in range(20):
        time.sleep(0.5)
        r = subprocess.run(['tasklist', '/FI', 'PID eq %d' % st['ffmpeg']], capture_output=True, text=True).stdout
        if str(st['ffmpeg']) not in r:
            break
    subprocess.run(['taskkill', '/PID', str(st['mirror'])], capture_output=True)
    os.remove(STATE)
    size = os.path.getsize(st['out']) if os.path.exists(st['out']) else 0
    print('stopped:', st['out'], size, 'bytes')


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    if sys.argv[1] == 'start':
        start(sys.argv[2] if len(sys.argv) > 2 else time.strftime('mirror_%Y%m%d_%H%M%S.mp4'))
    elif sys.argv[1] == 'stop':
        stop()
    else:
        print(load() or 'not recording')
