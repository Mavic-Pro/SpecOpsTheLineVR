"""Release gate: automatic regression test of the VR mod (no headset needed).

Starts the game in synthetic-pose mode, loads a combat level and runs short
phases while reading the STATS / SHOT / WATCHDOG lines of the log:

  stand     still, 20 s           -> eye pairing (no repeated / unknown eyes)
  sweep     gun swept, head still -> view must not jump (cover jitter, 0.9.2)
  move      walking               -> eye pairing while moving
  checkpoint  'ce' jump + cutscene -> eye pairing right after the cinema screen
  fire      M4 burst              -> shots along the barrel (< 0.3 degrees)
  gestures  simulated physical reload -> "magazine inserted" in the log
  cinema    SetCinematicMode on/off -> arms released and re-acquired, no crash

Before the game: build/Release/unit_tests.exe (pure functions of the updater
and the ini parser) must pass.

Exit code 0 = all checks passed, 1 = something failed (details printed and
saved to shots/regress.json). Saves are backed up and restored.

    python tools/regress.py
"""
import json
import math
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import gamectl as g  # noqa: E402
import levelsweep as ls  # noqa: E402
import deepsweep as ds  # noqa: E402

MAP = 'SP_Gorge02_P'
STATS = re.compile(r'STATS: present L=(\d+) R=(\d+) unknown=(\d+) repeated=(\d+) \| eye misses=\d+ \| '
                   r'head blocked=\d+ \| cinema switches=(\d+) \| max view jump=([\d.]+) \| cinema=(\d)'
                   r'(?: \| frame tags disagree=(\d+) resync=(\d+) empty=(\d+))?')
VEC = r'\(([-\d.]+), ([-\d.]+), ([-\d.]+)\)'
SHOT = re.compile(r'SHOT: from ' + VEC + r' to ' + VEC + r', game wanted ' + VEC + r', barrel -> ' + VEC +
                  r', barrel origin ' + VEC + r' axis ' + VEC + r', impact ' + VEC)

# Thresholds (per phase, gameplay STATS lines only).
MAX_REPEATED_FRAC = 0.005   # repeated eyes / presents
MAX_LR_IMBALANCE = 0.01     # |L - R| / presents (plus 2 frames of slack)
MAX_VIEW_JUMP_STILL = 3.0   # units per tick while the body does not move
MAX_SHOT_ANGLE = 0.3        # degrees between shot direction and barrel


def stats_since(pos):
    rows = []
    for m in STATS.finditer(ls.log_since(pos)):
        L, R, unk, rep, csw = (int(x) for x in m.groups()[:5])
        rows.append({'L': L, 'R': R, 'unknown': unk, 'repeated': rep, 'cinema_switches': csw,
                     'jump': float(m.group(6)), 'cinema': m.group(7) == '1',
                     'tag_disagree': int(m.group(8) or 0), 'tag_empty': int(m.group(10) or 0)})
    return rows


def check_pairing(name, rows, failures, skip_first=0):
    game = [r for r in rows[skip_first:] if not r['cinema'] and r['L'] + r['R'] > 0]
    if not game:
        failures.append('%s: no gameplay STATS lines' % name)
        return {}
    L = sum(r['L'] for r in game)
    R = sum(r['R'] for r in game)
    rep = sum(r['repeated'] for r in game)
    tot = L + R
    dis = sum(r['tag_disagree'] for r in game)
    res = {'L': L, 'R': R, 'repeated': rep, 'lines': len(game), 'tag_disagree': dis}
    if dis > 0.01 * tot:
        failures.append('%s: frame tag and eye origin disagree %d times' % (name, dis))
    if rep > MAX_REPEATED_FRAC * tot:
        failures.append('%s: repeated eyes %d of %d presents' % (name, rep, tot))
    if abs(L - R) > MAX_LR_IMBALANCE * tot + 2:
        failures.append('%s: left/right imbalance L=%d R=%d' % (name, L, R))
    return res


def phase(name, cmds, seconds):
    pos = ls.log_size()
    g.focus()
    for c in cmds:
        g.send_cmds([c])
        time.sleep(0.3)
    time.sleep(seconds)
    return pos


def unit_tests(failures):
    exe = os.path.join(ROOT, 'build', 'Release', 'unit_tests.exe')
    if not os.path.exists(exe):
        failures.append('unit: build/Release/unit_tests.exe missing (cmake --build build --config Release --target unit_tests)')
        return 'missing'
    r = subprocess.run([exe], capture_output=True, text=True)
    out = (r.stdout or '').strip().splitlines()
    if r.returncode != 0:
        failures.append('unit: ' + (out[-1] if out else 'exit %d' % r.returncode))
    return out[-1] if out else 'no output'


