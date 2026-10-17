"""Mini-debugger con breakpoint hardware per trovare la catena di puntatori
stabile verso un campo gia' noto (es. la posizione del personaggio).

Si comporta come "Find out what accesses this address" di Cheat Engine:
si aggancia al processo come debugger reale (DebugActiveProcess), piazza un
watchpoint hardware (registro DR0) sull'indirizzo target su ogni thread, e
alla prima scrittura intercettata stampa EIP, i registri e i byte
dell'istruzione — da cui si legge a mano quale registro conteneva il
puntatore base dell'oggetto (es. "mov [esi+0x50], eax" => l'oggetto e' in ESI,
il campo e' all'offset +0x50).

Uso:
    python hwbp.py --target 0x01BCBCE4
    (subito dopo, nel gioco, muovi il personaggio cosi' il campo viene scritto)

Si disattacca da solo pulendo i debug register appena trova un hit (o dopo
il timeout), senza chiudere il gioco.
"""
import argparse
import ctypes
import sys
import time
from ctypes import wintypes

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

PROCESS_QUERY_INFORMATION = 0x0400
DBG_CONTINUE = 0x00010002
DBG_EXCEPTION_NOT_HANDLED = 0x80010001

EXCEPTION_DEBUG_EVENT = 1
CREATE_THREAD_DEBUG_EVENT = 2
CREATE_PROCESS_DEBUG_EVENT = 3
EXIT_THREAD_DEBUG_EVENT = 4
EXIT_PROCESS_DEBUG_EVENT = 5
LOAD_DLL_DEBUG_EVENT = 6
UNLOAD_DLL_DEBUG_EVENT = 7
OUTPUT_DEBUG_STRING_EVENT = 8
RIP_EVENT = 9

EXCEPTION_SINGLE_STEP = 0x80000004
STATUS_WX86_SINGLE_STEP = 0x4000001E  # equivalente WOW64 di EXCEPTION_SINGLE_STEP

THREAD_GET_CONTEXT = 0x0008
THREAD_SET_CONTEXT = 0x0010
THREAD_SUSPEND_RESUME = 0x0002
THREAD_QUERY_INFORMATION = 0x0040

CONTEXT_i386 = 0x00010000
CONTEXT_CONTROL = 0x1
CONTEXT_INTEGER = 0x2
CONTEXT_SEGMENTS = 0x4
CONTEXT_DEBUG_REGISTERS = 0x10
CONTEXT_FULL = CONTEXT_i386 | CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_SEGMENTS
CONTEXT_WANTED = CONTEXT_FULL | CONTEXT_i386 | CONTEXT_DEBUG_REGISTERS


class FLOATING_SAVE_AREA(ctypes.Structure):
    _fields_ = [
        ("ControlWord", wintypes.DWORD), ("StatusWord", wintypes.DWORD),
        ("TagWord", wintypes.DWORD), ("ErrorOffset", wintypes.DWORD),
        ("ErrorSelector", wintypes.DWORD), ("DataOffset", wintypes.DWORD),
        ("DataSelector", wintypes.DWORD), ("RegisterArea", ctypes.c_ubyte * 80),
        ("Cr0NpxState", wintypes.DWORD),
    ]


class CONTEXT(ctypes.Structure):
    _fields_ = [
        ("ContextFlags", wintypes.DWORD),
        ("Dr0", wintypes.DWORD), ("Dr1", wintypes.DWORD),
        ("Dr2", wintypes.DWORD), ("Dr3", wintypes.DWORD),
        ("Dr6", wintypes.DWORD), ("Dr7", wintypes.DWORD),
        ("FloatSave", FLOATING_SAVE_AREA),
        ("SegGs", wintypes.DWORD), ("SegFs", wintypes.DWORD),
        ("SegEs", wintypes.DWORD), ("SegDs", wintypes.DWORD),
        ("Edi", wintypes.DWORD), ("Esi", wintypes.DWORD),
        ("Ebx", wintypes.DWORD), ("Edx", wintypes.DWORD),
        ("Ecx", wintypes.DWORD), ("Eax", wintypes.DWORD),
        ("Ebp", wintypes.DWORD), ("Eip", wintypes.DWORD),
        ("SegCs", wintypes.DWORD), ("EFlags", wintypes.DWORD),
        ("Esp", wintypes.DWORD), ("SegSs", wintypes.DWORD),
        ("ExtendedRegisters", ctypes.c_ubyte * 512),
    ]


class EXCEPTION_RECORD(ctypes.Structure):
    _fields_ = [
        ("ExceptionCode", wintypes.DWORD),
        ("ExceptionFlags", wintypes.DWORD),
        ("ExceptionRecord", ctypes.c_void_p),
        ("ExceptionAddress", ctypes.c_void_p),
        ("NumberParameters", wintypes.DWORD),
        ("ExceptionInformation", ctypes.c_void_p * 15),
    ]


class EXCEPTION_DEBUG_INFO(ctypes.Structure):
    _fields_ = [("ExceptionRecord", EXCEPTION_RECORD), ("dwFirstChance", wintypes.DWORD)]


