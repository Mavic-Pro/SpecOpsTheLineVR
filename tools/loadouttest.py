"""Mira delle armi della campagna, cosi' come il giocatore le ha.

Per ogni livello: lo carica (TheLine_P?mission=...), salta le cinematiche,
attiva God e munizioni, spara con l'arma iniziale e, dopo Y (arma seguente),
con la seconda. Le righe COLPO della dll (hook su YWeapon::CalcWeaponFire)
danno l'angolo tra il colpo e il punto in cui arriva la canna (0 = giusto) e la
distanza dell'origine del colpo dall'asse della canna. Armi che sparano senza
righe COLPO = a proiettile (razzi, granate). Con GiveWeapon il Pawn restava
bloccato dopo la prima arma: qui si usano solo le armi date dal gioco.

    python tools/loadouttest.py                 # tutti i livelli
    python tools/loadouttest.py SP_Gorge02_P    # solo alcuni
Rapporto: shots/loadout/rapporto.json e foto.
"""
import json
import os
import re
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import gamectl as g  # noqa: E402
import levelsweep as ls  # noqa: E402
import weapontest as wt  # noqa: E402

OUT = os.path.join(ROOT, 'shots', 'loadout')


def current_weapon():
    st = ls.state_sample() or ''
    m = re.search(r'arma=(\S*)', st)
    return (m.group(1) if m else ''), st


def fire_and_measure(tag, d):
    cls, st = current_weapon()
    r = {'arma': cls, 'stato': st}
    if not cls or 'gameplay=1' not in st:
        r['esito'] = 'niente gameplay/arma'
        return r
    # I livelli aperti dal collaudo restano spesso in modalita' cinematica, che
    # blocca i comandi (fuoco, cambio arma, pausa): uscita forzata prima di sparare.
    g.send_cmds(['ucall pc SetCinematicMode 0 0 1 1 1 1'])
    time.sleep(1.0)
    a0 = wt.ammo(cls)
    pos = ls.log_size()
    g.focus()
    g.send_cmds(['pad fake 0 0 0 0 0 1 0 900'])
    time.sleep(0.5)
    g.send_cmds(['shot lo_%s' % tag])
    time.sleep(2.0)
    a1 = wt.ammo(cls)
    shots = wt.COLPO_RE.findall(ls.log_since(pos))
    r['munizioni'] = [a0, a1]
    r['colpi'] = len(shots)
    angs, offs = [], []
    for s in shots:
        v = [float(x) for x in s]
        start, end, target, borig, baxis = v[0:3], v[3:6], v[9:12], v[12:15], v[15:18]
        a = wt.angle(wt.sub(end, start), wt.sub(target, start))
        if a is not None:
            angs.append(a)
        offs.append(wt.dist_to_line(start, borig, baxis)[0])
    if shots:
        r['angolo_max'] = round(max(angs), 3) if angs else None
        r['partenza_fuori_asse_max'] = round(max(offs), 1)
        r['esito'] = 'OK' if angs and max(angs) < 0.5 else 'DA VERIFICARE'
    else:
        fired = a0 is not None and a1 is not None and a1 < a0
        r['esito'] = 'a proiettile (sparato, nessun colpo istantaneo)' if fired else 'non ha sparato'
    try:
        from PIL import Image
        src = os.path.join(g.SHOT_DIR, 'lo_%s.bmp' % tag)
        Image.open(src).resize((480, 500)).save(os.path.join(d, tag + '.png'))
        os.remove(src)
    except Exception as e:  # noqa: BLE001
        r['foto'] = str(e)
    return r


def test_map(name):
    rep = {'mappa': name}
    ls.ensure_game()
    g.focus()
    g.send_cmds(['console open TheLine_P?mission=%s?game=SRAssets.YGameInfo_SP_Content' % name])
    time.sleep(45)
    for c in ['cheats', 'ucall cheat God', 'ucall cheat AllAmmo', 'firelog on']:
        g.send_cmds([c])
        time.sleep(0.8)
    wt.ensure_free()
    for c in ['synth 0 -25 0', 'synthaim 12 -20', 'synthhand 40 20 -30', 'synthleft 0 0 0', 'mode synth']:
        g.send_cmds([c])
        time.sleep(0.5)
    time.sleep(1.5)
    rep['prima'] = fire_and_measure(name + '_1', OUT)
    g.send_cmds(['ucall pc SetCinematicMode 0 0 1 1 1 1'])
    time.sleep(1.0)
    g.send_cmds(['pad fake 0 0 0 0 0 0 8000 200'])  # Y: arma seguente
    time.sleep(3.5)
    g.send_cmds(['ucall cheat AllAmmo'])
    time.sleep(0.8)
    rep['seconda'] = fire_and_measure(name + '_2', OUT)
    return rep


def main():
    os.makedirs(OUT, exist_ok=True)
    ls.backup_saves()
    maps = sys.argv[1:] or ls.MAPS
    path = os.path.join(OUT, 'rapporto.json')
    report = json.load(open(path, encoding='utf-8')) if os.path.exists(path) else {}
    try:
        for m in maps:
            try:
                r = test_map(m)
            except Exception as e:  # noqa: BLE001
                r = {'mappa': m, 'errore': repr(e)}
            report[m] = r
            json.dump(report, open(path, 'w', encoding='utf-8'), indent=1, ensure_ascii=False)
            for k in ('prima', 'seconda'):
                x = r.get(k) or {}
                print('%-22s %-7s %-28s %-40s colpi=%s angolo=%s fuori asse=%s munizioni=%s' % (
                    m, k, x.get('arma'), x.get('esito'), x.get('colpi'), x.get('angolo_max'),
                    x.get('partenza_fuori_asse_max'), x.get('munizioni')), flush=True)
            if r.get('errore'):
                print(m, 'ERRORE', r['errore'], flush=True)
    finally:
        g.send_cmds(['synthaim 0 0', 'synth 0 0 0', 'mode hmd'])
        ls.gamectl('kill')
        ls.restore_saves()


if __name__ == '__main__':
    main()
