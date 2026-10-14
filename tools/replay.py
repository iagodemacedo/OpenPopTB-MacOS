# Differential replay: loads enter.snap into Unicorn, runs the function until it returns and compares with leave.snap.
# usage: python tools/replay.py <dir with enter.snap and leave.snap> [--trace]
import sys, struct
import unicorn as U
from unicorn import x86_const as UX

REGS = [UX.UC_X86_REG_EAX, UX.UC_X86_REG_ECX, UX.UC_X86_REG_EDX, UX.UC_X86_REG_EBX,
        UX.UC_X86_REG_ESP, UX.UC_X86_REG_EBP, UX.UC_X86_REG_ESI, UX.UC_X86_REG_EDI]
NAMES = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']


def load(p):
    with open(p, 'rb') as f:
        regs = struct.unpack('<12I', f.read(48))
        st = struct.unpack('<8d', f.read(64))
        n, = struct.unpack('<I', f.read(4))
        regions = []
        for _ in range(n):
            a, l = struct.unpack('<II', f.read(8))
            regions.append((a, f.read(l)))
    return regs + (st,), regions


def setup_fs(uc, base):
    # minimal GDT with one data descriptor for FS
    gdt = 0xfff00000
    uc.mem_map(gdt, 0x1000)
    def desc(base, limit, access, flags):
        d = limit & 0xffff
        d |= (base & 0xffffff) << 16
        d |= access << 40
        d |= ((limit >> 16) & 0xf) << 48
        d |= flags << 52
        d |= ((base >> 24) & 0xff) << 56
        return struct.pack('<Q', d)
    uc.mem_write(gdt + 8 * 3, desc(base, 0xfffff, 0xf3, 0xc))   # data, ring 3
    uc.reg_write(UX.UC_X86_REG_GDTR, (0, gdt, 0x1000, 0))
    uc.reg_write(UX.UC_X86_REG_FS, 3 * 8 | 3)


def main():
    d = sys.argv[1]
    trace = '--trace' in sys.argv
    (r0, reg0), (r1, reg1) = load(d + '/enter.snap'), load(d + '/leave.snap')
    uc = U.Uc(U.UC_ARCH_X86, U.UC_MODE_32)
    for a, b in reg0:
        sz = (len(b) + 0xfff) & ~0xfff
        uc.mem_map(a, sz)
        uc.mem_write(a, b)
    for r, v in zip(REGS, r0[:8]): uc.reg_write(r, v)
    uc.reg_write(UX.UC_X86_REG_EFLAGS, r0[8])
    uc.reg_write(UX.UC_X86_REG_FPCW, r0[10])
    try:
        if "--fs" in sys.argv: setup_fs(uc, r0[9])
    except Exception as e:
        print('fs:', e)
    esp = r0[4]
    ret = struct.unpack('<I', uc.mem_read(esp, 4))[0]
    import pathlib
    func = int(pathlib.Path(d + '/func').read_text(), 16) if pathlib.Path(d + '/func').exists() else None
    if func is None:
        print('missing func file with the address'); return
    count = [0]
    if trace:
        def hk(uc_, addr, size, ud):
            count[0] += 1
        uc.hook_add(U.UC_HOOK_CODE, hk)
    try:
        uc.emu_start(func, ret)
    except U.UcError as e:
        print('Unicorn stopped:', e, 'eip=%x' % uc.reg_read(UX.UC_X86_REG_EIP))
    print('instructions:', count[0] if trace else '(no trace)')
    out = [uc.reg_read(r) for r in REGS]
    for i, n in enumerate(NAMES):
        if out[i] != r1[i]: print('reg %s: ref=%08x c=%08x' % (n, out[i], r1[i]))
    total = 0
    for (a, b1) in reg1:
        b0 = bytes(uc.mem_read(a, len(b1)))
        if b0 == b1: continue
        # group differences
        i, n = 0, len(b1)
        runs = []
        while i < n:
            if b0[i] != b1[i]:
                j = i
                while j < n and (b0[j] != b1[j] or (j + 1 < n and b0[j + 1] != b1[j + 1])): j += 1
                runs.append((a + i, j - i))
                i = j
            else: i += 1
        total += len(runs)
        for (x, l) in runs[:30]:
            print('mem %08x+%d ref=%s c=%s' % (x, l, bytes(uc.mem_read(x, min(l, 16))).hex(), b1[x - a:x - a + min(l, 16)].hex()))
    print('diverging regions:', total)


if __name__ == '__main__' and '--cmp' not in sys.argv:
    main()


def compare_trace(d, lo, hi):
    """Compares the C trace (c.trace) against Unicorn instruction by instruction within [lo, hi)."""
    import array
    (r0, reg0) = load(d + '/enter.snap')
    uc = U.Uc(U.UC_ARCH_X86, U.UC_MODE_32)
    for a, b in reg0:
        uc.mem_map(a, (len(b) + 0xfff) & ~0xfff); uc.mem_write(a, b)
    for r, v in zip(REGS, r0[:8]): uc.reg_write(r, v)
    uc.reg_write(UX.UC_X86_REG_EFLAGS, r0[8])
    uc.reg_write(UX.UC_X86_REG_FPCW, r0[10])
    ct = array.array('I'); ct.frombytes(open(d + '/c.trace', 'rb').read())
    n = len(ct) // 9
    caddrs = set(ct[i * 9] for i in range(n))
    st = {'i': 0, 'bad': 0, 'prev': []}
    def hk(uc_, addr, size, ud):
        if not (lo <= addr < hi) or addr not in caddrs: return
        i = st['i']
        if i >= n: uc_.emu_stop(); return
        rec = ct[i * 9:(i + 1) * 9]
        regs = [uc_.reg_read(r) for r in REGS]
        if rec[0] != addr or list(rec[1:]) != regs:
            print('DIVERGENCE at step %d: unicorn at %x, C at %x' % (i, addr, rec[0]))
            for k, nm in enumerate(NAMES):
                mark = '  <--' if regs[k] != rec[1 + k] else ''
                print('  %s ref=%08x c=%08x%s' % (nm, regs[k], rec[1 + k], mark))
            print('  last addresses:', ' '.join('%x' % x for x in st['prev'][-12:]))
            uc_.emu_stop(); st['bad'] = 1; return
        st['prev'].append(addr)
        st['i'] = i + 1
    uc.hook_add(U.UC_HOOK_CODE, hk)
    func = int(open(d + '/func').read(), 16)
    ret = struct.unpack('<I', uc.mem_read(r0[4], 4))[0]
    try: uc.emu_start(func, ret)
    except U.UcError as e: print('Unicorn stopped:', e)
    if not st['bad']: print('traces match over %d steps (of %d)' % (st['i'], n))


if __name__ == '__main__' and '--cmp' in sys.argv:
    lo, hi = (int(x, 16) for x in sys.argv[sys.argv.index('--cmp') + 1].split('-'))
    compare_trace(sys.argv[1], lo, hi)
