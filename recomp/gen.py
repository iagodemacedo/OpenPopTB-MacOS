# Static x86 (32-bit) -> C recompiler.
# usage: python gen.py popTB.exe functions.tsv outdir
import sys, os, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from image import *
from capstone import x86_const as X

CC = {'o': 0, 'no': 1, 'b': 2, 'c': 2, 'nae': 2, 'ae': 3, 'nb': 3, 'nc': 3, 'e': 4, 'z': 4, 'ne': 5, 'nz': 5,
      'be': 6, 'na': 6, 'a': 7, 'nbe': 7, 's': 8, 'ns': 9, 'p': 10, 'pe': 10, 'np': 11, 'po': 11,
      'l': 12, 'nge': 12, 'ge': 13, 'nl': 13, 'le': 14, 'ng': 14, 'g': 15, 'nle': 15}

R32 = {X.X86_REG_EAX: 'eax', X.X86_REG_ECX: 'ecx', X.X86_REG_EDX: 'edx', X.X86_REG_EBX: 'ebx',
       X.X86_REG_ESP: 'esp', X.X86_REG_EBP: 'ebp', X.X86_REG_ESI: 'esi', X.X86_REG_EDI: 'edi'}
R16 = {X.X86_REG_AX: 'eax', X.X86_REG_CX: 'ecx', X.X86_REG_DX: 'edx', X.X86_REG_BX: 'ebx',
       X.X86_REG_SP: 'esp', X.X86_REG_BP: 'ebp', X.X86_REG_SI: 'esi', X.X86_REG_DI: 'edi'}
R8L = {X.X86_REG_AL: 'eax', X.X86_REG_CL: 'ecx', X.X86_REG_DL: 'edx', X.X86_REG_BL: 'ebx'}
R8H = {X.X86_REG_AH: 'eax', X.X86_REG_CH: 'ecx', X.X86_REG_DH: 'edx', X.X86_REG_BH: 'ebx'}
STREG = {getattr(X, 'X86_REG_ST%d' % i): i for i in range(8)}

MASK = {1: '0xffu', 2: '0xffffu', 4: '0xffffffffu'}
SIGN = {1: '0x80u', 2: '0x8000u', 4: '0x80000000u'}
STYPE = {1: 'int8_t', 2: 'int16_t', 4: 'int32_t'}
UTYPE = {1: 'uint8_t', 2: 'uint16_t', 4: 'uint32_t'}

FLAG_W = {'add', 'sub', 'cmp', 'test', 'and', 'or', 'xor', 'neg', 'imul', 'mul', 'sahf', 'popfd', 'div', 'idiv',
          'scasb', 'scasw', 'scasd', 'cmpsb', 'cmpsw', 'cmpsd', 'bsf'}
FLAG_RW = {'adc', 'sbb', 'inc', 'dec', 'rcl', 'rcr', 'rol', 'ror', 'bt', 'bts', 'btr', 'btc', 'bsr', 'cmc',
           'repe cmpsb', 'repne cmpsb', 'repe scasb', 'repne scasb', 'repe scasd', 'repne scasd', 'repe cmpsd',
           'repne cmpsd', 'repe cmpsw', 'repne scasw'}
SHIFTS = {'shl', 'sal', 'shr', 'sar', 'shld', 'shrd'}


PARTIAL = {'inc', 'dec', 'rol', 'ror', 'bt', 'bts', 'btr', 'btc', 'bsr', 'sahf',
           'repe cmpsb', 'repne cmpsb', 'repe scasb', 'repne scasb', 'repe scasd', 'repne scasd', 'repe cmpsd',
           'repne cmpsd', 'repe cmpsw', 'repne scasw'}


def flag_reads(m, ins):
    if m in PARTIAL: return 'partial'
    if m in SHIFTS: return 'partial' if ins.operands[-1].type != X.X86_OP_IMM else False
    if m in FLAG_RW: return True
    if m.startswith('j') and m not in ('jmp', 'jecxz', 'jcxz'): return True
    if m.startswith('set') or m.startswith('cmov'): return True
    if m in ('pushfd', 'pushf', 'lahf', 'loope', 'loopne'): return True
    return False


def flag_writes(m, ins):
    if m in FLAG_W or m in FLAG_RW: return True
    if m in SHIFTS: return True
    return False


HOOKS = {int(x, 16) for x in os.environ.get('POPTB_HOOK', '').split(',') if x}
_tr = os.environ.get('POPTB_TRACE_RANGE', '')
TRACE_RANGE = tuple(int(x, 16) for x in _tr.split('-')) if _tr else None


class Func:
    def __init__(self, g, entry, addrs):
        self.g, self.entry = g, entry
        self.addrs = sorted(addrs)
        self.aset = set(addrs)


