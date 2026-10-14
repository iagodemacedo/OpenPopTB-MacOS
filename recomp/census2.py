import sys, collections
sys.path.insert(0, sys.argv[1])
from image import *
img = Image(sys.argv[2], sys.argv[3])
entries, funcs, tables, bad = discover(img)
allins = set().union(*funcs.values())
owner = {}
for a in allins:
    ins = img.insn(a)
    for k in range(a, a+ins.size): owner[k]=a
jtloc=set()
for a,(t,l) in tables.items(): jtloc |= set(range(t, t+4*len(l)))
kinds = collections.Counter(); seeds=set(); ex=collections.defaultdict(list)
for r in sorted(img.relocs):
    t = img.u32(r)
    if not img.is_code(t) or r in jtloc or t in allins: continue
    if r in owner:
        ins = img.insn(owner[r]); k = ins.mnemonic + (' imm' if any(o.type==X.X86_OP_IMM for o in ins.operands) else ' mem')
    elif img.is_code(r): k='code-data'
    else: k='data:'+next(n for n,va,sz,ch in img.sections if va<=r<va+sz)
    kinds[k]+=1; ex[k].append((hex(r),hex(t)))
    if k in ('push imm','mov imm') or k.startswith('data:'): seeds.add(t)
print(kinds)
for k in kinds: print(k, ex[k][:6])
print('seeds', len(seeds))
