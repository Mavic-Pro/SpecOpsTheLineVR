"""Collaudo della mira di tutte le armi del giocatore.

Per ogni classe d'arma (sottoclassi di YWeapon, escluse postazioni fisse,
modalita' secondarie "_Scnd" e armi dell'IA "_AI"): la da' a Walker, la rende
corrente, spara una raffica con una posa fissa del controller finto e legge le
righe COLPO della dll (hook su YWeapon::CalcWeaponFire). Misura:
  - angolo tra la direzione del colpo e il punto in cui arriva la canna (0 = giusto);
  - distanza dell'origine del colpo dall'asse della canna (il colpo parte dalla canna).
Armi senza righe COLPO = a proiettile (razzi, granate): segnalate a parte, con
la variazione delle munizioni per capire se hanno sparato.

    python tools/weapontest.py                     # tutte
    python tools/weapontest.py YWeapon_M4Carbine   # solo alcune
Rapporto: shots/weapons/rapporto.json e foto per arma.
"""
import json
import math
import os
import re
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import gamectl as g  # noqa: E402
import levelsweep as ls  # noqa: E402
import ue3  # noqa: E402
import deepsweep as ds  # noqa: E402

OUT = os.path.join(ROOT, 'shots', 'weapons')
VEC = r'\(([-\d.]+), ([-\d.]+), ([-\d.]+)\)'
COLPO_RE = re.compile(r'COLPO: da ' + VEC + r' verso ' + VEC + r', il gioco voleva ' + VEC + r', canna -> ' + VEC +
                      r', canna origine ' + VEC + r' asse ' + VEC)


def weapon_classes():
    p = ue3.Proc()
    n = ue3.Names(p)
    o = ue3.Objects(p, n)
    base = o.find_class('YWeapon')
    stationary = o.find_class('YWeapon_Stationary')
    out = []
    for x in o.ptrs:
        if not x or o.name(o.cls(x)) != 'Class':
            continue
        chain, c = [], x
        for _ in range(12):
            if not c:
                break
            chain.append(c)
            c = p.u32(c + ue3.STRUCT_SUPER)
        name = o.name(x)
        if base not in chain or x == base or stationary in chain:
            continue
        if '_Scnd' in name or '_AI' in name or name.endswith('Base') or name.startswith('Default'):
            continue
        out.append(name)
    return sorted(set(out))


def ammo(cls):
    p = ue3.Proc()
    n = ue3.Names(p)
    o = ue3.Objects(p, n)
    c = o.find_class(cls)
    for x in o.ptrs:
        if x and o.cls(x) == c and not o.name(x).startswith('Default'):
            return p.i32(x + 0x764)
    return None


def sub(a, b):
    return [a[i] - b[i] for i in range(3)]


def norm(v):
    return math.sqrt(sum(x * x for x in v))


def angle(a, b):
    na, nb = norm(a), norm(b)
    if na < 1e-6 or nb < 1e-6:
        return None
    c = sum(a[i] * b[i] for i in range(3)) / (na * nb)
    return math.degrees(math.acos(max(-1.0, min(1.0, c))))


def dist_to_line(pt, origin, axis):
    d = sub(pt, origin)
    t = sum(d[i] * axis[i] for i in range(3))
    perp = [d[i] - t * axis[i] for i in range(3)]
    return norm(perp), t


def ensure_free():
    """Gameplay libero: cinematiche saltate e, se resta la modalita' cinematica
    (filmato di passaggio mai partito), uscita forzata."""
    st = ls.state_sample()
    if st and 'gameplay=1' in st and 'cinema=0' in st:
        return
    ds.wait_gameplay([], 40)
    st = ls.state_sample()
    if st and 'cinematic=1' in st and 'viewtarget=YPawn_Walker' in st:
        g.send_cmds(['ucall pc SetCinematicMode 0 0 1 1 1 1'])
        time.sleep(2)
    g.send_cmds(['mode synth'])
    time.sleep(0.5)


