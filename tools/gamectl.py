"""Controllo del gioco per i test automatici della mod (nessuna dipendenza
oltre a Pillow per convertire gli screenshot in PNG).

    python tools/gamectl.py deploy                 # copia la dll (gioco chiuso)
    python tools/gamectl.py launch [args...]       # avvia via Steam, es. "SP_Road_P"
    python tools/gamectl.py play [args...]         # avvia e arriva in gameplay (Continua partita)
    python tools/gamectl.py alive
    python tools/gamectl.py kill
    python tools/gamectl.py cmd "mode synth" "synth 30 0 4"
    python tools/gamectl.py shot nome [--out dir]  # screenshot dal back buffer -> PNG
    python tools/gamectl.py log [N]                # ultime N righe di specopsvr.log
    python tools/gamectl.py waitlog REGEX [secondi]
    python tools/gamectl.py key ESC|ENTER|W|... [secondi_pressione]
    python tools/gamectl.py mouse DX DY [passi]    # movimento relativo
    python tools/gamectl.py focus

I tasti usano scan code (KEYEVENTF_SCANCODE) cosi' arrivano anche a
DirectInput/raw input; prima di ogni input la finestra del gioco viene
portata in primo piano.
"""
import ctypes
import ctypes.wintypes as wt
import os
import re
import shutil
import subprocess
import sys
import time

GAME_DIR = os.environ.get("SPECOPS_GAME_DIR",
                          r"D:\SteamLibrary\steamapps\common\SpecOps_TheLine\Binaries\Win32")
PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE_NAME = "SpecOpsTheLine.exe"
APP_ID = "50300"
LOG = os.path.join(GAME_DIR, "specopsvr.log")
CMD_FILE = os.path.join(GAME_DIR, "specopsvr_cmd.txt")
SHOT_DIR = os.path.join(GAME_DIR, "specopsvr_shots")

user32 = ctypes.WinDLL("user32", use_last_error=True)

SCANCODES = {
    "ESC": 0x01, "ENTER": 0x1C, "SPACE": 0x39, "TAB": 0x0F, "BACKSPACE": 0x0E,
    "W": 0x11, "A": 0x1E, "S": 0x1F, "D": 0x20, "E": 0x12, "Q": 0x10, "F": 0x21,
    "R": 0x13, "C": 0x2E, "SHIFT": 0x2A, "CTRL": 0x1D, "ALT": 0x38,
    "UP": (0xE0, 0x48), "DOWN": (0xE0, 0x50), "LEFT": (0xE0, 0x4B), "RIGHT": (0xE0, 0x4D),
    "F1": 0x3B, "F10": 0x44, "TILDE": 0x29,
}


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wt.WORD), ("wScan", wt.WORD), ("dwFlags", wt.DWORD),
                ("time", wt.DWORD), ("dwExtraInfo", ctypes.c_size_t)]


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", wt.LONG), ("dy", wt.LONG), ("mouseData", wt.DWORD),
                ("dwFlags", wt.DWORD), ("time", wt.DWORD), ("dwExtraInfo", ctypes.c_size_t)]


class _INPUTUNION(ctypes.Union):
    _fields_ = [("ki", KEYBDINPUT), ("mi", MOUSEINPUT)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", wt.DWORD), ("u", _INPUTUNION)]


def _send(inp):
    if user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT)) != 1:
        raise OSError(ctypes.get_last_error(), "SendInput fallita")


def _key_event(name, up):
    code = SCANCODES[name.upper()]
    flags = 0x0008  # KEYEVENTF_SCANCODE
    if isinstance(code, tuple):
        flags |= 0x0001  # KEYEVENTF_EXTENDEDKEY
        code = code[1]
    if up:
        flags |= 0x0002
    inp = INPUT(type=1)
    inp.u.ki = KEYBDINPUT(0, code, flags, 0, 0)
    _send(inp)


def game_pid():
    out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {EXE_NAME}", "/FO", "CSV", "/NH"],
                         capture_output=True, text=True).stdout
    m = re.search(r'"[^"]+","(\d+)"', out)
    return int(m.group(1)) if m else None


def game_window():
    pid = game_pid()
    if not pid:
        return None
    found = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(hwnd, _):
        p = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid and user32.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True

    user32.EnumWindows(cb, 0)
    return found[0] if found else None


