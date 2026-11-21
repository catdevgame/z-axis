"""diffcall_search.py: the search behind diffcall.py.

Blind generated calls reach a function and stop at its first rare branch, so a
pass says little. This module turns the rounds of a snapshot into a coverage-guided
search. Every case that returns reports the blocks of the original it ran; a case
with a new set of blocks (a new path, not only a new block: quar1's trickRelease
differs only where two old blocks meet) is a seed and is mutated in the next round,
the branches into blocks nothing has run first (the corpus's coverage, --new-blocks).

Direction comes from the listing. `Listing` cuts a function into the recorder's
blocks and slices each conditional branch back to its sources: an argument, a load
(whose runtime address the seed's read log gives), a call's result, against a
constant, a mask or the other operand. `Hunt.solve` then proposes the values that
go the other way:

  flip   a seed again with a compared field, argument or global set to the value
         that takes the other side of a branch
  force  ... with a callee's result set to it (the callee ran; the result is an
         input of the recorded call, as the replay answers it from the recording)
  havoc  ... with any field it read set to one of the function's constants, a bit
         flipped, or a usual float
  repair a case that faulted again with the pointer it went through (a field, an
         argument, a result) pointing at a zeroed block, one layer per round
  format library calls with real structured inputs: format strings built from
         conversion specifiers and the game's own, with matching arguments or
         text, made at the function or a caller that takes the format (sscanf
         builds __svfscanf's FILE); FILEs from the snapshot
  caller a target that stays shallow reached through its static callers

None of this knows a function by name. An input where the original faults or hangs
is dropped by the runner as before; only original against readable decides.
"""

import bisect
import collections
import re
import struct

ZERO, V0, V1, GP, SP, RA = 0, 2, 3, 28, 29, 31
CALL_CLOBBERED = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 24, 25}

LOAD_SIZE = {0x20: (1, True), 0x24: (1, False), 0x21: (2, True), 0x25: (2, False), 0x23: (4, True), 0x27: (4, False),
             0x22: (4, True), 0x26: (4, True), 0x37: (8, False), 0x1A: (8, False), 0x1B: (8, False), 0x1E: (16, False)}
FLOAT_LOADS = {0x31: 4, 0x36: 16}
STORE_SIZE = {0x28: 1, 0x29: 2, 0x2B: 4, 0x2A: 4, 0x2E: 4, 0x3F: 8, 0x2C: 8, 0x2D: 8, 0x1F: 16}
FLOAT_STORES = {0x39: 4, 0x3E: 16}

# The poke arena (diffcall.cpp: [sp - ARENA_DROP, sp)): the parameters' blocks, then blocks
# for repairs, then a format, a text, the varargs and their out buffers.
ARENA_DROP = 0x8000
ARENA_SKIP = 0x200
ARENA_SIZE = 0x7800
PARAM_BLOCK = 0x700
REPAIR_FROM, REPAIR_BLOCK, REPAIR_COUNT = 0x3800, 0x400, 12
FORMAT_AT, TEXT_AT, VA_AT, OUT_AT, OUT_SLOT = 0x6800, 0x6A00, 0x6C00, 0x6D00, 0x40


def sx16(v):
    return v - 0x10000 if v & 0x8000 else v


def s32(v):
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v


def sext32(v):
    """A 32-bit value as a register holds it: a value from the listing's slices is 32 bits, and a v0 or
    argument of 0x00000000FFFFFFFF is no int the game makes (bgez tests bit 63, an int32 its bit 31)."""
    v &= 0xFFFFFFFFFFFFFFFF
    return v | 0xFFFFFFFF00000000 if v < (1 << 32) and v & 0x80000000 else v


