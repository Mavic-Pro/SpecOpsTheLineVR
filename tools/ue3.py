"""Lettura degli oggetti del motore UE3 di Spec Ops: The Line dall'esterno.

Funzioni di base (lettura memoria, GNames, GObjects, proprieta' UnrealScript)
usate per trovare offset di classi e proprieta' senza indovinare.

    python tools/ue3.py names 42425            # testo di un FName
    python tools/ue3.py findname Weapon        # indici FName con quel testo
    python tools/ue3.py props Pawn             # proprieta' della classe con offset
    python tools/ue3.py objects WeaponName     # oggetti con quel nome
"""
import ctypes
import os
import re
import struct
import subprocess
import sys

GNAMES = 0x014C1EC8
GOBJECTS = 0x014D41EC  # TArray<UObject*>
# UObject: +0x04 Index, +0x28 Outer, +0x2C Name (FName), +0x34 Class.
# UField: +0x40 Next. UStruct: +0x3C SuperField, +0x4C Children.
# UProperty: +0x44 ArrayDim, +0x64 Offset. UBoolProperty: bit mask in testa (vedi props).
OBJ_OUTER, OBJ_NAME, OBJ_CLASS = 0x28, 0x2C, 0x34
FIELD_NEXT, STRUCT_SUPER, STRUCT_CHILDREN = 0x40, 0x3C, 0x4C
PROP_ARRAYDIM, PROP_OFFSET = 0x44, 0x64

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenProcess.restype = ctypes.c_void_p
k32.ReadProcessMemory.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
                                  ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]


class Proc:
    def __init__(self, image=os.environ.get("SPECOPS_EXE", "SpecOpsTheLine.exe")):
        out = subprocess.check_output(["tasklist", "/FI", f"IMAGENAME eq {image}", "/FO", "CSV", "/NH"]).decode(
            "mbcs", "replace")
        m = re.search(r'"[^"]+","(\d+)"', out)
        if not m:
            sys.exit("gioco non in esecuzione")
        self.pid = int(m.group(1))
        self.h = k32.OpenProcess(0x0410, False, self.pid)

    def read(self, addr, n):
        buf = ctypes.create_string_buffer(n)
        got = ctypes.c_size_t()
        if not k32.ReadProcessMemory(self.h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)):
            return None
        return buf.raw[:got.value]

    def u32(self, addr):
        b = self.read(addr, 4)
        return struct.unpack("<I", b)[0] if b else None

    def i32(self, addr):
        b = self.read(addr, 4)
        return struct.unpack("<i", b)[0] if b else None

    def f32(self, addr, n=1):
        b = self.read(addr, 4 * n)
        return struct.unpack("<%df" % n, b) if b else None


class Names:
    def __init__(self, p):
        self.p = p
        self.data = p.u32(GNAMES)
        self.num = p.u32(GNAMES + 4)
        self.cache = {}

    def get(self, idx):
        if idx in self.cache:
            return self.cache[idx]
        if idx < 0 or idx >= self.num:
            return None
        e = self.p.u32(self.data + 4 * idx)
        if not e:
            return None
        raw = self.p.read(e + 0x10, 64) or b""
        # ANSI o UTF-16: se il secondo byte e' 0 e' wide.
        if len(raw) > 1 and raw[1] == 0:
            s = raw.decode("utf-16-le", "replace").split("\0")[0]
        else:
            s = raw.split(b"\0")[0].decode("latin-1")
        self.cache[idx] = s
        return s

    def find(self, text):
        return [i for i in range(self.num) if self.get(i) == text]


class Objects:
    def __init__(self, p, names):
        self.p, self.n = p, names
        d = p.u32(GOBJECTS)
        num = p.u32(GOBJECTS + 4)
        arr = b""
        for a in range(0, num * 4, 0x10000):
            arr += p.read(d + a, min(0x10000, num * 4 - a))
        self.ptrs = struct.unpack("<%dI" % num, arr)

    def name(self, o):
        return self.n.get(self.p.u32(o + OBJ_NAME)) if o else None

    def cls(self, o):
        return self.p.u32(o + OBJ_CLASS)

    def full(self, o):
        parts = []
        while o and len(parts) < 8:
            parts.append(self.name(o))
            o = self.p.u32(o + OBJ_OUTER)
        return ".".join(reversed(parts))

    def find(self, name, clsname=None):
        out = []
        for o in self.ptrs:
            if o and self.name(o) == name and (clsname is None or self.name(self.cls(o)) == clsname):
                out.append(o)
        return out

    def find_class(self, name):
        r = self.find(name, "Class")
        return r[0] if r else None

    def props(self, cls, inherited=True):
        """[(classe, nome, tipo, offset, arraydim)] della classe e delle superclassi."""
        out = []
        c = cls
        while c:
            f = self.p.u32(c + STRUCT_CHILDREN)
            while f:
                t = self.name(self.cls(f))
                if t and t.endswith("Property"):
                    dim = self.p.u32(f + PROP_ARRAYDIM)
                    if t == "BoolProperty":
                        dim = -self.p.u32(f + 0x84)  # maschera del bit (negativa = bool)
                    out.append((self.name(c), self.name(f), t, self.p.u32(f + PROP_OFFSET), dim))
                f = self.p.u32(f + FIELD_NEXT)
            if not inherited:
                break
            c = self.p.u32(c + STRUCT_SUPER)
        return out

    def prop_offset(self, clsname, prop):
        for c, nme, t, off, dim in self.props(self.find_class(clsname)):
            if nme == prop:
                return off
        return None


if __name__ == "__main__":
    p = Proc()
    n = Names(p)
    cmd = sys.argv[1] if len(sys.argv) > 1 else "info"
    if cmd == "info":
        print("GNames data=%#x num=%d" % (n.data, n.num))
        for i in range(10):
            print(i, n.get(i))
    elif cmd == "names":
        for a in sys.argv[2:]:
            print(a, n.get(int(a)))
    elif cmd == "props":
        o = Objects(p, n)
        flt = sys.argv[3].lower() if len(sys.argv) > 3 else ""
        for c, nme, t, off, dim in sorted(o.props(o.find_class(sys.argv[2])), key=lambda x: x[3]):
            if flt in nme.lower():
                extra = "" if dim == 1 else (" bit 0x%x" % -dim if dim < 0 else " [%d]" % dim)
                print("%-22s %-34s %-18s +0x%04x%s" % (c, nme, t, off, extra))
    elif cmd == "objects":
        o = Objects(p, n)
        for x in o.find(sys.argv[2]):
            print(hex(x), o.name(o.cls(x)), o.full(x))
    elif cmd == "findname":
        print(n.find(sys.argv[2]))
