# Loads the PE and discovers code by recursive descent.
# Output: dict of functions -> set of instruction addresses, jump tables.
import pefile, capstone
from capstone import x86_const as X

IMAGE_SCN_MEM_EXECUTE = 0x20000000

class Image:
    def __init__(self, path, seeds_tsv=None):
        self.pe = pe = pefile.PE(path)
        self.base = pe.OPTIONAL_HEADER.ImageBase
        self.entry = self.base + pe.OPTIONAL_HEADER.AddressOfEntryPoint
        self.size = pe.OPTIONAL_HEADER.SizeOfImage
        self.mem = bytearray(self.size)
        self.mem[:len(pe.header)] = pe.header
        self.sections = []
        for s in pe.sections:
            va = self.base + s.VirtualAddress
            d = s.get_data()
            n = min(len(d), s.Misc_VirtualSize or len(d))
            self.mem[s.VirtualAddress:s.VirtualAddress + n] = d[:n]
            self.sections.append((s.Name.rstrip(b'\0').decode(), va, max(s.Misc_VirtualSize, len(d)), s.Characteristics))
        # The 2018 GOG builds list a second segment with the same name as a code segment (CSEG) with data-only flags
        # although it holds code: a segment is code when it, or a segment of the same name, is executable.
        code_names = {n for n, _, _, ch in self.sections if ch & IMAGE_SCN_MEM_EXECUTE}
        self.code_ranges = [(va, va + sz) for n, va, sz, ch in self.sections if n in code_names]
        self.relocs = set()
        for b in getattr(pe, 'DIRECTORY_ENTRY_BASERELOC', []):
            for e in b.entries:
                if e.type == 3:
                    self.relocs.add(self.base + e.rva)
        self.imports = {}  # iat addr -> (dll, name)
        for e in getattr(pe, 'DIRECTORY_ENTRY_IMPORT', []):
            for i in e.imports:
                name = i.name.decode() if i.name else '#%d' % i.ordinal
                self.imports[i.address] = (e.dll.decode().lower(), name)
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True
        self.icache = {}
        self.seeds = set()
        if seeds_tsv:
            for line in open(seeds_tsv):
                self.seeds.add(int(line.split('\t')[0], 16))
        self.exports = {}
        for e in getattr(getattr(pe, 'DIRECTORY_ENTRY_EXPORT', None), 'symbols', []) or []:
            a = self.base + e.address
            if e.name: self.exports[e.name.decode()] = a
            self.seeds.add(a)

    def is_code(self, a):
        return any(lo <= a < hi for lo, hi in self.code_ranges)

    def in_image(self, a):
        return self.base <= a < self.base + self.size

    def u32(self, a):
        o = a - self.base
        return int.from_bytes(self.mem[o:o + 4], 'little')

    def insn(self, a):
        i = self.icache.get(a)
        if i is None:
            o = a - self.base
            try:
                i = next(self.md.disasm(bytes(self.mem[o:o + 16]), a, 1))
            except StopIteration:
                i = False
            self.icache[a] = i
        return i


def jump_table(img, ins):
    """jmp [reg*4 + tbl] -> list of targets (consecutive relocated entries)."""
    op = ins.operands[0]
    # also accepts base + index*4 + table (2D tables in the assembly code in CSEG)
    if op.type != X.X86_OP_MEM or op.mem.scale != 4 or op.mem.index == 0:
        return None
    tbl = op.mem.disp & 0xffffffff
    out = []
    a = tbl
    # zeroed leading rows (index never used) show up in 2D tables
    k = 0
    while op.mem.base and a not in img.relocs and img.u32(a) == 0 and k < 64:
        a += 4; k += 1
    while a in img.relocs and img.is_code(img.u32(a)):
        out.append(img.u32(a))
        a += 4
        if len(out) > 1024:
            break
    return (tbl, out) if out else None


def mem_target(ins):
    op = ins.operands[0]
    if op.type == X.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0 and op.mem.segment == 0:
        return op.mem.disp & 0xffffffff
    return None


COND_JUMPS = {'je', 'jne', 'jb', 'jae', 'jbe', 'ja', 'jl', 'jge', 'jle', 'jg', 'js', 'jns',
              'jo', 'jno', 'jp', 'jnp', 'jecxz', 'jcxz', 'loop', 'loope', 'loopne'}


def discover(img, extra_seeds=()):
    entries = {img.entry} | {s for s in img.seeds if img.is_code(s)} | set(extra_seeds)
    funcs = {}
    tables = {}
    bad = set()
    work = list(entries)
    while work:
        f = work.pop()
        if f in funcs:
            continue
        seen = set()
        stack = [f]
        while stack:
            a = stack.pop()
            if a in seen:
                continue
            if a != f and a in entries:
                # entering another function via fall-through/jmp: keep going (duplicated code)
                pass
            ins = img.insn(a)
            if not ins or not img.is_code(a):
                bad.add(a)
                seen.add(a)
                continue
            seen.add(a)
            m = ins.mnemonic
            nxt = a + ins.size
            if m == 'call':
                op = ins.operands[0]
                if op.type == X.X86_OP_IMM:
                    t = op.imm & 0xffffffff
                    if img.is_code(t) and t not in entries:
                        entries.add(t); work.append(t)
                stack.append(nxt)
            elif m in ('ret', 'retf', 'iret', 'iretd', 'hlt', 'ud2', 'int3'):
                pass
            elif m == 'jmp':
                op = ins.operands[0]
                if op.type == X.X86_OP_IMM:
                    t = op.imm & 0xffffffff
                    if t in entries and t != f:
                        pass  # tail call
                    else:
                        stack.append(t)
                else:
                    jt = jump_table(img, ins)
                    if jt:
                        tables[a] = jt
                        stack.extend(jt[1])
            elif m in COND_JUMPS:
                stack.append(ins.operands[0].imm & 0xffffffff)
                stack.append(nxt)
            else:
                stack.append(nxt)
        funcs[f] = seen
    return entries, funcs, tables, bad


def discover_all(img):
    """Fixed point: seeds coming from relocs (vtables, callbacks, push imm)."""
    extra = set()
    while True:
        entries, funcs, tables, bad = discover(img, extra)
        allins = set().union(*funcs.values())
        owner = {}
        for a in allins:
            for k in range(a, a + img.insn(a).size):
                owner[k] = a
        jtloc = set()
        for a, (t, l) in tables.items():
            jtloc |= set(range(t, t + 4 * len(l)))
        new = set()
        for r in img.relocs:
            t = img.u32(r)
            if not img.is_code(t) or r in jtloc or t in entries:
                continue
            if r in owner:
                ins = img.insn(owner[r])
                if ins.mnemonic in ('push', 'mov', 'cmp') and any(o.type == X.X86_OP_IMM for o in ins.operands):
                    new.add(t)
            elif not img.is_code(r):
                new.add(t)
        new -= extra
        if not new:
            return entries, funcs, tables, bad, allins
        extra |= new