def float_bits(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def bits_float(v):
    return struct.unpack('<f', struct.pack('<I', v & 0xFFFFFFFF))[0]


def control(w, pc):
    """(kind, target): the recorder's kinds of control flow (fncheck_traced_gen.py control)."""
    op, rs, rt = w >> 26, (w >> 21) & 31, (w >> 16) & 31
    offset = (pc + 4 + (sx16(w & 0xFFFF) << 2)) & 0xFFFFFFFF
    if op == 0:
        fn = w & 63
        if fn == 8:
            return 'jr', None
        if fn == 9:
            return 'call', None
        return None, None
    if op == 2:
        return 'jump', (pc & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
    if op == 3:
        return 'call', (pc & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
    if op == 1:
        if rt in (0, 1, 2, 3):
            return 'branch', offset
        if rt in (0x10, 0x11, 0x12, 0x13):
            return 'call', offset
        return None, None
    if op in (4, 5, 6, 7, 0x14, 0x15, 0x16, 0x17):
        return 'branch', offset
    if op in (0x11, 0x12) and rs == 8:
        return 'branch', offset
    return None, None


class Branch:
    __slots__ = ('pc', 'block', 'taken', 'fall', 'cond')

    def __init__(self, pc, block, taken, fall):
        self.pc, self.block, self.taken, self.fall, self.cond = pc, block, taken, fall, False


class Listing:
    """One function: its blocks (as the recorder cuts them), branches, loads, and slices."""

    def __init__(self, elf, start, end):
        self.start, self.end = start, end
        self.words = {}
        pc = start
        while pc < end:
            w = elf.word(pc)
            if w is None:
                break
            self.words[pc] = w
            pc += 4
        lead = {start}
        branches = []
        self.jumps = []             # jr that is not a return: maybe a switch
        for a, w in self.words.items():
            kind, target = control(w, a)
            if kind in ('branch', 'jump', 'jr'):
                lead.add(a + 8)
                if kind == 'branch' or (kind == 'jump' and target is not None):
                    lead.add(target)
                if kind == 'branch':
                    branches.append((a, target))
                if kind == 'jr' and (w >> 21) & 31 != RA:
                    self.jumps.append(a)
        real = [a for a, w in self.words.items() if w]
        last = max(real) if real else start
        self.leaders = sorted(a for a in lead if start <= a < end and a <= last)
        self.branches = {}
        for a, target in branches:
            b = self.block_of(a)
            br = Branch(a, b, target, a + 8)
            w = self.words[a]
            br.cond = not (w >> 26 == 4 and (w >> 21) & 31 == (w >> 16) & 31)   # (beq r, r: a jump)
            self.branches[b] = br
        self.memory_ops = None

    def block_of(self, pc):
        i = bisect.bisect_right(self.leaders, pc) - 1
        return self.leaders[i] if i >= 0 else self.start

    def constants(self):
        """The numbers this function compares with or masks by: the dictionary of `havoc`."""
        out = set()
        for w in self.words.values():
            op = w >> 26
            imm = w & 0xFFFF
            if op in (0x0A, 0x0B):
                out.add(sx16(imm) & 0xFFFFFFFF)
            elif op in (0x0C, 0x0E):
                out.add(imm)
            elif op == 0x09 and (w >> 21) & 31 == 0 and imm:
                out.add(sx16(imm) & 0xFFFFFFFF)
            elif op == 0x0F and imm:
                out.add(imm << 16)
        return sorted(out)

    def mem_ops(self, block):
        """The block's (pc, is_store) in order, for the read log's index of a load."""
        if self.memory_ops is None:
            self.memory_ops = collections.defaultdict(list)
            for a in sorted(self.words):
                op = self.words[a] >> 26
                if op in LOAD_SIZE or op in FLOAT_LOADS:
                    self.memory_ops[self.block_of(a)].append((a, False))
                elif op in STORE_SIZE or op in FLOAT_STORES:
                    self.memory_ops[self.block_of(a)].append((a, True))
        return self.memory_ops.get(block, [])

    def nth_op(self, block, n, store):
        """The nth (1-based) load, or store, of a block."""
        ops = [a for a, s in self.mem_ops(block) if s == store]
        return ops[n - 1] if 0 < n <= len(ops) else None

    def op_index(self, pc):
        """(block, index): a load's place in its block's loads, as the read log orders them."""
        block = self.block_of(pc)
        ops = [a for a, s in self.mem_ops(block) if not s]
        return block, ops.index(pc) if pc in ops else None

    # ---- slices: what a register holds at a pc, walking the listing back ------------------------
    #
    # ('const', v), ('arg', i), ('farg', i), ('load', pc, size, signed), ('call', pc), ('callf', pc),
    # ('add', e, k), ('and', e, mask), ('sll'|'srl'|'sra', e, n), ('lt', a, b, signed), ('sub', a, b),
    # ('bits', e), None. Linear, so a guess across a join: a wrong one costs a case.

    def _before(self, q):
        """q is the delay slot of a call: its pc."""
        w = self.words.get(q - 4)
        return q - 4 if w is not None and control(w, q - 4)[0] == 'call' else None

    def gpr(self, reg, pc, depth=0):
        if reg == ZERO:
            return ('const', 0)
        if depth > 8:
            return None
        q = pc - 4
        low = max(self.start, pc - 0x140)
        while q >= low:
            w = self.words.get(q)
            if w is None:
                return None
            kind, _ = control(w, q)
            call = self._before(q)
            if call is not None:
                if reg == V0:
                    return ('call', call)
                if reg in CALL_CLOBBERED:
                    return None
            if kind in ('jr', 'jump') and q != pc - 4:
                return None
            found, e = self._define(reg, q, w, depth)
            if found:
                return e
            q -= 4
        if q < self.start and 4 <= reg <= 11:
            return ('arg', reg - 4)
        return None

    def _define(self, reg, q, w, depth):
        op, rs, rt, rd, sa, fn = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31, w & 63
        imm = w & 0xFFFF
        if op == 0x0F:
            return (True, ('const', (sx16(imm) << 16) & 0xFFFFFFFF)) if rt == reg else (False, None)
        if op in (0x09, 0x19):
            if rt != reg:
                return False, None
            if rs == ZERO:
                return True, ('const', sx16(imm) & 0xFFFFFFFF)
            return True, add(self.gpr(rs, q, depth + 1), sx16(imm))
        if op == 0x0D:
            if rt != reg:
                return False, None
            e = self.gpr(rs, q, depth + 1)
            return True, ('const', e[1] | imm) if e and e[0] == 'const' else None
        if op == 0x0C:
            return (True, ('and', self.gpr(rs, q, depth + 1), imm)) if rt == reg else (False, None)
        if op == 0x0E:
            return (True, ('sub', self.gpr(rs, q, depth + 1), ('const', imm))) if rt == reg else (False, None)
        if op in (0x0A, 0x0B):
            if rt != reg:
                return False, None
            return True, ('lt', self.gpr(rs, q, depth + 1), ('const', sx16(imm) & 0xFFFFFFFF), op == 0x0A)
        if op == 0:
            if rd != reg or w == 0:
                return False, None
            if fn in (0x21, 0x2D, 0x25):
                if rs == ZERO:
                    return True, self.gpr(rt, q, depth + 1)
                if rt == ZERO:
                    return True, self.gpr(rs, q, depth + 1)
                a, b = self.gpr(rs, q, depth + 1), self.gpr(rt, q, depth + 1)
                if fn != 0x25 and b and b[0] == 'const':
                    return True, add(a, s32(b[1]))
                if fn != 0x25 and a and a[0] == 'const':
                    return True, add(b, s32(a[1]))
                return True, None
            if fn in (0x23, 0x2F, 0x26):
                return True, ('sub', self.gpr(rs, q, depth + 1), self.gpr(rt, q, depth + 1))
            if fn == 0x24:
                a, b = self.gpr(rs, q, depth + 1), self.gpr(rt, q, depth + 1)
                if b and b[0] == 'const':
                    return True, ('and', a, b[1])
                if a and a[0] == 'const':
                    return True, ('and', b, a[1])
                return True, None
            if fn in (0x2A, 0x2B):
                return True, ('lt', self.gpr(rs, q, depth + 1), self.gpr(rt, q, depth + 1), fn == 0x2A)
            if fn in (0x00, 0x02, 0x03):
                return True, ({0: 'sll', 2: 'srl', 3: 'sra'}[fn], self.gpr(rt, q, depth + 1), sa)
            return True, None
        if op in LOAD_SIZE:
            if rt != reg:
                return False, None
            size, signed = LOAD_SIZE[op]
            if rs == SP:
                stored = self.spill(q, sx16(imm), depth)
                if stored is not False:
                    return True, stored
            return True, ('load', q, size, signed)
        if op == 0x11 and rs == 0 and rt == reg:                 # mfc1
            return True, ('bits', self.fpr(rd, q, depth + 1))
        if op == 0x1C and rd == reg:                             # MMI
            return True, None
        if op == 0x12 and rs in (1, 2) and rt == reg:            # qmfc2, cfc2
            return True, None
        return False, None

    def spill(self, q, offset, depth):
        """What a stack slot held: the register stored there before q, or False."""
        a = q - 4
        while a >= max(self.start, q - 0x400):
            w = self.words[a]
            op, rs, rt = w >> 26, (w >> 21) & 31, (w >> 16) & 31
            if op in STORE_SIZE and rs == SP and sx16(w & 0xFFFF) == offset:
                return self.gpr(rt, a, depth + 1)
            if op in FLOAT_STORES and rs == SP and sx16(w & 0xFFFF) == offset:
                return self.fpr(rt, a, depth + 1)
            a -= 4
        return False

    def fpr(self, reg, pc, depth=0):
        if depth > 8:
            return None
        q = pc - 4
        low = max(self.start, pc - 0x140)
        while q >= low:
            w = self.words.get(q)
            if w is None:
                return None
            call = self._before(q)
            if call is not None:
                if reg == 0:
                    return ('callf', call)
                if reg < 20:
                    return None
            op, rs, rt, rd, sa, fn = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31, w & 63
            if op in FLOAT_LOADS and rt == reg:
                if rs == SP:
                    stored = self.spill(q, sx16(w & 0xFFFF), depth)
                    if stored is not False:
                        return stored
                return ('load', q, 4, False)
            if op == 0x11:
                if rs == 4 and rd == reg:                        # mtc1
                    return self.gpr(rt, q, depth + 1)
                if rs == 16 and sa == reg:                       # S format: fd is in the sa field
                    if fn == 6:                                  # mov.s
                        return self.fpr(rd, q, depth + 1)
                    return None
            q -= 4
        if q < self.start and 12 <= reg < 20:
            return ('farg', reg - 12)
        return None

    def condition(self, br):
        """A branch's condition as what it tests: (kind, ...) of the sources, or None."""
        w = self.words[br.pc]
        op, rs, rt = w >> 26, (w >> 21) & 31, (w >> 16) & 31
        if op in (4, 5, 0x14, 0x15):
            a, b = self.gpr(rs, br.pc), self.gpr(rt, br.pc)
            if a and b and b == ('const', 0):
                if a[0] == 'lt':
                    return ('lt', a[1], a[2], a[3])
                if a[0] == 'and':
                    return ('bit', a[1], a[2])
                if a[0] == 'sub':
                    return ('eq', a[1], a[2])
            return ('eq', a, b)
        if op in (6, 7, 0x16, 0x17, 1):
            a = self.gpr(rs, br.pc)
            if a and a[0] == 'and':
                return ('bit', a[1], a[2])
            return ('sign', a)
        if op == 0x11:
            q = br.pc - 4
            while q >= max(self.start, br.pc - 0x28):
                c = self.words[q]
                if c >> 26 == 0x11 and (c >> 21) & 31 == 16 and c & 0x30 == 0x30:
                    return ('fcmp', self.fpr((c >> 11) & 31, q), self.fpr((c >> 16) & 31, q))
                q -= 4
        return None

    def switches(self, elf):
        """[(index expression, {block: value})] of the jump tables: `jr` through a loaded word
        whose table the slice ends at, the index bounded by a sltiu before it."""
        out = []
        for pc in self.jumps:
            w = self.words[pc]
            target = self.gpr((w >> 21) & 31, pc)
            if not target or target[0] != 'load':
                continue
            load = self.words[target[1]]
            base = self.gpr((load >> 21) & 31, target[1])
            # base = table + (index << 2): the two summands, by hand.
            q, index, table = target[1] - 4, None, None
            while q >= max(self.start, target[1] - 0x30):
                x = self.words[q]
                op, fn, rd, rs, rt = x >> 26, x & 63, (x >> 11) & 31, (x >> 21) & 31, (x >> 16) & 31
                if op == 0 and fn in (0x21, 0x2D) and rd == (load >> 21) & 31:
                    a, b = self.gpr(rs, q), self.gpr(rt, q)
                    for x1, x2 in ((a, b), (b, a)):
                        if x1 and x1[0] == 'sll' and x2 and x2[0] == 'const':
                            index, table = x1[1], x2[1]
                    break
                q -= 4
            if table is None:
                table = base[1] if base and base[0] == 'add' and base[1] and base[1][0] == 'const' else None
            if index is None or table is None:
                continue
            table = (table + sx16(load & 0xFFFF)) & 0xFFFFFFFF
            blocks = {}
            for i in range(256):
                word = elf.word(table + 4 * i)
                if word is None or not self.start <= word < self.end:
                    break
                blocks.setdefault(self.block_of(word), i)
            if blocks:
                out.append((index, blocks))
        return out


# ---- what later code reads past a call's low words ------------------------------------

def wide_reads(w):
    """The general registers an instruction reads all 128 bits of."""
    op, rs, rt, fn = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 63
    if op == 0x1F:                                  # sq
        return {rt}
    if op == 0x1C and fn in (8, 9, 0x28, 0x29):     # the MMI groups: parallel operations, por, pcpy*
        return {rs, rt} - {ZERO}
    if op == 0x12 and rs == 5:                      # qmtc2
        return {rt}
    return set()


def writes_gpr(w):
    """The general register an instruction sets, or None."""
    op, rs, rt, rd, fn = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, w & 63
    if op == 0:
        if fn in (8, 0x0C, 0x0D, 0x0F, 0x11, 0x13, 0x18, 0x19, 0x1A, 0x1B):
            return None
        return rd or None
    if op == 0x1C:
        return rd or None
    if op in (0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x18, 0x19) or op in LOAD_SIZE:
        return rt or None
    if op == 0x11 and rs in (0, 1, 2):
        return rt or None
    if op == 0x12 and rs in (1, 2):
        return rt or None
    return None


def vu0_uses(w):
    """(vf registers read, vf registers set) by a COP2 instruction, or None if not one."""
    op, rs, rt, rd, sa, fn = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31, w & 63
    if op == 0x36:
        return set(), {rt}
    if op == 0x3E:
        return {rt}, set()
    if op != 0x12:
        return None
    if rs == 1:                                     # qmfc2
        return {rd}, set()
    if rs == 5:                                     # qmtc2
        return set(), {rd}
    if rs >= 0x10:
        if fn >= 0x3C:
            return {rd}, {rt}
        return {rd, rt}, {sa}
    return None


def upper_reads(elf, functions):
    """{address: (args, returns, vu0)} (fncheck_main.cpp --upper): the argument registers whose upper
    half a function reads before it sets them, and the general and vf registers some static
    caller reads after it returns (linear from the jal, to the first jump)."""
    out = collections.defaultdict(lambda: [0, 0, 0])
    for start, end in functions:
        pc = start
        live = set(range(4, 12))
        while pc < end and live:
            w = elf.word(pc)
            if w is None or control(w, pc)[0] in ('jr', 'jump'):
                break
            for r in wide_reads(w) & live:
                out[start][0] |= 1 << (r - 4)
            if control(w, pc)[0] == 'call':
                break
            d = writes_gpr(w)
            live.discard(d)
            pc += 4
        pc = start
        while pc < end:
            w = elf.word(pc)
            if w is None:
                break
            if w >> 26 == 3:
                target = (pc & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
                gprs, vfs = {V0, V1}, set()
                seen_vf = set()
                q = pc + 8
                for _ in range(48):
                    x = elf.word(q)
                    if x is None or q >= end:
                        break
                    k = control(x, q)[0]
                    if k in ('jr', 'jump', 'call'):
                        break
                    for r in wide_reads(x) & gprs:
                        out[target][1] |= 1 << r
                    gprs.discard(writes_gpr(x))
                    uses = vu0_uses(x)
                    if uses:
                        reads, sets = uses
                        vfs |= (reads - seen_vf - {0})
                        seen_vf |= sets
                    q += 4
                for v in vfs:
                    out[target][2] |= 1 << v
            pc += 4
    return out


def add(e, k):
    if e is None:
        return None
    if e[0] == 'const':
        return ('const', (e[1] + k) & 0xFFFFFFFF)
    if e[0] == 'add':
        return ('add', e[1], e[2] + k)
    return ('add', e, k)


# ---- a case ---------------------------------------------------------------------------

class Case:
    """A case as the runner reads it, and what its run told."""

    __slots__ = ('target', 'entry', 'invoke', 'ints', 'floats', 'pokes', 'forces', 'kinds', 'blocks', 'reads', 'calls', 'wild', 'sp', 'logged', 'frame')

    def __init__(self, target, entry, invoke, ints, floats, pokes, forces, kinds, sp):
        self.target, self.entry, self.invoke = target, entry, invoke
        self.ints, self.floats, self.pokes, self.forces, self.kinds, self.sp = ints, floats, pokes, forces, kinds, sp
        self.blocks = frozenset()
        self.reads = {}             # block: [(address, size, value)] of its first visit
        self.calls = {}             # site: (v0, f0 bits, target)
        self.wild = None            # (block, ordinal, address, store)
        self.logged = False         # its feedback came
        self.frame = sp             # where the call's stack starts (an invocation's is elsewhere)

    @classmethod
    def parse(cls, text, kinds, sp):
        """From a case line's words after the id."""
        target, ints, floats, pokes = text.split()[:4]
        invoke = target.startswith('~')
        word, _, via = target.lstrip('~').partition('@')
        t = int(word, 16)
        forces, plain = {}, []
        if pokes != '-':
            for p in pokes.split(','):
                if p.startswith('^'):
                    key, _, value = p.partition('=')
                    forces[key] = value
                else:
                    plain.append(p)
        return cls(t, int(via, 16) if via else t, invoke, [int(v, 16) for v in ints.split(',')],
                   [int(v, 16) for v in floats.split(',')], plain, forces, list(kinds), sp)

    def copy(self, kind):
        c = Case(self.target, self.entry, self.invoke, list(self.ints), list(self.floats), list(self.pokes), dict(self.forces),
                 sorted(set(self.kinds) | {kind}), self.sp)
        c.frame = self.frame
        return c

    def text(self):
        name = ('~' if self.invoke else '') + '%x' % self.target + ('@%x' % self.entry if self.entry != self.target else '')
        pokes = self.pokes + ['%s=%s' % kv for kv in self.forces.items()]
        return '%s %s %s %s' % (name, ','.join('%x' % v for v in self.ints), ','.join('%x' % v for v in self.floats),
                                ','.join(pokes) if pokes else '-')

    def arena(self):
        return self.sp + ARENA_SKIP

    def apply(self, edits):
        """A case with the edits made: ('poke', address, size, value), ('force', site, value),
        ('forcef', site, bits), ('arg', i, value), ('farg', i, bits). None if one cannot be made."""
        for e in edits:
            kind = e[0]
            if kind == 'poke':
                address, size, value = e[1:]
                if size > 8:
                    size = 8
                self.pokes.append('%x=%s' % (address, (value & ((1 << (8 * size)) - 1)).to_bytes(size, 'little').hex()))
            elif kind == 'force':
                self.forces['^%x' % e[1]] = '%x' % sext32(e[2])
            elif kind == 'forcef':
                self.forces['^%x:f' % e[1]] = '%x' % e[2]
            elif kind == 'arg':
                if self.entry != self.target:
                    return None
                self.ints[e[1]] = sext32(e[2])
            elif kind == 'farg':
                if self.entry != self.target:
                    return None
                self.floats[e[1]] = e[2] & 0xFFFFFFFF
        return self


# ---- the hunt for one target ----------------------------------------------------------

USUAL_FLOATS = (0.0, 1.0, -1.0, 0.5, 2.0, 100.0, -100.0, 1e6, -1e6)
USUAL_INTS = (0, 1, 2, 3, 0xFF, 0xFFFF, 0xFFFFFFFF, 0x7FFFFFFF, 0x80000000)


class Hunt:
    def __init__(self, target, listing, corpus, stub=0, constant=lambda address: False):
        self.target = target
        self.stub = stub            # a function that only returns
        self.constant = constant    # an address the game never writes: no input
        self.listing = listing
        self.corpus = corpus        # blocks the recordings run
        self.ran = set()            # ... this search's cases
        self.paths = set()
        self.constants = listing.constants()
        self.switches = None
        self.conditions = {}
        self.begin_snapshot()
        self.gained = 0             # blocks outside the corpus
        self.fault_sites = set()
        self.snapshots = 0
        self.cases = 0
        self.first_reach = None

    def begin_snapshot(self):
        self.seeds = []
        self.faults = []
        self.tried = set()
        self.repairs = 0
        self.idle = 0

    def missing(self):
        return [b for b in self.listing.leaders if b not in self.corpus and b not in self.ran]

    def take(self, case):
        """A returned or faulted case's feedback. True if it opened a new path."""
        fresh = False
        if case.blocks:
            new = case.blocks - self.ran
            self.gained += sum(1 for b in new if b not in self.corpus)
            self.ran |= case.blocks
            if case.blocks not in self.paths and case.wild is None:
                self.paths.add(case.blocks)
                fresh = True
        return fresh

    def value_of(self, e, case):
        """What the seed had for expression e, or None."""
        if e is None:
            return None
        k = e[0]
        if k == 'const':
            return e[1]
        if k == 'arg':
            return case.ints[e[1]] if case.entry == case.target else None
        if k == 'farg':
            return case.floats[e[1]] if case.entry == case.target else None
        if k == 'load':
            row = self.read_of(e[1], case)
            return row[2] if row else None
        if k in ('call', 'callf'):
            row = case.calls.get(e[1])
            return (row[0] if k == 'call' else row[1]) if row else None
        if k == 'add':
            v = self.value_of(e[1], case)
            return None if v is None else (v + e[2]) & 0xFFFFFFFF
        if k == 'and':
            v = self.value_of(e[1], case)
            return None if v is None else v & e[2]
        if k == 'sll':
            v = self.value_of(e[1], case)
            return None if v is None else (v << e[2]) & 0xFFFFFFFF
        if k in ('srl', 'sra'):
            v = self.value_of(e[1], case)
            return None if v is None else (v & 0xFFFFFFFF) >> e[2]
        if k == 'bits':
            return self.value_of(e[1], case)
        return None

    def read_of(self, pc, case):
        block, index = self.listing.op_index(pc)
        rows = case.reads.get(block)
        if rows is None or index is None or index >= len(rows):
            return None
        return rows[index]

    def assign(self, e, value, case):
        """The edits that make expression e hold value, or None."""
        if e is None:
            return None
        k = e[0]
        if k == 'load':
            row = self.read_of(e[1], case)
            if not row:
                return None
            address, size, _ = row
            if case.frame - 0x10000 <= (address & 0x1FFFFFFF) < case.frame or self.constant(address):
                return None         # (the function's own frame, or a constant)
            return [('poke', address, e[2] if e[2] < size else size, value)]
        if k == 'arg':
            return [('arg', e[1], value)]
        if k == 'farg':
            return [('farg', e[1], value)]
        if k == 'call':
            return [('force', e[1], value)]
        if k == 'callf':
            return [('forcef', e[1], value)]
        if k == 'add':
            return self.assign(e[1], (value - e[2]) & 0xFFFFFFFF, case)
        if k == 'and':
            had = self.value_of(e[1], case)
            return self.assign(e[1], ((had or 0) & ~e[2]) | (value & e[2]), case)
        if k == 'sll':
            return self.assign(e[1], (value & 0xFFFFFFFF) >> e[2], case)
        if k in ('srl', 'sra'):
            had = self.value_of(e[1], case) or 0
            return self.assign(e[1], ((value << e[2]) | (had & ((1 << e[2]) - 1))) & 0xFFFFFFFF, case)
        if k == 'bits':
            return self.assign(e[1], value, case)
        return None

    def condition(self, br):
        if br.pc not in self.conditions:
            self.conditions[br.pc] = self.listing.condition(br)
        return self.conditions[br.pc]

    def solve(self, br, case, block):
        """Edits to try for a branch of the seed, either way (the slice does not know which is which)."""
        c = self.condition(br)
        if c is None:
            return []
        out = []

        def want(e, values):
            for v in values:
                edits = self.assign(e, v, case)
                if edits:
                    out.append(edits)

        kind = c[0]
        if kind == 'eq':
            a, b = c[1], c[2]
            va, vb = self.value_of(a, case), self.value_of(b, case)
            if a and b:
                if vb is not None:
                    want(a, [vb, (vb + 1) & 0xFFFFFFFF, block])
                if va is not None and b[0] != 'const':
                    want(b, [va, (va + 1) & 0xFFFFFFFF])
            elif a:
                want(a, [0, 1, block])
            elif b:
                want(b, [0, 1, block])
        elif kind == 'bit':
            had = self.value_of(c[1], case)
            mask = c[2]
            want(c[1], [(had or 0) | mask, (had or 0) & ~mask & 0xFFFFFFFF, mask])
        elif kind == 'lt':
            a, b = c[1], c[2]
            if b and b[0] == 'const':
                want(a, [(b[1] - 1) & 0xFFFFFFFF, b[1], (b[1] + 1) & 0xFFFFFFFF])
            else:
                va, vb = self.value_of(a, case), self.value_of(b, case)
                if vb is not None:
                    want(a, [(vb - 1) & 0xFFFFFFFF, vb, (vb + 1) & 0xFFFFFFFF])
                if va is not None and b is not None:
                    want(b, [(va - 1) & 0xFFFFFFFF, va, (va + 1) & 0xFFFFFFFF])
        elif kind == 'sign':
            want(c[1], [0, 1, 0xFFFFFFFF, block])
        elif kind == 'fcmp':
            a, b = c[1], c[2]
            va, vb = self.value_of(a, case), self.value_of(b, case)
            usual = [float_bits(x) for x in USUAL_FLOATS]
            for e, other in ((a, vb), (b, va)):
                if e is None or e[0] == 'const':
                    continue
                near = []
                if other is not None:
                    x = bits_float(other)
                    if x == x and abs(x) != float('inf'):
                        near = [other, float_bits(x + 1.0), float_bits(x - 1.0)]
                want(e, near + usual)
        return out

    def frontier(self, rng):
        """(rank, seed, branch): the seeds' branches with a side the seed did not take, the
        sides nothing ran first, then those only the corpus ran, then the rest (new paths)."""
        out = []
        branches = self.listing.branches
        for seed in self.seeds[-32:]:
            for b in seed.blocks:
                br = branches.get(b)
                if br is None or not br.cond:
                    continue
                sides = [s for s in (br.taken, br.fall) if s not in seed.blocks]
                if not sides:
                    continue
                other = sides[0]
                rank = 0 if other not in self.corpus and other not in self.ran else (1 if other not in self.ran else 2)
                out.append((rank, rng.random(), seed, br))
        out.sort(key=lambda r: r[:2])
        return out

    def switch_cases(self, elf, rng, budget):
        """Seeds again with a jump table's index at the values of blocks they did not run."""
        if self.switches is None:
            self.switches = self.listing.switches(elf)
        out = []
        for index, blocks in self.switches:
            for seed in self.seeds[-8:]:
                for block, value in sorted(blocks.items(), key=lambda kv: (kv[0] in self.ran, rng.random())):
                    if block in seed.blocks or len(out) >= budget:
                        continue
                    edits = self.assign(index, value, seed)
                    if edits:
                        made = self.make(seed, 'flip', edits)
                        if made:
                            out.append(made)
        return out

    def make(self, seed, kind, edits):
        kind2 = 'force' if any(e[0].startswith('force') for e in edits) else kind
        c = seed.copy(kind2).apply(edits)
        if c is None:
            return None
        text = c.text()
        if text in self.tried:
            return None
        self.tried.add(text)
        return c

    def propose(self, elf, rng, budget):
        """The next round's cases, [Case]."""
        out = []
        if not self.seeds and not self.faults:
            return out
        out += self.repair_cases(max(4, budget // 4))
        if not self.seeds:
            return out
        out += self.switch_cases(elf, rng, budget // 3)
        room = budget - budget // 4
        for rank, _, seed, br in self.frontier(rng):
            if len(out) >= room:
                break
            block = seed.arena() + REPAIR_FROM + REPAIR_BLOCK * (rng.randrange(REPAIR_COUNT))
            tried = 0
            for edits in self.solve(br, seed, block):
                made = self.make(seed, 'flip', edits)
                if made:
                    out.append(made)
                    tried += 1
                    if tried >= (6 if rank == 0 else 3):
                        break
        out += self.havoc(rng, budget - len(out))
        return out

    def havoc(self, rng, budget):
        out = []
        if budget <= 0 or not self.seeds:
            return out
        for _ in range(budget * 4):
            if len(out) >= budget:
                break
            seed = rng.choice(self.seeds[-32:])
            pool = [r for rows in seed.reads.values() for r in rows
                    if not seed.frame - 0x10000 <= (r[0] & 0x1FFFFFFF) < seed.frame and not self.constant(r[0])]
            # (A tail call comes back past the function: its site is no place to force a result.)
            sites = [s for s in seed.calls if self.listing.start <= s < self.listing.end]
            if sites and (not pool or rng.random() < 0.25):
                site = rng.choice(sites)
                v0 = seed.calls[site][0]
                value = rng.choice([0, 1, 0xFFFFFFFFFFFFFFFF, v0 ^ 1, 2])
                made = self.make(seed, 'havoc', [('force', site, value)])
            elif pool:
                address, size, had = rng.choice(pool)
                n = min(size, 8)
                choices = [rng.choice(USUAL_INTS), had ^ (1 << rng.randrange(8 * n))]
                if self.constants:
                    choices.append(rng.choice(self.constants))
                if n == 4:
                    choices.append(float_bits(rng.choice(USUAL_FLOATS)))
                made = self.make(seed, 'havoc', [('poke', address, n, rng.choice(choices))])
            else:
                continue
            if made:
                out.append(made)
        return out

    def repair_cases(self, budget):
        """Faulted cases again with the pointer they went through at a zeroed block."""
        out = []
        faults, self.faults = self.faults, []
        for case in faults[:budget * 2]:
            if len(out) >= budget:
                break
            block, ordinal, _, store = case.wild
            if store == 2:
                # A call through a pointer: its register, at a function that returns at once.
                w = self.listing.words.get(ordinal)
                if w is None or not self.stub:
                    continue
                e = self.listing.gpr((w >> 21) & 31, ordinal)
                pointer = self.stub
            else:
                pc = self.listing.nth_op(block, ordinal, bool(store))
                if pc is None:
                    continue
                w = self.listing.words[pc]
                e = self.listing.gpr((w >> 21) & 31, pc)
                self.repairs += 1
                pointer = case.arena() + REPAIR_FROM + REPAIR_BLOCK * (self.repairs % REPAIR_COUNT)
            if e and e[0] == 'add':
                e = e[1]
            edits = self.assign(e, pointer, case)
            if edits:
                made = self.make(case, 'repair', edits)
                if made:
                    out.append(made)
        return out


def find_stub(elf, functions):
    """A function that returns at once with v0 0, for a function pointer a made-up object lacks."""
    for start in functions:
        if elf.word(start) == 0x03E00008 and elf.word(start + 4) in (0x0000102D, 0x00001021, 0x24020000):
            return start
    return 0


def parse_reads(path, cases):
    """batch-<n>.reads into the cases: {id: Case}."""
    current = None
    try:
        f = open(path)
    except OSError:
        return
    with f:
        for line in f:
            if line[0] == '#':
                current = cases.get(line[1:].strip())
                if current is not None:
                    current.logged = True
                continue
            if current is None:
                continue
            c = line.split()
            if c[0] == 'S':
                current.frame = int(c[1], 16)
            elif c[0] == 'R':
                if int(c[2], 16) == 1:
                    current.reads.setdefault(int(c[1], 16), []).append((int(c[3], 16), int(c[4], 16), int(c[5], 16)))
            elif c[0] == 'C':
                current.calls.setdefault(int(c[1], 16), (int(c[3], 16), int(c[4], 16), int(c[2], 16)))
            elif c[0] == 'W':
                current.wild = (int(c[1], 16), int(c[2], 16), int(c[3], 16), int(c[5]))


# ---- structured inputs ----------------------------------------------------------------

FLAGS = ('', '', '', '-', '+', ' ', '#', '0', '-0', '+#', ' 0')
WIDTHS = ('', '', '', '1', '2', '5', '12', '40', '*')
PRECISIONS = ('', '', '', '.', '.0', '.1', '.2', '.5', '.12', '.*')
LENGTHS = ('', '', '', 'h', 'hh', 'l', 'l', 'll', 'L', 'q')
INT_CONVERSIONS = 'diouxX'
FLOAT_CONVERSIONS = 'fFeEgG'
SETS = ('[a-z]', '[^0-9]', '[0-9a-f]', '[]x]', '[a\\-b]')
INT_VALUES = (0, 1, 7, 255, 0xFFFFFFFFFFFFFFFF, 0xFFFFFFFFFFFFFF01, 1000, 65536, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF80000000,
              0x123456789A, 0xFFFFFFEDCBA98766, 0x7FFFFFFFFFFFFFFF, 0x8000000000000000)
DOUBLE_VALUES = (0.0, 1.0, -1.0, 0.5, 1.5, 3.14159, 1e10, 1e-10, 1e100, 1e-300, 123456.789)
SCAN_NUMBERS = ('0', '7', '-5', '+12', '123', '65535', '2147483647', '2147483648', '4294967295', '4294967296', '123456789012',
                '-123456789012', '9223372036854775807', '18446744073709551616', '0x1A', '0X7f', '017', '0', '-0x1a')
SCAN_FLOATS = ('1.5', '-2.5e3', '1e10', '.5', '5.', '1e-10', '1e309', '0', 'x')
SCAN_WORDS = ('abc', 'hello', 'a-b', 'x', ']x', 'zz9', 'A')
TEXTS = ('', 'a', 'abc', 'Hello, world', '  42', '-7', '123456789012', '0x1F', '1.5e3', '%d', 'A' * 40, 'path/to/file.ext', 'true', 'x y z')


def format_case(rng, real_formats, scanning=False):
    """(format, printf arguments, scanf text, scanf argument count): one format string, built from
    conversion specifiers or, a quarter of the time, one of the game's own. A format to scan has scanf's
    grammar: its flags and precisions are printf's, and a format with them stops at them."""
    if real_formats and rng.random() < 0.25:
        fmt = rng.choice(real_formats)
    elif scanning:
        fmt = ''
        for _ in range(rng.choice((1, 1, 2, 3))):
            if rng.random() < 0.3:
                fmt += rng.choice(('x', ' ', 'ab', ':', '\t', '%%'))
            fmt += '%' + rng.choice(('', '', '', '*')) + rng.choice(('', '', '', '1', '2', '5', '12', '40')) + rng.choice(LENGTHS) + \
                rng.choice((*INT_CONVERSIONS, *INT_CONVERSIONS, 'c', 's', 'p', 'n', *FLOAT_CONVERSIONS, *SETS))
    else:
        fmt = ''
        for _ in range(rng.choice((1, 1, 2, 3, 4))):
            if rng.random() < 0.3:
                fmt += rng.choice(('x', ' ', 'ab', ':', '\t', '%%'))
            fmt += '%' + rng.choice(FLAGS) + rng.choice(WIDTHS) + rng.choice(PRECISIONS) + rng.choice(LENGTHS) + \
                rng.choice(INT_CONVERSIONS + INT_CONVERSIONS + 'cspn' + FLOAT_CONVERSIONS + '%')
        if rng.random() < 0.3:
            fmt += 'z'
    args, text, outs = [], '', 0
    for m in re.finditer(r'%([-+ #0*]*)(\d*|\*)(\.\d*|\.\*)?(hh|ll|h|l|L|q|z)?(\[\^?\]?[^\]]*\]|.)', fmt):
        flags, width, prec, _, conv = m.groups()
        if '*' in flags or width == '*':
            args.append(('int', rng.choice((0, 1, 5, 12, 0xFFFFFFFFFFFFFFFD))))
        if prec == '.*':
            args.append(('int', rng.choice((0, 1, 5, 0xFFFFFFFFFFFFFFFF))))
        if conv == '%':
            text += '%'
            continue
        if conv in INT_CONVERSIONS:
            args.append(('int', rng.choice(INT_VALUES)))
            text += rng.choice(SCAN_NUMBERS) + ' '
        elif conv in FLOAT_CONVERSIONS:
            args.append(('int', struct.unpack('<Q', struct.pack('<d', rng.choice(DOUBLE_VALUES)))[0]))
            text += rng.choice(SCAN_FLOATS) + ' '
        elif conv == 'c':
            args.append(('int', rng.choice((0x41, 0, 0xFF))))
            text += 'q'
        elif conv == 's':
            args.append(('text', 0))
            text += rng.choice(SCAN_WORDS) + ' '
        elif conv == 'p':
            args.append(('int', rng.choice(INT_VALUES)))
            text += '0x1234 '
        elif conv == 'n':
            args.append(('out', 0))
        else:
            args.append(('text', 0))
            text += rng.choice(SCAN_WORDS) + ' '
        if '*' not in flags:
            outs += 1
    return fmt, args, text, outs


def real_formats(elf):
    """The game's own strings with a conversion in them."""
    out = []
    for v, o, n in elf.segments:
        for m in re.finditer(rb'[\x20-\x7E\t\n]{2,120}\x00', elf.data[o:o + n]):
            s = m.group(0)[:-1]
            if b'%' in s and re.search(rb'%[-+ #0]*\d*(\.\d*)?[hlLq]*[diouxXcspnfeEgG]', s):
                out.append(s.decode('ascii'))
    return sorted(set(out))


def text_poke(at, text):
    return '%x=%s' % (at, (text.encode('latin-1') + b'\0').hex())


def structured_cases(entry, roles, n, rng, formats, make_base):
    """n cases of `entry` (the target or a caller), the format parameter a built format, its text
    parameter what that format reads, the varargs (registers after the format, a va_list block if
    the entry takes one) values or out buffers. `make_base(entry)` gives base ints and floats."""
    out = []
    params = sorted(roles)
    fmt_at = next((i for i in params if roles[i] == 'format'), None)
    if fmt_at is None:
        return out
    for _ in range(n):
        ints, floats, pokes, sp = make_base(entry)
        arena = sp + ARENA_SKIP
        scanning = rng.random() < 0.5
        fmt, args, text, outs = format_case(rng, formats, scanning)
        pokes = list(pokes) + [text_poke(arena + FORMAT_AT, fmt), text_poke(arena + TEXT_AT, text if scanning else rng.choice(TEXTS))]
        ints = list(ints)
        ints[fmt_at] = arena + FORMAT_AT
        for i in params:
            if roles[i] == 'text':
                ints[i] = arena + TEXT_AT
        slots = []
        for k, (kind, value) in enumerate(args[:12]):
            out_at = arena + OUT_AT + (k % 16) * OUT_SLOT
            if scanning or kind == 'out':
                slots.append(out_at)
            elif kind == 'text':
                slots.append(arena + TEXT_AT if rng.random() < 0.9 else 0)
            else:
                slots.append(value)
        # (Registers after the format, in case the entry is variadic; and a block of 8-byte
        # slots, the EE's va_list, for a parameter after the format that points at a zero block.)
        for k, v in enumerate(slots):
            if fmt_at + 1 + k < 8:
                ints[fmt_at + 1 + k] = v
        pokes.append('%x=%s' % (arena + VA_AT, b''.join(struct.pack('<Q', v & 0xFFFFFFFFFFFFFFFF) for v in slots + [0]).hex()))
        out.append((ints, floats, pokes, sp, fmt, slots))
    return out
