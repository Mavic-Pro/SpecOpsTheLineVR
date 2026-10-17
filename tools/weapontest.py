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
COLPO_RE = re.compile(r'SHOT: from ' + VEC + r' to ' + VEC + r', game wanted ' + VEC + r', barrel -> ' + VEC +
                      r', barrel origin ' + VEC + r' axis ' + VEC + r', impact ' + VEC)


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
        # Classi astratte (UClass.ClassFlags +0xDC, CLASS_Abstract 0x1) e di
        # supporto (_Content): un GiveWeapon fallito lascia l'arsenale bloccato.
        if p.u32(x + 0xDC) & 1 or name.endswith('_Content'):
            continue
        # Non armi da fuoco del giocatore: esplosivi, coltello, cannello, armi
        # del multigiocatore, di prova, "nessuna arma", armi della squadra/IA.
        if re.search(r'_(Expl_|Knife|Blowtorch|MP[A-Z]|No[A-Z]|PDA|TestWeapon)|EliteAI|Squad$', name):
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
    """Da' l'arma, la equipaggia davvero (SwitchWeapon + EndSwitchWeapon: senza,
    il cambio restava a meta' e l'arma non sparava), spara col grilletto finto
    e misura per ogni colpo la distanza tra il punto d'impatto e il punto del
    mirino (dove arriva la canna)."""
    r = {'classe': cls}
    ensure_free()
    pos = ls.log_size()
    g.send_cmds(['pad fake 0 0 0 0 0 0 0 200'])
    time.sleep(0.4)
    g.send_cmds(['giveweapon ' + cls])
    time.sleep(1.0)
    g.send_cmds(['ucall arsenal SwitchWeapon %s 1' % cls])
    time.sleep(3.5)
    g.send_cmds(['ucall arsenal EndSwitchWeapon'])
    time.sleep(0.6)
    g.send_cmds(['ucall cheat AllAmmo'])
    time.sleep(0.6)
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
    if not COLPO_RE.search(ls.log_since(fpos)):  # grilletto ignorato: fuoco chiesto all'arsenale
        g.send_cmds(['ucall arsenal StartFire 0 0'])
        time.sleep(0.9)
        g.send_cmds(['ucall arsenal StopFire 0 0'])
        time.sleep(1.5)
    a1 = ammo(cls)
    r['munizioni'] = [a0, a1]
    shots = COLPO_RE.findall(ls.log_since(fpos))
    r['colpi'] = len(shots)
    angs, errs, offs = [], [], []
    for s in shots:
        v = [float(x) for x in s]
        start, end, target, borig, baxis, hit = v[0:3], v[3:6], v[9:12], v[12:15], v[15:18], v[18:21]
        ang = angle(sub(end, start), sub(target, start))
        if ang is not None:
            angs.append(ang)
        offs.append(dist_to_line(start, borig, baxis)[0])
        if norm(hit) > 1.0:
            errs.append(norm(sub(hit, target)))
    if shots:
        r['angolo_max'] = round(max(angs), 3) if angs else None
        r['partenza_fuori_asse_max'] = round(max(offs), 1)
        r['impatti_misurati'] = len(errs)
        r['errore_impatto_max'] = round(max(errs), 1) if errs else None
        r['errore_impatto_medio'] = round(sum(errs) / len(errs), 1) if errs else None
        ok = angs and max(angs) < 0.5 and max(offs) < 1.0 and errs and max(errs) < 5.0
        r['esito'] = 'OK' if ok else 'DA VERIFICARE'
    else:
        fired = a0 is not None and a1 is not None and a1 < a0
        r['esito'] = 'a proiettile (ha sparato)' if fired else 'non ha sparato'
    src = os.path.join(g.SHOT_DIR, 'wt_%s.bmp' % cls)
    try:
        from PIL import Image
        Image.open(src).resize((480, 500)).save(os.path.join(d, cls + '.png'))
        os.remove(src)
    except Exception as e:  # noqa: BLE001
        r['foto'] = str(e)
    return r


MAP = 'SP_Gorge02_P'
SETUP = ['cheats', 'ucall cheat God', 'firelog on', 'synth 0 -30 0', 'synthaim 0 -35', 'synthhand 40 20 -30',
         'synthleft 0 0 0', 'mode synth']


def fresh_level():
    """Gioco riavviato da zero e livello caricato una volta: dopo ricaricamenti
    ripetuti con la console, o dopo un GiveWeapon fallito, il fuoco smette di
    funzionare; con un avvio pulito le armi sparano una dopo l'altra."""
    ls.gamectl('kill')
    g.launch(['-nostartupmovies', '-windowed', 'ResX=2400', 'ResY=2500'])
    time.sleep(55)
    g.focus()
    g.send_cmds(['console open TheLine_P?mission=%s?game=SRAssets.YGameInfo_SP_Content' % MAP])
    time.sleep(45)
    ensure_free()
    for c in SETUP:
        g.send_cmds([c])
        time.sleep(0.6)
    time.sleep(1.0)


def main():
    os.makedirs(OUT, exist_ok=True)
    ls.backup_saves()
    ls.ensure_game()
    classes = sys.argv[1:] or weapon_classes()
    print('armi da provare:', len(classes), flush=True)
    fresh_level()
    report = []

    def show(cls, r):
        print('%-32s %-26s colpi=%s impatto-mirino max/medio=%s/%s angolo=%s fuori asse=%s munizioni=%s' % (
            cls, r.get('esito'), r.get('colpi'), r.get('errore_impatto_max'), r.get('errore_impatto_medio'),
            r.get('angolo_max'), r.get('partenza_fuori_asse_max'), r.get('munizioni')), flush=True)

    try:
        for cls in classes:
            r = test_weapon(cls, OUT)
            if r.get('esito') in ('non ha sparato', 'non equipaggiata') or not ls.running():
                # conferma su un gioco appena avviato
                fresh_level()
                r = test_weapon(cls, OUT)
                r['ripetuta'] = True
                fresh_level()
            report.append(r)
            show(cls, r)
            json.dump(report, open(os.path.join(OUT, 'rapporto.json'), 'w', encoding='utf-8'), indent=1,
                      ensure_ascii=False)
    finally:
        g.send_cmds(['synthaim 0 0', 'synth 0 0 0', 'mode hmd'])
        ls.gamectl('kill')
        ls.restore_saves()


if __name__ == '__main__':
    main()
