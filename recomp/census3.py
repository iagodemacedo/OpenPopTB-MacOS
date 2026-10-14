import sys, collections
sys.path.insert(0, sys.argv[1])
from image import *
img = Image(sys.argv[2], sys.argv[3])
entries, funcs, tables, bad, allins = discover_all(img)
print('funcs', len(funcs), 'tables', len(tables), 'bad', len(bad), 'insns', len(allins), 'emitted', sum(len(v) for v in funcs.values()))
print('bad', [hex(b) for b in sorted(bad)[:40]])
c = collections.Counter(img.insn(a).mnemonic for a in allins)
print(sorted(c.items(), key=lambda x:-x[1]))
