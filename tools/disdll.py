# tools/disdll.py <dll> <addr> [n]: disassembles a DLL, annotating imports
import os, sys
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME = os.environ.get('POPTB_GAME', os.path.dirname(ROOT))
sys.path.insert(0, os.path.join(ROOT, 'recomp'))
from image import *
img = Image(sys.argv[1])
a = int(sys.argv[2], 16); n = int(sys.argv[3]) if len(sys.argv) > 3 else 60
for _ in range(n):
    i = img.insn(a)
    if not i: print(hex(a), '??'); break
    s = i.op_str
    if '[0x' in s:
        try:
            t = int(s.split('[0x')[1].split(']')[0], 16)
            if t in img.imports: s += '  ; ' + img.imports[t][1]
        except ValueError: pass
    print('%x  %-6s %s' % (a, i.mnemonic, s)); a += i.size
