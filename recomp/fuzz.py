# Differential fuzzer: each instruction form in popTB.exe runs in Unicorn (reference) and in the generated C.
# usage: python fuzz.py popTB.exe functions.tsv outdir [samples_per_form] [states_per_sample]
import sys, os, random, struct, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from image import *
from gen import Gen, Func, CC, flag_reads
from capstone import x86_const as X
import unicorn as U
from unicorn import x86_const as UX

SKIP = {'call', 'ret', 'jmp', 'int', 'int3', 'out', 'in', 'cli', 'sti', 'hlt', 'iretd', 'retf', 'cpuid', 'rdtsc',
        'nop', 'wait', 'fwait', 'leave', 'pushal', 'popal', 'insd', 'insb', 'outsd', 'outsb', 'into', 'arpl', 'bound'}
F_CF, F_PF, F_AF, F_ZF, F_SF, F_OF = 1, 4, 16, 64, 128, 2048
ALLF = F_CF | F_PF | F_ZF | F_SF | F_OF

UREG = [UX.UC_X86_REG_EAX, UX.UC_X86_REG_ECX, UX.UC_X86_REG_EDX, UX.UC_X86_REG_EBX,
        UX.UC_X86_REG_ESP, UX.UC_X86_REG_EBP, UX.UC_X86_REG_ESI, UX.UC_X86_REG_EDI]
R32I = {X.X86_REG_EAX: 0, X.X86_REG_ECX: 1, X.X86_REG_EDX: 2, X.X86_REG_EBX: 3,
        X.X86_REG_ESP: 4, X.X86_REG_EBP: 5, X.X86_REG_ESI: 6, X.X86_REG_EDI: 7}


def hash32(x):
    x &= 0xffffffff
    x ^= x >> 16; x = (x * 0x7feb352d) & 0xffffffff
    x ^= x >> 15; x = (x * 0x846ca68b) & 0xffffffff
    x ^= x >> 16
    return x


def page_bytes(page, seed):
    return bytes(hash32(page + i + seed * 0x9E3779B9) & 0xff for i in range(4096))


import math, struct as _st


def d2x(v):
    """double -> (mantissa, exponent) in Unicorn's 80-bit format."""
    if v == 0: return (0, 0x8000 if math.copysign(1, v) < 0 else 0)
    if math.isinf(v): return (1 << 63, 0x7fff | (0x8000 if v < 0 else 0))
    if math.isnan(v): return (0xC000000000000000, 0x7fff)
    m, e = math.frexp(abs(v))
    man = int(m * (1 << 64)) & 0xffffffffffffffff
    return (man, (e - 1 + 16383) | (0x8000 if v < 0 else 0))


def x2d(t):
    man, se = t
    sign = -1.0 if se & 0x8000 else 1.0
    e = se & 0x7fff
    if e == 0 and man == 0: return sign * 0.0
    if e == 0x7fff: return float('nan') if (man << 1) & 0xffffffffffffffff else sign * float('inf')
    try:
        return sign * math.ldexp(man, e - 16383 - 63)
    except OverflowError:
        return sign * float('inf')


TRANSC = {'fsin', 'fcos', 'fpatan', 'fyl2x', 'f2xm1', 'fsqrt', 'fscale', 'fptan', 'fsincos'}


def form_key(ins):
    parts = [ins.mnemonic]
    for o in ins.operands:
        if o.type == X.X86_OP_REG:
            n = ins.reg_name(o.reg)
            parts.append('r%d%s' % (o.size, 'h' if n.endswith('h') and len(n) == 2 else ''))
        elif o.type == X.X86_OP_IMM: parts.append('i%d' % o.size)
        else: parts.append('m%d' % o.size)
    return tuple(parts)


