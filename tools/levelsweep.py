"""Collaudo automatico dei livelli della campagna.

Per ogni livello: lo carica con "console open <mappa>", osserva (stato,
cinema, ViewTarget, arma, fps, blocchi), salta le cinematiche (A), prova a
giocare (camminata, rotazione, mira, fuoco) e fotografa. Se il gioco si
chiude lo riavvia e prosegue. Rapporto in shots/sweep/<mappa>/*.png e
shots/sweep/rapporto.json.

ATTENZIONE: caricare livelli crea checkpoint che sovrascrivono i salvataggi
(Steam Cloud). Lo script fa un backup all'inizio e lo ripristina alla fine.

    python tools/levelsweep.py                 # tutti i livelli
    python tools/levelsweep.py SP_Gorge01_P    # solo alcuni
"""
import json
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import gamectl as g  # noqa: E402

LOG = os.path.join(g.GAME_DIR, 'specopsvr.log')
OUT = os.path.join(ROOT, 'shots', 'sweep')
def _find_save_dir():
    """Cartella dei salvataggi di Spec Ops (Steam Cloud, app 50300) del primo
    account Steam che ne ha una; SPECOPS_SAVE_DIR la sostituisce."""
    if os.environ.get('SPECOPS_SAVE_DIR'):
        return os.environ['SPECOPS_SAVE_DIR']
    import glob
    hits = glob.glob(os.path.join('C:' + os.sep, 'Program Files (x86)', 'Steam', 'userdata', '*', '50300', 'remote'))
    return hits[0] if hits else ''


SAVE_DIR = _find_save_dir()
SAVE_BACKUP = os.path.join(ROOT, 'savebackup_20261017')
MAPS = ['SP_Desert01_p', 'SP_Desert02_p', 'SP_Gorge01_P', 'SP_Gorge02_P', 'SP_CityGate01_P', 'SP_CityGate02_p',
        'SP_Road_P', 'SP_ShipsGraveyard_P', 'SP_StealingWater01_p', 'SP_StealingWater02_p', 'SP_Atrium01_P',
        'SP_Atrium02_P', 'SP_RadioMan02_P', 'SP_RadioMan03_P', 'SP_Bridge_p', 'SP_DollsHouse_P',
        'SP_BurjDubai_P', 'SP_Epilogue_p', 'SP_Credits_P']
STATE_RE = re.compile(r'STATO (gameplay=\d cinema=\d cinematic=\d pawn=\S* viewtarget=\S* arma=\S*)')


def backup_saves():
    if not os.path.isdir(SAVE_BACKUP):
        os.makedirs(SAVE_BACKUP)
        for f in os.listdir(SAVE_DIR):
            shutil.copy2(os.path.join(SAVE_DIR, f), SAVE_BACKUP)
        print('backup dei salvataggi in', SAVE_BACKUP)


def restore_saves():
    for f in os.listdir(SAVE_BACKUP):
        dst = os.path.join(SAVE_DIR, f)
        shutil.copyfile(os.path.join(SAVE_BACKUP, f), dst)
        os.utime(dst, None)  # piu' recente della copia nel cloud: Steam tiene questa
    print('salvataggi originali ripristinati')


def gamectl(*args, timeout=900):
    return subprocess.run(['python', os.path.join(ROOT, 'tools', 'gamectl.py'), *args], capture_output=True,
                          text=True, timeout=timeout).stdout


def running():
    out = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq SpecOpsTheLine.exe'], capture_output=True,
                         text=True).stdout
    return 'SpecOpsTheLine.exe' in out


def log_size():
    try:
        return os.path.getsize(LOG)
    except OSError:
        return 0


def log_since(pos):
    with open(LOG, 'rb') as f:
        f.seek(pos)
        return f.read().decode('utf-8', 'replace')


def ensure_game():
    if not running():
        print('  (avvio del gioco)', flush=True)
        gamectl('play')


def state_sample():
    pos = log_size()
    g.send_cmds(['dumpcam'])
    time.sleep(1.5)
    m = STATE_RE.findall(log_since(pos))
    return m[-1] if m else None


def shot(name):
    g.send_cmds(['shot ' + name])
    time.sleep(1.5)


