"""Generate src/game_sigs.inc: byte signatures for every fixed address the mod
uses, so that a different SpecOpsTheLine.exe (non-Steam copy, other build) can
be supported by searching the code instead of trusting the Steam addresses.

    python tools/gensigs.py [path\\to\\SpecOpsTheLine.exe]   # writes src/game_sigs.inc
    python tools/gensigs.py --check <other.exe>             # report which signatures resolve there

How a signature is built (always from the Steam exe, build 12/07/2012):
- bytes that change between builds are wildcards: every dword listed in the
  .reloc table (absolute addresses of globals, vtables, strings, functions) and
  the rel32 operand of call/jmp/jcc (targets move when code moves);
- the window starts at the address itself (code) or at the instruction that
  references the global (data) and grows one instruction at a time until it
  matches exactly once in .text.
Import-table slots are resolved at run time by DLL + function name/ordinal.
"""
import os
import re
import struct
import sys

import capstone
import pefile

DEFAULT_EXE = r'D:\SteamLibrary\steamapps\common\SpecOps_TheLine\Binaries\Win32\SpecOpsTheLine.exe'
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'src', 'game_sigs.inc')

# (id, Steam VA). Code = function start, mid-function hook point or return address.
CODE = [
    ('ExecGetPawnViewLocation', 0x00BBD730),
    ('ExecGetViewTarget', 0x0057E900),
    ('CameraHook', 0x007ECAB2),
    ('ActorMoveSmooth', 0x0088FB70),
    ('ViewportDraw', 0x008F6A80),
    ('AllocateViewState', 0x00C4F370),
    ('GetFovAngle', 0x006F1EA0),
    ('PerspectiveMatrix', 0x009EAE80),
    ('CalcSceneView', 0x0075C460),
    ('CsvRetPerspectiveA', 0x0075CAC5),
    ('CsvRetPerspectiveB', 0x0075CBBA),
    ('GetPlayerViewPoint', 0x00AB83F0),
    ('SingleLineCheck', 0x009BDE30),
    ('SelectMarkable', 0x008389D0),
    ('CalcWeaponFire', 0x004A3D50),
    ('CanvasProject', 0x00A15D90),
    ('ViewportSize', 0x00B45070),
    ('UiViewportSize', 0x00574390),
    ('SceneViewportSize', 0x00F539E0),
    ('EngineCrashFilter', 0x00827770),
    ('CwfRetInstant', 0x0093FEA7),
    ('CwfRetShotgun', 0x00F6302E),
    ('CwfRetScript', 0x00C27281),
    ('RaiseWeapon', 0x0052F2B0),
    ('UpdateSkelPose', 0x006455E0),
    ('HearStimFill', 0x00E428A0),
    ('RetFromBotHandler', 0x00A367A3),
    ('InstantFire', 0x0093F510),
]
# Globals / vtables: found through an instruction that references them.
DATA = [
    ('GSystemSettings', 0x014D77B0),
    ('GSystemSettingsVTable', 0x0113A00C),
    ('NearClipPlane', 0x0141578C),
    ('GWorld', 0x014DF734),
    ('GObjects', 0x014D41EC),
    ('GNames', 0x014C1EC8),
    ('PadSlots', 0x014EB910),
]
# Import-table slots: (id, Steam VA, dll, name or ordinal)
IAT = [
    ('IatGetSystemMetrics', 0x01109660, 'USER32.dll', 'GetSystemMetrics'),
    ('IatSystemParametersInfoW', 0x01109664, 'USER32.dll', 'SystemParametersInfoW'),
    ('IatSetWindowPos', 0x011096C4, 'USER32.dll', 'SetWindowPos'),
    ('IatMoveWindow', 0x01109570, 'USER32.dll', 'MoveWindow'),
    ('IatGetClientRect', 0x01109614, 'USER32.dll', 'GetClientRect'),
    ('IatGetForegroundWindow', 0x011095C4, 'USER32.dll', 'GetForegroundWindow'),
    ('IatGetActiveWindow', 0x011096A4, 'USER32.dll', 'GetActiveWindow'),
    ('IatGetFocus', 0x011096D0, 'USER32.dll', 'GetFocus'),
    ('IatShowWindow', 0x0110963C, 'USER32.dll', 'ShowWindow'),
    ('IatSetWindowPlacement', 0x0110968C, 'USER32.dll', 'SetWindowPlacement'),
    ('IatGetCommandLineW', 0x01109184, 'KERNEL32.dll', 'GetCommandLineW'),
    ('IatMemcpy', 0x01109438, 'MSVCR90.dll', 'memcpy'),
    ('IatMemmove', 0x0110943C, 'MSVCR90.dll', 'memmove'),
    ('IatFmodSetListener', 0x01109878, 'fmod_event.dll',
     '?set3DListenerAttributes@EventSystem@FMOD@@QAG?AW4FMOD_RESULT@@HPBUFMOD_VECTOR@@000@Z'),
    ('IatXInputSetState', 0x01109748, 'XINPUT1_3.dll', 3),
    ('IatXInputGetState', 0x0110974C, 'XINPUT1_3.dll', 2),
]
MAX_INSNS = 40
# At least this many non-wildcard bytes: a shorter window is unique here but
# could match the wrong place in another build (a wrong hook is worse than none).
MIN_FIXED = 12


