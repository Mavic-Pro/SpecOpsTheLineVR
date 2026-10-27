"""Controllo dal vivo delle liste libere dell'allocatore del gioco (FMallocWindows), copia A.

    python tools/heapcheck.py [secondi] [intervallo]

Righe "ok" oppure "ANOMALY" (blocco libero fuori dal suo pool, o con NumFreeBlocks
assurdo) con il contenuto del blocco. Un'anomalia vale solo se resta uguale per
~0,6 s (still_bad): le liste cambiano mentre le leggiamo da fuori.
Strutture (26/10, dal disassemblato di Malloc 0x004D12A0):
  GMalloc = [0x1451E38] (proxy thread-safe), allocatore = [GMalloc+4];
  +4: tabelle {FirstPool, ExhaustedPool, BlockSize} da 12 byte; +0x288: tabella per dimensione;
  FPoolInfo 32 byte {Bytes, OsBytes, Taken, Mem, Table, FirstMem, Next, PrevLink};
  FFreeMem {Next, NumFreeBlocks}; Malloc restituisce FirstMem + (--NumFreeBlocks) * BlockSize.
Il crash del cambio di livello del 26/10 (memset su un indirizzo spazzatura in un
TArray da 0x48 byte) era un NumFreeBlocks rovinato nella tabella da 0x50."""
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ue3  # noqa: E402

p = ue3.Proc()


def u32(a):
    return struct.unpack('<I', p.read(a, 4))[0]


def tables():
    inner = u32(u32(0x1451E38) + 4)
    out = []
    for i in range(48):
        a = inner + 4 + 12 * i
        first, exh, bs = struct.unpack('<III', p.read(a, 12))
        if bs == 0 or bs > 0x8000:
            break
        out.append((a, bs))
    return out


def check_table(ta, bs):
    bad = []
    pool = u32(ta)
    npools = nfree = 0
    seen = set()
    while pool and npools < 20000:
        if pool in seen:
            bad.append(('pool loop', hex(pool)))
            break
        seen.add(pool)
        npools += 1
        Bytes, OsBytes, Taken, Mem, Table, FirstMem, Next, Prev = struct.unpack('<8I', p.read(pool, 32))
        f = FirstMem
        k = 0
        while f and k < 5000:
            k += 1
            if not (Mem <= f < Mem + Bytes):
                bad.append(('free block outside pool', hex(pool), hex(Mem), hex(Bytes), hex(f)))
                break
            nxt, nb = struct.unpack('<II', p.read(f, 8))
            if nb < 1 or f + nb * bs > Mem + Bytes:
                bad.append(('bad NumFreeBlocks', hex(pool), hex(f), hex(nxt), hex(nb), p.read(f, min(bs, 0x50)).hex()))
                break
            nfree += nb
            f = nxt
        pool = Next
    return npools, nfree, bad


def still_bad(ta, blk, hdr):
    """Il blocco e' ancora nella lista libera di un pool della tabella, con la stessa intestazione."""
    pool = u32(ta)
    k = 0
    while pool and k < 20000:
        k += 1
        Bytes, OsBytes, Taken, Mem, Table, FirstMem, Next, Prev = struct.unpack('<8I', p.read(pool, 32))
        if Mem <= blk < Mem + Bytes:
            f, j = FirstMem, 0
            while f and j < 5000:
                j += 1
                if f == blk:
                    return p.read(f, 8) == hdr
                if not (Mem <= f < Mem + Bytes):
                    return False
                f = u32(f)
            return False
        pool = Next
    return False


def confirmed():
    out = []
    for ta, bs in tables():
        try:
            n, fr, bad = check_table(ta, bs)
        except Exception:
            continue
        for b in bad:
            if b[0] != 'bad NumFreeBlocks':
                continue
            blk, hdr = int(b[2], 16), struct.pack('<II', int(b[3], 16), int(b[4], 16))
            ok = True
            for _ in range(4):
                time.sleep(0.15)
                try:
                    ok = still_bad(ta, blk, hdr)
                except Exception:
                    ok = False
                if not ok:
                    break
            if ok:
                out.append((bs, b))
    return out


if __name__ == '__main__':
    dur = float(sys.argv[1]) if len(sys.argv) > 1 else 0
    iv = float(sys.argv[2]) if len(sys.argv) > 2 else 1
    t0 = time.time()
    while True:
        t = time.strftime('%H:%M:%S')
        try:
            bad = confirmed()
            for bs, b in bad:
                print(t, 'ANOMALY table 0x%X' % bs, b, flush=True)
            if not bad:
                print(t, 'ok', flush=True)
        except Exception as e:
            print(t, 'read failed', e, flush=True)
        if time.time() - t0 >= dur:
            break
        time.sleep(iv)