def sweep_map(name):
    d = os.path.join(OUT, name)
    os.makedirs(d, exist_ok=True)
    rep = {'mappa': name, 'stati': [], 'errori': [], 'caricata': False}
    ensure_game()
    g.focus()
    pos = log_size()
    t0 = time.time()
    g.send_cmds(['console open ' + name])
    while time.time() - t0 < 180:
        time.sleep(2)
        if not running():
            rep['errori'].append('gioco chiuso durante il caricamento')
            break
        if time.time() - t0 > 8 and re.search(r'Puntatore al Pawn catturato', log_since(pos)):
            rep['caricata'] = True
            break
    rep['secondi_caricamento'] = round(time.time() - t0)
    # osservazione passiva (intro, cinematiche)
    for k in range(6):
        if not running():
            break
        time.sleep(3)
        rep['stati'].append(state_sample())
        if k in (0, 5):
            shot('sw_%s_a%d' % (name, k))
    # cinematiche: A per saltarle (CancelCineA)
    # tasti provati in ordine: A, B, X, A tenuto (alcune scene vogliono altro)
    for press in ['1000 200', '2000 200', '4000 200', '1000 1500', '1000 200', '2000 200']:
        s = rep['stati'][-1] if rep['stati'] else None
        if not running() or not s or 'cinema=0' in s:
            break
        g.focus()
        g.send_cmds(['pad fake 0 0 0 0 0 0 ' + press])
        time.sleep(5)
        rep['stati'].append('dopo A: %s' % state_sample())
    # prova di gioco
    if running():
        shot('sw_%s_g0' % name)
        g.focus()
        for cmd in ['pad fake 0 0.9 0 0 0 0 0 3000', 'pad fake 0 0.9 0.6 0 0 0 0 2500',
                    'pad fake 0 0 0 0 1 1 0 1500', 'pad fake 0 0.9 -0.6 0 0 0 0 2500']:
            g.send_cmds([cmd])
            time.sleep(3.2)
            rep['stati'].append('gioco: %s' % state_sample())
        shot('sw_%s_g1' % name)
    txt = log_since(pos)
    rep['blocchi'] = [int(x) for x in re.findall(r'WATCHDOG: thread di rendering fermo da (\d+)s', txt)]
    rep['fps'] = [float(x) for x in re.findall(r'PROFILO: tick [\d.]+ ms \(([\d.]+) fps', txt)]
    rep['errori'] += sorted(set(re.findall(r'(ERRORE[^\n]*|eccezione[^\n]*)', txt)))[:10]
    rep['cinema_eventi'] = len(re.findall(r'CINEMA: schermo virtuale \(', txt))
    rep['armi'] = sorted(set(re.findall(r'ARMA: (\S+)', txt)))
    if not running():
        rep['errori'].append('gioco chiuso')
    from PIL import Image
    for f in os.listdir(g.SHOT_DIR):
        if f.startswith('sw_%s_' % name) and f.endswith('.bmp'):
            try:
                Image.open(os.path.join(g.SHOT_DIR, f)).resize((480, 500)).save(
                    os.path.join(d, f.replace('.bmp', '.png')))
                os.remove(os.path.join(g.SHOT_DIR, f))
            except Exception as e:  # noqa: BLE001
                rep['errori'].append('immagine %s: %s' % (f, e))
    return rep


def main():
    maps = sys.argv[1:] or MAPS
    os.makedirs(OUT, exist_ok=True)
    backup_saves()
    path = os.path.join(OUT, 'rapporto.json')
    report = json.load(open(path, encoding='utf-8')) if os.path.exists(path) else {}
    try:
        for m in maps:
            print('==', m, flush=True)
            try:
                r = sweep_map(m)
            except Exception as e:  # noqa: BLE001
                r = {'mappa': m, 'errori': ['collaudo: %r' % e]}
            report[m] = r
            json.dump(report, open(path, 'w', encoding='utf-8'), indent=1, ensure_ascii=False)
            fps = r.get('fps') or [0]
            print('   caricata=%s in %ss, fps min/med %.0f/%.0f, blocchi %s, cinema %s, armi %s, errori %s' % (
                r.get('caricata'), r.get('secondi_caricamento'), min(fps), sum(fps) / len(fps), r.get('blocchi'),
                r.get('cinema_eventi'), r.get('armi'), r.get('errori')), flush=True)
            for s in (r.get('stati') or []):
                print('     ', s, flush=True)
    finally:
        gamectl('kill')
        restore_saves()


if __name__ == '__main__':
    main()
