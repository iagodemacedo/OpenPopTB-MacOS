# tools/dis.py <addr> [n]: disassembles popTB.exe
import os, sys
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME = os.environ.get('POPTB_GAME', os.path.dirname(ROOT))
sys.path.insert(0, os.path.join(ROOT, 'recomp'))
from image import *
img = Image(os.path.join(GAME, 'popTB.exe'))
a = int(sys.argv[1], 16); n = int(sys.argv[2]) if len(sys.argv) > 2 else 40
for _ in range(n):
    i = img.insn(a)
    if not i: print(hex(a), '??'); break
    print('%x  %-8s %s' % (a, i.mnemonic, i.op_str)); a += i.size
