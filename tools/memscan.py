"""Scanner di memoria differenziale (stile Cheat Engine) per SpecOpsTheLine.exe.

Uso tipico (fase 2 del mod VR, ricerca della struct camera):
    python memscan.py scan     snap1.json  --min 100 --max 120000
    (il giocatore si muove un po')
    python memscan.py rescan   snap1.json snap2.json --mindelta 20 --maxdelta 3000
    (il giocatore si muove ancora, in direzione diversa se possibile)
    python memscan.py rescan   snap2.json snap3.json --mindelta 20 --maxdelta 3000
    python memscan.py list     snap3.json
"""
import argparse
import ctypes
import json
import math
import struct
import subprocess
from ctypes import wintypes

PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
MEM_COMMIT = 0x1000
MEM_PRIVATE = 0x20000
PAGE_READWRITE = 0x04
PAGE_WRITECOPY = 0x08
PAGE_EXECUTE_READWRITE = 0x40

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)


class MEMORY_BASIC_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("BaseAddress", ctypes.c_void_p),
        ("AllocationBase", ctypes.c_void_p),
        ("AllocationProtect", wintypes.DWORD),
        ("RegionSize", ctypes.c_size_t),
        ("State", wintypes.DWORD),
        ("Protect", wintypes.DWORD),
        ("Type", wintypes.DWORD),
    ]


def get_pid(image_name: str) -> int:
    out = subprocess.check_output(
        ["tasklist", "/FI", f"IMAGENAME eq {image_name}", "/FO", "CSV", "/NH"],
        text=True,
    )
    line = out.strip().splitlines()[0]
    parts = [p.strip('"') for p in line.split('","')]
    return int(parts[1])


def open_process(pid: int):
    h = kernel32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    if not h:
        raise OSError(f"OpenProcess fallita (errore {ctypes.get_last_error()}); il processo e' ancora in esecuzione?")
    return h


def list_writable_private_regions(h):
    regions = []
    addr = 0
    mbi = MEMORY_BASIC_INFORMATION()
    max_addr = 0x7FFFFFFF0000
    while addr < max_addr:
        res = kernel32.VirtualQueryEx(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi))
        if res == 0:
            break
        if (
            mbi.State == MEM_COMMIT
            and mbi.Type == MEM_PRIVATE
            and mbi.Protect in (PAGE_READWRITE, PAGE_WRITECOPY, PAGE_EXECUTE_READWRITE)
        ):
            regions.append((int(mbi.BaseAddress or 0), int(mbi.RegionSize)))
        base = int(mbi.BaseAddress or 0)
        size = int(mbi.RegionSize) or 0x1000
        addr = base + size
    return regions


MEM_IMAGE = 0x1000000
MEM_MAPPED = 0x40000
PAGE_READONLY = 0x02
PAGE_EXECUTE_READ = 0x20


def list_all_readable_regions(h):
    """Come list_writable_private_regions ma include anche le sezioni
    dell'immagine (.exe/.dll) e le mappature di file, utile per trovare
    puntatori statici verso dati dinamici dell'heap."""
    regions = []
    addr = 0
    mbi = MEMORY_BASIC_INFORMATION()
    max_addr = 0x7FFFFFFF0000
    readable = (
        PAGE_READWRITE, PAGE_WRITECOPY, PAGE_EXECUTE_READWRITE,
        PAGE_READONLY, PAGE_EXECUTE_READ,
    )
    while addr < max_addr:
        res = kernel32.VirtualQueryEx(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi))
        if res == 0:
            break
        if mbi.State == MEM_COMMIT and mbi.Protect in readable:
            regions.append((int(mbi.BaseAddress or 0), int(mbi.RegionSize), mbi.Type))
        base = int(mbi.BaseAddress or 0)
        size = int(mbi.RegionSize) or 0x1000
        addr = base + size
    return regions


def read_region(h, base: int, size: int) -> bytes:
    buf = ctypes.create_string_buffer(size)
    read = ctypes.c_size_t(0)
    ok = kernel32.ReadProcessMemory(h, ctypes.c_void_p(base), buf, size, ctypes.byref(read))
    if not ok:
        return b""
    return buf.raw[: read.value]


def unpack_fmt(dtype: str) -> str:
    return "f" if dtype == "f" else "i"


def cmd_scan(args):
    pid = get_pid("SpecOpsTheLine.exe")
    h = open_process(pid)
    regions = list_writable_private_regions(h)
    candidates = {}
    total_bytes = 0
    fmt_char = unpack_fmt(args.dtype)
    for base, size in regions:
        data = read_region(h, base, size)
        if not data:
            continue
        total_bytes += len(data)
        n = len(data) // 4
        if n == 0:
            continue
        try:
            values = struct.unpack_from(f"<{n}{fmt_char}", data, 0)
        except struct.error:
            continue
        lo, hi = args.min, args.max
        for i, v in enumerate(values):
            if args.dtype == "f" and not math.isfinite(v):
                continue
            av = abs(v)
            if lo <= av <= hi:
                candidates[base + i * 4] = v
    with open(args.out, "w") as f:
        json.dump({str(k): v for k, v in candidates.items()}, f)
    print(f"PID={pid} regioni={len(regions)} byte_letti={total_bytes} candidati={len(candidates)} -> {args.out}")


def region_type_name(t: int) -> str:
    return {MEM_IMAGE: "IMAGE", MEM_PRIVATE: "PRIVATE", MEM_MAPPED: "MAPPED"}.get(t, hex(t))