class Image:
    def __init__(self, path):
        self.pe = pe = pefile.PE(path)
        self.base = pe.OPTIONAL_HEADER.ImageBase
        text = next(s for s in pe.sections if s.Name.rstrip(b'\0') == b'.text')
        self.text_va = self.base + text.VirtualAddress
        self.text = text.get_data()[:text.Misc_VirtualSize]
        self.relocs = set()
        for block in getattr(pe, 'DIRECTORY_ENTRY_BASERELOC', []):
            for e in block.entries:
                if e.type == 3:  # HIGHLOW
                    self.relocs.add(self.base + e.rva)
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True

    def in_text(self, va):
        return self.text_va <= va < self.text_va + len(self.text)

    def u32(self, va):
        return struct.unpack_from('<I', self.text, va - self.text_va)[0]

    def insns(self, va):
        off = va - self.text_va
        return self.md.disasm(self.text[off:off + MAX_INSNS * 15], va)

    def mask_of(self, insn):
        """Per-byte mask (True = wildcard) of one instruction."""
        m = [False] * insn.size
        for i in range(insn.size):
            if insn.address + i in self.relocs:
                for k in range(4):
                    if i + k < insn.size:
                        m[i + k] = True
        rel_branch = insn.group(capstone.x86.X86_GRP_JUMP) or insn.group(capstone.x86.X86_GRP_CALL)
        if rel_branch and insn.imm_size == 4 and insn.operands and insn.operands[0].type == capstone.x86.X86_OP_IMM:
            for k in range(4):
                m[insn.imm_offset + k] = True
        return m

    def count(self, pat, mask, limit=2):
        rx = re.compile(b''.join(b'.' if w else re.escape(bytes([b])) for b, w in zip(pat, mask)), re.DOTALL)
        n = 0
        for _ in rx.finditer(self.text):
            n += 1
            if n >= limit:
                break
        return n

    def signature(self, va):
        """Shortest unique masked window starting at va, or None."""
        pat, mask = b'', []
        for insn in self.insns(va):
            pat += bytes(insn.bytes)
            mask += self.mask_of(insn)
            if sum(not w for w in mask) < MIN_FIXED:
                continue
            if self.count(pat, mask) == 1:
                return pat, mask
        return None

    def code_signature(self, va, starts=12):
        """Shortest unique window starting at va or a few instructions after it
        (function prologues are common to many functions). Returns
        (pat, mask, skip): the address is match - skip."""
        best = None
        for i, insn in enumerate(self.insns(va)):
            if i >= starts:
                break
            sig = self.signature(insn.address)
            if sig and (not best or len(sig[0]) < len(best[0])):
                best = (sig[0], sig[1], insn.address - va)
        return best

    def instruction_at_ref(self, pos):
        """Instruction that contains the dword at pos (a reloc), if any."""
        for start in range(pos - 1, pos - 8, -1):
            try:
                insn = next(self.md.disasm(self.text[start - self.text_va:start - self.text_va + 15], start))
            except StopIteration:
                continue
            if insn.address + insn.size >= pos + 4 and (insn.disp_offset == pos - start or insn.imm_offset == pos - start):
                return insn
        return None