def test_weapon(cls, d):
    r = {'classe': cls}
    # Cinematiche partite nel frattempo: saltate (A/B) prima di provare l'arma.
    ensure_free()
    pos = ls.log_size()
    g.send_cmds(['pad fake 0 0 0 0 0 0 0 200'])  # grilletto rilasciato
    time.sleep(0.5)
    g.send_cmds(['giveweapon ' + cls])
    time.sleep(4.0)  # animazione di estrazione
    g.send_cmds(['ucall cheat AllAmmo'])
    time.sleep(1.0)
    st = ls.state_sample()
    r['stato'] = st
    if not st or ('arma=' + cls) not in st:
        r['esito'] = 'non equipaggiata'
        r['log'] = re.findall(r'GIVEWEAPON[^\n]*', ls.log_since(pos))[-1:]
        return r
    a0 = ammo(cls)
    fpos = ls.log_size()
    g.focus()
    g.send_cmds(['pad fake 0 0 0 0 0 1 0 900'])
    time.sleep(0.5)
    g.send_cmds(['shot wt_' + cls])
    time.sleep(2.0)
    a1 = ammo(cls)
    r['munizioni'] = [a0, a1]
    shots = COLPO_RE.findall(ls.log_since(fpos))
    r['colpi'] = len(shots)
    angs, offs, ahead = [], [], []
    for s in shots:
        v = [float(x) for x in s]
        start, end, target, borig, baxis = v[0:3], v[3:6], v[9:12], v[12:15], v[15:18]
        ang = angle(sub(end, start), sub(target, start))
        if ang is not None:
            angs.append(ang)
        off, t = dist_to_line(start, borig, baxis)
        offs.append(off)
        ahead.append(t)
    if shots:
        r['angolo_max'] = round(max(angs), 3) if angs else None
        r['partenza_fuori_asse_max'] = round(max(offs), 1)
        r['partenza_lungo_canna'] = [round(min(ahead), 1), round(max(ahead), 1)]
        ok = angs and max(angs) < 0.5 and max(offs) < 6.0
        r['esito'] = 'OK' if ok else 'DA VERIFICARE'
    else:
        r['esito'] = 'nessun colpo istantaneo' + (' (ha sparato: proiettile)' if a0 is not None and a1 is not None and a1 < a0 else '')
    src = os.path.join(g.SHOT_DIR, 'wt_%s.bmp' % cls)
    try:
        from PIL import Image
        Image.open(src).resize((480, 500)).save(os.path.join(d, cls + '.png'))
        os.remove(src)
    except Exception as e:  # noqa: BLE001
        r['foto'] = str(e)
    return r


def main():
    os.makedirs(OUT, exist_ok=True)
    ls.backup_saves()
    ls.ensure_game()
    g.focus()
    # Inizio di Gorge02: gameplay subito, nessuna cinematica, si puo' sparare
    # (Desert01 aperto dal collaudo resta in modalita' cinematica e blocca gli spari).
    g.send_cmds(['console open TheLine_P?mission=SP_Gorge02_P?game=SRAssets.YGameInfo_SP_Content'])
    time.sleep(45)
    ensure_free()
    for c in ['cheats', 'ucall cheat God', 'firelog on', 'synth 0 -25 0', 'synthaim 12 -20', 'synthhand 40 20 -30',
              'synthleft 0 0 0', 'mode synth']:
        g.send_cmds([c])
        time.sleep(0.8)
    classes = sys.argv[1:] or weapon_classes()
    print('armi da provare:', len(classes), flush=True)
    report = []
    try:
        for cls in classes:
            r = test_weapon(cls, OUT)
            report.append(r)
            print('%-32s %-28s colpi=%s angolo=%s fuori asse=%s munizioni=%s' % (
                cls, r.get('esito'), r.get('colpi'), r.get('angolo_max'), r.get('partenza_fuori_asse_max'),
                r.get('munizioni')), flush=True)
            json.dump(report, open(os.path.join(OUT, 'rapporto.json'), 'w', encoding='utf-8'), indent=1,
                      ensure_ascii=False)
            if not ls.running():
                print('gioco chiuso', flush=True)
                break
    finally:
        g.send_cmds(['synthaim 0 0', 'synth 0 0 0', 'mode hmd'])


if __name__ == '__main__':
    main()
