# tools/calltree.py <c.trace> <dll or exe> [filter_base]: rebuilds the call tree from a C trace
import array
import os, sys
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME = os.environ.get('POPTB_GAME', os.path.dirname(ROOT))
sys.path.insert(0, os.path.join(ROOT, 'recomp'))
from image import *
img = Image(sys.argv[2])
a = array.array('I'); a.frombytes(open(sys.argv[1], 'rb').read())
n = len(a) // 9
names = {v: k for k, v in img.exports.items()}
depth = 0
stack = []
for i in range(n):
    addr = a[i * 9]
    ins = img.insn(addr)
    if not ins: continue
    if i > 0:
        prev = img.insn(a[(i - 1) * 9])
        if prev and prev.mnemonic == 'call' and addr != prev.address + prev.size:
            print('%s%x %s(ecx=%x)' % ('  ' * depth, addr, names.get(addr, ''), a[i * 9 + 2]))
            stack.append(prev.address + prev.size); depth += 1
    if ins.mnemonic == 'ret' and stack:
        depth -= 1
        nxt = a[(i + 1) * 9 + 1] if i + 1 < n else a[i * 9 + 1]
        print('%s-> eax=%x' % ('  ' * depth, nxt))
        stack.pop()