class Gen:
    def __init__(self, img, entries, funcs, tables):
        self.img, self.entries, self.funcs, self.tables = img, entries, funcs, tables
        self.unknown = collections.Counter()
        self.hooked = False

    # ---------- operands ----------
    def ea(self, op, ins, noseg=False):
        m = op.mem
        parts = []
        if m.base:
            parts.append('R->' + R32[m.base])
        if m.index:
            parts.append('R->%s*%d' % (R32[m.index], m.scale) if m.scale != 1 else 'R->' + R32[m.index])
        if m.disp or not parts:
            parts.append('0x%xu' % (m.disp & 0xffffffff))
        e = ' + '.join(parts)
        if m.segment == X.X86_REG_FS and not noseg:
            e = 'R->fs_base + ' + e
        return '(uint32_t)(%s)' % e

    def rd(self, ins, op, size=None):
        size = size or op.size
        if op.type == X.X86_OP_REG:
            r = op.reg
            if r in R32: return 'R->' + R32[r]
            if r in R16: return '(R->%s & 0xffffu)' % R16[r]
            if r in R8L: return '(R->%s & 0xffu)' % R8L[r]
            if r in R8H: return '((R->%s >> 8) & 0xffu)' % R8H[r]
            raise Exception('reg %s' % ins.reg_name(r))
        if op.type == X.X86_OP_IMM:
            return '0x%xu' % (op.imm & int(MASK[size][:-1], 16))
        if op.type == X.X86_OP_MEM:
            return 'rd%d(%s)' % (size * 8, self.ea(op, ins))
        raise Exception('op')

    def wr(self, ins, op, val, size=None):
        size = size or op.size
        if op.type == X.X86_OP_REG:
            r = op.reg
            if r in R32: return 'R->%s = %s;' % (R32[r], val)
            if r in R16: return 'R->{0} = (R->{0} & 0xffff0000u) | ((%s) & 0xffffu);'.format(R16[r]) % val
            if r in R8L: return 'R->{0} = (R->{0} & 0xffffff00u) | ((%s) & 0xffu);'.format(R8L[r]) % val
            if r in R8H: return 'R->{0} = (R->{0} & 0xffff00ffu) | (((%s) & 0xffu) << 8);'.format(R8H[r]) % val
            raise Exception('reg %s' % ins.reg_name(r))
        if op.type == X.X86_OP_MEM:
            return 'wr%d(%s, %s);' % (size * 8, self.ea(op, ins), val)
        raise Exception('wr op')

    def sx(self, e, size):
        return '(uint32_t)(int32_t)(%s)(%s)' % (STYPE[size], e)

    # ---------- flags ----------
    def lazy(self, kind, size, a='a', b='b', r='r', cf='0', of='0'):
        return ('R->cc_op = CCOP(%s, %d); R->cc_a = %s; R->cc_b = %s; R->cc_res = %s; R->cc_cf = %s; R->cc_of = %s;'
                % (kind, size, a, b, r, cf, of))

    def fused(self, kind, size, cc, a='a', b='b', r='r', cf='0', of='0'):
        m = MASK[size]; st = STYPE[size]
        if kind == 'CC_SUB':
            A, B = '(%s & %s)' % (a, m), '(%s & %s)' % (b, m)
            sA, sB = '(%s)%s' % (st, a), '(%s)%s' % (st, b)
            t = {2: '%s < %s' % (A, B), 3: '%s >= %s' % (A, B), 4: '%s == %s' % (A, B), 5: '%s != %s' % (A, B),
                 6: '%s <= %s' % (A, B), 7: '%s > %s' % (A, B), 12: '%s < %s' % (sA, sB), 13: '%s >= %s' % (sA, sB),
                 14: '%s <= %s' % (sA, sB), 15: '%s > %s' % (sA, sB), 8: '(%s)%s < 0' % (st, r), 9: '(%s)%s >= 0' % (st, r)}
            if cc in t: return t[cc]
        if kind == 'CC_LOGIC':
            sr = '(%s)%s' % (st, r)
            t = {4: '(%s & %s) == 0' % (r, m), 5: '(%s & %s) != 0' % (r, m), 8: sr + ' < 0', 9: sr + ' >= 0',
                 12: sr + ' < 0', 13: sr + ' >= 0', 14: sr + ' <= 0', 15: sr + ' > 0', 2: '0', 3: '1',
                 6: '(%s & %s) == 0' % (r, m), 7: '(%s & %s) != 0' % (r, m), 0: '0', 1: '1'}
            if cc in t: return t[cc]
        return 'cc_test(%d, eflags_of(CCOP(%s, %d), %s, %s, %s, %s, %s))' % (cc, kind, size, a, b, r, cf, of)

    # ---------- function ----------
    def gen_func(self, f, out):
        img = self.img
        addrs = f.addrs
        aset = f.aset
        labels = set()
        succ = {}
        for a in addrs:
            ins = img.insn(a)
            m = ins.mnemonic
            nxt = a + ins.size
            s = []
            if m == 'call':
                s = [nxt]
            elif m in ('ret', 'retf', 'hlt', 'int3', 'iretd', 'ud2'):
                s = None
            elif m == 'jmp':
                op = ins.operands[0]
                if op.type == X.X86_OP_IMM:
                    t = op.imm & 0xffffffff
                    if t in aset:
                        s = [t]; labels.add(t)
                    else:
                        s = None
                elif a in self.tables:
                    s = [t for t in self.tables[a][1] if t in aset]
                    labels.update(s)
                else:
                    s = None
            elif m in COND_JUMPS:
                t = ins.operands[0].imm & 0xffffffff
                s = [t, nxt]; labels.add(t)
            else:
                s = [nxt]
            succ[a] = s
        # flag liveness
        live_in = {a: False for a in addrs}
        rw = {}
        for a in addrs:
            ins = img.insn(a); m = ins.mnemonic
            rw[a] = (flag_reads(m, ins), flag_writes(m, ins), m == 'call')
        changed = True
        live_out = {}
        while changed:
            changed = False
            for a in reversed(addrs):
                s = succ[a]
                lo = True if s is None else any(live_in.get(t, True) for t in s)
                live_out[a] = lo
                r_, w_, c_ = rw[a]
                if r_ == 'partial':
                    li = lo
                else:
                    li = r_ or (lo and not w_ and not c_)
                if li != live_in[a]:
                    live_in[a] = li; changed = True
        self.live_out = live_out
        self.labels = labels
        hooked = f.entry in HOOKS
        self.hooked = hooked
        out.append('void f_%x(CPU *R) {' % f.entry)
        if hooked:
            out.append('  hook_enter(R, 0x%xu);' % f.entry)
        if addrs[0] != f.entry:
            out.append('  goto L_%x;' % f.entry)
            labels.add(f.entry)
        prev_ft = None
        i = 0
        n = len(addrs)
        while i < n:
            a = addrs[i]
            ins = img.insn(a)
            if prev_ft is not None and prev_ft != a:
                out.append('  goto L_%x;' % prev_ft)
                labels.add(prev_ft)
            # producer+jcc/setcc fusion
            nxt_ins = None
            if i + 1 < n and addrs[i + 1] == a + ins.size and addrs[i + 1] not in labels:
                nxt_ins = img.insn(addrs[i + 1])
            out.append('L_%x:;' % a)
            if TRACE_RANGE and TRACE_RANGE[0] <= a < TRACE_RANGE[1]:
                out.append('  TR(0x%xu);' % a)
            code, consumed = self.gen_ins(f, ins, nxt_ins)
            out.append('  { ' + code + ' }')
            if consumed:
                i += 1
                ins = nxt_ins
                a = ins.address
            s = succ.get(a)
            m = ins.mnemonic
            if s is None:
                prev_ft = None
            elif m == 'jmp':
                prev_ft = None
            else:
                prev_ft = a + ins.size
            i += 1
        if prev_ft is not None:
            out.append('  goto L_%x;' % prev_ft)
        out.append('}')
        return labels

    def need_label_fix(self, f, text):
        pass

    # ---------- instruction ----------
    def gen_ins(self, f, ins, nxt):
        m = ins.mnemonic
        a = ins.address
        ops = ins.operands
        live = self.live_out[a]
        try:
            return self._gen(f, ins, m, a, ops, live, nxt)
        except Exception as e:
            self.unknown[m + ' ' + str(e)[:40]] += 1
            return 'rt_trap(R, 0x%x, "%s %s");' % (a, m, ins.op_str), False

    def consumer(self, nxt):
        """returns cc if nxt is a fusible jcc/setcc."""
        if nxt is None: return None
        m = nxt.mnemonic
        if m.startswith('j') and m[1:] in CC: return ('j', CC[m[1:]])
        if m.startswith('set') and m[3:] in CC: return ('set', CC[m[3:]])
        return None

    def emit_consumer(self, nxt, kind, cc, cexpr):
        if kind == 'j':
            t = nxt.operands[0].imm & 0xffffffff
            return ' if (%s) %s' % (cexpr, self.jump_to(t, nxt))
        return ' ' + self.wr(nxt, nxt.operands[0], '(%s) ? 1u : 0u' % cexpr)

    def jump_to(self, t, ins):
        if t in self.cur.aset:
            if t <= ins.address:
                return '{ YIELD(); goto L_%x; }' % t
            return 'goto L_%x;' % t
        return '{ dispatch(R, 0x%xu); return; }' % t

    def flagged(self, kind, size, nxt, live, body, a='a', b='b', r='r', cf='0', of='0'):
        """body computes a,b,r and writes the destination; appends lazy flags and the fused consumer."""
        # flags must be written BEFORE the branch: the target may re-read the same flags
        c = self.consumer(nxt)
        s = body
        consumed = False
        if c:
            # the consumer's live_out already covers the jcc target and fall-through
            if self.live_out[nxt.address]:
                s += ' ' + self.lazy(kind, size, a, b, r, cf, of)
            s += self.emit_consumer(nxt, c[0], c[1], self.fused(kind, size, c[1], a, b, r, cf, of))
            consumed = True
            return s, consumed
        if live:
            s += ' ' + self.lazy(kind, size, a, b, r, cf, of)
        return s, consumed


    def _gen(self, f, ins, m, a, ops, live, nxt):
        self.cur = f
        img = self.img
        nexta = a + ins.size
        rd, wr = self.rd, self.wr
        if m in ('mov',):
            return wr(ins, ops[0], rd(ins, ops[1], ops[0].size)), False
        if m == 'movzx':
            return wr(ins, ops[0], rd(ins, ops[1])), False
        if m == 'movsx':
            return wr(ins, ops[0], self.sx(rd(ins, ops[1]), ops[1].size)), False
        if m == 'lea':
            return wr(ins, ops[0], self.ea(ops[1], ins, noseg=True)), False
        if m == 'push':
            if ops[0].type == X.X86_OP_IMM:
                return 'PUSH(0x%xu);' % (ops[0].imm & 0xffffffff), False
            if ops[0].size == 2:
                return 'R->esp -= 2; wr16(R->esp, %s);' % rd(ins, ops[0]), False
            return 'PUSH(%s);' % rd(ins, ops[0]), False
        if m == 'pop':
            if ops[0].size == 2:
                return '{ uint32_t v = rd16(R->esp); R->esp += 2; %s }' % wr(ins, ops[0], 'v'), False
            if ops[0].type == X.X86_OP_MEM:
                # x86 computes the address after incrementing esp
                return 'uint32_t v = rd32(R->esp); R->esp += 4; %s' % wr(ins, ops[0], 'v'), False
            return 'uint32_t v = POP(); %s' % wr(ins, ops[0], 'v'), False
        if m in ('add', 'sub', 'cmp', 'and', 'or', 'xor', 'test', 'adc', 'sbb'):
            sz = ops[0].size
            A = rd(ins, ops[0]); B = rd(ins, ops[1], sz)
            if ops[1].type == X.X86_OP_IMM and ops[1].size < sz:
                B = '0x%xu' % (ops[1].imm & int(MASK[sz][:-1], 16))
            if m in ('xor', 'sub') and ops[0].type == X.X86_OP_REG and ops[1].type == X.X86_OP_REG and ops[0].reg == ops[1].reg:
                body = 'uint32_t a = 0, b = 0, r = 0; ' + wr(ins, ops[0], '0')
                return self.flagged('CC_LOGIC' if m == 'xor' else 'CC_SUB', sz, nxt, live, body)
            expr = {'add': 'a + b', 'sub': 'a - b', 'cmp': 'a - b', 'and': 'a & b', 'or': 'a | b', 'xor': 'a ^ b',
                    'test': 'a & b', 'adc': 'a + b + cf', 'sbb': 'a - b - cf'}[m]
            kind = {'add': 'CC_ADD', 'sub': 'CC_SUB', 'cmp': 'CC_SUB', 'adc': 'CC_ADC', 'sbb': 'CC_SBB'}.get(m, 'CC_LOGIC')
            body = 'uint32_t a = %s, b = %s; ' % (A, B)
            if m in ('adc', 'sbb'):
                body += 'uint32_t cf = get_cf(R); '
            body += 'uint32_t r = (%s) & %s; ' % (expr, MASK[sz])
            if m not in ('cmp', 'test'):
                body += wr(ins, ops[0], 'r')
            return self.flagged(kind, sz, nxt, live, body, cf='cf' if m in ('adc', 'sbb') else '0')
        if m in ('inc', 'dec'):
            sz = ops[0].size
            body = 'uint32_t a = %s, b = 1; uint32_t r = (a %s 1) & %s; %s' % (
                rd(ins, ops[0]), '+' if m == 'inc' else '-', MASK[sz], wr(ins, ops[0], 'r'))
            c = self.consumer(nxt)
            need = live or (c is not None)
            if need:
                body += ' uint32_t cf = get_cf(R);'
            return self.flagged('CC_INC' if m == 'inc' else 'CC_DEC', sz, nxt, live, body, cf='cf' if need else '0')
        if m == 'neg':
            sz = ops[0].size
            body = 'uint32_t a = 0, b = %s; uint32_t r = (0u - b) & %s; %s' % (rd(ins, ops[0]), MASK[sz], wr(ins, ops[0], 'r'))
            return self.flagged('CC_NEG', sz, nxt, live, body, a='b')
        if m == 'not':
            return wr(ins, ops[0], '~%s' % rd(ins, ops[0])), False
        if m in ('shl', 'sal', 'shr', 'sar'):
            sz = ops[0].size; bits = sz * 8
            if len(ops) == 1:
                cnt = '1u'
            elif ops[1].type == X.X86_OP_IMM:
                cnt = '%du' % (ops[1].imm & 31)
            else:
                cnt = '(R->ecx & 31u)'
            sgn = SIGN[sz]
            if m in ('shl', 'sal'):
                calc = 'r = (a << n) & %s; cf = n <= %d ? (a >> (%d - n)) & 1u : 0u; of = ((r & %s) != 0) ^ cf;' % (MASK[sz], bits, bits, sgn)
                kind = 'CC_SHL'
            elif m == 'shr':
                calc = 'r = n < 32 ? (a >> n) : 0; cf = (a >> (n - 1)) & 1u; of = (a & %s) != 0;' % sgn
                kind = 'CC_SHR'
            else:
                calc = 'r = (uint32_t)((int32_t)(%s)a >> (n > %d ? %d : n)) & %s; cf = (uint32_t)((int32_t)(%s)a >> (n > %d ? %d : n - 1)) & 1u; of = 0;' % (
                    STYPE[sz], bits - 1, bits - 1, MASK[sz], STYPE[sz], bits - 1, bits - 1)
                kind = 'CC_SAR'
            body = 'uint32_t n = %s; if (n) { uint32_t a = %s, b = n, r, cf, of; %s %s' % (cnt, rd(ins, ops[0]), calc, wr(ins, ops[0], 'r'))
            c = self.consumer(nxt)
            if c and cnt != '(R->ecx & 31u)':
                if self.live_out[nxt.address]: body += ' ' + self.lazy(kind, sz, cf='cf', of='of')
                body += self.emit_consumer(nxt, c[0], c[1], self.fused(kind, sz, c[1], 'a', 'b', 'r', 'cf', 'of'))
                return body + ' }', True
            if live:
                body += ' ' + self.lazy(kind, sz, cf='cf', of='of')
            return body + ' }', False
        if m in ('rol', 'ror', 'rcl', 'rcr'):
            sz = ops[0].size
            if len(ops) == 1: cnt = '1u'
            elif ops[1].type == X.X86_OP_IMM: cnt = '%du' % (ops[1].imm & 31)
            else: cnt = '(R->ecx & 31u)'
            return '{ uint32_t r = rt_%s(R, %s, %s, %d, %d); %s }' % (m, rd(ins, ops[0]), cnt, sz, 1 if live else 0, wr(ins, ops[0], 'r')), False
        if m in ('shld', 'shrd'):
            sz = ops[0].size
            cnt = '%du' % (ops[2].imm & 31) if ops[2].type == X.X86_OP_IMM else '(R->ecx & 31u)'
            if sz != 4: raise Exception('shld16')
            if m == 'shld':
                calc = 'r = (a << n) | (b >> (32 - n)); cf = (a >> (32 - n)) & 1u;'
            else:
                calc = 'r = (a >> n) | (b << (32 - n)); cf = (a >> (n - 1)) & 1u;'
            body = 'uint32_t n = %s; if (n) { uint32_t a = %s, b = %s, r, cf; %s %s' % (cnt, rd(ins, ops[0]), rd(ins, ops[1]), calc, wr(ins, ops[0], 'r'))
            if live: body += ' ' + self.lazy('CC_SHL', 4, cf='cf', of='0')
            return body + ' }', False
        if m == 'imul':
            if len(ops) == 1:
                sz = ops[0].size
                if sz == 4:
                    return ('int64_t p = (int64_t)(int32_t)R->eax * (int32_t)%s; R->eax = (uint32_t)p; R->edx = (uint32_t)(p >> 32); '
                            'uint32_t o = p != (int64_t)(int32_t)p; %s' % (rd(ins, ops[0]), self.lazy('CC_MUL', 4, 'R->eax', '0', 'R->eax', 'o', 'o') if live else '')), False
                if sz == 2:
                    return ('int32_t p = (int32_t)(int16_t)R->eax * (int16_t)%s; R->eax = (R->eax & 0xffff0000u) | (p & 0xffff); R->edx = (R->edx & 0xffff0000u) | ((p >> 16) & 0xffff); '
                            'uint32_t o = p != (int16_t)p; %s' % (rd(ins, ops[0]), self.lazy('CC_MUL', 2, 'p', '0', 'p', 'o', 'o') if live else '')), False
                return ('int32_t p = (int32_t)(int8_t)R->eax * (int8_t)%s; R->eax = (R->eax & 0xffff0000u) | (p & 0xffff); '
                        'uint32_t o = p != (int8_t)p; %s' % (rd(ins, ops[0]), self.lazy('CC_MUL', 1, 'p', '0', 'p', 'o', 'o') if live else '')), False
            sz = ops[0].size
            if len(ops) == 2:
                x, y = rd(ins, ops[0]), rd(ins, ops[1], sz)
            else:
                x, y = rd(ins, ops[1]), '0x%xu' % (ops[2].imm & 0xffffffff)
            T = STYPE[sz]
            return ('int64_t p = (int64_t)(%s)%s * (int64_t)(%s)%s; uint32_t r = (uint32_t)p; %s uint32_t o = p != (int64_t)(%s)p; %s'
                    % (T, x, T, y, wr(ins, ops[0], 'r'), T, self.lazy('CC_MUL', sz, 'r', '0', 'r', 'o', 'o') if live else '')), False
        if m == 'mul':
            sz = ops[0].size
            if sz == 4:
                return ('uint64_t p = (uint64_t)R->eax * %s; R->eax = (uint32_t)p; R->edx = (uint32_t)(p >> 32); uint32_t o = R->edx != 0; %s'
                        % (rd(ins, ops[0]), self.lazy('CC_MUL', 4, 'R->eax', '0', 'R->eax', 'o', 'o') if live else '')), False
            if sz == 1:
                return ('uint32_t p = (R->eax & 0xffu) * %s; R->eax = (R->eax & 0xffff0000u) | (p & 0xffff); uint32_t o = (p >> 8) != 0; %s'
                        % (rd(ins, ops[0]), self.lazy('CC_MUL', 1, 'p', '0', 'p', 'o', 'o') if live else '')), False
            return ('uint32_t p = (R->eax & 0xffffu) * %s; R->eax = (R->eax & 0xffff0000u) | (p & 0xffff); R->edx = (R->edx & 0xffff0000u) | (p >> 16); uint32_t o = (p >> 16) != 0; %s'
                    % (rd(ins, ops[0]), self.lazy('CC_MUL', 2, 'p', '0', 'p', 'o', 'o') if live else '')), False
        if m in ('div', 'idiv'):
            sz = ops[0].size
            d = rd(ins, ops[0])
            if sz == 4:
                if m == 'div':
                    return ('uint32_t d = %s; if (!d) div_trap(R, 0x%x); uint64_t n = ((uint64_t)R->edx << 32) | R->eax; '
                            'R->eax = (uint32_t)(n / d); R->edx = (uint32_t)(n %% d);' % (d, a)), False
                return ('int32_t d = (int32_t)%s; if (!d) div_trap(R, 0x%x); int64_t n = (int64_t)(((uint64_t)R->edx << 32) | R->eax); '
                        'if (d == -1) { R->eax = (uint32_t)(-n); R->edx = 0; } else { R->eax = (uint32_t)(n / d); R->edx = (uint32_t)(n %% d); }' % (d, a)), False
            if sz == 2:
                if m == 'div':
                    return ('uint32_t d = %s; if (!d) div_trap(R, 0x%x); uint32_t n = ((R->edx & 0xffffu) << 16) | (R->eax & 0xffffu); '
                            'R->eax = (R->eax & 0xffff0000u) | ((n / d) & 0xffff); R->edx = (R->edx & 0xffff0000u) | (n %% d);' % (d, a)), False
                return ('int32_t d = (int16_t)%s; if (!d) div_trap(R, 0x%x); int32_t n = (int32_t)(((R->edx & 0xffffu) << 16) | (R->eax & 0xffffu)); '
                        'R->eax = (R->eax & 0xffff0000u) | ((uint32_t)(n / d) & 0xffff); R->edx = (R->edx & 0xffff0000u) | ((uint32_t)(n %% d) & 0xffff);' % (d, a)), False
            if m == 'div':
                return ('uint32_t d = %s; if (!d) div_trap(R, 0x%x); uint32_t n = R->eax & 0xffffu; '
                        'R->eax = (R->eax & 0xffff0000u) | ((n / d) & 0xff) | ((n %% d) << 8);' % (d, a)), False
            return ('int32_t d = (int8_t)%s; if (!d) div_trap(R, 0x%x); int32_t n = (int16_t)(R->eax & 0xffffu); '
                    'R->eax = (R->eax & 0xffff0000u) | ((uint32_t)(n / d) & 0xff) | (((uint32_t)(n %% d) & 0xff) << 8);' % (d, a)), False
        if m == 'cdq': return 'R->edx = (uint32_t)((int32_t)R->eax >> 31);', False
        if m == 'cwd': return 'R->edx = (R->edx & 0xffff0000u) | ((R->eax & 0x8000u) ? 0xffffu : 0);', False
        if m == 'cbw': return 'R->eax = (R->eax & 0xffff0000u) | ((uint32_t)(int8_t)R->eax & 0xffffu);', False
        if m == 'cwde': return 'R->eax = (uint32_t)(int16_t)R->eax;', False
        if m == 'xchg':
            if ops[0].type == X.X86_OP_REG and ops[1].type == X.X86_OP_REG and ops[0].reg == ops[1].reg:
                return '', False
            return 'uint32_t x = %s, y = %s; %s %s' % (rd(ins, ops[0]), rd(ins, ops[1]), wr(ins, ops[0], 'y'), wr(ins, ops[1], 'x')), False
        if m == 'bswap':
            return '%s' % wr(ins, ops[0], '__builtin_bswap32(%s)' % rd(ins, ops[0])), False
        if m in ('bt', 'bts', 'btr', 'btc'):
            sz = ops[0].size
            bits = sz * 8
            upd = {'bt': None, 'bts': 'v | (1u << b)', 'btr': 'v & ~(1u << b)', 'btc': 'v ^ (1u << b)'}[m]
            if ops[0].type == X.X86_OP_MEM and ops[1].type == X.X86_OP_REG:
                # a signed bit offset can fall outside the addressed word
                o = rd(ins, ops[1])
                s_ = ('int32_t off = (int32_t)%s; uint32_t ad = %s + (uint32_t)((off >> %d) * %d); uint32_t b = (uint32_t)off & %d; '
                      'uint32_t v = rd%d(ad); uint32_t c = (v >> b) & 1u; ' % (o, self.ea(ops[0], ins), 5 if sz == 4 else 4, sz, bits - 1, bits))
                if upd: s_ += 'wr%d(ad, %s); ' % (bits, upd)
            else:
                bit = '(%s & %d)' % (rd(ins, ops[1]), bits - 1)
                s_ = 'uint32_t v = %s; uint32_t b = %s; uint32_t c = (v >> b) & 1u; ' % (rd(ins, ops[0]), bit)
                if upd: s_ += wr(ins, ops[0], upd)
            s_ += ' set_flags(R, (get_flags(R) & ~1u) | c);'
            return s_, False
        if m == 'bsr':
            return ('uint32_t v = %s; uint32_t f = get_flags(R) & ~0x40u; if (v) { %s } else f |= 0x40u; set_flags(R, f);'
                    % (rd(ins, ops[1]), wr(ins, ops[0], '31 - __builtin_clz(v)'))), False
        if m.startswith('set') and m[3:] in CC:
            return wr(ins, ops[0], 'cond(R, %d) ? 1u : 0u' % CC[m[3:]]), False
        if m.startswith('j') and m[1:] in CC:
            t = ops[0].imm & 0xffffffff
            return 'if (cond(R, %d)) %s' % (CC[m[1:]], self.jump_to(t, ins)), False
        if m == 'jecxz':
            return 'if (R->ecx == 0) %s' % self.jump_to(ops[0].imm & 0xffffffff, ins), False
        if m == 'jmp':
            op = ops[0]
            if op.type == X.X86_OP_IMM:
                t = op.imm & 0xffffffff
                if t in f.aset: return ('YIELD(); goto L_%x;' if t <= a else 'goto L_%x;') % t, False
                if t in self.entries: return 'f_%x(R); return;' % t, False
                return 'dispatch(R, 0x%xu); return;' % t, False
            tgt = rd(ins, op, 4)
            if a in self.tables:
                cases = ' '.join('case 0x%xu: goto L_%x;' % (t, t) for t in sorted(set(self.tables[a][1])) if t in f.aset)
                return 'switch (%s) { %s default: dispatch(R, %s); return; }' % (tgt, cases, tgt), False
            return 'uint32_t t = %s; dispatch(R, t); return;' % tgt, False
        if m == 'call':
            op = ops[0]
            if op.type == X.X86_OP_IMM:
                t = op.imm & 0xffffffff
                if t in self.entries:
                    return 'PUSH(0x%xu); f_%x(R);' % (nexta, t), False
                return 'PUSH(0x%xu); dispatch(R, 0x%xu);' % (nexta, t), False
            return 'uint32_t t = %s; PUSH(0x%xu); dispatch(R, t);' % (rd(ins, op, 4), nexta), False
        if m == 'ret':
            n = ops[0].imm if ops else 0
            if self.hooked:
                return 'R->esp += %d; hook_leave(R, 0x%xu); return;' % (4 + n, f.entry), False
            return 'R->esp += %d; return;' % (4 + n), False
        if m == 'leave':
            return 'R->esp = R->ebp; R->ebp = POP();', False
        if m == 'pushal':
            return ('uint32_t s = R->esp; PUSH(R->eax); PUSH(R->ecx); PUSH(R->edx); PUSH(R->ebx); PUSH(s); '
                    'PUSH(R->ebp); PUSH(R->esi); PUSH(R->edi);'), False
        if m == 'popal':
            return ('R->edi = POP(); R->esi = POP(); R->ebp = POP(); R->esp += 4; R->ebx = POP(); '
                    'R->edx = POP(); R->ecx = POP(); R->eax = POP();'), False
        if m == 'pushfd':
            return 'PUSH(get_flags(R) | 0x202u | (R->df << 10));', False
        if m == 'popfd':
            return 'uint32_t v = POP(); set_flags(R, v & 0x8d5u); R->df = (v >> 10) & 1;', False
        if m == 'sahf':
            return 'set_flags(R, (get_flags(R) & ~0xd5u) | ((R->eax >> 8) & 0xd5u));', False
        if m == 'lahf':
            return 'R->eax = (R->eax & 0xffff00ffu) | (((get_flags(R) & 0xd5u) | 2u) << 8);', False
        if m == 'cld': return 'R->df = 0;', False
        if m == 'std': return 'R->df = 1;', False
        if m == 'xlatb': return 'R->eax = (R->eax & 0xffffff00u) | rd8(R->ebx + (R->eax & 0xffu));', False
        if m in ('nop', 'wait', 'fwait'): return '', False
        if m == 'cpuid': return 'rt_cpuid(R);', False
        if m == 'rdtsc': return 'uint64_t t = rt_rdtsc(); R->eax = (uint32_t)t; R->edx = (uint32_t)(t >> 32);', False
        if m in ('int', 'int3', 'out', 'in', 'cli', 'sti', 'hlt', 'into', 'iretd', 'retf'):
            return 'rt_trap(R, 0x%x, "%s");' % (a, m), False
        # strings
        parts = m.split()
        base = parts[-1]
        rep = parts[0] if len(parts) == 2 else ''
        if base[:-1] in ('movs', 'stos', 'scas', 'cmps', 'lods') and base[-1] in 'bwd':
            sz = {'b': 1, 'w': 2, 'd': 4}[base[-1]]
            r = {'': 0, 'rep': 1, 'repe': 1, 'repz': 1, 'repne': 2, 'repnz': 2}[rep]
            if base[:-1] == 'lods':
                return 'do_lods(R, %d);' % sz, False
            return 'rep_%s(R, %d, %d);' % (base[:-1], sz, r), False
        if m.startswith('f'):
            return self.fpu(ins, m, a, ops), False
        raise Exception('unimpl')

    # ---------- x87 ----------
    def fmem(self, ins, op):
        e = self.ea(op, ins)
        return {4: 'rdf32(%s)', 8: 'rdf64(%s)', 10: 'rdf80(%s)'}[op.size] % e

    def fpu(self, ins, m, a, ops):
        rd = self.rd
        def st(op): return STREG[op.reg]
        if m == 'fld':
            if ops[0].type == X.X86_OP_MEM: return 'fpush(R, %s);' % self.fmem(ins, ops[0])
            return 'double v = ST(%d); fpush(R, v);' % st(ops[0])
        if m in ('fst', 'fstp'):
            p = ' fpop(R);' if m == 'fstp' else ''
            if ops[0].type == X.X86_OP_MEM:
                e = self.ea(ops[0], ins)
                w = {4: 'wrf32(%s, (float)ST(0));', 8: 'wrf64(%s, ST(0));', 10: 'wrf80(%s, ST(0));'}[ops[0].size] % e
                return w + p
            return 'ST(%d) = ST(0);' % st(ops[0]) + p
        if m == 'fild':
            e = self.ea(ops[0], ins)
            v = {2: '(int16_t)rd16(%s)', 4: '(int32_t)rd32(%s)', 8: '(int64_t)rd64(%s)'}[ops[0].size] % e
            return 'fpush(R, (double)%s);' % v
        if m in ('fist', 'fistp'):
            e = self.ea(ops[0], ins)
            sz = ops[0].size
            w = {2: 'wr16(%s, (uint32_t)fist(R, ST(0), 16));', 4: 'wr32(%s, (uint32_t)fist(R, ST(0), 32));',
                 8: 'wr64(%s, (uint64_t)fist(R, ST(0), 64));'}[sz] % e
            return w + (' fpop(R);' if m == 'fistp' else '')
        arith = {'fadd': '%s + %s', 'fmul': '%s * %s', 'fsub': '%s - %s', 'fsubr': '%s - %s', 'fdiv': '%s / %s', 'fdivr': '%s / %s'}
        mb = m[:-1] if m.endswith('p') and m[:-1] in arith else m
        if mb in arith:
            rev = mb in ('fsubr', 'fdivr')
            pop = m.endswith('p')
            def comb(d, s):
                return arith[mb] % ((s, d) if rev else (d, s))
            if len(ops) == 1 and ops[0].type == X.X86_OP_MEM:
                return 'ST(0) = FPR(%s);' % comb('ST(0)', self.fmem(ins, ops[0]))
            # register form: decode from the bytes (capstone's text is ambiguous)
            b0, b1 = ins.bytes[0], ins.bytes[1]
            if b1 < 0xc0 or b0 not in (0xd8, 0xdc, 0xde): raise Exception('fpu enc')
            i = b1 & 7; sub = (b1 >> 3) & 7
            OPS = {0: '%s + %s', 1: '%s * %s', 4: '%s - %s', 5: '%s - %s', 6: '%s / %s', 7: '%s / %s'}
            if b0 == 0xd8:
                d, s_ = 0, i
                rv = sub in (5, 7)          # fsubr/fdivr st0 = sti op st0
            else:
                d, s_ = i, 0
                rv = sub in (4, 6)          # DC/DE E0/F0: st(i) = st0 op st(i)
            x, y = ('ST(%d)' % s_, 'ST(%d)' % d) if rv else ('ST(%d)' % d, 'ST(%d)' % s_)
            r = 'ST(%d) = FPR(%s);' % (d, OPS[sub] % (x, y))
            return r + (' fpop(R);' if b0 == 0xde else '')
        iar = {'fiadd': '+', 'fimul': '*', 'fisub': '-', 'fidiv': '/', 'fisubr': '-r', 'fidivr': '/r'}
        if m in iar:
            e = self.ea(ops[0], ins)
            v = {2: '(double)(int16_t)rd16(%s)', 4: '(double)(int32_t)rd32(%s)'}[ops[0].size] % e
            o = iar[m]
            if o.endswith('r'): return 'ST(0) = FPR(%s %s ST(0));' % (v, o[0])
            return 'ST(0) = FPR(ST(0) %s %s);' % (o, v)
        if m in ('fcom', 'fcomp', 'fcompp', 'fucom', 'fucomp', 'fucompp'):
            if ops and ops[0].type == X.X86_OP_MEM: s = self.fmem(ins, ops[0])
            elif ops: s = 'ST(%d)' % st(ops[-1])
            else: s = 'ST(1)'
            npop = 2 if m.endswith('pp') else 1 if m.endswith('p') else 0
            return 'fcom_set(R, ST(0), %s);' % s + ' fpop(R);' * npop
        if m == 'ficom' or m == 'ficomp':
            e = self.ea(ops[0], ins)
            v = {2: '(double)(int16_t)rd16(%s)', 4: '(double)(int32_t)rd32(%s)'}[ops[0].size] % e
            return 'fcom_set(R, ST(0), %s);' % v + (' fpop(R);' if m == 'ficomp' else '')
        if m == 'ftst': return 'fcom_set(R, ST(0), 0.0);'
        if m == 'fxch':
            i = ins.bytes[1] & 7 if ins.bytes[1] >= 0xc8 else 1
            return 'double t = ST(0); ST(0) = ST(%d); ST(%d) = t;' % (i, i)
        simple = {'fchs': 'ST(0) = -ST(0);', 'fabs': 'ST(0) = fabs(ST(0));', 'fsqrt': 'ST(0) = FPR(sqrt(ST(0)));',
                  'fsin': 'ST(0) = sin(ST(0));', 'fcos': 'ST(0) = cos(ST(0));', 'frndint': 'ST(0) = frnd(R, ST(0));',
                  'fscale': 'ST(0) = ldexp(ST(0), (int)trunc(ST(1)));', 'f2xm1': 'ST(0) = exp2(ST(0)) - 1.0;',
                  'fyl2x': 'ST(1) = ST(1) * log2(ST(0)); fpop(R);', 'fpatan': 'ST(1) = atan2(ST(1), ST(0)); fpop(R);',
                  'fld1': 'fpush(R, 1.0);', 'fldz': 'fpush(R, 0.0);', 'fldpi': 'fpush(R, M_PI);',
                  'fldl2e': 'fpush(R, M_LOG2E);', 'fldlg2': 'fpush(R, 0.30102999566398119521);', 'fldln2': 'fpush(R, M_LN2);',
                  'fldl2t': 'fpush(R, 3.32192809488736234787);', 'fxam': 'fxam(R);', 'fnclex': 'R->fsw &= 0x7f00;',
                  'fninit': 'R->fcw = 0x37f; R->fsw = 0; R->ftop = 0;', 'fptan': 'ST(0) = tan(ST(0)); fpush(R, 1.0);',
                  'fsincos': 'double v = ST(0); ST(0) = sin(v); fpush(R, cos(v));', 'fprem': 'ST(0) = fmod(ST(0), ST(1));',
                  'fdecstp': 'R->ftop = (R->ftop - 1) & 7;', 'fincstp': 'R->ftop = (R->ftop + 1) & 7;'}
        if m in simple: return simple[m]
        if m == 'fnstsw':
            if ops and ops[0].type == X.X86_OP_REG: return 'R->eax = (R->eax & 0xffff0000u) | fstsw(R);'
            return 'wr16(%s, fstsw(R));' % self.ea(ops[0], ins)
        if m == 'fnstcw': return 'wr16(%s, R->fcw);' % self.ea(ops[0], ins)
        if m == 'fldcw': return 'R->fcw = rd16(%s);' % self.ea(ops[0], ins)
        if m == 'fnsave': return 'fnsave(R, %s);' % self.ea(ops[0], ins)
        if m == 'frstor': return 'frstor(R, %s);' % self.ea(ops[0], ins)
        if m == 'ffree': return ''
        raise Exception('fpu')


