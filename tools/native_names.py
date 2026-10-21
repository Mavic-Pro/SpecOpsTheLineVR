"""Native function names of SpecOpsTheLine.exe from the UE3 registration table.

The shipping exe keeps the native UnrealScript names (e.g.
"APawnexecGetPawnViewLocation") because the engine binds script bytecode to
natives by name: `.data` holds 8-byte records {name pointer, function pointer}.
This scans the whole file for such records and writes `address<TAB>function<TAB>name`
lines (the REA/Ghidra seed format), e.g. for the persistent Ghidra project.

    python tools/native_names.py [exe] [out.tsv]
"""
import re
import struct
import sys

import pefile

EXE = sys.argv[1] if len(sys.argv) > 1 else r'D:\SteamLibrary\steamapps\common\SpecOps_TheLine\Binaries\Win32\SpecOpsTheLine.exe'
OUT = sys.argv[2] if len(sys.argv) > 2 else 'native_names.tsv'

pe = pefile.PE(EXE, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
data = open(EXE, 'rb').read()
text = next(s for s in pe.sections if s.Name.rstrip(b'\0') == b'.text')
t_lo, t_hi = base + text.VirtualAddress, base + text.VirtualAddress + text.Misc_VirtualSize


def va_to_off(va):
    try:
        return pe.get_offset_from_rva(va - base)
    except Exception:
        return None


# "AClassexecName" / "UClassexecName" -> AClass::execName
NAME = re.compile(rb'^([AU][A-Za-z0-9_]+?)(exec[A-Za-z0-9_]+)$')
names = {}
for sec in pe.sections:
    if sec.Name.rstrip(b'\0') not in (b'.data', b'.rdata'):
        continue
    start, end = sec.PointerToRawData, sec.PointerToRawData + sec.SizeOfRawData
    for off in range(start, end - 8, 4):
        sp, fp = struct.unpack_from('<II', data, off)
        if not (t_lo <= fp < t_hi):
            continue
        so = va_to_off(sp)
        if so is None or so >= len(data):
            continue
        s = data[so:so + 160].split(b'\0', 1)[0]
        m = NAME.match(s)
        if m:
            names[fp] = '%s::%s' % (m.group(1).decode(), m.group(2).decode())

with open(OUT, 'w', encoding='ascii', newline='\n') as f:
    f.write('# rea-ghidra-seeds-v1 compatible: address<TAB>kind<TAB>name (UE3 native registration table)\n')
    for fp in sorted(names):
        f.write('0x%x\tfunction\t%s\n' % (fp, names[fp]))
print('%d native functions -> %s' % (len(names), OUT))
for fp in (0x00BBD730, 0x00D2E7F0, 0x0057E900):
    print(hex(fp), names.get(fp))
