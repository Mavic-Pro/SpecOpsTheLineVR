"""Angular error of every shot, per weapon, from the mod log.

Reads the COLPO lines (hook on YWeapon::CalcWeaponFire, with "firelog on") and
the ARMA lines (current weapon). For each shot: angle at the muzzle between the
real impact and the point the barrel points at (the green marker). Unlike the
distance on the ground this does not depend on the scene (grazing hits).

    python tools/shotreport.py [log ...]   (default: the last four sessions)
"""
import math
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import gamectl as g  # noqa: E402

VEC = r'\(([-\d.]+), ([-\d.]+), ([-\d.]+)\)'
# English log (current) and Italian log (versions before 0.9.0).
COLPO = re.compile(r'(?:SHOT: from|COLPO: da) ' + VEC + r' (?:to|verso) ' + VEC + r', (?:game wanted|il gioco voleva) ' + VEC +
                   r', (?:barrel|canna) -> ' + VEC + r', (?:barrel origin|canna origine) ' + VEC + r' (?:axis|asse) ' + VEC +
                   r', (?:impact|impatto) ' + VEC)
ARMA = re.compile(r'(?:WEAPON|ARMA): (\S+)')


def angle(a, b):
    na = math.sqrt(sum(x * x for x in a))
    nb = math.sqrt(sum(x * x for x in b))
    if na < 1e-6 or nb < 1e-6:
        return None
    c = sum(a[i] * b[i] for i in range(3)) / (na * nb)
    return math.degrees(math.acos(max(-1.0, min(1.0, c))))


def main():
    paths = sys.argv[1:] or [os.path.join(g.GAME_DIR, n) for n in
                             ('specopsvr.3.log', 'specopsvr.2.log', 'specopsvr.1.log', 'specopsvr.log')]
    lines = []
    for path in paths:  # anche le sessioni precedenti (il log ruota a ogni avvio)
        if os.path.exists(path):
            lines += open(path, encoding='utf-8', errors='replace').readlines()
    weapon = '?'
    per = {}
    for line in lines:
        m = ARMA.search(line)
        if m:
            weapon = m.group(1)
            continue
        m = COLPO.search(line)
        if not m:
            continue
        v = [float(x) for x in m.groups()]
        start, target, axis, hit = v[0:3], v[9:12], v[15:18], v[18:21]
        if math.sqrt(sum(x * x for x in hit)) < 1.0:
            continue  # nessun impatto (cielo)
        shot = [hit[i] - start[i] for i in range(3)]
        a_marker = angle(shot, [target[i] - start[i] for i in range(3)])
        a_barrel = angle(shot, axis)
        if a_marker is not None and a_barrel is not None:
            per.setdefault(weapon, []).append((a_barrel, a_marker))

    def stats(xs):
        xs = sorted(xs)
        return xs[len(xs) // 2], xs[min(len(xs) - 1, int(len(xs) * 0.9))], xs[-1]

    print('%-30s %5s   %-24s   %-24s' % ('arma', 'colpi', 'vs canna: med/p90/max', 'vs mirino: med/p90/max'))
    for w, xs in sorted(per.items()):
        b = stats([x[0] for x in xs])
        m = stats([x[1] for x in xs])
        print('%-30s %5d   %6.2f %6.2f %6.2f      %6.2f %6.2f %6.2f' % ((w, len(xs)) + b + m))


if __name__ == '__main__':
    main()