def main():
    # usage: gen.py outdir exe[:seeds.tsv] [dll ...]
    outdir = sys.argv[1]
    specs = sys.argv[2:]
    os.makedirs(outdir, exist_ok=True)
    allfuncs = []   # (address, Gen, set)
    imports = []
    total_unknown = collections.Counter()
    gens = []
    for spec in specs:
        path, _, tsv = spec.rpartition(':')
        if not tsv.endswith('.tsv'): path, tsv = spec, ''   # a ':' inside the path, no seeds
        img = Image(path, tsv or None)
        entries, funcs, tables, bad, allins = discover_all(img)
        g = Gen(img, entries, funcs, tables)
        gens.append(g)
        for k in sorted(funcs): allfuncs.append((k, g))
        for addr, (dll, name) in sorted(img.imports.items()):
            imports.append((addr, dll, name))
        print('%s: %d functions, %d instructions' % (os.path.basename(path), len(funcs), len(allins)))
    allfuncs.sort(key=lambda x: x[0])
    nfiles = 72
    per = (len(allfuncs) + nfiles - 1) // nfiles
    with open(os.path.join(outdir, 'funcs.h'), 'w') as h:
        h.write('#pragma once\n#include "cpu.h"\n#include "rt_ops.h"\nvoid hook_enter(CPU *R, uint32_t f);\nvoid hook_leave(CPU *R, uint32_t f);\nextern int trace_on;\nvoid trace_rec(CPU *R, uint32_t a);\n#define TR(a) do { if (trace_on) trace_rec(R, a); } while (0)\n')
        for k, _ in allfuncs: h.write('void f_%x(CPU *R);\n' % k)
    for old in os.listdir(outdir):
        if old.startswith('f') and old.endswith('.c'): os.remove(os.path.join(outdir, old))
    for fi in range(nfiles):
        chunk = allfuncs[fi * per:(fi + 1) * per]
        if not chunk: continue
        out = ['#include "funcs.h"', '#pragma clang diagnostic ignored "-Wunused-label"',
               '#pragma clang diagnostic ignored "-Wunused-variable"', '#pragma clang diagnostic ignored "-Wunused-but-set-variable"']
        for k, g in chunk:
            g.gen_func(Func(g, k, g.funcs[k]), out)
        with open(os.path.join(outdir, 'f%02d.c' % fi), 'w') as fh:
            fh.write('\n'.join(out) + '\n')
    with open(os.path.join(outdir, 'table.c'), 'w') as fh:
        fh.write('#include "funcs.h"\n#include "table.h"\nconst FnEntry gen_fn_table[] = {\n')
        for k, _ in allfuncs: fh.write('  {0x%xu, f_%x},\n' % (k, k))
        fh.write('};\nconst int gen_fn_table_len = %d;\n' % len(allfuncs))
        fh.write('const ImportEntry gen_import_table[] = {\n')
        for addr, dll, name in imports:
            fh.write('  {0x%xu, "%s", "%s"},\n' % (addr, dll, name))
        fh.write('};\nconst int gen_import_table_len = %d;\n' % len(imports))
    for g in gens: total_unknown.update(g.unknown)
    print('functions', len(allfuncs), 'unknown', total_unknown.most_common(40))


if __name__ == '__main__':
    main()