def flag_mask(ins, regs):
    m = ins.mnemonic
    base = m.split()[-1]
    if m in ('div', 'idiv'): return 0
    if m in ('mul', 'imul'): return F_CF | F_OF
    if m in ('bt', 'bts', 'btr', 'btc'): return F_CF
    if m in ('bsr', 'bsf'): return F_ZF
    if m in ('shl', 'sal', 'shr', 'sar', 'rol', 'ror', 'rcl', 'rcr', 'shld', 'shrd'):
        ops = ins.operands
        c = ops[-1]
        if len(ops) == 1 or (m in ('shld', 'shrd') and False): cnt = 1
        elif c.type == X.X86_OP_IMM: cnt = c.imm & 31
        else: cnt = regs[1] & 31
        if m in ('shld', 'shrd') and c.type != X.X86_OP_IMM: cnt = regs[1] & 31
        if cnt == 0: return ALLF
        sz = ops[0].size * 8
        mask = ALLF
        if m in ('rol', 'ror', 'rcl', 'rcr'):
            mask = ALLF if cnt == 1 else ALLF & ~F_OF
            if m in ('rcl', 'rcr') and sz < 32 and cnt > sz: mask = 0
            return mask
        if cnt != 1: mask &= ~F_OF
        if cnt >= sz: mask &= ~F_CF   # shift >= width: CF undefined
        if m in ('shld', 'shrd'): mask &= ~F_OF
        return mask
    return ALLF