def main():
    failures, report = [], {}
    report['unit'] = unit_tests(failures)
    print('unit tests:', report['unit'])
    if failures:
        print('RESULT: FAIL')
        sys.exit(1)
    ls.backup_saves()
    try:
        ls.gamectl('kill')
        g.launch(['-nostartupmovies', '-windowed', 'ResX=2400', 'ResY=2500'])
        time.sleep(55)
        g.focus()
        g.send_cmds(['console open TheLine_P?mission=%s?game=SRAssets.YGameInfo_SP_Content' % MAP])
        # God mode as soon as gameplay starts: after a fixed 45 s wait Walker
        # (still, leaning out of cover) could already be dead and the phases
        # ran on the respawn (20/10).
        time.sleep(20)
        ds.wait_gameplay([], 90)
        for c in ['cheats', 'ucall cheat God', 'firelog on', 'spread off', 'mode synth', 'synth 0 -10 0',
                  'synthaim 0 -10', 'synthhand 40 20 -20']:
            g.send_cmds([c])
            time.sleep(0.6)
        start = ls.log_size()

        pos = phase('stand', [], 20)
        report['stand'] = check_pairing('stand', stats_since(pos), failures)

        # Gun swept left/right with head and body still: the view must not move.
        pos = ls.log_size()
        for yaw in (-40, -20, 0, 20, 40, 0):
            g.send_cmds(['synthaim %d -10' % yaw])
            time.sleep(1.5)
        time.sleep(4)
        rows = stats_since(pos)
        report['sweep'] = check_pairing('sweep', rows, failures)
        jump = max((r['jump'] for r in rows if not r['cinema']), default=0.0)
        report['sweep']['max_view_jump'] = jump
        if jump > MAX_VIEW_JUMP_STILL:
            failures.append('sweep: view jumped %.1f units with the body still' % jump)

        pos = phase('move', ['pad fake 0 0.8 0 0 0 0 0 6000'], 8)
        report['move'] = check_pairing('move', stats_since(pos), failures)

        cps = ds.checkpoint_events()[0]
        if cps:
            pos = ls.log_size()
            g.send_cmds(['ce ' + cps[0]])
            time.sleep(8)
            ds.wait_gameplay([], 60)
            for c in ['mode synth', 'synth 0 -10 0', 'synthaim 0 -10']:
                g.send_cmds([c])
                time.sleep(0.4)
            time.sleep(15)
            report['checkpoint'] = check_pairing('checkpoint ' + cps[0], stats_since(pos), failures)
        else:
            report['checkpoint'] = 'no checkpoint events'

        pos = ls.log_size()
        g.focus()
        g.send_cmds(['ucall cheat AllAmmo', 'pad fake 0 0 0 0 0 1 0 1500'])
        time.sleep(3)
        angles = []
        for m in SHOT.finditer(ls.log_since(pos)):
            v = [float(x) for x in m.groups()]
            d = [v[3 + i] - v[i] for i in range(3)]
            a = v[15:18]
            nd = math.sqrt(sum(x * x for x in d))
            na = math.sqrt(sum(x * x for x in a))
            if nd > 1 and na > 0.5:
                c = sum(d[i] * a[i] for i in range(3)) / (nd * na)
                angles.append(math.degrees(math.acos(max(-1.0, min(1.0, c)))))
        report['fire'] = {'shots': len(angles), 'max_angle': round(max(angles), 3) if angles else None}
        if not angles:
            failures.append('fire: no SHOT lines')
        elif max(angles) > MAX_SHOT_ANGLE:
            failures.append('fire: shot %.2f degrees off the barrel' % max(angles))

        # Physical reload simulated from fake poses: the state machine must
        # reach "magazine inserted" (gesture pipeline alive, M14 kept).
        # The synthetic left hand sits on the gun (two-hand grip) and would
        # block the magazine grab: move it away first, as m14test does.
        g.send_cmds(['synthleft 10 -45 -25', 'reload physical'])
        time.sleep(1.5)
        pos = ls.log_size()
        g.send_cmds(['gesturesim reload'])
        time.sleep(4)
        txt = ls.log_since(pos)
        report['gestures'] = {'magazine_inserted': 'GESTURE: magazine inserted' in txt}
        if not report['gestures']['magazine_inserted']:
            failures.append('gestures: simulated physical reload never reached "magazine inserted"')

        # Game-driven cinematic on Walker (H1): arms must be released while
        # bCinematicMode is on and come back afterwards, with stereo intact.
        pos = ls.log_size()
        g.send_cmds(['ucall pc SetCinematicMode 1 0 0 1 1 1'])
        time.sleep(6)
        g.send_cmds(['ucall pc SetCinematicMode 0 0 0 1 1 1'])
        time.sleep(8)
        txt = ls.log_since(pos)
        report['cinema'] = check_pairing('cinema', stats_since(pos), failures)
        report['cinema']['arms_reacquired'] = bool(re.search(r"ARMS: Walker's IK controls", txt))
        if not report['cinema']['arms_reacquired']:
            failures.append('cinema: arms not re-acquired after SetCinematicMode 0')

        txt = ls.log_since(start)
        stalls = re.findall(r'WATCHDOG: render thread stalled for (\d+)s', txt)
        report['watchdog'] = len(stalls)
        if stalls:
            failures.append('watchdog: render thread stalled (%s s)' % ', '.join(stalls[:5]))
        if re.search(r'CRASH|exception in', txt):
            failures.append('crash/exception lines in the log')
    finally:
        g.send_cmds(['synthaim 0 0', 'synth 0 0 0', 'mode hmd'])
        time.sleep(1)
        ls.gamectl('kill')
        ls.restore_saves()
    report['failures'] = failures
    os.makedirs(os.path.join(ROOT, 'shots'), exist_ok=True)
    json.dump(report, open(os.path.join(ROOT, 'shots', 'regress.json'), 'w'), indent=1)
    print(json.dumps(report, indent=1))
    print('RESULT:', 'PASS' if not failures else 'FAIL')
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
