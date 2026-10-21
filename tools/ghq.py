"""Query the persistent Ghidra project of SpecOpsTheLine.exe (decompiled code
and callers) in a few seconds, without re-analysing the exe.

    python tools/ghq.py 0x00BBD730                 # function containing an address
    python tools/ghq.py "APawn_execGetPawnViewLocation"
    python tools/ghq.py "*FastRope*" "UCanvas_exec*"   # name patterns (* = anything)

Setup (once): Ghidra 12.1.x + JDK 21, project created with
`analyzeHeadless <dir> SpecOps -import SpecOpsTheLine.exe`, then names from
`tools/native_names.py` applied with ApplyNames.java (see docs/TECHNICAL.md).
"""
import os
import subprocess
import sys

GHIDRA = os.environ.get('GHIDRA_INSTALL_DIR', r'C:\Users\Matti\Tools\ghidra_12.1.4_PUBLIC')
PROJECT_DIR = os.environ.get('SPECOPS_GHIDRA_PROJECT', r'C:\Users\Matti\Tools\ghidra_projects')
SCRIPTS = os.environ.get('SPECOPS_GHIDRA_SCRIPTS', r'C:\Users\Matti\Tools\ghidra_scripts')

if len(sys.argv) < 2:
    sys.exit(__doc__)
cmd = [os.path.join(GHIDRA, 'support', 'analyzeHeadless.bat'), PROJECT_DIR, 'SpecOps', '-process',
       'SpecOpsTheLine.exe', '-noanalysis', '-readOnly', '-scriptPath', SCRIPTS, '-postScript', 'Decomp.java']
cmd += sys.argv[1:]
out = subprocess.run(cmd, capture_output=True, text=True, errors='replace').stdout
keep = False
for line in out.splitlines():
    if 'Decomp.java> ' in line:  # intestazioni "=== nome @ indirizzo" e "--- chiamata da"
        keep = True
        print(line.split('Decomp.java> ', 1)[1].replace(' (GhidraScript)', ''))
        continue
    if not keep or line.startswith('INFO') or line.startswith('WARN'):
        continue
    print(line.replace(' (GhidraScript)', ''))