def cmd_findptr(args):
    pid = get_pid("SpecOpsTheLine.exe")
    h = open_process(pid)
    regions = list_all_readable_regions(h)
    lo = args.target - args.back
    hi = args.target
    hits = []
    for base, size, rtype in regions:
        data = read_region(h, base, size)
        if not data:
            continue
        n = len(data) // 4
        if n == 0:
            continue
        try:
            values = struct.unpack_from(f"<{n}I", data, 0)
        except struct.error:
            continue
        for i, v in enumerate(values):
            if lo <= v <= hi:
                hits.append((base + i * 4, rtype, v))
    print(f"Trovati {len(hits)} puntatori nel range [0x{lo:08X}, 0x{hi:08X}]:")
    for addr, rtype, v in hits[: args.limit]:
        tag = "  <-- STABILE (nell'immagine exe)" if rtype == MEM_IMAGE else ""
        print(f"  0x{addr:08X}  [{region_type_name(rtype)}]  punta a 0x{v:08X} (offset campo +0x{args.target - v:X}){tag}")


def cmd_scanregion(args):
    pid = get_pid("SpecOpsTheLine.exe")
    h = open_process(pid)
    base = args.center - args.radius
    size = args.radius * 2
    data = read_region(h, base, size)
    if not data:
        print("Lettura fallita per questa regione.")
        return
    n = len(data) // 4
    fmt_char = unpack_fmt(args.dtype)
    values = struct.unpack_from(f"<{n}{fmt_char}", data, 0)
    candidates = {}
    for i, v in enumerate(values):
        if args.dtype == "f" and not math.isfinite(v):
            continue
        candidates[base + i * 4] = v
    with open(args.out, "w") as f:
        json.dump({str(k): v for k, v in candidates.items()}, f)
    print(f"regione=0x{base:08X}+{size} candidati={len(candidates)} -> {args.out}")


def cmd_rescan(args):
    pid = get_pid("SpecOpsTheLine.exe")
    h = open_process(pid)
    with open(args.inp) as f:
        old = {int(k): v for k, v in json.load(f).items()}
    survivors = {}
    fmt_char = unpack_fmt(args.dtype)
    for addr, old_val in old.items():
        data = read_region(h, addr, 4)
        if len(data) < 4:
            continue
        (new_val,) = struct.unpack(f"<{fmt_char}", data)
        if args.dtype == "f" and not math.isfinite(new_val):
            continue
        delta = abs(new_val - old_val)
        if args.mindelta <= delta <= args.maxdelta:
            survivors[addr] = new_val
    with open(args.out, "w") as f:
        json.dump({str(k): v for k, v in survivors.items()}, f)
    print(f"ingresso={len(old)} superstiti={len(survivors)} -> {args.out}")


def cmd_list(args):
    with open(args.inp) as f:
        data = {int(k): v for k, v in json.load(f).items()}
    addrs = sorted(data.keys())
    print(f"Totale candidati: {len(addrs)}")
    # Raggruppa indirizzi vicini (stessa struct: es. X,Y,Z a 4 o 8 byte di distanza)
    groups = []
    current = []
    for a in addrs:
        if current and a - current[-1] > args.proximity:
            groups.append(current)
            current = []
        current.append(a)
    if current:
        groups.append(current)
    groups = [g for g in groups if len(g) >= args.mingroup]
    print(f"Gruppi con almeno {args.mingroup} indirizzi vicini (<= {args.proximity} byte): {len(groups)}")
    for g in groups[: args.limit]:
        base = g[0]
        print(f"  base=0x{base:08X}  " + "  ".join(f"+0x{a-base:X}={data[a]:.3f}" for a in g))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_scan = sub.add_parser("scan", help="Primo scan: tutti i valori nel range dato")
    p_scan.add_argument("out")
    p_scan.add_argument("--min", type=float, default=100.0)
    p_scan.add_argument("--max", type=float, default=120000.0)
    p_scan.add_argument("--dtype", choices=["f", "i"], default="f", help="f=float (posizione), i=int32 (rotazione Unreal Rotator)")
    p_scan.set_defaults(func=cmd_scan)

    p_find = sub.add_parser("findptr", help="Cerca chi punta a un indirizzo (per trovare una catena di puntatori stabile)")
    p_find.add_argument("--target", type=lambda x: int(x, 0), required=True)
    p_find.add_argument("--back", type=lambda x: int(x, 0), default=0, help="Cerca anche puntatori che puntano fino a N byte prima del target (base di un oggetto)")
    p_find.add_argument("--limit", type=int, default=100)
    p_find.set_defaults(func=cmd_findptr)

    p_region = sub.add_parser("scanregion", help="Scan di una finestra ristretta di memoria (nessun filtro di valore, prende tutto)")
    p_region.add_argument("out")
    p_region.add_argument("--center", type=lambda x: int(x, 0), required=True)
    p_region.add_argument("--radius", type=lambda x: int(x, 0), default=0x1000)
    p_region.add_argument("--dtype", choices=["f", "i"], default="i")
    p_region.set_defaults(func=cmd_scanregion)

    p_re = sub.add_parser("rescan", help="Filtra i candidati per variazione (delta) dall'ultimo scan")
    p_re.add_argument("inp")
    p_re.add_argument("out")
    p_re.add_argument("--mindelta", type=float, default=10.0)
    p_re.add_argument("--maxdelta", type=float, default=3000.0)
    p_re.add_argument("--dtype", choices=["f", "i"], default="f")
    p_re.set_defaults(func=cmd_rescan)

    p_list = sub.add_parser("list", help="Mostra i candidati rimasti, raggruppati per vicinanza in memoria")
    p_list.add_argument("inp")
    p_list.add_argument("--proximity", type=int, default=16)
    p_list.add_argument("--mingroup", type=int, default=2)
    p_list.add_argument("--limit", type=int, default=50)
    p_list.set_defaults(func=cmd_list)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