class Fuzz:
    def __init__(self, img, g, allins):
        self.img, self.g = img, g
        self.allins = allins

    def gen_c(self, tid, insns):
        g = self.g
        a = insns[0].address
        f = Func(g, a, [i.address for i in insns])
        g.cur = f
        g.live_out = {i.address: True for i in insns}
        g.labels = set()
        orig = g.jump_to
        g.jump_to = lambda t, ins: '{ taken = 1; return; }'   # exit now: code after the branch must not run
        try:
            code, consumed = g._gen(f, insns[0], insns[0].mnemonic, a, insns[0].operands, True, insns[1] if len(insns) > 1 else None)
            if len(insns) > 1 and not consumed:
                return None
        finally:
            g.jump_to = orig
        return 'static void t_%d(CPU *R) { %s }' % (tid, code)

    def rand_state(self, rnd, ins):
        def val():
            r = rnd.random()
            if r < 0.15: return rnd.choice([0, 1, 0xffffffff, 0x80000000, 0x7fffffff, 0xff, 0x80, 0x7f, 0xffff, 0x8000, 0x7fff, 2, 31, 32, 33])
            if r < 0.35: return rnd.getrandbits(8)
            if r < 0.5: return rnd.getrandbits(16)
            return rnd.getrandbits(32)
        regs = [val() for _ in range(8)]
        regs[4] = 0x00800000 + rnd.getrandbits(12) * 4
        # registers used as base/index point to a valid region
        for o in ins.operands:
            if o.type == X.X86_OP_MEM:
                for r in (o.mem.base, o.mem.index):
                    if r in R32I and R32I[r] != 4:
                        regs[R32I[r]] = 0x01000000 + rnd.getrandbits(16) * 4 if rnd.random() < 0.9 else rnd.getrandbits(8)
        m = ins.mnemonic
        if m.startswith('rep'):
            regs[1] = rnd.getrandbits(6)
            regs[6] = 0x01000000 + rnd.getrandbits(16)
            regs[7] = 0x01200000 + rnd.getrandbits(16)
        elif any(m.startswith(p) for p in ('movs', 'stos', 'scas', 'cmps', 'lods')):
            regs[6] = 0x01000000 + rnd.getrandbits(16)
            regs[7] = 0x01200000 + rnd.getrandbits(16)
        if m == 'xlatb':
            regs[3] = 0x01000000 + rnd.getrandbits(16)
        def fv():
            r = rnd.random()
            if r < 0.2: return float(rnd.randint(-1000, 1000))
            if r < 0.4: return rnd.uniform(-2.0, 2.0)
            if r < 0.6: return rnd.uniform(-1e6, 1e6)
            if r < 0.7: return rnd.choice([0.0, -0.0, 0.5, -0.5, 1.5, 2.5, -2.5, 1e-300, 6755399441055744.0])
            return rnd.uniform(-1e3, 1e3) * 2 ** rnd.randint(-30, 30)
        self.fst = [fv() for _ in range(8)]
        flags = 0x2 | (rnd.getrandbits(1) * F_CF) | (rnd.getrandbits(1) * F_PF) | (rnd.getrandbits(1) * F_AF) | \
                (rnd.getrandbits(1) * F_ZF) | (rnd.getrandbits(1) * F_SF) | (rnd.getrandbits(1) * F_OF)
        return regs, flags

    def run_uc(self, insns, regs, flags, seed, fst):
        img = self.img
        uc = U.Uc(U.UC_ARCH_X86, U.UC_MODE_32)
        code_pages = {}
        for i in insns:
            for p in (i.address & ~0xfff, (i.address + i.size - 1) & ~0xfff):
                if p not in code_pages:
                    o = p - img.base
                    code_pages[p] = bytes(img.mem[o:o + 4096]).ljust(4096, b'\0')
        mapped = {}
        for p, b in code_pages.items():
            uc.mem_map(p, 4096); uc.mem_write(p, b); mapped[p] = 'code'
        writes = {}

        def on_unmapped(uc_, access, addr, size, value, ud):
            for p in {addr & ~0xfff, (addr + size - 1) & ~0xfff}:
                if p not in mapped:
                    if p < 0x10000: return False
                    uc_.mem_map(p, 4096); uc_.mem_write(p, page_bytes(p, seed)); mapped[p] = 'data'
            return True

        def on_write(uc_, access, addr, size, value, ud):
            for k in range(size): writes[addr + k] = None

        uc.hook_add(U.UC_HOOK_MEM_UNMAPPED, on_unmapped)
        uc.hook_add(U.UC_HOOK_MEM_WRITE, on_write)
        for r, v in zip(UREG, regs): uc.reg_write(r, v)
        uc.reg_write(UX.UC_X86_REG_EFLAGS, flags)
        uc.reg_write(UX.UC_X86_REG_FPCW, 0x27f)
        uc.reg_write(UX.UC_X86_REG_FPSW, 0)
        uc.reg_write(UX.UC_X86_REG_FPTAG, 0)
        for k in range(8): uc.reg_write(UX.UC_X86_REG_FP0 + k, d2x(fst[k]))
        start = insns[0].address
        end = insns[-1].address + insns[-1].size
        try:
            uc.emu_start(start, 0xffffffff, count=len(insns))
        except U.UcError:
            return None
        out = [uc.reg_read(r) for r in UREG]
        ofl = uc.reg_read(UX.UC_X86_REG_EFLAGS)
        eip = uc.reg_read(UX.UC_X86_REG_EIP)
        wr = sorted((a, uc.mem_read(a, 1)[0]) for a in writes)
        fout = [x2d(uc.reg_read(UX.UC_X86_REG_FP0 + k)) for k in range(8)]
        fsw = uc.reg_read(UX.UC_X86_REG_FPSW)
        return out, ofl, eip, [p for p, k in mapped.items() if k == 'data'], wr, end, fout, fsw