def fmt(pat, mask):
    return ' '.join('??' if w else '%02X' % b for b, w in zip(pat, mask))


def build(img):
    rows = []
    for name, va in CODE:
        sig = img.code_signature(va)
        if not sig:
            sys.exit(f'{name} @ {va:#x}: no unique signature')
        pat, mask, skip = sig
        rows.append(f'SIG_CODE({name}, 0x{va:08X}, {skip}, "{fmt(pat, mask)}")')
        print(f'{name:24s} {va:#010x} +{skip:<3d} {len(pat):3d} bytes')
    for name, target in DATA:
        best = None
        refs = [p for p in sorted(img.relocs) if img.in_text(p) and img.in_text(p + 3) and img.u32(p) == target]
        for pos in refs[:300]:
            insn = img.instruction_at_ref(pos)
            if not insn:
                continue
            sig = img.signature(insn.address)
            if sig and (not best or len(sig[0]) < len(best[1][0])):
                best = (insn.address, sig, pos - insn.address)
                if len(sig[0]) <= 12:
                    break
        if not best:
            sys.exit(f'{name} @ {target:#x}: no unique reference among {len(refs)}')
        ref, sig, off = best
        rows.append(f'SIG_DATA({name}, 0x{target:08X}, 0x{ref:08X}, {off}, "{fmt(*sig)}")')
        print(f'{name:24s} {target:#010x} via {ref:#010x}+{off} {len(sig[0]):3d} bytes ({len(refs)} refs)')
    imports = {}
    for e in img.pe.DIRECTORY_ENTRY_IMPORT:
        for i in e.imports:
            imports[i.address] = (e.dll.decode(), i.name.decode() if i.name else i.ordinal)
    for name, va, dll, fn in IAT:
        if imports.get(va) != (dll, fn):
            sys.exit(f'{name}: import slot {va:#x} is {imports.get(va)}, expected {(dll, fn)}')
        key = f'"{fn}", 0' if isinstance(fn, str) else f'nullptr, {fn}'
        rows.append(f'SIG_IAT({name}, 0x{va:08X}, "{dll}", {key})')
    return rows


def check(img, path_inc):
    """Resolve the generated signatures in another exe (offline compatibility report)."""
    ok = 0
    lines = open(path_inc, encoding='utf-8').read().splitlines()
    for line in lines:
        m = re.match(r'SIG_(CODE|DATA)\((\w+), 0x(\w+)(?:, 0x(\w+))?, (\d+), "([^"]+)"\)', line)
        if not m:
            continue
        kind, name, va, ref, off, pat = m.groups()
        toks = pat.split()
        rx = re.compile(b''.join(b'.' if t == '??' else re.escape(bytes([int(t, 16)])) for t in toks), re.DOTALL)
        hits = [img.text_va + h.start() for h in rx.finditer(img.text)][:3]
        res = '-'
        if len(hits) == 1:
            ok += 1
            res = f'{hits[0] - int(off):#010x}' if kind == 'CODE' else f'{img.u32(hits[0] + int(off)):#010x}'
        print(f'{name:24s} {"OK " if len(hits) == 1 else "NO "} hits={len(hits)} -> {res}')
    print(f'{ok} signatures resolved')


def main():
    args = sys.argv[1:]
    if args and args[0] == '--check':
        check(Image(args[1]), OUT)
        return
    img = Image(args[0] if args else DEFAULT_EXE)
    rows = build(img)
    with open(OUT, 'w', encoding='utf-8', newline='\n') as f:
        f.write('// Generated by tools/gensigs.py from the Steam SpecOpsTheLine.exe (TimeDateStamp 0x50041F0C).\n')
        f.write('// Do not edit by hand: add the address to the lists in gensigs.py and run it again.\n')
        f.write('// "??" = byte that changes between builds (relocated address or rel32 branch target).\n')
        for r in rows:
            f.write(r + '\n')
    print(f'{len(rows)} entries -> {OUT}')


if __name__ == '__main__':
    main()
