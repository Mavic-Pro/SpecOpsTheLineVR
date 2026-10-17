"""Collaudo profondo: ogni checkpoint di ogni livello della campagna.

I livelli hanno eventi Kismet da console lasciati dagli sviluppatori per
saltare ai checkpoint (cp01, cp02, CP03, ...; li usava il loro test
automatico YSeqAct_AutoTestInit). Per ogni livello: lo carica, attiva il
CheatManager e God (comando "cheats" della dll), legge dalla memoria gli
eventi che corrispondono a un YSeqAct_Checkpoint e per ciascuno: "ce <nome>",
osservazione (stato, cinema, ViewTarget, arma), salto delle cinematiche,
prova di gioco e foto. Rapporto in shots/deep/rapporto.json.

ATTENZIONE: i checkpoint sovrascrivono i salvataggi (Steam Cloud): backup
all'inizio e ripristino alla fine come in levelsweep.py.

    python tools/deepsweep.py                  # tutti i livelli
    python tools/deepsweep.py SP_Gorge01_P     # solo alcuni
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
import ue3  # noqa: E402

OUT = os.path.join(ROOT, 'shots', 'deep')


def checkpoint_events():
    """Eventi da console del livello che portano a un checkpoint, in ordine naturale."""
    p = ue3.Proc()
    n = ue3.Names(p)
    o = ue3.Objects(p, n)
    ev_cls = o.find_class('SeqEvent_Console')
    cp_cls = o.find_class('YSeqAct_Checkpoint')
    ev_off = o.prop_offset('SeqEvent_Console', 'ConsoleEventName')
    cp_names, events = set(), set()
    for x in o.ptrs:
        if not x:
            continue
        c = o.cls(x)
        if c == cp_cls and not o.name(x).startswith('Default__'):
            flags = p.u32(x + 0x11C)
            if not flags & 0x4:  # m_isLevelstart: e' l'inizio del livello, gia' visto
                cp_names.add(n.get(p.u32(x + 0x104)).lower())
        elif c == ev_cls and not o.name(x).startswith('Default__'):
            events.add(n.get(p.u32(x + ev_off)))
    picked = [e for e in events if e and (e.lower() in cp_names or re.fullmatch(r'cp\d+[a-z]?', e, re.I))]

    def key(e):
        m = re.match(r'[a-z_]*?(\d+)([a-z]?)', e, re.I)
        return (int(m.group(1)), m.group(2).lower()) if m else (999, e.lower())
    return sorted(set(picked), key=key), sorted(e for e in events if e)


def wait_gameplay(rep, seconds=40):
    """Aspetta il controllo di Walker premendo A/B per saltare le cinematiche."""
    t0 = time.time()
    presses = ['1000 200', '2000 200', '1000 1500', '4000 200']
    k = 0
    while time.time() - t0 < seconds and ls.running():
        s = ls.state_sample()
        rep.append(s)
        if s and 'gameplay=1' in s and 'cinema=0' in s:
            return True
        time.sleep(3)
        if time.time() - t0 > 9:  # lascia partire la scena, poi prova a saltarla
            g.focus()
            g.send_cmds(['pad fake 0 0 0 0 0 0 ' + presses[k % len(presses)]])
            k += 1
    return False


def play_test(rep):
    g.focus()
    for cmd in ['pad fake 0 0.9 0 0 0 0 0 2500', 'pad fake 0 0 0.7 0 0 0 0 1500', 'pad fake 0 0 0 0 1 1 0 1500']:
        g.send_cmds([cmd])
        time.sleep(2.8)
    rep.append('gioco: %s' % ls.state_sample())


def save_shot(name, d):
    ls.shot(name)
    from PIL import Image
    src = os.path.join(g.SHOT_DIR, name + '.bmp')
    try:
        Image.open(src).resize((480, 500)).save(os.path.join(d, name + '.png'))
        os.remove(src)
    except Exception as e:  # noqa: BLE001
        return 'immagine %s: %s' % (name, e)
    return None


def deep_map(name):
    d = os.path.join(OUT, name)
    os.makedirs(d, exist_ok=True)
    rep = {'mappa': name, 'checkpoint': {}, 'errori': []}
    ls.ensure_game()
    g.focus()
    pos = ls.log_size()
    t0 = time.time()
    # Come fa il gioco (menu, checkpoint): mappa persistente TheLine_P + missione.
    g.send_cmds(['console open TheLine_P?mission=%s?game=SRAssets.YGameInfo_SP_Content' % name])
    while time.time() - t0 < 180 and ls.running():
        time.sleep(2)
        if time.time() - t0 > 8 and re.search(r'Puntatore al Pawn catturato', ls.log_since(pos)):
            break
    rep['inizio'] = []
    wait_gameplay(rep['inizio'], 50)
    g.send_cmds(['cheats'])
    time.sleep(1)
    g.send_cmds(['ucall cheat God'])
    time.sleep(1)
    cps, allev = checkpoint_events()
    rep['eventi'] = allev
    rep['salti'] = cps
    print('   checkpoint:', cps, flush=True)
    for cp in cps:
        if not ls.running():
            rep['errori'].append('gioco chiuso prima di %s' % cp)
            break
        cpos = ls.log_size()
        r = {'stati': []}
        g.send_cmds(['ce ' + cp])
        time.sleep(8)
        r['raggiunto'] = wait_gameplay(r['stati'], 60)
        last = [x for x in r['stati'] if x]
        if not r['raggiunto'] and last and 'cinematic=1' in last[-1] and 'viewtarget=YPawn_Walker' in last[-1]:
            # Aprendo un livello direttamente alcune intro restano ferme su un
            # filmato di transizione (YSeqAct_FullscreenMoviePlayer) che nel gioco
            # vero parte dal livello precedente: si esce a mano dalla modalita'
            # cinematica e lo si segna nel rapporto.
            g.send_cmds(['ucall pc SetCinematicMode 0 0 1 1 1 1'])
            time.sleep(3)
            r['cinematica_forzata'] = True
            r['raggiunto'] = wait_gameplay(r['stati'], 15)
        if ls.running():
            err = save_shot('dp_%s_%s_a' % (name, cp), d)
            if err:
                rep['errori'].append(err)
            if r['raggiunto']:
                play_test(r['stati'])
                save_shot('dp_%s_%s_b' % (name, cp), d)
        txt = ls.log_since(cpos)
        r['blocchi'] = [int(x) for x in re.findall(r'WATCHDOG: thread di rendering fermo da (\d+)s', txt)]
        r['cinema_eventi'] = len(re.findall(r'CINEMA: schermo virtuale \(', txt))
        r['ucall'] = re.findall(r'UCALL: (eccezione[^\n]*|ritorno[^\n]*)', txt)
        rep['checkpoint'][cp] = r
        last = [s for s in r['stati'] if s]
        print('   %-10s raggiunto=%s forzata=%s cinema_eventi=%d blocchi=%s ultimo=%s' % (
            cp, r['raggiunto'], r.get('cinematica_forzata', False), r['cinema_eventi'], r['blocchi'],
            last[-1] if last else None), flush=True)
    txt = ls.log_since(pos)
    rep['fps'] = [float(x) for x in re.findall(r'PROFILO: tick [\d.]+ ms \(([\d.]+) fps', txt)]
    rep['errori'] += sorted(set(re.findall(r'(ERRORE[^\n]*|eccezione[^\n]*)', txt)))[:10]
    rep['armi'] = sorted(set(re.findall(r'ARMA: (\S+)', txt)))
    rep['viste'] = sorted(set(re.findall(r'viewtarget=(\S+)', txt)))
    rep['pawn'] = sorted(set(re.findall(r'pawn=(\S+)', txt)))
    if not ls.running():
        rep['errori'].append('gioco chiuso')
    return rep


def main():
    maps = sys.argv[1:] or ls.MAPS
    os.makedirs(OUT, exist_ok=True)
    ls.backup_saves()
    path = os.path.join(OUT, 'rapporto.json')
    report = json.load(open(path, encoding='utf-8')) if os.path.exists(path) else {}
    try:
        for m in maps:
            print('==', m, flush=True)
            try:
                r = deep_map(m)
            except Exception as e:  # noqa: BLE001
                r = {'mappa': m, 'errori': ['collaudo: %r' % e]}
            report[m] = r
            json.dump(report, open(path, 'w', encoding='utf-8'), indent=1, ensure_ascii=False)
            fps = r.get('fps') or [0]
            print('   fps min/med %.0f/%.0f, armi %s, viste %s, errori %s' % (
                min(fps), sum(fps) / len(fps), r.get('armi'), r.get('viste'), r.get('errori')), flush=True)
    finally:
        ls.gamectl('kill')
        ls.restore_saves()


if __name__ == '__main__':
    main()