def main():
    exe, tsv, outdir = sys.argv[1:4]
    per = int(sys.argv[4]) if len(sys.argv) > 4 else 3
    nst = int(sys.argv[5]) if len(sys.argv) > 5 else 12
    img = Image(exe, tsv)
    entries, funcs, tables, bad, allins = discover_all(img)
    g = Gen(img, entries, funcs, tables)
    fz = Fuzz(img, g, allins)
    rnd = random.Random(1234)
    forms = collections.defaultdict(list)
    pairs = collections.defaultdict(list)
    for a in sorted(allins):
        ins = img.insn(a)
        m = ins.mnemonic
        if m in SKIP or m.startswith('loop') or m in ('fnsave', 'frstor', 'fninit'): continue
        if any(o.type == X.X86_OP_MEM and o.mem.segment for o in ins.operands): continue
        if m == 'push' or m == 'pop':
            if any(o.type == X.X86_OP_REG and o.reg not in R32I for o in ins.operands): continue
        k = form_key(ins)
        if len(forms[k]) < per: forms[k].append(ins)
        nxt = img.insn(a + ins.size)
        if nxt and (a + ins.size) in allins and g.consumer(nxt):
            pk = (k, nxt.mnemonic)
            if len(pairs[pk]) < 2: pairs[pk].append((ins, nxt))
    cases = []
    for k, lst in forms.items():
        for ins in lst: cases.append([ins])
    for k, lst in pairs.items():
        for p in lst: cases.append(list(p))
    os.makedirs(outdir, exist_ok=True)
    cfuncs, recs = [], []
    tid = 0
    skipped = collections.Counter()
    for insns in cases:
        try:
            c = fz.gen_c(tid, insns)
        except Exception as e:
            skipped['gen ' + insns[0].mnemonic] += 1
            continue
        if c is None or 'rt_trap' in c:
            skipped['trap ' + insns[0].mnemonic] += 1
            continue
        ok_states = []
        for s in range(nst):
            regs, flags = fz.rand_state(rnd, insns[0])
            seed = rnd.getrandbits(31)
            fst = fz.fst
            r = fz.run_uc(insns, regs, flags, seed, fst)
            if r is None: continue
            out, ofl, eip, pages, wr, end, fout, fsw = r
            last = insns[-1]
            taken = 0
            if last.mnemonic.startswith('j'):
                taken = 1 if eip != end else 0
            # producer+jcc/setcc pairs also check the written flags (the branch target may re-read them)
            mask = flag_mask(insns[0], regs)
            if len(insns) == 1 and (insns[0].mnemonic.startswith('j') or insns[0].mnemonic.startswith('set')): mask = ALLF
            isf = insns[0].mnemonic.startswith('f')
            fmode = 0 if not isf else (2 if insns[0].mnemonic in TRANSC else 1)
            ok_states.append((regs, flags, seed, pages, out, ofl, mask, taken, wr, fst, fout, fsw, fmode))
        if not ok_states:
            skipped['uc ' + insns[0].mnemonic] += 1
            continue
        cfuncs.append(c)
        text = '; '.join('%s %s' % (i.mnemonic, i.op_str) for i in insns)
        code_pages = sorted({p for i in insns for p in (i.address & ~0xfff, (i.address + i.size - 1) & ~0xfff)})
        recs.append((tid, text, code_pages, ok_states))
        tid += 1
    with open(os.path.join(outdir, 'fz_tests.c'), 'w') as f:
        f.write('#include "cpu.h"\n#include "rt_ops.h"\nextern int taken;\n#pragma clang diagnostic ignored "-Wunused-variable"\n')
        f.write('\n'.join(cfuncs) + '\n')
        f.write('typedef void (*tfn)(CPU *);\nconst tfn fz_fns[] = {%s};\nconst int fz_n = %d;\n' % (', '.join('t_%d' % i for i in range(tid)), tid))
        f.write('const char *fz_text[] = {\n%s};\n' % ',\n'.join('"%s"' % r[1].replace('"', "'") for r in recs))
    with open(os.path.join(outdir, 'fz_cases.bin'), 'wb') as f:
        f.write(struct.pack('<I', len(recs)))
        for tid_, text, cps, states in recs:
            f.write(struct.pack('<III', tid_, len(cps), len(states)))
            for p in cps: f.write(struct.pack('<I', p))
            for regs, flags, seed, pages, out, ofl, mask, taken, wr, fst, fout, fsw, fmode in states:
                f.write(struct.pack('<8II', *regs, flags))
                f.write(struct.pack('<8d', *fst))
                f.write(struct.pack('<II', seed, len(pages)))
                for p in pages: f.write(struct.pack('<I', p))
                f.write(struct.pack('<8IIII', *out, ofl, mask, taken))
                f.write(struct.pack('<I', len(wr)))
                for a, b in wr: f.write(struct.pack('<IB', a, b))
                f.write(struct.pack('<8dII', *fout, fsw, fmode))
    print('cases', len(recs), 'skipped', skipped.most_common(20))


if __name__ == '__main__':
    main()