def _center_cursor(hwnd):
    """Porta il cursore al centro della parte VISIBILE della finestra: la
    finestra (2400x2500) e' piu' grande del monitor, e col movimento relativo
    il cursore finiva sulla barra delle applicazioni, dove il clic destro
    (mira) apriva la Jump List di Edge rubando il focus al gioco."""
    r = wt.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(r))
    sw, sh = user32.GetSystemMetrics(0), user32.GetSystemMetrics(1)
    left, top = max(r.left, 0), max(r.top, 0)
    right, bottom = min(r.right, sw), min(r.bottom, sh - 60)
    user32.SetCursorPos((left + right) // 2, (top + bottom) // 2)


def _close_popups():
    """Chiude menu/jump list rimasti in primo piano (non del gioco)."""
    fg = user32.GetForegroundWindow()
    cls = ctypes.create_unicode_buffer(128)
    user32.GetClassNameW(fg, cls, 128)
    if cls.value in ("Windows.UI.Core.CoreWindow", "#32768", "Shell_TrayWnd"):
        _key_event("ESC", False)
        _key_event("ESC", True)
        time.sleep(0.3)


def focus():
    hwnd = game_window()
    if not hwnd:
        print("finestra del gioco non trovata")
        return False
    _close_popups()
    _center_cursor(hwnd)
    if user32.GetForegroundWindow() == hwnd:
        return True
    # Trucco ALT: Windows concede SetForegroundWindow a chi ha appena
    # generato input. Se non basta, si aggancia temporaneamente l'input del
    # nostro thread a quello della finestra in primo piano.
    for attempt in range(3):
        _key_event("ALT", False)
        _key_event("ALT", True)
        if user32.IsIconic(hwnd):
            user32.ShowWindow(hwnd, 9)  # SW_RESTORE
        fg = user32.GetForegroundWindow()
        fg_thread = user32.GetWindowThreadProcessId(fg, None)
        me = ctypes.WinDLL("kernel32").GetCurrentThreadId()
        attached = fg_thread and fg_thread != me and user32.AttachThreadInput(me, fg_thread, True)
        user32.BringWindowToTop(hwnd)
        user32.SetForegroundWindow(hwnd)
        if attached:
            user32.AttachThreadInput(me, fg_thread, False)
        time.sleep(0.3)
        if user32.GetForegroundWindow() == hwnd:
            break
    ok = user32.GetForegroundWindow() == hwnd
    print("focus:", "ok" if ok else "NON ottenuto")
    return ok


def key(name, hold=0.08):
    focus()
    _key_event(name, False)
    time.sleep(hold)
    _key_event(name, True)


def mouse(dx, dy, steps=20):
    focus()
    for _ in range(steps):
        inp = INPUT(type=0)
        inp.u.mi = MOUSEINPUT(int(dx / steps), int(dy / steps), 0, 0x0001, 0, 0)  # MOUSEEVENTF_MOVE
        _send(inp)
        time.sleep(0.016)


def click(hold=0.05, button="left"):
    focus()
    down, up = (0x0002, 0x0004) if button == "left" else (0x0008, 0x0010)
    inp = INPUT(type=0)
    inp.u.mi = MOUSEINPUT(0, 0, 0, down, 0, 0)
    _send(inp)
    time.sleep(hold)
    inp.u.mi = MOUSEINPUT(0, 0, 0, up, 0, 0)
    _send(inp)


def steam_exe():
    import winreg
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam") as k:
        return winreg.QueryValueEx(k, "SteamExe")[0]


def launch(args):
    if game_pid():
        print("gioco gia' in esecuzione")
        return
    subprocess.Popen([steam_exe(), "-applaunch", APP_ID, *args])
    for _ in range(40):
        time.sleep(1)
        if game_pid():
            print("avviato, pid", game_pid())
            return
    # A volte Steam ignora -applaunch (16/10: lo considerava ancora in
    # esecuzione): avvio diretto dell'eseguibile, steam_api si collega lo stesso.
    print("Steam non ha avviato il gioco, avvio diretto dell'eseguibile")
    subprocess.Popen([os.path.join(GAME_DIR, EXE_NAME), *args], cwd=GAME_DIR)
    for _ in range(60):
        time.sleep(1)
        if game_pid():
            print("avviato, pid", game_pid())
            return
    print("il gioco non si e' avviato")


def kill():
    # Prima chiusura gentile (WM_CLOSE): il 16/10 una terminazione forzata
    # (taskkill /F) ha coinciso con un "device removed" nel driver Oculus di
    # SteamVR, che poi rifiutava le texture di tutte le app (errore 105).
    pid = game_pid()
    hwnd = game_window()
    if pid and hwnd:
        user32.PostMessageW(hwnd, 0x0010, 0, 0)  # WM_CLOSE
        for _ in range(15):
            time.sleep(1)
            if not game_pid():
                break
    pid = game_pid()
    if pid:
        subprocess.run(["taskkill", "/PID", str(pid), "/F"], capture_output=True)
        time.sleep(2)
    print("terminato" if not game_pid() else "ancora vivo!")


def deploy():
    if game_pid():
        print("gioco in esecuzione: chiudilo prima (kill)")
        return
    src = os.path.join(PROJECT_DIR, "deploy", "Release", "d3d9.dll")
    shutil.copy2(src, os.path.join(GAME_DIR, "d3d9.dll"))
    print("copiato", src)
    # La configurazione si copia solo se non esiste: non sovrascrivere le
    # modifiche dell'utente.
    ini = os.path.join(GAME_DIR, "specopsvr.ini")
    if not os.path.exists(ini):
        shutil.copy2(os.path.join(PROJECT_DIR, "deploy", "specopsvr.ini"), ini)
        print("copiato specopsvr.ini")


def send_cmds(lines):
    # Append: se la dll non ha ancora consumato comandi precedenti, non li
    # perdiamo.
    with open(CMD_FILE, "a", encoding="utf-8") as f:
        for line in lines:
            f.write(line + "\n")


def shot(name, out_dir):
    bmp = os.path.join(SHOT_DIR, name + ".bmp")
    if os.path.exists(bmp):
        os.remove(bmp)
    send_cmds([f"shot {name}"])
    for _ in range(50):
        time.sleep(0.2)
        if os.path.exists(bmp) and os.path.getsize(bmp) > 0:
            time.sleep(0.2)
            break
    else:
        print("screenshot non arrivato (gioco in pausa/minimizzato?)")
        return
    from PIL import Image
    os.makedirs(out_dir, exist_ok=True)
    png = os.path.join(out_dir, name + ".png")
    Image.open(bmp).save(png)
    print(png)


def tail(n):
    with open(LOG, encoding="utf-8", errors="replace") as f:
        lines = f.readlines()
    print("".join(lines[-n:]), end="")


def waitlog(pattern, timeout):
    start_size = os.path.getsize(LOG) if os.path.exists(LOG) else 0
    rx = re.compile(pattern)
    deadline = time.time() + timeout
    while time.time() < deadline:
        if os.path.exists(LOG):
            size = os.path.getsize(LOG)
            if size < start_size:  # log ricreato da un nuovo avvio
                start_size = 0
            with open(LOG, encoding="utf-8", errors="replace") as f:
                f.seek(start_size)
                for line in f:
                    if rx.search(line):
                        print(line.rstrip())
                        return True
        if not game_pid():
            print("il gioco non e' piu' in esecuzione")
            return False
        time.sleep(0.5)
    print("timeout")
    return False


def camera_pointers():
    """Puntatori camera loggati dall'ultimo avvio della dll."""
    with open(LOG, encoding="utf-8", errors="replace") as f:
        lines = f.read().split("hook avviato")[-1].splitlines()
    return [l.rsplit("0x", 1)[-1] for l in lines if "vera camera catturato" in l]


def gameplay_state():
    """Chiede alla dll se il giocatore controlla Walker (riga STATO nel log)."""
    size = os.path.getsize(LOG)
    send_cmds(["dumpcam"])
    for _ in range(20):
        time.sleep(0.25)
        with open(LOG, encoding="utf-8", errors="replace") as f:
            f.seek(size)
            for line in f:
                if "STATO gameplay=" in line:
                    return line.strip().endswith("=1")
    return None


def play(args):
    """Avvia il gioco (se serve) e arriva in gameplay: titolo -> menu ->
    Campagna -> Continua partita, poi la cinematica iniziale. Lo stato e'
    chiesto alla dll (dumpcam -> riga "STATO gameplay=1" quando il giocatore
    controlla Walker). Preme ENTER solo nei menu (prima del caricamento)."""
    if not game_pid():
        launch(args or ["-nostartupmovies", "-windowed", "ResX=2400", "ResY=2500"])
    deadline = time.time() + 300
    # Fase 1: ENTER (titolo -> Campagna -> Continua) finche' non parte il
    # caricamento del livello, riconosciuto da un nuovo puntatore camera nel
    # log. ENTER in piu' durante il caricamento/cinematica riportavano al menu.
    start_cams = len(camera_pointers())
    loading = False
    while time.time() < deadline and not loading:
        if not game_pid():
            print("il gioco non e' piu' in esecuzione")
            return False
        if gameplay_state():
            print("in gameplay")
            return True
        if camera_pointers():  # il menu e' pronto (la dll ha trovato la sua camera)
            key("ENTER", 0.15)
        for _ in range(8):
            time.sleep(1)
            if len(camera_pointers()) > max(start_cams, 1):
                loading = True
                break
    # Fase 2: attesa del controllo di Walker (dopo la cinematica iniziale).
    while time.time() < deadline:
        if not game_pid():
            print("il gioco non e' piu' in esecuzione")
            return False
        if gameplay_state():
            print("in gameplay")
            return True
        time.sleep(5)
    print("gameplay non raggiunto")
    return False


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        return
    c = a[0]
    if c == "deploy": deploy()
    elif c == "launch": launch(a[1:])
    elif c == "play": sys.exit(0 if play(a[1:]) else 1)
    elif c == "alive": print(game_pid() or "non in esecuzione")
    elif c == "kill": kill()
    elif c == "cmd": send_cmds(a[1:])
    elif c == "shot":
        out = a[a.index("--out") + 1] if "--out" in a else os.path.join(PROJECT_DIR, "shots")
        shot(a[1], out)
    elif c == "log": tail(int(a[1]) if len(a) > 1 else 30)
    elif c == "waitlog": sys.exit(0 if waitlog(a[1], float(a[2]) if len(a) > 2 else 60) else 1)
    elif c == "key": key(a[1], float(a[2]) if len(a) > 2 else 0.08)
    elif c == "mouse": mouse(int(a[1]), int(a[2]), int(a[3]) if len(a) > 3 else 20)
    elif c == "focus": focus()
    elif c == "click": click(float(a[1]) if len(a) > 1 else 0.05, a[2] if len(a) > 2 else "left")
    else: print(__doc__)


if __name__ == "__main__":
    main()
