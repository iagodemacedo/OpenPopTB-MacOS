import sys, collections
sys.path.insert(0, sys.argv[1])
from image import *
img = Image(sys.argv[2], sys.argv[3])
entries, funcs, tables, bad = discover(img)
print('funcs', len(funcs), 'tables', len(tables), 'bad', len(bad))
allins = set().union(*funcs.values())
print('unique insns', len(allins), 'total emitted', sum(len(v) for v in funcs.values()))
# reloc targets in code not covered
jtent = set(); jtloc=set()
for a,(t,l) in tables.items():
    jtent |= set(l); jtloc |= set(range(t, t+4*len(l)))
rt = collections.Counter()
unc = []
for r in img.relocs:
    t = img.u32(r)
    if img.is_code(t) and r not in jtloc:
        if t not in allins: unc.append((r,t))
print('reloc code targets not covered', len(unc))
for r,t in sorted(unc)[:40]: print(hex(r), hex(t), img.is_code(r))
c = collections.Counter(img.insn(a).mnemonic for a in allins)
print(sorted(c.items(), key=lambda x:-x[1]))
print('bad', [hex(b) for b in sorted(bad)[:30]])
secs = collections.Counter()
for a in allins:
    for n,va,sz,ch in img.sections:
        if va<=a<va+sz: secs[(n,hex(va))]+=1
print(secs)