class CREATE_THREAD_DEBUG_INFO(ctypes.Structure):
    _fields_ = [
        ("hThread", wintypes.HANDLE),
        ("lpThreadLocalBase", ctypes.c_void_p),
        ("lpStartAddress", ctypes.c_void_p),
    ]


class CREATE_PROCESS_DEBUG_INFO(ctypes.Structure):
    _fields_ = [
        ("hFile", wintypes.HANDLE), ("hProcess", wintypes.HANDLE), ("hThread", wintypes.HANDLE),
        ("lpBaseOfImage", ctypes.c_void_p), ("dwDebugInfoFileOffset", wintypes.DWORD),
        ("nDebugInfoSize", wintypes.DWORD), ("lpThreadLocalBase", ctypes.c_void_p),
        ("lpStartAddress", ctypes.c_void_p), ("lpImageName", ctypes.c_void_p),
        ("fUnicode", wintypes.WORD),
    ]


class EXIT_THREAD_DEBUG_INFO(ctypes.Structure):
    _fields_ = [("dwExitCode", wintypes.DWORD)]


class DEBUG_EVENT_UNION(ctypes.Union):
    _fields_ = [
        ("Exception", EXCEPTION_DEBUG_INFO),
        ("CreateThread", CREATE_THREAD_DEBUG_INFO),
        ("CreateProcessInfo", CREATE_PROCESS_DEBUG_INFO),
        ("ExitThread", EXIT_THREAD_DEBUG_INFO),
        ("pad", ctypes.c_ubyte * 160),
    ]


class DEBUG_EVENT(ctypes.Structure):
    _fields_ = [
        ("dwDebugEventCode", wintypes.DWORD),
        ("dwProcessId", wintypes.DWORD),
        ("dwThreadId", wintypes.DWORD),
        ("u", DEBUG_EVENT_UNION),
    ]


kernel32.DebugActiveProcess.argtypes = [wintypes.DWORD]
kernel32.WaitForDebugEvent.argtypes = [ctypes.POINTER(DEBUG_EVENT), wintypes.DWORD]
kernel32.ContinueDebugEvent.argtypes = [wintypes.DWORD, wintypes.DWORD, wintypes.DWORD]
kernel32.OpenThread.restype = wintypes.HANDLE
# Questo script e' a 64 bit (Python installato e' x64) ma il gioco e' a 32
# bit: per leggere/scrivere i registri di debug di un thread WOW64 da un
# debugger a 64 bit servono le API Wow64*, non le GetThreadContext normali
# (che altrimenti "riescono" silenziosamente senza toccare i registri reali).
kernel32.Wow64GetThreadContext.argtypes = [wintypes.HANDLE, ctypes.POINTER(CONTEXT)]
kernel32.Wow64SetThreadContext.argtypes = [wintypes.HANDLE, ctypes.POINTER(CONTEXT)]
GetThreadContext = kernel32.Wow64GetThreadContext
SetThreadContext = kernel32.Wow64SetThreadContext


def get_pid(image_name: str) -> int:
    import subprocess
    out = subprocess.check_output(
        ["tasklist", "/FI", f"IMAGENAME eq {image_name}", "/FO", "CSV", "/NH"], text=True
    )
    line = out.strip().splitlines()[0]
    parts = [p.strip('"') for p in line.split('","')]
    return int(parts[1])


