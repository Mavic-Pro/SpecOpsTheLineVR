"""Records what the headset shows and plays: the mod's mirror window
("mirrorwin on", right eye, always on top, never focused) captured by ffmpeg,
plus the audio of the Windows default playback device (WASAPI loopback,
build/Release/wasapiloop.exe), muxed into one mp4 at stop.
No OculusMirror: over Link it takes the VR focus away from SteamVR (22/10:
input focus captured, controllers dead, then the Link session dropped).

    python tools/mirrorrec.py start [out.mp4]   # opens the mirror window and records
    python tools/mirrorrec.py stop              # closes the file cleanly and the mirror window
    python tools/mirrorrec.py status

Requires the game running with the mod and ffmpeg (winget Gyan.FFmpeg).
"""
import glob
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gamectl  # noqa: E402

TEMP = os.environ.get('TEMP', '.')
STATE = os.path.join(TEMP, 'specopsvr_mirrorrec.json')
STOPFILE = os.path.join(TEMP, 'specopsvr_mirrorrec.stop')
TITLE = 'SpecOpsVR mirror'


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


def mirror_window():
    import ctypes
    import ctypes.wintypes as wt
    user32 = ctypes.windll.user32
    return user32.FindWindowW('SpecOpsVRMirror', None) or None


def start(out):
    if load():
        print('already recording:', load())
        return
    if not gamectl.game_pid():
        sys.exit('the game is not running')
    gamectl.send_cmds(['mirrorwin on'])
    for _ in range(20):
        if mirror_window():
            break
        time.sleep(0.5)
    else:
        sys.exit('mirror window not found (is the mod active?)')
    try:
        os.remove(STOPFILE)
    except OSError:
        pass
    out = os.path.abspath(out)
    # Helper detached from this console: keeps ffmpeg's stdin to send "q"
    # (clean mp4) when the stop file appears.
    helper = subprocess.Popen([sys.executable, os.path.abspath(__file__), '_run', out],
                              creationflags=subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.DETACHED_PROCESS,
                              stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(4)
    if helper.poll() is not None:
        sys.exit('recording helper exited at once, see ' + out + '.ffmpeg.log')
    json.dump({'helper': helper.pid, 'out': out}, open(STATE, 'w'))
    print('recording', out)


LOOP = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'build', 'Release', 'wasapiloop.exe')


def _run(out):
    ffmpeg = ffmpeg_exe()
    log = open(out + '.ffmpeg.log', 'w')
    video, wav = out + '.video.mp4', out + '.wav'
    cmd = [ffmpeg, '-y', '-loglevel', 'warning', '-f', 'gdigrab', '-framerate', '30', '-i', 'title=' + TITLE,
           '-c:v', 'libx264', '-preset', 'ultrafast', '-pix_fmt', 'yuv420p', '-vf', 'pad=ceil(iw/2)*2:ceil(ih/2)*2', video]
    # Audio: what the Windows default playback device plays (the headset's
    # device over Link or Virtual Desktop, where the mod sends the game).
    # CREATE_NO_WINDOW: the helper has no console, so a console child would
    # get a new terminal window that takes the foreground from the game
    # (22/10: the game's menu ignored the pad while ffmpeg's terminal had
    # the focus).
    noWin = subprocess.CREATE_NO_WINDOW
    audio = subprocess.Popen([LOOP, wav], stdout=log, stderr=log, creationflags=noWin) if os.path.exists(LOOP) else None
    rec = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=log, stderr=log, creationflags=noWin)
    while rec.poll() is None and not os.path.exists(STOPFILE):
        time.sleep(0.5)
    if audio and audio.poll() is None:
        open(wav + '.stop', 'w').close()
    if rec.poll() is None:
        try:
            rec.stdin.write(b'q')
            rec.stdin.flush()
        except OSError:
            pass
        for _ in range(40):
            if rec.poll() is not None:
                break
            time.sleep(0.5)
        if rec.poll() is None:
            rec.kill()
    if audio:
        for _ in range(40):
            if audio.poll() is not None:
                break
            time.sleep(0.5)
        if audio.poll() is None:
            audio.kill()
    # Mux: video + audio (both started together) into the final file.
    if audio and os.path.exists(wav) and os.path.getsize(wav) > 44:
        mux = subprocess.run([ffmpeg, '-y', '-loglevel', 'warning', '-i', video, '-i', wav, '-c:v', 'copy', '-c:a', 'aac',
                              '-b:a', '192k', '-shortest', out], stdout=log, stderr=log)
        if mux.returncode == 0:
            os.remove(video)
            os.remove(wav)
        else:
            os.replace(video, out)
    else:
        os.replace(video, out)
    try:
        os.remove(STOPFILE)
    except OSError:
        pass


def stop():
    st = load()
    if not st:
        print('not recording')
        return
    open(STOPFILE, 'w').close()
    for _ in range(40):
        time.sleep(0.5)
        if not os.path.exists(STOPFILE):
            break
    if gamectl.game_pid():
        gamectl.send_cmds(['mirrorwin off'])
    os.remove(STATE)
    size = os.path.getsize(st['out']) if os.path.exists(st['out']) else 0
    print('stopped:', st['out'], size, 'bytes')


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    if sys.argv[1] == 'start':
        start(sys.argv[2] if len(sys.argv) > 2 else time.strftime('mirror_%Y%m%d_%H%M%S.mp4'))
    elif sys.argv[1] == '_run':
        _run(sys.argv[2])
    elif sys.argv[1] == 'stop':
        stop()
    else:
        print(load() or 'not recording')