def set_hw_breakpoint(hThread, addr, enable: bool, mode: str = "write"):
    ctx = CONTEXT()
    ctx.ContextFlags = CONTEXT_WANTED
    if not GetThreadContext(hThread, ctypes.byref(ctx)):
        return False
    if enable:
        ctx.Dr0 = addr
        if mode == "execute":
            # R/W=00 (esecuzione), LEN deve essere 00 (1 byte) per le
            # specifiche Intel sui breakpoint di esecuzione.
            rw, length = 0b00, 0b00
        else:
            rw, length = 0b01, 0b11  # scrittura, 4 byte
        ctx.Dr7 = (ctx.Dr7 & ~0x000F0003) | 0x1 | (rw << 16) | (length << 18)
    else:
        ctx.Dr0 = 0
        ctx.Dr7 = ctx.Dr7 & ~0x1
    return bool(SetThreadContext(hThread, ctypes.byref(ctx)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", type=lambda x: int(x, 0), required=True)
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("--mode", choices=["write", "execute"], default="write")
    ap.add_argument("--all-threads", action="store_true",
                     help="Arma il breakpoint su tutti i thread invece che solo sul principale (piu' rischioso)")
    args = ap.parse_args()

    pid = get_pid("SpecOpsTheLine.exe")
    print(f"PID={pid}, aggancio come debugger...")
    if not kernel32.DebugActiveProcess(pid):
        print("DebugActiveProcess fallita, errore:", ctypes.get_last_error())
        sys.exit(1)
    kernel32.DebugSetProcessKillOnExit(False)

    armed_threads = {}
    exception_counts = {}
    found = False
    start = time.time()
    ev = DEBUG_EVENT()

    MAX_EXCEPTIONS_PER_THREAD = 5  # circuito di sicurezza: oltre questa
    # soglia sullo stesso thread senza una risoluzione pulita, disarmiamo
    # quel thread invece di rischiare un loop che lo blocca (causa di un
    # crash osservato in un test precedente).

    try:
        while time.time() - start < args.timeout:
            if not kernel32.WaitForDebugEvent(ctypes.byref(ev), 500):
                continue
            code = ev.dwDebugEventCode
            tid = ev.dwThreadId
            cont_status = DBG_CONTINUE

            if code == CREATE_PROCESS_DEBUG_EVENT:
                h = ev.u.CreateProcessInfo.hThread
                if set_hw_breakpoint(h, args.target, True, args.mode):
                    armed_threads[tid] = h
                    print(f"  breakpoint armato su thread principale {tid}")

            elif code == CREATE_THREAD_DEBUG_EVENT:
                # Di default armiamo SOLO il thread principale: toccare il
                # CONTEXT (incluso lo stato SSE/FPU) di ogni thread di un
                # motore che ne crea 100+ e' il sospetto principale per un
                # crash osservato in un test precedente. --all-threads
                # ripristina il comportamento vecchio se necessario.
                if args.all_threads:
                    h = ev.u.CreateThread.hThread
                    if set_hw_breakpoint(h, args.target, True, args.mode):
                        armed_threads[tid] = h
                        print(f"  breakpoint armato su nuovo thread {tid}")

            elif code == EXCEPTION_DEBUG_EVENT:
                excode = ev.u.Exception.ExceptionRecord.ExceptionCode
                if tid in armed_threads:
                    exception_counts[tid] = exception_counts.get(tid, 0) + 1
                    if exception_counts[tid] > MAX_EXCEPTIONS_PER_THREAD:
                        h = armed_threads.pop(tid)
                        set_hw_breakpoint(h, args.target, False)
                        print(f"  [sicurezza] troppe eccezioni sul thread {tid}, disarmato per evitare un loop")
                        kernel32.ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, DBG_CONTINUE)
                        continue
                print(f"  [debug] eccezione 0x{excode:08X} su thread {tid} (armato={tid in armed_threads})")
                if excode in (EXCEPTION_SINGLE_STEP, STATUS_WX86_SINGLE_STEP) and tid in armed_threads:
                    h = armed_threads[tid]
                    ctx = CONTEXT()
                    ctx.ContextFlags = CONTEXT_WANTED
                    GetThreadContext(h, ctypes.byref(ctx))
                    print(f"  Dr6=0x{ctx.Dr6:08X} Dr7=0x{ctx.Dr7:08X}")
                    print("\n*** SINGLE-STEP SU THREAD ARMATO (possibile hit) ***")
                    print(f"thread={tid}  EIP=0x{ctx.Eip:08X}")
                    print(f"EAX=0x{ctx.Eax:08X} EBX=0x{ctx.Ebx:08X} ECX=0x{ctx.Ecx:08X} EDX=0x{ctx.Edx:08X}")
                    print(f"ESI=0x{ctx.Esi:08X} EDI=0x{ctx.Edi:08X} EBP=0x{ctx.Ebp:08X} ESP=0x{ctx.Esp:08X}")
                    buf = ctypes.create_string_buffer(16)
                    read = ctypes.c_size_t(0)
                    hproc = kernel32.OpenProcess(PROCESS_QUERY_INFORMATION | 0x0010, False, pid)
                    kernel32.ReadProcessMemory(hproc, ctypes.c_void_p(ctx.Eip), buf, 16, ctypes.byref(read))
                    print("byte a EIP:", buf.raw[: read.value].hex(" "))
                    found = True
                    # Disarma subito questo thread: ci basta un hit, e
                    # disattivare riduce il rischio di ulteriori loop.
                    ctx.Dr6 = 0
                    SetThreadContext(h, ctypes.byref(ctx))
                    set_hw_breakpoint(h, args.target, False)
                    armed_threads.pop(tid, None)
                # Qualsiasi altra eccezione (incluso il breakpoint di
                # attacco standard che Windows genera sempre al primo
                # DebugActiveProcess) va lasciata proseguire con DBG_CONTINUE:
                # usare DBG_EXCEPTION_NOT_HANDLED qui la fa trattare come
                # fatale dal processo debuggato (causa di un crash osservato
                # in test precedenti).
                cont_status = DBG_CONTINUE

            kernel32.ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, cont_status)
            if found:
                break
    finally:
        for tid, h in armed_threads.items():
            set_hw_breakpoint(h, args.target, False)
        kernel32.DebugActiveProcessStop(pid)
        print("\nDistaccato dal processo (gioco non toccato).")

    if not found:
        print("Nessun hit entro il timeout: il campo non e' stato scritto, o il thread giusto non era tra quelli armati.")


if __name__ == "__main__":
    main()
