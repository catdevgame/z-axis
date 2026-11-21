#!/usr/bin/env python3
"""Build-time step of the held-register table (src/game/game.cmake; docs/READABLE.md
"Writing a readable function", the callee-saved registers at a call):

The original keeps values in callee-saved registers (s0-s7, fp, f20-f31) across its
calls, and each callee's prologue saves them into its frame. A readable function keeps
those values in C++ locals, so its callees would save other words (dead stack that the
game later copies into records). This table gives the runtime, per function and call
site of the original, the value each of those registers holds at the jal, as an
expression over what the runtime has at the call (ee/held.h): constants, the function's
entry registers and frame, the call's arguments and target, the last call's results,
what it gave the last call, and memory loads with no store or call since, combined by
the original's own ALU and FPU operations.

The values come from a dataflow over the generated code's instruction comments (the
blocks of fncheck_traced_gen.py): value numbering, a phi where the paths into a block
disagree (registers equal on every path share one), the state split by the call made
last (the runtime knows which of the original's call sites that was, so a value that
differs only by the call before it is still known). A value that depends on how often a
loop with no call in it ran, or on an operation not modelled, is not expressible: the
runtime gives that register its entry value, as before, and readable_held_unknown.tsv
lists each one. A site's cases (one per call made before it) must agree on a value, or
the call's arguments must tell the cases apart, else the value is dropped; a case that
carries what an earlier site set is followed back to that site's value for the test
(BMX engineMain's s4). The FPU's condition is a register of the state (c.eq.s, c.lt.s,
c.le.s; bc1t/bc1f select on it), and an instruction the disassembly left as `.word` is
read from its comment (cvt.w.s).

    readable_held_gen.py --scan GAME=DIR [--scan ...] [--list traced_blocks.list] --out OUT.cpp
                         [--unknown OUT.tsv] [--jobs N]
    readable_held_gen.py --from DIR --out OUT.cpp
    readable_held_gen.py --scan GAME=DIR [--scan ...] [--list ...] --write DIR | --check DIR [--unknown OUT.tsv]
    readable_held_gen.py --self-test

The table as committed data (runtime/tools/held/<game>.txt.xz: each function's rows as the scan makes them, one
line each, xz): --write puts the scanned games' there, --check exits 1 naming the games whose committed data is not
what the scan makes, and --from makes the table's sources from the committed data alone, no generated code read (a
build with no game code, the app's and a source build's: game.cmake). The sources made --from are byte-identical to
a scan's of the same games. Proof scaffolding (SCAFFOLDING.md): it shrinks as the readable functions stop needing
the original's held values.
"""
import argparse
import bisect
import collections
import heapq
import multiprocessing
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fncheck_traced_gen as g  # noqa: E402
import readable_held_code as code  # noqa: E402

GPR = g.GPR_NUM
SAVED = [16, 17, 18, 19, 20, 21, 22, 23, 30] + [32 + f for f in range(20, 32)]
SAVED_SET = set(SAVED)
ARGS_INT = list(range(4, 12))
ARGS_FLT = [32 + 12 + i for i in range(8)]
# entry values the runtime has: the arguments (snapshot), the callee-saved (its entry model), sp, gp, ra
ENTRY_OK = set(ARGS_INT) | set(ARGS_FLT) | SAVED_SET | {28, 29, 31}
CALLER_SAVED = [r for r in range(1, 26) if r not in range(16, 24)] + [31] + [32 + f for f in range(0, 20)]
MASK64 = (1 << 64) - 1
LIMIT = int(os.environ.get('HELD_LIMIT', '60'))
ENTRY = 'E'

ALU3 = {'addu', 'daddu', 'subu', 'dsubu', 'and', 'or', 'xor', 'nor', 'slt', 'sltu', 'sllv', 'srlv', 'srav',
        'dsllv', 'dsrlv', 'dsrav', 'movn', 'movz',
        'add.s', 'sub.s', 'mul.s', 'div.s', 'max.s', 'min.s'}
ALU2I = {'addiu', 'daddiu', 'andi', 'ori', 'xori', 'slti', 'sltiu', 'sll', 'srl', 'sra', 'dsll', 'dsrl', 'dsra',
         'dsll32', 'dsrl32', 'dsra32'}
ALU2 = {'mov.s', 'neg.s', 'abs.s', 'sqrt.s', 'cvt.s.w', 'cvt.w.s', 'mtc1', 'mfc1'}


def sx32(v):
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v


def reg_of(name):
    if name in GPR:
        return GPR[name]
    if name.startswith('f') and name[1:].isdigit():
        return 32 + int(name[1:])
    return None


def walk(v):
    yield v
    if isinstance(v, tuple):
        for x in v:
            if isinstance(x, tuple):
                yield from walk(x)


def has(v, pred):
    return any(pred(x) for x in walk(v))


def likely_branch(w):
    op = w >> 26
    if op in (20, 21, 22, 23):
        return True
    if op == 1 and ((w >> 16) & 31) in (2, 3, 18, 19):
        return True
    if op == 17 and ((w >> 21) & 31) == 8 and (w >> 17) & 1:
        return True
    return False


class Fn:
    def __init__(self, text, start, end, insns):
        self.text, self.start, self.end, self.insns = text, start, end, insns
        self.lead = g.leaders(start, end, insns)
        self.lead_set = set(self.lead)
        # The frame's address goes somewhere (a register other than sp gets sp + k): a store
        # through sp may then be what a load through another register reads.
        self.escapes = False
        for a, (w, mn, ops) in insns.items():
            rs = g.REG.findall(ops)
            if mn in g.LOADS or mn in g.STORES:
                continue
            if len(rs) >= 2 and rs[0] not in ('sp',) and 'sp' in rs[1:]:
                self.escapes = True


def outside_frame(base, origin=()):
    """A load base that cannot point into this function's own frame: a constant (a global),
    gp-relative, or worked out from the entry registers alone (the frame is new at entry), or
    from saved registers that held such a value before the last call (origin, ORIGIN)."""
    for x in walk(base):
        if isinstance(x, tuple) and x[:1] == ('prev',) and x[1] in origin:
            continue
        if isinstance(x, tuple) and x and x[0] in ('prev', 'res', 'load', 'rload', 'phi', 'clob', 'stale', 'op',
                                                    'callarg'):
            return False
        if x == ('entry', 29):
            return False
    return True


def overlaps(o1, n1, o2, n2):
    return o1 < o2 + n2 and o2 < o1 + n1


# ---- path facts: the branches a path took, for the phis one branch decides --------------
#
# A state keeps, under FACTS, the conditions of the branches the path into it took since its last
# call: (kind, condition, taken), kind 'nz' (taken when the condition is not 0) or 'z'. Where paths
# meet and a register differs, a condition every incoming path has, taken on some and not on others,
# whose outcome alone decides the value, makes it a select (movn/movz of the two values on it), which
# the runtime works out as any other value: DM2's showGameScreen keeps its screen (a0, or the last
# one for 0x1D) in s3 and s3 << 2 in s4 across its calls; as a phi it was not known, and the callee got
# the entry s4 (another value since the table gives runScript's).
FACTS = -1
# Whether the path has made a store that can reach memory outside the frame, or a call, since the
# entry: until then a load reads the memory as it was at the entry, an "entry load" ('eload'), which
# the runtime reads when the function is entered (ee/held.h), so no later store makes it stale.
DIRTY = -2
ENTRY_LOADS = 8  # (ee::held::kEntryLoads)
# Since the last call returned, whether the path has made no store that can reach memory outside the frame: until
# then a load reads memory as it was at that return, a "return load" ('rload', ..., site), which the runtime reads
# when that call returns (ee/held.h), so no later store makes it stale (BMX saveObjectState: s0 = *a1 after its
# first call, then a store through s0). The key holds the call's site.
RCLEAN = -4
RETURN_LOADS = 8  # (ee::held::kReturnLoads, per site)
# The loads of the clean stretch, {load: its return load}: a load stays a plain load (read at the call, in a replay
# too) until a store may change what it read; then it becomes its return load rather than stale.
RELIG = -6
FACT_LIMIT = 12
# The words the function stored in its own frame, {(offset from the entry sp, width): value}: a load of one gives
# the value stored, so a spill read back is the value it spilled (BMX timerDisplayMessage's message argument, kept
# at 0x54(sp) across its calls; forEachCellOnSegment's cell bounds). Across a call only the values over the entry
# alone stay (what the call got is the next call's 'prev'), and none once the frame's address has gone to a call
# or into memory (ESCAPED): a callee may write it then.
FRAME = -8
ESCAPED = -9
FRAME_PAIRS = {'sw': ('lw',), 'sd': ('ld',), 'swc1': ('lwc1',)}
FRAME_REGS = -10  # (the saved registers that held a frame address at the last call: their 'prev' points into it)
# The frame loads kept as loads whose word holds a value over entry loads, {load: that value} (step): a program (loop or
# path) reads no frame word, so it takes the value (Fn.frame_vals, those every state agrees on).
FRAME_VALS = -11


def points_to_frame(v, regs):
    """Whether value v may be an address in the function's frame."""
    fr = regs.get(FRAME_REGS, ())

    def at(x):
        if not isinstance(x, tuple) or not x:
            return False
        if x == ('entry', 29) or (x[0] == 'prev' and x[1] in fr):
            return True
        if x[0] in ('load', 'eload', 'rload', 'stale'):
            return False  # (a word read from the frame is not its address)
        return any(at(y) for y in x[1:])
    return at(v)
# The multiply/divide unit's results, as registers of the state: LO, HI (pipeline 0) and LO1, HI1 (pipeline 1,
# the EE's mult1/div1). Caller-saved: a call leaves them not known.
LO, HI, LO1, HI1 = 64, 65, 66, 67
# The FPU's condition (FCR31's C bit) as a register of the state: c.eq.s, c.lt.s and c.le.s set it (1 or 0 over the
# two floats' bits, c.f.s 0), bc1t/bc1f branch on it; so a select one of those branches makes is a value, as a
# GPR branch's is (DM2's overlayRulerFrame: a count or the count + 1 by c.eq.s). Caller-saved: a call leaves it not
# known.
FCC = 68
FPU_COMPARES = {'c.eq.s', 'c.lt.s', 'c.le.s'}
# The disassembly writes some instructions as `.word 0x4600xxxx # cvt.w.s $f0, $f1 ...`: the comment names them.
WORD_INSN = re.compile(r'0x[0-9a-fA-F]+\s+#\s*([a-z][a-z0-9.]*)\s+([^#]*)')
MULDIV = {0x18: 'mult', 0x19: 'multu', 0x1A: 'div', 0x1B: 'divu'}  # (SPECIAL; MMI's mult1 .. divu1 the same functs)
# The saved registers whose value before the last call cannot point into this function's frame (constants, the
# entry's registers but sp): after the call they hold it again (the callee restores them), so a store to the frame
# does not change what a load through them read (DM2's buildRigid loads through its a0, kept in s4 across calls).
ORIGIN = -3
SELECT_LIMIT = 48  # nodes in a select's value (beyond: a phi)
SELECT_DEPTH = 3  # selects inside a select's arm (the paths on one side of a branch, again split by a branch)


def facts_of(regs):
    return regs.get(FACTS, ())


def drop_facts(regs, dead):
    f = regs.get(FACTS)
    if f:
        kept = tuple(x for x in f if not has(x[1], dead))
        if kept != f:
            regs[FACTS] = kept


def branch_condition(w, regs):
    """(kind, condition) of a conditional branch from its operands' values, or None (an FPU branch)."""
    op, rs, rt = w >> 26, (w >> 21) & 31, (w >> 16) & 31

    def val(r):
        return ('const', 0) if r == 0 else regs.get(r, ('entry', r))
    a, b = val(rs), val(rt)
    if op in (4, 5, 20, 21):
        c = a if rt == 0 else b if rs == 0 else ('alu', 'xor', a, b, None, ('cond',))
        return ('z' if op in (4, 20) else 'nz', c)
    if op in (6, 7, 22, 23):  # blez, bgtz: 0 < rs
        return ('z' if op in (6, 22) else 'nz', ('alu', 'slt', ('const', 0), a, None, ('cond',)))
    if op == 1 and rt in (0, 1, 2, 3):  # bltz, bgez: rs < 0
        return ('nz' if rt in (0, 2) else 'z', ('alu', 'slt', a, ('const', 0), None, ('cond',)))
    if op == 17 and rs == 8 and rt in (0, 1, 2, 3):  # bc1f, bc1t, bc1fl, bc1tl: the FPU's condition
        c = regs.get(FCC)
        if c is None or c == ('entry', FCC):
            return None
        return ('nz' if rt in (1, 3) else 'z', c)
    return None


def add_fact(regs, cond, taken):
    if cond is None:
        return
    f = tuple(x for x in facts_of(regs) if x[1] != cond[1]) + ((cond[0], cond[1], taken),)
    regs[FACTS] = f[-FACT_LIMIT:]


def size_of(v):
    return sum(1 for x in walk(v) if isinstance(x, tuple))


def entry_only(v):
    """A value the runtime has at the function's entry: constants, entry registers, entry loads."""
    for x in walk(v):
        if isinstance(x, tuple) and x and x[0] not in ('const', 'entry', 'alu', 'eload'):
            return False
        if isinstance(x, tuple) and x[:1] == ('entry',) and x[1] not in ENTRY_OK:
            return False
        if isinstance(x, tuple) and x[:1] == ('alu',) and x[1] not in ALU_IDS:
            return False
    return True


TAGS = ('select', 'cond', 'loopcond')


def entry_value(v):
    """entry_only, with the selects the branches make over the entry (their tags are no values)."""
    for x in walk(v):
        if isinstance(x, tuple) and x and x[0] in TAGS:
            continue
        if isinstance(x, tuple) and x and x[0] not in ('const', 'entry', 'alu', 'eload'):
            return False
        if isinstance(x, tuple) and x[:1] == ('entry',) and x[1] not in ENTRY_OK:
            return False
        if isinstance(x, tuple) and x[:1] == ('alu',) and x[1] not in ALU_IDS:
            return False
    return True


def switch_cases(state):
    """{jump register's value: case address} of the jump-table cases a path took (its facts)."""
    out = {}
    for kind, cond, taken in facts_of(state):
        if kind == 'z' and taken and cond[:2] == ('alu', 'xor') and cond[3][:1] == ('const',) and cond[5] == ('cond',):
            out.setdefault(cond[2], set()).add(cond[3][1])
    return out


def switch_select(b, incoming, vals):
    """The select a jump table makes: every incoming path went through the same switch (the same jump
    register's value) to one case each, and the cases give the values: movz chains over that register,
    the last value the rest's (only these paths come in)."""
    per = [switch_cases(s) for s in incoming]
    keys = set(per[0])
    for p in per[1:]:
        keys &= set(p)
    for j in sorted(keys, key=repr):
        if any(len(p[j]) != 1 for p in per):
            continue
        by_value = collections.OrderedDict()
        for p, v in zip(per, vals):
            by_value.setdefault(v, set()).add(next(iter(p[j])))
        if len(by_value) < 2:
            continue
        # (a case that came in with two values: no select)
        seen = {}
        if any(seen.setdefault(c, v) != v for v, cs in by_value.items() for c in cs):
            continue
        items = list(by_value.items())
        out = items[-1][0]
        for v, cases in reversed(items[:-1]):
            for c in sorted(cases):
                out = ('alu', 'movz', v, ('alu', 'xor', j, ('const', c), None, ('cond',)), out, ('select', b))
        if size_of(out) <= SELECT_LIMIT:
            return out
    return None


def equal_of(incoming, vals):
    """A value every incoming path's branches prove the register equal to (a loop left when s0 == s3: s0 is
    s3's value there), or None. Only a value with no phi in it."""
    common = None
    for s, v in zip(incoming, vals):
        same = {v}
        for kind, cond, taken in facts_of(s):
            if not ((kind == 'z' and taken) or (kind == 'nz' and not taken)):
                continue
            if cond[:2] == ('alu', 'xor') and cond[5:] == (('cond',),):
                if cond[2] == v:
                    same.add(cond[3])
                elif cond[3] == v:
                    same.add(cond[2])
            elif cond == v:
                same.add(('const', 0))
        common = same if common is None else common & same
        if not common:
            return None
    for e in sorted(common, key=repr):
        if not has(e, lambda y: isinstance(y, tuple) and y[:1] == ('phi',)):
            return e
    return None


def select_of(b, incoming, k, vals, depth=0):
    """The select the branches make of register k's values (one per incoming state), or None. A branch whose
    paths on one side still differ selects again among those paths, by a branch they all went through (DM2
    overlayRulerShow's a1: 0 when s2 is 0; else 1 or 0 by a bc1fl whose delay slot sets it, two paths of one
    block)."""
    if any(has(v, lambda y: isinstance(y, tuple) and y[:1] == ('phi',)) for v in vals):
        return None
    common = None
    for s in incoming:
        conds = {x[1]: x for x in facts_of(s)}
        common = conds if common is None else {c: x for c, x in common.items() if c in conds}
        if common is None:
            return None
    v = switch_select(b, incoming, vals)
    if v is not None:
        return v
    for cond, (kind, _, _) in sorted((common or {}).items(), key=lambda kv: repr(kv[0])):
        if any({x[1]: x for x in facts_of(s)}[cond][0] != kind for s in incoming):
            continue
        took = [{x[1]: x for x in facts_of(s)}[cond][2] for s in incoming]
        if all(took) or not any(took):
            continue

        def arm(side):
            ins = [s for s, t in zip(incoming, took) if t == side]
            vs = [v for v, t in zip(vals, took) if t == side]
            if len(set(vs)) == 1:
                return vs[0]
            return select_of(b, ins, k, vs, depth + 1) if depth < SELECT_DEPTH else None
        vt, vf = arm(True), arm(False)
        if vt is None or vf is None:
            continue
        v = ('alu', 'movn' if kind == 'nz' else 'movz', vt, cond, vf, ('select', b))
        if size_of(v) <= SELECT_LIMIT:
            return v
    return None


def plain_address(base):
    """(the base's constant, or None, the base without it): a constant address, or base + offset."""
    if base[0] == 'const':
        return base[1], None
    return None, base


def stale(a, k, old):
    """A register whose value the runtime can no longer work out (a store or a later pass of the same
    instruction changed what it was made of): ('stale', where, register, 32-bit) - whether it was a
    32-bit value stays known (sign_extended)."""
    return ('stale', a, k, sign_extended(old))


def kill_loads(regs, fn, store, a):
    """A store: the loads it may change are stale. store: (base value, offset, bytes, via sp). A load is
    not changed by a store whose bytes it provably does not share: both at constant addresses, or the
    same base value (the same expression in this state: the same number) at offsets that do not meet.
    (Not for the forms that mask the address: lq/sq, the unaligned pairs.)"""
    base, off, n, via_sp = store
    origin = regs.get(ORIGIN, ())

    def apart(y):
        if base is None or y[1] not in EXACT_WIDTH or n == 0:
            return False
        if base[0] == 'const' and y[2][0] == 'const':
            return not overlaps((y[2][1] + y[3]) & 0xFFFFFFFF, g.LOADS[y[1]], (base[1] + off) & 0xFFFFFFFF, n)
        return y[2] == base and not overlaps(y[3], g.LOADS[y[1]], off, n)

    def dead(y):
        if not (isinstance(y, tuple) and y[0] == 'load'):
            return False
        if y[2] == ('entry', 29) or (y[2][0] == 'alu' and y[2][1] == 'daddiu' and y[2][2] == ('entry', 29)):
            lo = y[3] + (y[2][4] if y[2][0] == 'alu' else 0)
            if via_sp:
                return overlaps(lo, g.LOADS[y[1]], off, n)
            return fn.escapes and not apart(y)
        if via_sp:
            return fn.escapes and not outside_frame(y[2], origin)
        return not apart(y)
    elig = regs.get(RELIG, {})

    def swap(v):
        if isinstance(v, tuple) and v[:1] == ('load',) and dead(v):
            return elig.get(v)
        if isinstance(v, tuple) and v[:1] in (('alu',), ('load',), ('eload',), ('rload',)):
            out = []
            for x in v:
                if isinstance(x, tuple):
                    y = swap(x)
                    if y is None:
                        return None
                    out.append(y)
                else:
                    out.append(x)
            return tuple(out)
        return v
    for k, x in list(regs.items()):
        if k >= 0 and has(x, dead):
            y = swap(x) if elig else None
            regs[k] = y if y is not None and not has(y, dead) else stale(a, k, x)
    drop_facts(regs, dead)
    drop_frame(regs, dead)


def drop_frame(regs, dead):
    """The frame words whose stored value read what is no longer there (or was made again) forgotten."""
    fr = regs.get(FRAME)
    if fr and any(has(v[1], dead) for v in fr.values()):
        regs[FRAME] = {k: v for k, v in fr.items() if not has(v[1], dead)}


EXACT_WIDTH = {'lb', 'lbu', 'lh', 'lhu', 'lw', 'lwu', 'lwc1', 'ld', 'ldc1'}
EXACT_STORES = {'sb', 'sh', 'sw', 'swc1', 'sd', 'sdc1'}


def muldiv(regs, a, w, val, define):
    """The multiply/divide unit (mult, multu, div, divu and pipeline 1's mult1 .. divu1; mflo, mfhi, mtlo,
    mthi and theirs): LO and HI as the original's operation on its operands (ee/held.cpp works them out as
    the recompiler's code does). Another instruction of the unit (madd, the MMI's) leaves them not known.
    Returns whether w was one of the unit's."""
    op, funct = w >> 26, w & 0x3F
    rs, rt, rd = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
    if op not in (0, 0x1C):
        return False
    lo, hi = (LO, HI) if op == 0 else (LO1, HI1)

    def reg(r):
        return ('const', 0) if r == 0 else regs.get(r, ('entry', r))
    if funct in MULDIV:
        name = MULDIV[funct]
        x, y = reg(rs), reg(rt)
        # (one define: it makes what the last pass of this instruction gave stale, then the other two are set)
        define(lo, ('alu', name + '.lo', x, y, None, a))
        regs[hi] = ('alu', name + '.hi', x, y, None, a)
        if name.startswith('mult') and rd:
            regs[rd] = regs[lo]
        return True
    if funct in (0x10, 0x12):  # mfhi, mflo (mfhi1, mflo1)
        define(rd, regs.get(hi if funct == 0x10 else lo, ('op', 'mf', a)))
        return True
    if funct in (0x11, 0x13):  # mthi, mtlo (mthi1, mtlo1)
        define(hi if funct == 0x11 else lo, reg(rs))
        return True
    if op == 0x1C:
        # (madd and the MMI's other operations: what they leave in LO, HI, LO1, HI1 is not modelled; their
        # register results as reg_access names them, after this)
        for k in (LO, HI, LO1, HI1):
            define(k, ('op', 'mmi', a))
    return False


def dirty(regs):
    """A store that can reach memory outside the frame (or a syscall): no entry or return load after it."""
    regs[DIRTY] = True
    regs.pop(RCLEAN, None)


def return_only(v, site):
    """A value the runtime has when the call at `site` returns: constants, the entry's registers (but sp),
    what that call got and gave back, entry loads and that call's return loads, combined by the ALU."""
    for x in walk(v):
        if not isinstance(x, tuple) or not x:
            continue
        if x[0] in ('const', 'prev', 'res', 'eload'):
            continue
        if x[0] == 'entry' and x[1] in ENTRY_OK and x[1] != 29:
            continue
        if x[0] == 'alu' and x[1] in ALU_IDS:
            continue
        if x[0] == 'rload' and x[4] == site:
            continue
        if x[0] in ('cond', 'select'):
            continue
        return False
    return True


def step(regs, a, w, mn, ops, fn):
    if mn == '.word':
        m = WORD_INSN.match(ops)
        if m:
            step(regs, a, w, m.group(1), m.group(2).strip(), fn)
            return
    rs = g.REG.findall(ops)

    def val(n):
        r = reg_of(n)
        if r == 0:
            return ('const', 0)
        return regs.get(r, ('entry', r))

    def define(r, v):
        if r is None or r == 0:
            return
        def redone(y):
            return isinstance(y, tuple) and y[0] in ('op', 'load', 'alu') and y[-1] == a
        for k, x in list(regs.items()):
            if k != r and k >= 0 and has(x, redone):
                regs[k] = stale(a, k, x)
        drop_facts(regs, redone)
        drop_frame(regs, redone)
        regs[r] = v

    if mn in g.STORES:
        m = g.MEM.match(ops)
        stored = val(m.group(1)) if m else None
        if stored is not None and points_to_frame(stored, regs) and not (m and m.group(3) == 'sp'):
            regs[ESCAPED] = True  # (the frame's address in memory)
        if m and m.group(3) == 'sp':
            sp = regs.get(29, ('entry', 29))
            spoff = sp[4] if sp[0] == 'alu' and sp[1] == 'daddiu' and sp[2] == ('entry', 29) else 0
            at = spoff + g.parse_int(m.group(2))
            kill_loads(regs, fn, (None, at, g.STORES[mn], True), a)
            if at + g.STORES[mn] > 0 or not (sp[0] == 'entry' or spoff):
                dirty(regs)  # (above the entry sp: the caller's)
                regs.pop(FRAME, None)
            else:
                lo, width = (at, g.STORES[mn]) if mn in EXACT_STORES else (at - 15, 32)  # (unaligned, sq: around it)
                frame = {k: v for k, v in regs.get(FRAME, {}).items() if not overlaps(k[0], k[1], lo, width)}
                if mn in FRAME_PAIRS and stored is not None and not has(stored, lambda y: isinstance(y, tuple) and
                                                                        y[:1] in (('clob',), ('stale',), ('op',))):
                    frame[(at, g.STORES[mn])] = (mn, stored)
                regs[FRAME] = frame
        elif m and mn in EXACT_STORES:
            kill_loads(regs, fn, (val(m.group(3)), g.parse_int(m.group(2)), g.STORES[mn], False), a)
            dirty(regs)
            if regs.get(ESCAPED) or points_to_frame(val(m.group(3)), regs):
                regs.pop(FRAME, None)
        else:
            kill_loads(regs, fn, (None, 0, 0, False), a)
            dirty(regs)
            regs.pop(FRAME, None)
        return
    if mn in g.LOADS and mn not in ('lwl', 'lwr', 'ldl', 'ldr'):
        m = g.MEM.match(ops)
        word = None
        if m and m.group(3) == 'sp' and FRAME in regs:
            sp = regs.get(29, ('entry', 29))
            spoff = sp[4] if sp[0] == 'alu' and sp[1] == 'daddiu' and sp[2] == ('entry', 29) else 0
            at = spoff + g.parse_int(m.group(2))
            hit = regs[FRAME].get((at, g.LOADS[mn])) if (sp[0] == 'entry' or spoff) else None
            if hit is not None and mn in FRAME_PAIRS.get(hit[0], ()):
                v = hit[1]
                if mn == 'lw' and not sign_extended(v):
                    v = ('alu', 'addu', v, ('const', 0), None, None)  # (the low word, sign-extended: one value wherever read)
                if not has(v, lambda y: isinstance(y, tuple) and y[:1] in (('eload',), ('rload',))):
                    define(reg_of(m.group(1)), v)  # (the word this function stored there)
                    return
                # (a word over an entry load stays the frame load, which a replay reads inside the readable's
                # ee::Local, as it reads no entry load: given as the stored value, DM2 readZfileLibraryAsset's s3 at
                # its 0x1b3754 call, over the word it stores at sp+0, was not known in a replay and 69 held words
                # differed. Programs, which read no frame word, take the stored value: FRAME_VALS)
                word = v if entry_value(v) else None
        if m:
            base = val(m.group(3))
            load = ('load', mn, base, g.parse_int(m.group(2)), a)
            if word is not None:
                regs[FRAME_VALS] = dict(regs.get(FRAME_VALS, {}))
                regs[FRAME_VALS][load] = word
            if not regs.get(DIRTY) and mn in LOAD_KINDS and entry_only(base) and not in_own_frame(load):
                load = ('eload', mn, base, g.parse_int(m.group(2)))
            elif RCLEAN in regs and mn in LOAD_KINDS and return_only(base, regs[RCLEAN]) and not in_own_frame(load):
                regs[RELIG] = dict(regs.get(RELIG, {}))
                regs[RELIG][load] = ('rload', mn, base, g.parse_int(m.group(2)), regs[RCLEAN])
            define(reg_of(m.group(1)), load)
            return
    if mn == 'lui' and len(rs) == 1:
        define(reg_of(rs[0]), ('const', sx32((w & 0xFFFF) << 16)))
        return
    if mn in ('addu', 'daddu', 'or') and len(rs) == 3 and 'zero' in rs[1:]:
        other = rs[2] if rs[1] == 'zero' else rs[1]
        v = val(other)
        if mn == 'addu' and v[0] != 'const':
            v = ('alu', 'addu', v, ('const', 0), None, a)  # (a 32-bit move sign-extends)
        define(reg_of(rs[0]), v)
        return
    if mn in ('addiu', 'daddiu') and len(rs) == 2:
        x = val(rs[1])
        imm = g.s16(w & 0xFFFF)
        if x[0] == 'const':
            v = x[1] + imm
            define(reg_of(rs[0]), ('const', sx32(v) if mn == 'addiu' else ((v + (1 << 63)) & MASK64) - (1 << 63)))
        elif mn == 'daddiu' or (mn == 'addiu' and rs[1] == 'sp'):
            define(reg_of(rs[0]), ('alu', 'daddiu', x, None, imm, None))
        else:
            define(reg_of(rs[0]), ('alu', mn, x, None, imm, a))
        return
    if mn in ALU2I and len(rs) == 2:
        x = val(rs[1])
        imm = (w & 0xFFFF) if mn in ('andi', 'ori', 'xori') else g.s16(w & 0xFFFF) if mn in ('slti', 'sltiu') else (w >> 6) & 31
        if x[0] == 'const' and mn in ('ori', 'andi', 'xori'):
            v = x[1] & MASK64
            v = v | imm if mn == 'ori' else v ^ imm if mn == 'xori' else v & imm
            define(reg_of(rs[0]), ('const', v - (1 << 64) if v >> 63 else v))
            return
        define(reg_of(rs[0]), ('alu', mn, x, None, imm, a))
        return
    if mn in ALU3 and len(rs) == 3:
        if mn in ('movn', 'movz'):
            define(reg_of(rs[0]), ('alu', mn, val(rs[1]), val(rs[2]), val(rs[0]), a))
            return
        define(reg_of(rs[0]), ('alu', mn, val(rs[1]), val(rs[2]), None, a))
        return
    if mn in ALU2 and len(rs) == 2:
        if mn == 'mtc1':
            x = val(rs[0])
            define(reg_of(rs[1]), ('const', x[1] & 0xFFFFFFFF) if x[0] == 'const' else ('alu', 'mtc1', x, None, None, a))
            return
        if mn == 'mov.s':
            define(reg_of(rs[0]), val(rs[1]))
            return
        define(reg_of(rs[0]), ('alu', mn, val(rs[1]), None, None, a))
        return
    if mn in FPU_COMPARES and len(rs) == 2:
        regs[FCC] = ('alu', mn, val(rs[0]), val(rs[1]), None, a)
        return
    if mn == 'c.f.s':
        regs[FCC] = ('const', 0)
        return
    if mn == 'c1' and (w >> 26) == 0x11 and not rs:
        # A COP1 operation the disassembly leaves undecoded (EE's sqrt.s fd, ft, as "c1 0x504": DM2's
        # matrixToEuler keeps sqrt(x*x + y*y) in f20 across its calls): it names no register, so
        # reg_access sees no write. Its fd is written (but by a compare, which sets only the condition):
        # not a value the table models, so not given.
        if (w >> 21) & 31 in (0x10, 0x14) and (w & 63) < 0x30:
            define(32 + ((w >> 6) & 31), ('op', mn, a))
        return
    if mn == 'syscall':
        # (the kernel's answer: v0, v1)
        define(2, ('op', mn, a))
        define(3, ('op', mn, a))
        dirty(regs)
        return
    if muldiv(regs, a, w, val, define):
        return
    (rg, rf), (wg, wf) = g.reg_access(w, mn, ops)
    for r in range(32):
        if (wg >> r) & 1:
            define(r, ('op', mn, a))
    for f in range(32):
        if (wf >> f) & 1:
            define(32 + f, ('op', mn, a))


def after_call(regs, states=None):
    """The state after a call from the state before it (regs; states: every case's, for ORIGIN)."""
    out = {}
    for r in SAVED:
        out[r] = ('prev', r)
    for r in (28, 29, 30):
        if r in regs:
            out[r] = regs[r]
    out[30] = ('prev', 30)
    out[2] = ('res', 2)
    out[3] = ('res', 3)
    out[32] = ('res', 32)
    for r in CALLER_SAVED + [LO, HI, LO1, HI1]:
        if r not in (2, 3, 32):
            out[r] = ('clob', r)
    origin = frozenset(r for r in SAVED if all(
        outside_frame(s.get(r, ('entry', r)), s.get(ORIGIN, ())) and s.get(r, ('entry', r)) != ('entry', 29)
        for s in (states or [regs])))
    if origin:
        out[ORIGIN] = origin
    # (a fact over the entry and constants holds after the call; one over what the call can change
    # or what the last call got does not)
    kept = tuple(x for x in facts_of(regs) if not has(x[1], lambda y: isinstance(y, tuple) and y[0] in (
        'load', 'rload', 'prev', 'res', 'clob', 'stale', 'op', 'phi', 'callarg')))
    if kept:
        out[FACTS] = kept
    out[DIRTY] = True
    # (a frame word stays across the call when it is over the entry alone and the frame's address went nowhere)
    escaped = any(s.get(ESCAPED) or any(points_to_frame(s.get(r, ('entry', r)), s) for r in ARGS_INT)
                  for s in (states or [regs]))
    fr = frozenset(r for r in SAVED for s in (states or [regs]) if points_to_frame(s.get(r, ('entry', r)), s))
    if fr:
        out[FRAME_REGS] = fr
    if not escaped:
        frames = [s.get(FRAME) for s in (states or [regs])]
        if all(f for f in frames):
            kept = {k: v for k, v in frames[0].items() if entry_value(v[1]) and all(f.get(k) == v for f in frames[1:])}
            if kept:
                out[FRAME] = kept
    else:
        out[ESCAPED] = True
    return out


def merge_states(b, incoming, visits, sticky):
    keys = set()
    for s in incoming:
        keys |= set(s)
    keys.discard(FACTS)
    keys.discard(DIRTY)
    keys.discard(ORIGIN)
    keys.discard(RCLEAN)
    keys.discard(RELIG)
    keys.discard(FRAME)
    keys.discard(ESCAPED)
    keys.discard(FRAME_REGS)
    keys.discard(FRAME_VALS)
    def isphi(v):
        return isinstance(v, tuple) and v[:2] == ('phi', b)
    vec = {k: tuple(s.get(k, ('entry', k)) for s in incoming) for k in keys}
    norm = {k: tuple('SELF' if isphi(v) else v for v in vec[k]) for k in keys}
    classes = collections.defaultdict(list)
    for k in sorted(keys):
        classes[norm[k]].append(k)
    groups = list(classes.values())
    while True:
        where = {k: i for i, grp in enumerate(groups) for k in grp}
        new_groups = []
        for i, grp in enumerate(groups):
            split = collections.defaultdict(list)
            for k in grp:
                split[tuple(where.get(v[2], -1) if isphi(v) else None for v in vec[k])].append(k)
            for sig, part in split.items():
                if any(s is not None and s != i for s in sig):
                    new_groups += [[k] for k in part]
                else:
                    new_groups.append(part)
        if len(new_groups) == len(groups):
            break
        groups = new_groups
    if visits > LIMIT:
        groups = [[k] for k in sorted(keys)]
    out = {}
    for grp in groups:
        k0 = grp[0]
        vals = [v for v in vec[k0] if not (isphi(v) and v[2] in grp)]
        if vals and all(v == vals[0] for v in vals) and not has(vals[0], isphi) and not (visits > LIMIT and k0 in sticky):
            for k in grp:
                if vals[0] != ('entry', k):
                    out[k] = vals[0]
        else:
            for k in grp:
                v = select_of(b, incoming, k, vec[k]) if visits <= LIMIT and k not in sticky else None
                if v is None and visits <= LIMIT and k not in sticky:
                    v = equal_of(incoming, vec[k])
                if v is not None:
                    out[k] = v
                    continue
                out[k] = ('phi', b, min(grp))
                sticky.add(k)
    # (the facts every path has, with the same outcome)
    common = set(facts_of(incoming[0]))
    for s in incoming[1:]:
        common &= set(facts_of(s))
    if common:
        out[FACTS] = tuple(x for x in facts_of(incoming[0]) if x in common)
    if any(s.get(DIRTY) for s in incoming):
        out[DIRTY] = True
    origin = frozenset.intersection(*(frozenset(s.get(ORIGIN, ())) for s in incoming))
    if origin:
        out[ORIGIN] = origin
    elig = [s.get(RELIG, {}) for s in incoming]
    common = {k: v for k, v in elig[0].items() if all(e.get(k) == v for e in elig[1:])}
    if common:
        out[RELIG] = common
    clean = {s.get(RCLEAN) for s in incoming}
    if len(clean) == 1 and None not in clean:
        out[RCLEAN] = clean.pop()
    # (the frame words every path stored, alike or as the select the branches make)
    frames = [s.get(FRAME) for s in incoming]
    if all(f for f in frames):
        common = {}
        for k, v in frames[0].items():
            vs = [f.get(k) for f in frames]
            if any(x is None or x[0] != v[0] for x in vs):
                continue
            if all(x == v for x in vs):
                common[k] = v
            elif visits <= LIMIT:
                sel = select_of(b, incoming, None, [x[1] for x in vs])
                if sel is not None:
                    common[k] = (v[0], sel)
        if common:
            out[FRAME] = common
    if any(s.get(ESCAPED) for s in incoming):
        out[ESCAPED] = True
    vals = [s.get(FRAME_VALS, {}) for s in incoming]
    common = {k: v for k, v in vals[0].items() if all(x.get(k) == v for x in vals[1:])}
    if common:
        out[FRAME_VALS] = common
    fr = frozenset().union(*(s.get(FRAME_REGS, ()) for s in incoming))
    if fr:
        out[FRAME_REGS] = fr
    return out


def analyse(fn):
    """{site: (callee value, {prev key: regs at the callee's entry})}, prev key the site of
    the call before (ENTRY: none since the entry)."""
    insns = fn.insns
    ends = {}
    for i, b in enumerate(fn.lead):
        ends[b] = fn.lead[i + 1] if i + 1 < len(fn.lead) else fn.end
    sites = {}
    returned = {}  # site -> the facts its call's return kept from before the call (after_call)
    edges = collections.defaultdict(dict)  # block -> {pred: {key: regs}}
    visits = collections.Counter()
    sticky = collections.defaultdict(set)
    work, queued = [fn.start], {fn.start}
    while work:
        b = heapq.heappop(work)
        queued.discard(b)
        visits[b] += 1
        if visits[b] > 400:
            raise RuntimeError('no fixpoint at 0x%x' % b)
        # per key, the merge of the edges that have it
        per_key = collections.defaultdict(list)
        for p in sorted(edges[b]):
            for key, regs in edges[b][p].items():
                per_key[key].append(regs)
        if b == fn.start:
            per_key[ENTRY].append({})
        state = {}
        for key in per_key:
            sk = sticky[(b, key)]
            state[key] = merge_states(b, per_key[key], visits[b], sk)
        succ = []
        a = b
        stop = ends[b]
        while a < stop and a in insns:
            w, mn, ops = insns[a]
            kind, target = g.control(a, w)
            if kind is None:
                for regs in state.values():
                    step(regs, a, w, mn, ops, fn)
                a += 4
                continue
            delay = insns.get(a + 4)
            if kind == 'call':
                rs = g.REG.findall(ops)
                links = w >> 26 == 3 or (w >> 26 == 0 and (w & 0x3F) == 9 and (w >> 11) & 31 == 31)
                callees = {}
                for key, regs in state.items():
                    if w >> 26 == 3:
                        callees[key] = ('const', ((a + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2))
                    else:
                        callees[key] = regs.get(reg_of(rs[-1]), ('entry', reg_of(rs[-1]))) if rs else ('op', 'jalr', a)
                    if delay:
                        step(regs, a + 4, *delay, fn)
                if links:
                    sites[a] = (callees, {k: dict(v) for k, v in state.items()})
                    first = dict(next(iter(state.values()))) if state else {}
                    if state:  # (the facts every state has)
                        common = set.intersection(*(set(facts_of(v)) for v in state.values()))
                        first[FACTS] = tuple(x for x in facts_of(first) if x in common)
                    state = {a: after_call(first, list(state.values()))}
                    state[a][RCLEAN] = a
                    returned[a] = frozenset(facts_of(state[a]))
                a += 8
                continue
            if kind == 'branch':
                likely = likely_branch(w)
                taken = {k: dict(v) for k, v in state.items()}
                conds = {k: (branch_condition(w, v) if not g.always_taken(w) else None) for k, v in state.items()}
                for k, regs in taken.items():
                    add_fact(regs, conds[k], True)
                if delay:
                    for regs in taken.values():
                        step(regs, a + 4, *delay, fn)
                    if not likely:
                        state = {k: dict(v) for k, v in taken.items()}
                for k, regs in state.items():
                    add_fact(regs, conds.get(k), False)
                if target in fn.lead_set:
                    succ.append((target, taken))
                if a + 8 in fn.lead_set and not g.always_taken(w):
                    succ.append((a + 8, state))
            elif kind == 'jump':
                if delay:
                    for regs in state.values():
                        step(regs, a + 4, *delay, fn)
                if fn.start <= target < fn.end and target in fn.lead_set:
                    succ.append((target, state))
            elif kind == 'jr':
                if delay:
                    for regs in state.values():
                        step(regs, a + 4, *delay, fn)
                if w != 0x42000018 and (w >> 21) & 31 != 31:
                    jr = (w >> 21) & 31
                    for c in g.jump_cases(fn.text, a) or []:
                        if c in fn.lead_set:
                            # (each case's path knows the register it jumped through held its address: a phi
                            # the switch decides becomes a select over it, see select_of)
                            to = {k: dict(v) for k, v in state.items()}
                            for regs in to.values():
                                j = regs.get(jr, ('entry', jr))
                                if j[:1] == ('eload',):
                                    # (the jump table is read-only: its word as a plain load at the call, which a
                                    # replay reads too; an entry load it does not)
                                    j = ('load', j[1], j[2], j[3], a)
                                add_fact(regs, ('z', ('alu', 'xor', j, ('const', c), None, ('cond',))), True)
                            succ.append((c, to))
            break
        else:
            if stop in fn.lead_set:
                succ.append((stop, state))
        # (an edge is keyed by its block and its place among the block's successors: a likely branch to its own
        # fall-through address, bnel s0, zero, +8 with the load in its delay slot, is two paths into one block,
        # and keyed by the block alone the second overwrote the first: AI's createNpcBubble took s2 as the
        # entry's where the delay slot had loaded it, and its path facts were the not-taken path's)
        for i, (t, s) in enumerate(succ):
            s = {key: {k: v for k, v in regs.items() if v != ('entry', k)} for key, regs in s.items()}
            if edges[t].get((b, i)) != s:
                edges[t][(b, i)] = s
                if t not in queued:
                    heapq.heappush(work, t)
                    queued.add(t)
    fn.edges = edges  # (the states on each edge at the fixpoint: a loop's entry, back and exit edges, for its program)
    fn.returned = returned  # (the facts each call's return held: a path's facts since the call are the others)
    seen, clash = {}, set()
    for st in [st for by_pred in edges.values() for by_key in by_pred.values() for st in by_key.values()] + \
            [st for _c, states in sites.values() for st in states.values()]:
        for k, v in st.get(FRAME_VALS, {}).items():
            if seen.setdefault(k, v) != v:
                clash.add(k)
    fn.frame_vals = {k: v for k, v in seen.items() if k not in clash}
    return sites


BAD = ('phi', 'clob', 'stale', 'op')


def express(v, regs, callee, args=False):
    """What the runtime evaluates for value v at the call (regs: the state there), or None.
    args: a sub-value equal to the call's argument N (or its target, for a jalr) may be
    that argument; without, only what does not depend on how the readable makes the call."""
    if args and v[0] != 'const':
        for i, r in enumerate(ARGS_INT):
            if regs.get(r, ('entry', r)) == v:
                return ('callarg', i)
        for i, r in enumerate(ARGS_INT):
            e = argument_less(regs.get(r, ('entry', r)), v, i)
            if e is not None:
                return e
        for i, r in enumerate(ARGS_FLT):
            if regs.get(r, ('entry', r)) == v:
                return ('callfarg', i)
        if callee[0] != 'const' and v == callee:
            return ('target',)
    if v[0] in ('const', 'prev', 'res', 'carried', 'loop'):
        return v
    if v[0] == 'entry':
        return v if v[1] in ENTRY_OK else None
    if v[0] == 'load':
        # (a word of the function's own frame is given, and ee reads it only inside one of the readable's
        # ee::Local objects, which sit at the original's offsets and which the body fills as the original does;
        # a spill the readable keeps in C++ is outside them, so not known there: ee/held.h State::localFrom)
        if v[1] not in LOAD_KINDS or reads_through_frame(v):
            return None
        base = express(v[2], regs, callee, args)
        return None if base is None else ('load', v[1], base, v[3])
    if v[0] == 'rload':
        # (read as the call at v[4] returned: its base from what the runtime has then, never this call's arguments)
        base = express(v[2], regs, callee)
        return None if base is None or not return_only(base, v[4]) else ('rload', v[1], base, v[3], v[4])
    if v[0] == 'eload':
        # (read at the entry: its base from the entry alone, never a call's argument)
        base = express(v[2], regs, callee)
        return None if base is None or not entry_only(base) else ('eload', v[1], base, v[3])
    if v[0] == 'alu':
        if v[1] not in ALU_IDS:
            return None
        parts = []
        for x in v[2:5]:
            if isinstance(x, tuple):
                e = express(x, regs, callee, args)
                if e is None:
                    return None
                parts.append(e)
            else:
                parts.append(x)
        tag = v[5] if len(v) > 5 and isinstance(v[5], tuple) and v[5][:1] == ('select',) else None
        return ('alu', v[1], parts[0], parts[1], parts[2]) + ((tag,) if tag else ())
    return None


SX32_OPS = {'addu', 'subu', 'addiu', 'sll', 'srl', 'sra', 'sllv', 'srlv', 'srav', 'slt', 'sltu', 'slti', 'sltiu',
            'mfc1', 'mult.lo', 'mult.hi', 'multu.lo', 'multu.hi', 'div.lo', 'div.hi', 'divu.lo', 'divu.hi'}


def sign_extended(v):
    """Whether value v is a 32-bit value as the EE keeps it (bits 63-32 copies of bit 31): what a 32-bit
    operation or load gives, or a constant."""
    if v[0] == 'const':
        return -(1 << 31) <= v[1] < (1 << 31)
    if v[0] == 'alu':
        return v[1] in SX32_OPS
    if v[0] in ('load', 'eload'):
        return v[1] in ('lb', 'lbu', 'lh', 'lhu', 'lw')
    if v[0] == 'stale':
        return v[3]
    return False


def argument_less(arg, v, i):
    """v from the call's integer argument i when the original made that argument as v plus a constant
    (a0 = s0 + 0x30 with s0 kept across the call: DM2's followCamera): the add undone, which is exact
    (daddiu always; addiu when v is a 32-bit value, which the add's sign extension keeps), else None."""
    if arg[:1] != ('alu',) or arg[2] != v or not isinstance(arg[4], int):
        return None
    if arg[1] == 'daddiu' or (arg[1] == 'addiu' and sign_extended(v)):
        return ('alu', arg[1], ('callarg', i), None, -arg[4])
    return None


def in_own_frame(load):
    """A load from the function's own frame (below its entry sp): what the original spilled there, which a
    readable function keeps in C++ (its frame holds other words). Above the entry sp is the caller's (stack
    arguments): those are as the original's."""
    base, off = load[2], load[3]
    if base == ('entry', 29):
        return off < 0
    if base[0] == 'alu' and base[1] in ('daddiu', 'addiu') and base[2] == ('entry', 29) and isinstance(base[4], int):
        return base[4] + off < 0
    return any(x == ('entry', 29) for x in walk(base))


def reads_through_frame(load):
    """A load at an address read from the function's own frame. A frame word is a value the runtime may read (the
    stack is open in zx-fncheck's RAM), but not one it may follow: a readable function keeps what the original
    spilled there in C++, so the word may be anything (a load at address 1 in DM2's newRider faulted the replay)."""
    return any(isinstance(x, tuple) and x and x[0] in ('load', 'eload') and in_own_frame(x) for x in walk(load[2]))


def best(v, regs, callee):
    e = express(v, regs, callee)
    pre_read = lambda x: isinstance(x, tuple) and x[:1] in (('eload',), ('rload',))
    if e is not None and has(e, pre_read):
        # (an entry or return load a replay does not read: the same value through the call's arguments, when
        # the call passes it or a part of it, is read from the call in a replay too)
        for i, r in enumerate(ARGS_INT):
            if regs.get(r, ('entry', r)) == v:
                return ('callarg', i)
        e2 = express(v, regs, callee, True)
        if e2 is not None and not has(e2, pre_read):
            return e2
    return e or express(v, regs, callee, True)

# ---- the table (ee/held.h numbers) ----------------------------------------------

OP_CONST, OP_ENTRY, OP_PREV, OP_RESULT, OP_ARG, OP_FARG, OP_TARGET, OP_LOAD, OP_ALU, OP_ENTRY_LOAD, OP_RETURN_LOAD, \
    OP_SKIP_IF, OP_SKIP, OP_LOOP, OP_CARRIED, OP_LEAF, OP_LEAF_END = range(1, 18)
LOAD_KINDS = {'lb': 0, 'lbu': 1, 'lh': 2, 'lhu': 3, 'lw': 4, 'lwu': 5, 'ld': 6, 'lwc1': 7, 'lq': 8}
# (id, operands popped)
ALU_LIST = [('addu', 2), ('daddu', 2), ('subu', 2), ('dsubu', 2), ('and', 2), ('or', 2), ('xor', 2), ('nor', 2),
            ('slt', 2), ('sltu', 2), ('sllv', 2), ('srlv', 2), ('srav', 2), ('dsllv', 2), ('dsrlv', 2), ('dsrav', 2),
            ('movn', 3), ('movz', 3),
            ('addiu', 1), ('daddiu', 1), ('andi', 1), ('ori', 1), ('xori', 1), ('slti', 1), ('sltiu', 1),
            ('sll', 1), ('srl', 1), ('sra', 1), ('dsll', 1), ('dsrl', 1), ('dsra', 1), ('dsll32', 1), ('dsrl32', 1),
            ('dsra32', 1),
            ('add.s', 2), ('sub.s', 2), ('mul.s', 2), ('div.s', 2), ('max.s', 2), ('min.s', 2),
            ('neg.s', 1), ('abs.s', 1), ('sqrt.s', 1), ('cvt.s.w', 1), ('cvt.w.s', 1), ('mtc1', 1), ('mfc1', 1),
            ('mult.lo', 2), ('mult.hi', 2), ('multu.lo', 2), ('multu.hi', 2), ('div.lo', 2), ('div.hi', 2),
            ('divu.lo', 2), ('divu.hi', 2),
            ('c.eq.s', 2), ('c.lt.s', 2), ('c.le.s', 2)]
ALU_IDS = {name: i for i, (name, _) in enumerate(ALU_LIST)}
ALU_ARITY = dict(ALU_LIST)
SLOT = {r: (r - 16 if r < 24 else 9 if r == 30 else r - 52 + 11) for r in SAVED}  # ee::savedSlot
CHECKED = 0x80  # (Value::slot: given only when the case's checks ran and matched, ee/held.h kChecked)
RESULT_SUB = {2: 0, 3: 1, 32: 2}
LOST = 0xFFFFFFFF


def fits32(v):
    """A constant as the table keeps it: 32 bits, sign-extended (an FPR's bits are their low word)."""
    return -(1 << 31) <= v < (1 << 32)


class Loads(list):
    """A function's entry loads (the list: each a postfix list ending in its load) and its return loads
    (ret: per call site, the same)."""
    def __init__(self):
        super().__init__()
        self.ret = collections.defaultdict(list)


def postfix(e, out, eloads=None):
    """e's nodes; an entry load is its index in eloads (the function's, each a postfix list
    ending in its load), appended when new; a return load its index in eloads.ret[site]."""
    kind = e[0]
    if kind == 'const':
        if not fits32(e[1]):
            raise ValueError('a 64-bit constant')
        out.append((OP_CONST, 0, e[1]))
    elif kind == 'entry':
        out.append((OP_ENTRY, e[1], 0))
    elif kind == 'prev':
        out.append((OP_PREV, SLOT[e[1]], 0))
    elif kind == 'res':
        out.append((OP_RESULT, RESULT_SUB[e[1]], 0))
    elif kind == 'callarg':
        out.append((OP_ARG, e[1], 0))
    elif kind == 'callfarg':
        out.append((OP_FARG, e[1], 0))
    elif kind == 'target':
        out.append((OP_TARGET, 0, 0))
    elif kind == 'carried':
        out.append((OP_CARRIED, e[1], 0))
    elif kind == 'loop':
        # [the carried registers' initial values] loop(n, program length) then per leaf: leaf(kind, condition
        # length) [condition] leaf-end(0, values length) [values]: see ee/held.h kLoop.
        _, carried, initial, leaves, result = e
        for x in initial:
            postfix(x, out, eloads)
        body = []
        for cond, leaf_kind, vals in leaves:
            c = postfix(cond, [], eloads) if cond is not None else [(OP_CONST, 0, 1)]
            v = []
            for x in (vals if leaf_kind in ('again', 'value') else (result,)):
                postfix(x, v, eloads)
            body += [(OP_LEAF, 0 if leaf_kind == 'again' else 1, len(c))] + c + [(OP_LEAF_END, 0, len(v))] + v
        if len(body) > (PATH_LIMIT if carried[:1] == (PLACE,) else LOOP_LIMIT):
            raise ValueError('a loop program too long')
        out.append((OP_LOOP, len(carried), len(body)))
        out += body
    elif kind == 'load':
        postfix(e[2], out, eloads)
        out.append((OP_LOAD, LOAD_KINDS[e[1]], e[3]))
    elif kind == 'eload':
        if eloads is None:
            raise ValueError('an entry load')
        own = tuple(postfix(e[2], [], eloads) + [(OP_LOAD, LOAD_KINDS[e[1]], e[3])])
        if own not in eloads:
            if len(eloads) >= ENTRY_LOADS:
                raise ValueError('entry loads')
            eloads.append(own)
        out.append((OP_ENTRY_LOAD, eloads.index(own), 0))
    elif kind == 'rload':
        if eloads is None or not hasattr(eloads, 'ret'):
            raise ValueError('a return load')
        own = tuple(postfix(e[2], [], eloads) + [(OP_LOAD, LOAD_KINDS[e[1]], e[3])])
        site = eloads.ret[e[4]]
        if own not in site:
            if len(site) >= RETURN_LOADS:
                raise ValueError('return loads')
            site.append(own)
        out.append((OP_RETURN_LOAD, site.index(own), 0))
    elif kind == 'alu' and e[1] in ('movn', 'movz') and len(e) > 5 and isinstance(e[5], tuple) and e[5][:1] == ('select',):
        # (a select the branches make: worked out lazily, the arm the condition picks only, so a load the
        # original made on the other path is not made: [condition] skip-if [taken arm] skip [other arm])
        postfix(e[3], out, eloads)
        taken = postfix(e[2], [], eloads)
        other = postfix(e[4], [], eloads)
        out.append((OP_SKIP_IF, 0 if e[1] == 'movn' else 1, len(taken) + 1))
        out += taken
        out.append((OP_SKIP, 0, len(other)))
        out += other
    elif kind == 'alu':
        n = ALU_ARITY[e[1]]
        for x in e[2:2 + n]:
            postfix(x, out, eloads)
        imm = e[3] if n == 1 and not isinstance(e[3], tuple) and e[3] is not None else \
            e[4] if n == 1 and not isinstance(e[4], tuple) and e[4] is not None else 0
        out.append((OP_ALU, ALU_IDS[e[1]], imm))
    else:
        raise ValueError(e)
    return out


def unchanged(v, key, r):
    return v == (('entry', r) if key == ENTRY else ('prev', r))


# ---- loop programs ------------------------------------------------------------------------------------------
# A value a loop with no call in it leaves (a list walked until a node passes a test, a count stepped until a
# bound) is a phi at the loop's header: how often the loop ran is not known here. It is known at the call: the
# runtime has the memory and the registers the loop read. So the loop goes into the table as a program (held.h
# kLoop): the header's phi registers are carried values, their initial values are the entry edge's, and each
# iteration takes the first leaf whose path condition (the branch facts since the loop was entered) holds: the
# site's path gives the result, a back edge's path gives the next carried values. The loop must be store-free
# since the last call (RCLEAN, or not DIRTY from the entry), so its loads read at the call what the original read.
LOOP_LIMIT = 400
PATH_LIMIT = 2000  # (a path program's nodes: a leaf per edge into a merge point, each setting every carried value)
CARRIED_LIMIT = 8
PATH_CARRIED = 16  # (ee::held::kCarriedValues)
LOOPCOND = ('loopcond',)


def phi_blocks(v):
    return {x[1] for x in walk(v) if isinstance(x, tuple) and x[:1] == ('phi',)}


def substitute(v, mapping):
    if not isinstance(v, tuple):
        return v
    if v in mapping:
        return mapping[v]
    return tuple(substitute(x, mapping) for x in v)


def fact_value(fact):
    """1 when the fact (kind, condition, taken) holds."""
    kind, cond, taken = fact
    if (kind == 'nz') == bool(taken):
        return ('alu', 'sltu', ('const', 0), cond, None, LOOPCOND)
    return ('alu', 'sltiu', cond, None, 1, LOOPCOND)


def path_condition(facts, before):
    """The facts a path added since the loop was entered, as one value (1 when all hold), or None for none."""
    new = [f for f in facts if f not in before]
    if not new:
        return None
    v = fact_value(new[0])
    for f in new[1:]:
        v = ('alu', 'and', v, fact_value(f), None, LOOPCOND)
    return v


def skips_loop(fn, site, key, b):
    """True when a path from the previous call's return (or the entry) under `key` reaches `site` without passing
    through the loop header b (the loop's entry test can skip it), or when that cannot be told. A loop program
    runs its loop before it gives a value, so it is only for a site every such path reaches through b; else the
    path program, whose leaves carry the entry test (a select of the loop's value and the skip path's has one phi
    block too)."""
    edges = getattr(fn, 'edges', None)
    if not edges:
        return True
    lead = sorted(fn.lead)

    def block_of(a):
        i = bisect.bisect_right(lead, a) - 1
        return lead[i] if i >= 0 else None
    sb = block_of(site)
    start = fn.start if key == ENTRY else block_of(key)
    if sb is None or start is None:
        return True
    if sb == b or start == b:
        return False
    if sb == start and (key == ENTRY or key < site):
        return True  # (the call's own block goes on to the site)
    preds = collections.defaultdict(list)
    for t, by_pred in edges.items():
        for (p, _i), by_key in by_pred.items():
            if key in by_key:
                preds[t].append(p)
    seen, work = {sb}, [sb]
    while work:
        t = work.pop()
        for p in preds[t]:
            if p == start:
                return True
            if p != b and p not in seen:
                seen.add(p)
                work.append(p)
    return False


def loop_program(fn, site, b, key, regs, callee, r, others=(), value=None, others_give=None):
    """('loop', carried, initial, leaves, result) for register r of the site state `regs` (holding ('phi', b, ..)
    tokens) under `key`, the loop headed at block b, or None. others: the states of the other sites the loop
    leaves to under `key`: their path conditions end an iteration too (a back edge is taken only when no site's
    path is), so of two sites' programs only the one the original reached gives a value, and two jalr sites of one
    walk stay apart (DM2 runRenderStage: both fitted, so the call had two places and was given nothing).
    value: the value to work out, if not register r's (a jalr's target). others_give: what the program gives when
    another site's path comes first (a jalr's target check: 0, which no call's target is, so the call is not this
    site's: DM2 runRenderStage's first draw call fitted its jalr site too). None too where a path from the
    previous call reaches the site without entering the loop (skips_loop): the program would run the loop there."""
    edges = getattr(fn, 'edges', None)
    if not edges or b not in edges or skips_loop(fn, site, key, b):
        return None
    if key == ENTRY:
        if regs.get(DIRTY):
            return None
    elif RCLEAN not in regs:
        return None
    entries, backs = [], []
    for p, by_key in sorted(edges[b].items()):
        st = by_key.get(key)
        if st is None:
            continue
        (backs if any(b in phi_blocks(v) for k, v in st.items() if isinstance(k, int) and k >= 0 and v is not None)
         else entries).append(st)
    if not backs or not entries:
        return None
    # The carried registers: the header's phis the value, the site's path and the back edges reach.
    carried = []
    v0 = regs[r] if value is None else value
    sources = [v0] + [f[1] for f in facts_of(regs)]
    changed = True
    while changed:
        changed = False
        for v in sources:
            for x in walk(v):
                if isinstance(x, tuple) and x[:2] == ('phi', b) and x[2] not in carried:
                    carried.append(x[2])
                    changed = True
        if len(carried) > CARRIED_LIMIT:
            return None
        sources = [st.get(c, ('entry', c)) for st in backs for c in carried] + [f[1] for st in backs for f in facts_of(st)]
    mapping = {('phi', b, c): ('carried', i) for i, c in enumerate(carried)}

    fv = getattr(fn, 'frame_vals', {})  # (a program reads no frame word: FRAME_VALS)

    def conv(v):
        if v is None or (phi_blocks(v) - {b}):
            return None
        return express(substitute(substitute(v, fv), mapping), regs, callee)
    initial = []
    for c in carried:
        vals = {st.get(c, ('entry', c)) for st in entries}
        if len(vals) != 1:
            return None
        v = vals.pop()
        if phi_blocks(v):
            return None
        e = express(substitute(v, fv), regs, callee)
        if e is None:
            return None
        initial.append(e)
    before = set(facts_of(entries[0]))
    for st in entries[1:]:
        before &= set(facts_of(st))
    leaves = []
    done = path_condition(facts_of(regs), before)
    if done is not None:
        done = conv(done)
        if done is None:
            return None
    result = conv(v0)
    if result is None:
        return None
    leaves.append((done, 'done', None))
    not_others = None  # (no other site's path: 1 - its condition, each)
    for st in others:
        pc = path_condition(facts_of(st), before)
        if pc is None:
            continue
        if others_give is not None:
            give = conv(pc)
            if give is None:
                return None
            leaves.append((give, 'value', (others_give,)))
        neg = ('alu', 'xori', pc, None, 1, LOOPCOND)
        not_others = neg if not_others is None else ('alu', 'and', not_others, neg, None, LOOPCOND)
    if not_others is not None:
        not_others = conv(not_others)
        if not_others is None:
            return None
    for st in backs:
        cond = path_condition(facts_of(st), before)
        if cond is not None:
            cond = conv(cond)
            if cond is None:
                return None
        if not_others is not None:
            cond = not_others if cond is None else ('alu', 'and', cond, not_others, None, LOOPCOND)
        vals = tuple(conv(st.get(c, ('entry', c))) for c in carried)
        if any(x is None for x in vals):
            return None
        leaves.append((cond, 'again', vals))
    return ('loop', tuple(carried), tuple(initial), tuple(leaves), result)


def loop_others(sites, site, key, b):
    """The states of the other sites the loop headed at b leaves to under `key` (their values or facts hold its
    phis)."""
    out = []
    for other, (_callees, states) in sites.items():
        if other == site or key not in states:
            continue
        st = states[key]
        if any(b in phi_blocks(v) for k, v in st.items() if isinstance(k, int) and k >= 0 and v is not None) or \
                any(b in phi_blocks(f[1]) for f in facts_of(st)):
            out.append(st)
    return out


def lazy_and(a, b):
    """a and b as 1/0 values, b worked out only when a holds (a load b makes may be unreadable otherwise)."""
    if a is None or b is None:
        return b if a is None else a
    return ('alu', 'movn', b, a, ('const', 0), ('select', 'and'))


def path_program(fn, site, key, regs, callee, value, others=(), others_give=None):
    """('loop', ...) for `value` at `site` under `key` as the original's code from the previous call (or the entry)
    to the site, run as a program, or None. For a cycle that merges at several blocks (DM2 overlayTimerMessage's
    minutes loop is entered at two of its blocks; forEachCellOnSegment's cell walk steps x and y on two paths), so
    no edge into one block carries that block's own phis and loop_program has no back edge to work from.

    The blocks on paths from the previous call to the site with two or more edges in (points) are the program's
    places: carried value 0 is the place the run is at (0 the previous call's return), then the points' phis the
    value and the paths need. A leaf per edge into a point: at its path's first place, with the branch facts the
    path added since, it moves to the point and sets the point's phis to what the edge holds there; the site's
    leaf, from its place with its path's facts, gives the value. A path that leaves for another call or the
    return has no leaf (no leaf taken: not known), so the value is given only when the run went the original's way
    to this site. Only when every branch on those paths decides by a fact (two edges out of a block contradict
    each other), with no store between the call and the site (loads read at the call what the original read).
    others, others_give: the other sites' (site, state) under `key`; where one's path comes first the program
    gives others_give (a jalr's target check: 0, so a call of another callee does not fit the jalr site: BMX
    drawQueuedNodes' default draw call fitted its node-callback jalr too). Only those reached from a place by a
    path with no merge and no store."""
    edges = getattr(fn, 'edges', None)
    if not edges:
        return None
    if key == ENTRY:
        if regs.get(DIRTY):
            return None
    elif RCLEAN not in regs:
        return None
    lead = sorted(fn.lead)

    def block_of(a):
        i = bisect.bisect_right(lead, a) - 1
        return lead[i] if i >= 0 else None
    sb = block_of(site)
    start = fn.start if key == ENTRY else block_of(key)
    if sb is None or start is None or (sb == start and key == ENTRY):
        return None
    preds = collections.defaultdict(list)
    succs = collections.defaultdict(list)
    for t, by_pred in edges.items():
        for (p, i), by_key in by_pred.items():
            st = by_key.get(key)
            if st is not None:
                preds[t].append((p, st))
                succs[p].append((t, st))
    if key == ENTRY and preds.get(start):
        return None
    # (the previous call's block: its edges out leave from the call's return, START; entered again it is the
    # site's block only, when the site is that call's own: a loop around one call)
    START = ('start',)
    region, work = {sb}, [sb]
    first = True
    while work:
        t = work.pop()
        if t == start and not first:
            continue
        first = False
        if not preds.get(t):
            return None
        for p, _st in preds[t]:
            if p not in region:
                region.add(p)
                work.append(p)
    points = sorted(t for t in region if (t != start or t == sb) and len(preds[t]) >= 2)
    if not points or len(region) > 64:
        return None
    place = {START: 0}
    place.update({b: i + 1 for i, b in enumerate(points)})
    # (no store between the call and the site but to the frame, whose words no value here may read; every branch
    # decided by a fact)
    spans = []
    for b in region:
        end = min([x for x in lead if x > b] + [fn.end])
        if b == start and key != ENTRY:
            spans.append((key + 8, end))
        if b == sb:
            spans.append((b, site))
        if b != start and b != sb:
            spans.append((b, end))
    for a, stop in spans:
        while a < stop:
            if a in fn.insns and fn.insns[a][1] in g.STORES:
                m = g.MEM.match(fn.insns[a][2])
                if not (m and m.group(3) == 'sp'):
                    return None
            a += 4
        outs = succs.get(b, [])
        for i in range(len(outs)):
            for j in range(i + 1, len(outs)):
                fa, fb = facts_of(outs[i][1]), facts_of(outs[j][1])
                if not any((k, c, not t) in fb for k, c, t in fa):
                    return None
        if any(len(facts_of(st)) >= FACT_LIMIT for _t, st in outs):
            return None
    if len(facts_of(regs)) >= FACT_LIMIT:
        return None

    def origin(p):
        seen = set()
        while True:
            if p == start:
                return START
            if p in place:
                return p
            if p in seen or len(preds[p]) != 1:
                return None
            seen.add(p)
            p = preds[p][0][0]
    point_facts = {START: None}
    for b in points:
        common = set(facts_of(preds[b][0][1]))
        for _p, st in preds[b][1:]:
            common &= set(facts_of(st))
        point_facts[b] = common

    returned = getattr(fn, 'returned', {}).get(key, frozenset())

    def new_facts(x, st):
        # (from the entry: every fact; from the call: every fact its return did not hold, those over the entry
        # alone too: DM2 goalObjectTrick's site 0x180cc8 is reached from 0x180bc8's return by the beqz on the
        # landed flag, an entry argument, or through the wheel loop; with only the facts over what the call left,
        # both edges out of the call's block had no condition, so the program walked the loop when landed was 0
        # and read the wheel's contact word the original never read)
        if point_facts[x] is None:
            return [f for f in facts_of(st) if key == ENTRY or f not in returned]
        return [f for f in facts_of(st) if f not in point_facts[x]]

    def cond_of(x, st):
        c = ('alu', 'sltiu', ('alu', 'xor', ('carried', 0), ('const', place[x]), None, LOOPCOND), None, 1, LOOPCOND)
        for f in new_facts(x, st):
            c = lazy_and(c, fact_value(f))
        return c
    xs = sb if sb in place else origin(preds[sb][0][0]) if len(preds[sb]) == 1 else None
    if xs is None:
        return None
    moves = []  # (from place, to point, edge state)
    for t in points:
        for p, st in preds[t]:
            x = origin(p)
            if x is None:
                return None
            moves.append((x, t, st))
    # The phis needed: those of the value, the site's path and every move's path, and what the moves set them to.
    def tokens(v):
        return {x for x in walk(v) if isinstance(x, tuple) and x[:1] == ('phi',)}
    need = tokens(value) | tokens(cond_of(xs, regs))
    for x, t, st in moves:
        need |= tokens(cond_of(x, st))
    changed = True
    while changed:
        changed = False
        for x, t, st in moves:
            for tok in [y for y in need if y[1] == t]:
                more = tokens(st.get(tok[2], ('entry', tok[2]))) - need
                if more:
                    need |= more
                    changed = True
        if len(need) + 1 > PATH_CARRIED:
            return None
    if any(tok[1] not in place for tok in need):
        return None
    carried = sorted(need)
    mapping = {tok: ('carried', i + 1) for i, tok in enumerate(carried)}

    fv = getattr(fn, 'frame_vals', {})  # (a program reads no frame word: FRAME_VALS)

    def conv(v):
        e = substitute(substitute(v, fv), mapping)
        if phi_blocks(e) or any(isinstance(x, tuple) and x[:1] in (('load',), ('eload',)) and in_own_frame(x)
                                for x in walk(e)):
            return None
        return express(e, regs, callee)
    leaves = []
    done = conv(cond_of(xs, regs))
    result = conv(value)
    if done is None or result is None:
        return None
    leaves.append((done, 'done', None))
    for x, t, st in moves:
        c = conv(cond_of(x, st))
        vals = [('const', place[t])]
        for i, tok in enumerate(carried):
            vals.append(conv(st.get(tok[2], ('entry', tok[2]))) if tok[1] == t else ('carried', i + 1))
        if c is None or any(v is None for v in vals):
            return None
        leaves.append((c, 'again', tuple(vals)))
    if others_give is not None:
        for site2, regs2 in others:
            sb2 = block_of(site2)
            if sb2 is None or len(preds.get(sb2, ())) != 1 or len(facts_of(regs2)) >= FACT_LIMIT:
                continue
            x2 = origin(preds[sb2][0][0])
            if x2 is None:
                continue
            # (its stretch from its place store-free too)
            clear, p = True, sb2
            while clear and p not in place and p != start:
                end = min([x for x in lead if x > p] + [fn.end])
                a = p
                while a < (site2 if p == sb2 else end):
                    if a in fn.insns and fn.insns[a][1] in g.STORES:
                        m = g.MEM.match(fn.insns[a][2])
                        clear = clear and bool(m and m.group(3) == 'sp')
                    a += 4
                p = preds[p][0][0] if len(preds[p]) == 1 else start
            c2 = conv(cond_of(x2, regs2)) if clear else None
            give = conv(others_give)
            if c2 is not None and give is not None:
                leaves.insert(1, (c2, 'value', (give,)))
    initial = tuple([('const', 0)] * (len(carried) + 1))
    return ('loop', tuple([PLACE] + carried), initial, tuple(leaves), result)


PLACE = 'place'
SINCE_CALL = ('prev', 'res', 'load', 'rload')


def since_call(fact):
    """A branch fact over what the previous call left or loads since (not over a loop's phi)."""
    return has(fact[1], lambda y: isinstance(y, tuple) and y and y[0] in SINCE_CALL) and not phi_blocks(fact[1])


def entry_condition(fn, b, key):
    """1 when the path came into the loop headed at b under `key` by one of its entry edges (the edges with no
    phi of b), by each edge's branch facts since the previous call; None when an entry edge has none (any path
    may have entered). A path to a value or fact over b's phis went through b, so this is a condition the call's
    path met (DM2 runRenderStage: after the jalr the walk goes on only when the callback said so and the node's
    flag is negative or a draw was begun; else the call is the 0x11951c site's)."""
    edges = getattr(fn, 'edges', None)
    if not edges or b not in edges:
        return None
    out = None
    for p, by_key in sorted(edges[b].items()):
        st = by_key.get(key)
        if st is None or any(b in phi_blocks(v) for k, v in st.items()
                             if isinstance(k, int) and k >= 0 and v is not None):
            continue
        since = [f for f in facts_of(st) if since_call(f)]
        if not since:
            return None
        c = path_condition(since, set())
        out = c if out is None else ('alu', 'or', out, c, None, LOOPCOND)
    return out


def function_rows(item):
    """(address, [(site, target, [(prev, sets, checks)])], [(site, reg, why)]) of one function."""
    addr, fn = item
    try:
        sites = analyse(fn)
    except RuntimeError as e:
        return addr, None, [(0, 0, str(e))]
    rows, unknown, eloads = [], [], Loads()
    stats = collections.Counter()  # (the proofs of the values given: site, checked, dropped)
    targets = collections.Counter()
    for site, (callees, states) in sites.items():
        c = next(iter(callees.values()))
        targets[c[1] if c[0] == 'const' else 0] += 1
    # What each case of each site gives each slot: ('carry',) unchanged since the previous call (or the entry),
    # None not known, else its expression.
    raw = {}
    for site, (callees, states) in sorted(sites.items()):
        ordered = sorted(states.items(), key=lambda kv: -1 if kv[0] == ENTRY else kv[0])
        given = []
        for key, regs in ordered:
            row = {}
            for r in SAVED:
                v = regs.get(r, ('entry', r))
                if unchanged(v, key, r):
                    row[r] = ('carry',)  # (as the previous call had it, or the entry)
                    continue
                e = best(v, regs, callees[key])
                if e is None and len(phi_blocks(v)) == 1:
                    b = next(iter(phi_blocks(v)))
                    e = loop_program(fn, site, b, key, regs, callees[key], r, loop_others(sites, site, key, b))
                    if e is not None:
                        stats['loop'] += 1
                if e is None and phi_blocks(v):
                    e = path_program(fn, site, key, regs, callees[key], v)
                    if e is not None:
                        stats['path program'] += 1
                if e is None:
                    unknown.append((site, r, kind_of(v)))
                row[r] = e
            given.append(row)
        raw[site] = (ordered, given)
    # A carry followed back: what the previous call got in the slot is what its own site gave it, when that is one
    # value over the entry alone (constants, entry registers, entry loads: the same whenever it is worked out) in
    # every case of that site; from the entry, the entry register. So a site whose cases differ only in that one
    # set the register and another carries what an earlier site set to the same (BMX engineMain's s4 = 0x240000,
    # set after one call, carried through a path with two more) proves the value instead of dropping it.
    settled = {}

    def settle(site, r, seen):
        if (site, r) in settled:
            return settled[(site, r)]
        if site in seen:
            return None
        seen = seen | {site}
        values = set()
        for key, row in zip(*raw[site]):
            e = row[r]
            if e == ('carry',):
                e = ('entry', r) if key[0] == ENTRY else settle(key[0], r, seen)
            if e is None or not entry_only(e):
                settled[(site, r)] = None
                return None
            values.add(e)
        settled[(site, r)] = values.pop() if len(values) == 1 else None
        return settled[(site, r)]

    for site, (callees, states) in sorted(sites.items()):
        c0 = next(iter(callees.values()))
        target = c0[1] if c0[0] == 'const' else 0
        # Another site the runtime could take for this one (the same target, or a jalr):
        # the call's arguments tell them apart.
        ambiguous = targets[target] > 1 or (target == 0 and len(sites) > 1) or (target != 0 and targets[0] > 0)
        cases = []
        per_slot = collections.defaultdict(list)
        ordered, given = raw[site]
        # The rows with their carries followed back where that settles them (the agreement test below): a case
        # carrying what an earlier site set agrees with one that sets it the same.
        followed = []
        for (key, regs), row in zip(ordered, given):
            frow = dict(row)
            for r in SAVED:
                if row[r] == ('carry',) and key != ENTRY and any(other[r] not in (('carry',), None) for other in given):
                    e = settle(key, r, frozenset())
                    if e is not None:
                        frow[r] = e
            followed.append(frow)
        for (key, regs), row, frow in zip(ordered, given, followed):
            # (the checks' expressions; made into nodes only if the case keeps them)
            checks = []
            for i, r in enumerate(ARGS_INT + ARGS_FLT):
                e = express(regs.get(r, ('entry', r)), regs, callees[key])
                if e is None and r in regs and len(phi_blocks(regs[r])) == 1:  # (an argument a loop made)
                    b = next(iter(phi_blocks(regs[r])))
                    e = loop_program(fn, site, b, key, regs, callees[key], r, loop_others(sites, site, key, b))
                if e is None and r in regs and phi_blocks(regs[r]):
                    e = path_program(fn, site, key, regs, callees[key], regs[r])
                if e is not None and all(fits32(x[1]) for x in walk(e) if isinstance(x, tuple) and x and x[0] == 'const'):
                    checks.append((i, e))
            if target == 0:
                e = express(callees[key], regs, callees[key])
                if e is None and len(phi_blocks(callees[key])) == 1:  # (a target a loop found: a node's callback)
                    b = next(iter(phi_blocks(callees[key])))
                    e = loop_program(fn, site, b, key, regs, callees[key], None, loop_others(sites, site, key, b),
                                     value=callees[key], others_give=('const', 0))
                if e is None and phi_blocks(callees[key]):
                    e = path_program(fn, site, key, regs, callees[key], callees[key],
                                     [(s2, st2[key]) for s2, (_c2, st2) in sites.items() if s2 != site and key in st2],
                                     ('const', 0))
                if e is not None and all(fits32(x[1]) for x in walk(e) if isinstance(x, tuple) and x[0] == 'const'):
                    checks.append((16, e))
            # The case's path condition: the branch facts since the previous call (the facts over what that call
            # left or loads since, which after_call does not keep from before), as one value, 1 when they hold.
            path_check = None
            # (from the entry every fact is since: none was made before it; those over the entry alone too)
            if len(given) > 1:
                since = [f for f in facts_of(regs) if key == ENTRY or has(f[1], lambda y: isinstance(y, tuple) and y
                                                                          and y[0] in ('prev', 'res', 'load', 'rload',
                                                                                       'phi'))]
                if since:
                    try:
                        pc = path_condition(since, set())
                        # (a fact over a loop's phi is that loop's: its program holds its own path condition)
                        if pc is not None and not phi_blocks(pc):
                            pc = express(pc, regs, callees[key])
                            path_check = postfix(pc, [], eloads) if pc is not None else None
                    except ValueError:
                        path_check = None
            # A condition the path from the previous call met, which proves nothing but rules the case out when
            # it fails (a check of kind 18): the facts since the call that are over no loop's phi, and for each
            # loop the path went through, how it came in (entry_condition). Where another site or another
            # place of the last call could be taken for this one, the cases that cannot be the original's go.
            need_check = None
            if key != ENTRY and (len(given) > 1 or ambiguous):
                pc = path_condition([f for f in facts_of(regs) if since_call(f)], set())
                if RCLEAN in regs:
                    loops = set()
                    for x in [regs.get(r) for r in SAVED + ARGS_INT + ARGS_FLT] + [callees[key]] + \
                            [f[1] for f in facts_of(regs)]:
                        if x is not None:
                            loops |= phi_blocks(x)
                    for b in sorted(loops):
                        ec = entry_condition(fn, b, key)
                        if ec is not None:
                            pc = ec if pc is None else ('alu', 'and', pc, ec, None, LOOPCOND)
                if pc is not None:
                    try:
                        pc = express(pc, regs, callees[key])
                        need_check = postfix(pc, [], eloads) if pc is not None else None
                    except ValueError:
                        need_check = None
            sets, needs_checks = [], False
            for r in SAVED:
                e = frow[r]
                # The site proves a value when every case of it gives the same one (which call came
                # before does not matter then); else the run has to prove the case was the path taken: the
                # case's argument checks, or (held4) its path condition, the branch facts since the previous
                # call worked out at the call (a check of kind 17: it must hold), both CHECKED. A case with
                # neither (no branch between the previous call's return and this call, no argument to tell)
                # is not given: the entry value, as before.
                proven = all(other[r] == e for other in followed) and e is not None
                if e == ('carry',):
                    per_slot[r].append(('same',))
                    if proven or len(given) == 1 or key == ENTRY:  # (from the entry: the entry value either way)
                        continue
                    if not checks and path_check is None:
                        stats['dropped carry'] += 1
                        sets.append((SLOT[r], None))
                        continue
                    stats['checked carry' if checks else 'path carry'] += 1
                    needs_checks = True
                    sets.append((SLOT[r] | CHECKED, postfix(('prev', r), [], eloads)))
                    continue
                if e is None:
                    sets.append((SLOT[r], None))
                    per_slot[r].append(None)
                    continue
                try:
                    nodes = postfix(e, [], eloads)
                except ValueError as why:
                    unknown.append((site, r, 'const64' if '64' in str(why) else
                                    'return-loads' if 'return' in str(why) else 'entry-loads'))
                    sets.append((SLOT[r], None))
                    per_slot[r].append(None)
                    continue
                per_slot[r].append(e)
                if proven or len(given) == 1:
                    stats['site'] += 1
                    sets.append((SLOT[r], nodes))
                elif e[0] == 'loop':
                    # (a loop program proves its own path: it gives a value only when the site's path condition
                    # holds at the call, and no other site's did on the way: the run's proof, as a check's; so
                    # it needs no other, which an argument the callee does not take could never give: DM2
                    # runRenderStage's first draw call after the entry was given nothing)
                    stats['loop path'] += 1
                    sets.append((SLOT[r], nodes))
                elif checks or path_check is not None:
                    stats['checked' if checks else 'path'] += 1
                    needs_checks = True
                    sets.append((SLOT[r] | CHECKED, nodes))
                else:
                    stats['dropped'] += 1
                    sets.append((SLOT[r], None))
            kept = []
            for i, e in (checks if (ambiguous or needs_checks) else []):
                try:
                    kept.append((i, postfix(e, [], eloads)))
                except ValueError:
                    pass
            if needs_checks and path_check is not None:
                kept.append((17, path_check))
            if need_check is not None and not (needs_checks and need_check == path_check):
                stats['ruled out by the path'] += 1
                kept.append((18, need_check))
            cases.append((0 if key == ENTRY else key, sets, kept))
        # After a call the runtime could not place: what every case agrees on, and only
        # that (a register it does not list is unknown then: what the last call got is
        # not known).
        lost = []
        for r in SAVED:
            vals = per_slot[r]
            if len(vals) == len(states) and vals and all(x == vals[0] for x in vals) and vals[0] not in (None, ('same',)) \
                    and not any(x and x[0] in ('prev', 'rload') for x in walk(vals[0]) if isinstance(x, tuple)):
                try:
                    lost.append((SLOT[r], postfix(vals[0], [], eloads)))
                except ValueError:
                    pass
        cases.append((LOST, lost, []))
        rows.append((site, target, cases))
    rows.sort(key=lambda r: (r[1], r[0]))  # (by target: the runtime looks a call's target up; jalrs, 0, first)
    return addr, (rows, eloads, stats), unknown


def kind_of(v):
    """Why a value is not expressible: the first part of it the runtime cannot have."""
    for x in walk(v):
        if isinstance(x, tuple) and x and x[0] in BAD:
            return x[0] + ((':' + x[1]) if x[0] == 'op' else '')
        if isinstance(x, tuple) and x and x[0] == 'entry' and x[1] not in ENTRY_OK:
            return 'entry:%s' % g.GPR_NAMES[x[1]] if x[1] < 32 else 'entry:f%d' % (x[1] - 32)
        if isinstance(x, tuple) and x and x[0] == 'load' and x[1] not in LOAD_KINDS:
            return 'load:' + x[1]
        if isinstance(x, tuple) and x and x[0] == 'load' and reads_through_frame(x):
            return 'frame-load'
        if isinstance(x, tuple) and x and x[0] == 'alu' and x[1] not in ALU_IDS:
            return 'alu:' + x[1]
    return 'other'


def chunked_functions(path):
    out = collections.defaultdict(dict)
    if not path:
        return out
    for line in open(path, encoding='utf-8'):
        line = line.rstrip('\n')
        if not line or line.startswith('#'):
            continue
        game, addr, gen, _out, *rest = line.split('\t')
        if rest and rest[0]:
            out[int(game)][int(addr, 16)] = [gen] + rest[0].split('|')
        elif os.path.basename(gen).startswith('entry_'):
            # (a registered function the recompiler wrote as one entry_ chunk: its row, as the frame table's)
            out[int(game)][int(addr, 16)] = [gen]
    return out


def functions(folder, chunked, read=None):
    for name in sorted(os.listdir(folder)):
        m = g.FUNC_FILE.search(name)
        if not m:
            continue
        addr = int(m.group(1), 16)
        whole = chunked.get(addr)
        # An entry_ file is a slice of some function, with no row of its own, unless the list names it as a
        # registered function's first chunk (AI's bindTexture: entry_001049d0 and three more chunks; it had no
        # row, so every call of its gave the entry values: the kFogStrip word of cov4's crane).
        if name.startswith('entry_') and not (whole and os.path.basename(whole[0]) == name):
            continue
        if read is not None:
            read += whole if whole else [os.path.join(folder, name)]
        text = g.whole_text(whole) if whole else open(os.path.join(folder, name), encoding='utf-8',
                                                       errors='replace').read()
        rm = g.RANGE.search(text or '')
        if not rm:
            continue
        insns = g.instructions(text)
        if insns:
            yield addr, Fn(text, int(rm.group(1), 16), int(rm.group(2), 16), insns)


def render(tables, chunks=code.CHUNKS):
    """The C++ source of the table (ee/held.h) and the sources of its programs as code (readable_held_code.py)."""
    funcs, sites, cases, sets, checks, nodes, entry_loads, return_loads = [], [], [], [], [], [], [], []
    for game, address, rows, eloads in tables:
        first_site = len(sites)
        flags = 0
        first_eload = len(entry_loads)
        for expr in eloads:
            for op, sub, imm in expr:
                if op == OP_ENTRY and (4 <= sub < 12 or 44 <= sub < 52):
                    flags |= 1
            entry_loads.append((0, len(expr), len(nodes)))
            nodes += expr
        for site, target, site_cases in rows:
            first_case = len(cases)
            first_rload = len(return_loads)
            for expr in getattr(eloads, 'ret', {}).get(site, []):
                for op, sub, imm in expr:
                    if op == OP_ENTRY and (4 <= sub < 12 or 44 <= sub < 52):
                        flags |= 1
                    if op == OP_RESULT:
                        flags |= 2
                flags |= 4
                return_loads.append((0, len(expr), len(nodes)))
                nodes += expr
            for prev, case_sets, case_checks in site_cases:
                first_set = len(sets)
                for slot, expr in case_sets:
                    if expr is None:
                        sets.append((slot, 0, 0))
                        continue
                    for op, sub, imm in expr:
                        if op == OP_ENTRY and (4 <= sub < 12 or 44 <= sub < 52):
                            flags |= 1
                        if op == OP_RESULT:
                            flags |= 2
                    sets.append((slot, len(expr), len(nodes)))
                    nodes += expr
                first_check = len(checks)
                for what, expr in case_checks:
                    for op, sub, imm in expr:
                        if op == OP_ENTRY and (4 <= sub < 12 or 44 <= sub < 52):
                            flags |= 1
                        if op == OP_RESULT:
                            flags |= 2
                    checks.append((what, len(expr), len(nodes)))
                    nodes += expr
                cases.append((prev, first_set, first_check, len(sets) - first_set, len(checks) - first_check))
            sites.append((site, target, first_case, len(cases) - first_case, first_rload,
                          len(return_loads) - first_rload))
        funcs.append((game, address, first_site, len(sites) - first_site, flags, first_eload, len(eloads)))

    def chunk(rows, fmt, per=4):
        return ''.join('    ' + ' '.join(fmt % r for r in rows[i:i + per]) + '\n' for i in range(0, len(rows), per))

    def imm(v):
        v &= (1 << 32) - 1
        v = v - (1 << 32) if v >> 31 else v
        return '%d' % v if v != -(1 << 31) else '(-0x7fffffff - 1)'
    node_rows = [(op, sub, imm(i)) for op, sub, i in nodes]
    # (each value's, check's and load's program as code: Value::program, the numbers in these four's order)
    numbers, programs = code.programs_of(nodes, sets + checks + entry_loads + return_loads)
    rows = [r + (n,) for r, n in zip(sets + checks + entry_loads + return_loads, numbers)]
    sets, checks = rows[:len(sets)], rows[len(sets):len(sets) + len(checks)]
    entry_loads = rows[len(sets) + len(checks):len(sets) + len(checks) + len(entry_loads)]
    return_loads = rows[len(sets) + len(checks) + len(entry_loads):]
    # (and each case's values and checks as code: Case::valuesCode, checksCode)
    value_codes, check_codes, case_values, case_checks = code.cases_of(nodes, cases, sets, checks)
    cases = [r + (v, k) for r, v, k in zip(cases, value_codes, check_codes)]
    header = ('// Generated by tools/readable_held_gen.py (readable_held_code.py): the held table\'s programs and cases as\n'
              '// code (ee/held.h Value::program, Case::valuesCode and checksCode; ee/held_program.h). Built from generated\n'
              '// code: never committed.\n')
    return ('// Generated by tools/readable_held_gen.py: the value of each callee-saved register at each call site\n'
            '// of the original, per call before it (%d functions, %d sites, %d cases, %d values, %d checks, %d nodes,\n'
            '// %d entry loads, %d return loads; ee/held.h). Built from generated code: never committed.\n'
            '#include "ee/held.h"\n\n%s\n'
            'namespace\n{\n'
            '    using namespace ee::held;\n'
            '    const Function kFunctions[] = {\n%s    {0u, 0u, 0u, 0u, 0u, 0u, 0u}};\n'
            '    const Site kSites[] = {\n%s    {0u, 0u, 0u, 0u, 0u, 0u}};\n'
            '    const Case kCases[] = {\n%s    {0u, 0u, 0u, 0u, 0u, 0u, 0u}};\n'
            '    const Value kValues[] = {\n%s    {0u, 0u, 0u, 0u}};\n'
            '    const Value kChecks[] = {\n%s    {0u, 0u, 0u, 0u}};\n'
            '    const Node kNodes[] = {\n%s    {0u, 0u, 0}};\n'
            '    const Value kEntryLoads[] = {\n%s    {0u, 0u, 0u, 0u}};\n'
            '    const Value kReturnLoads[] = {\n%s    {0u, 0u, 0u, 0u}};\n'
            '    const Registration g_held{{kFunctions, %du, kSites, kCases, kValues, kChecks, kNodes, kEntryLoads,\n'
            '                               kReturnLoads, kPrograms, kCaseValues, kCaseChecks}};\n}\n'
            % (len(funcs), len(sites), len(cases), len(sets), len(checks), len(nodes), len(entry_loads), len(return_loads),
               code.declarations(len(programs), len(case_values), len(case_checks)),
               chunk(funcs, '{%du, 0x%08xu, %du, %du, %du, %du, %du},', 2),
               chunk(sites, '{0x%08xu, 0x%08xu, %du, %du, %du, %du},', 3),
               chunk(cases, '{0x%08xu, %du, %du, %du, %du, %du, %du},', 3),
               chunk(sets, '{%du, %du, %du, %du},', 5),
               chunk(checks, '{%du, %du, %du, %du},', 5),
               chunk(node_rows, '{%du, %du, %s},', 5),
               chunk(entry_loads, '{%du, %du, %du, %du},', 5),
               chunk(return_loads, '{%du, %du, %du, %du},', 5),
               len(funcs))), code.chunk_sources(programs + case_values + case_checks, header, chunks)


def programs_path(out, k):
    """The source of the table's programs chunk k (readable_held_code.CHUNKS of them), beside the table's."""
    return os.path.join(os.path.dirname(os.path.abspath(out)), 'readable_held_programs_%02d.cpp' % k)


def write_if_changed(path, text):
    try:
        if open(path, encoding='utf-8').read() == text:
            return False
    except OSError:
        pass
    tmp = path + '.tmp'
    with open(tmp, 'w', encoding='utf-8') as f:
        f.write(text)
    os.replace(tmp, path)
    return True


def self_test():
    """A function that keeps a0 in s0 across two calls and a call's result in s1."""
    text = '\n'.join([
        '// Address: 0x100000 - 0x100030',
        '    // 0x100000: 0x27bdffe0  addiu       $sp, $sp, -0x20',
        '    // 0x100004: 0xffb00000  sd          $s0, 0x0($sp)',
        '    // 0x100008: 0xffbf0010  sd          $ra, 0x10($sp)',
        '    // 0x10000c: 0xc040100  jal         func_100400',
        '    // 0x100010: 0x80802d  daddu       $s0, $a0, $zero',
        '    // 0x100014: 0x40882d  daddu       $s1, $v0, $zero',
        '    // 0x100018: 0xc040200  jal         func_100800',
        '    // 0x10001c: 0x0  nop',
        '    // 0x100020: 0xdfbf0010  ld          $ra, 0x10($sp)',
        '    // 0x100024: 0xdfb00000  ld          $s0, 0x0($sp)',
        '    // 0x100028: 0x3e00008  jr          $ra',
        '    // 0x10002c: 0x27bd0020  addiu       $sp, $sp, 0x20',
        ''])
    insns = g.instructions(text)
    fn = Fn(text, 0x100000, 0x100030, insns)
    _, (rows, _eloads, _stats), unknown = function_rows((0x100000, fn))
    by_site = {site: cases for site, target, cases in rows}
    first = dict((prev, sets) for prev, sets, _ in by_site[0x10000c])
    assert first[0] == [(0, [(OP_ENTRY, 4, 0)])], first
    second = dict((prev, sets) for prev, sets, _ in by_site[0x100018])
    assert second[0x10000c] == [(1, [(OP_RESULT, 0, 0)])], second
    assert not unknown, unknown
    # A cvt.w.s the disassembly left as `.word`, and a select a bc1t makes: s0 = f12 as a word, s1 = 0 when
    # f12 == f13 (c.eq.s), else 1.
    text = '\n'.join([
        '// Address: 0x200000 - 0x200030',
        '    // 0x200000: 0x27bdffe0  addiu       $sp, $sp, -0x20',
        '    // 0x200004: 0xffbf0010  sd          $ra, 0x10($sp)',
        '    // 0x200008: 0x46006024  .word       0x46006024                   # cvt.w.s     $f0, $f12 # 00000000 <InstrIdType: CPU_COP1_FPUS>',
        '    // 0x20000c: 0x460d6032  c.eq.s      $f12, $f13',
        '    // 0x200010: 0x882d  daddu       $s1, $zero, $zero',
        '    // 0x200014: 0x45010002  bc1t        . + 4 + (0x2 << 2)',
        '    // 0x200018: 0x44100000  mfc1        $s0, $f0 (Delay Slot)',
        '    // 0x20001c: 0x24110001  addiu       $s1, $zero, 0x1',
        '    // 0x200020: 0xc080100  jal         func_200400',
        '    // 0x200024: 0x0  nop',
        '    // 0x200028: 0xdfbf0010  ld          $ra, 0x10($sp)',
        '    // 0x20002c: 0x3e00008  jr          $ra',
        '    // 0x200030: 0x27bd0020  addiu       $sp, $sp, 0x20',
        ''])
    insns = g.instructions(text)
    fn = Fn(text, 0x200000, 0x200034, insns)
    _, (rows, _eloads, _stats), unknown = function_rows((0x200000, fn))
    by_site = {site: cases for site, target, cases in rows}
    sets = dict(dict((prev, sets) for prev, sets, _ in by_site[0x200020])[0])
    assert sets[0] == [(OP_ENTRY, 44, 0), (OP_ALU, ALU_IDS['cvt.w.s'], 0), (OP_ALU, ALU_IDS['mfc1'], 0)], sets[0]
    assert sets[1] == [(OP_ENTRY, 44, 0), (OP_ENTRY, 45, 0), (OP_ALU, ALU_IDS['c.eq.s'], 0), (OP_SKIP_IF, 0, 2),
                       (OP_CONST, 0, 0), (OP_SKIP, 0, 1), (OP_CONST, 0, 1)], sets[1]  # (a lazy select: 0 when taken)
    assert not unknown, unknown
    # A list walked to the first node whose flag is set, then a call: s0 at the call is a loop program
    # (carried s0 from a0; again while the flag is 0: s0 = next; the call's path when it is not).
    text = '\n'.join([
        '// Address: 0x300000 - 0x300034',
        '    // 0x300000: 0x27bdffe0  addiu       $sp, $sp, -0x20',
        '    // 0x300004: 0xffbf0010  sd          $ra, 0x10($sp)',
        '    // 0x300008: 0x80802d  daddu       $s0, $a0, $zero',
        '    // 0x30000c: 0x8e020004  lw          $v0, 0x4($s0)',
        '    // 0x300010: 0x14400003  bnez        $v0, . + 4 + (0x3 << 2)',
        '    // 0x300014: 0x0  nop',
        '    // 0x300018: 0x8e100000  lw          $s0, 0x0($s0)',
        '    // 0x30001c: 0x1000fffb  b           . + 4 + (-0x5 << 2)',
        '    // 0x300020: 0x0  nop',
        '    // 0x300024: 0xc0c0400  jal         func_301000',
        '    // 0x300028: 0x202d  daddu       $a0, $zero, $zero',
        '    // 0x30002c: 0xdfbf0010  ld          $ra, 0x10($sp)',
        '    // 0x300030: 0x3e00008  jr          $ra',
        '    // 0x300034: 0x27bd0020  addiu       $sp, $sp, 0x20',
        ''])
    insns = g.instructions(text)
    fn = Fn(text, 0x300000, 0x300038, insns)
    _, (rows, _eloads, stats), unknown = function_rows((0x300000, fn))
    sets = dict(dict((prev, sets) for prev, sets, _ in dict((site, cases) for site, target, cases in rows)[0x300024])[0])
    assert sets[0][0] == (OP_ENTRY, 4, 0) and sets[0][1] == (OP_LOOP, 1, len(sets[0]) - 2), sets[0]
    leaves = [n for n in sets[0] if n[0] == OP_LEAF]
    assert [n[1] for n in leaves] == [1, 0] and stats['loop'] == 1 and not unknown, (sets[0], dict(stats), unknown)
    # The entry's a0 spilled to the frame, read back after a call into s0: s0 at the second call is that a0.
    text = '\n'.join([
        '// Address: 0x400000 - 0x400030',
        '    // 0x400000: 0x27bdffe0  addiu       $sp, $sp, -0x20',
        '    // 0x400004: 0xffbf0010  sd          $ra, 0x10($sp)',
        '    // 0x400008: 0xafa40000  sw          $a0, 0x0($sp)',
        '    // 0x40000c: 0xc100100  jal         func_400400',
        '    // 0x400010: 0x0  nop',
        '    // 0x400014: 0x8fb00000  lw          $s0, 0x0($sp)',
        '    // 0x400018: 0xc100200  jal         func_400800',
        '    // 0x40001c: 0x0  nop',
        '    // 0x400020: 0xdfbf0010  ld          $ra, 0x10($sp)',
        '    // 0x400024: 0x3e00008  jr          $ra',
        '    // 0x400028: 0x27bd0020  addiu       $sp, $sp, 0x20',
        ''])
    insns = g.instructions(text)
    fn = Fn(text, 0x400000, 0x40002c, insns)
    _, (rows, _eloads, _stats), unknown = function_rows((0x400000, fn))
    second = dict((prev, sets) for prev, sets, _ in dict((s, c) for s, t, c in rows)[0x400018])
    assert second[0x40000c] == [(0, [(OP_ENTRY, 4, 0), (OP_CONST, 0, 0), (OP_ALU, ALU_IDS['addu'], 0)])], second
    code.self_test()
    print('readable_held_gen: self-test ok')


# The committed data (--write, --check, --from): each game's functions' rows, one line each.
HELD_GAMES = {0: 'ai', 1: 'dm2', 2: 'bmx'}


def held_path(directory, game):
    return os.path.join(directory, HELD_GAMES[game] + '.txt.xz')


def tables_text(game, tables):
    """A game's rows as text: `ADDRESS<TAB>rows<TAB>entry loads<TAB>return loads` (Python literals of the scan's
    tuples), sorted by address."""
    lines = ['# readable_held_gen.py --write: %s\'s held table from its generated code (%d functions)'
             % (HELD_GAMES[game], len(tables))]
    for _game, addr, rows, eloads in sorted(tables, key=lambda t: t[1]):
        ret = sorted((site, exprs) for site, exprs in getattr(eloads, 'ret', {}).items() if exprs)
        lines.append('0x%08x\t%r\t%r\t%r' % (addr, rows, list(eloads), ret))
    return '\n'.join(lines) + '\n'


def read_data(path):
    import lzma
    if not os.path.exists(path):
        return None
    with lzma.open(path, 'rt', encoding='utf-8') as f:
        return f.read()


def write_data(path, text):
    import lzma
    # (xz with no time in it: the same text gives the same file)
    with open(path, 'wb') as f:
        f.write(lzma.compress(text.encode('utf-8'), format=lzma.FORMAT_XZ, check=lzma.CHECK_CRC64, preset=9))


def load_tables(directory, game):
    """The (game, address, rows, eloads) of a game's committed data, as the scan gives them; none when absent."""
    import ast
    text = read_data(held_path(directory, game))
    if text is None:
        return []
    out = []
    for line in text.splitlines():
        if not line or line.startswith('#'):
            continue
        addr, rows, eloads, ret = line.split('\t')
        loads = Loads()
        loads.extend(ast.literal_eval(eloads))
        for site, exprs in ast.literal_eval(ret):
            loads.ret[site] = exprs
        out.append((game, int(addr, 16), ast.literal_eval(rows), loads))
    return out


def main():
    if sys.argv[1:] == ['--self-test']:
        return self_test()
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--scan', action='append', default=[], help='GAME=DIR: a generated folder')
    p.add_argument('--list', help='traced_blocks.list: functions written as several entry_ chunks')
    p.add_argument('--out')
    p.add_argument('--unknown', help='TSV of the values the table cannot give')
    p.add_argument('--jobs', type=int, default=min(6, os.cpu_count() or 1))
    p.add_argument('--chunks', type=int, default=code.CHUNKS, help='sources of the programs as code (game.cmake\'s)')
    p.add_argument('--depfile', help='a depfile for --out: every generated file the table was read from')
    p.add_argument('--from', dest='from_dir', help='DIR: the committed data (no scan): the sources from it alone')
    p.add_argument('--write', help='DIR: the scanned games\' data written there (the integration lane\'s)')
    p.add_argument('--check', help='DIR: exit 1 when the committed data of a scanned game is not what the scan makes')
    a = p.parse_args()
    if a.from_dir:
        if not a.out:
            p.error('--from needs --out')
        tables = []
        for game in sorted(HELD_GAMES):
            tables += load_tables(a.from_dir, game)
        text, chunks = render(sorted(tables, key=lambda t: (t[0], t[1])), a.chunks)
        write_if_changed(a.out, text)
        for k, chunk_text in enumerate(chunks):
            write_if_changed(programs_path(a.out, k), chunk_text)
        print('readable_held_gen: %d functions, from %s' % (len(tables), a.from_dir))
        return
    if not (a.out or a.write or a.check):
        p.error('--out, --write or --check')
    chunked = chunked_functions(a.list)
    tables, unknown_rows, read = [], [], []
    proofs = collections.defaultdict(collections.Counter)
    with multiprocessing.Pool(a.jobs) as pool:
        for item in a.scan:
            game, folder = item.split('=', 1)
            game = int(game)
            for addr, rows, unknown in pool.imap(function_rows, functions(folder, chunked.get(game, {}), read), 16):
                if rows and rows[0]:
                    tables.append((game, addr) + rows[:2])
                    proofs[game].update(rows[2])
                unknown_rows += [(game, addr) + u for u in unknown]
    tables.sort(key=lambda t: (t[0], t[1]))
    scanned_games = sorted({int(item.split('=', 1)[0]) for item in a.scan})
    status = 0
    if a.write or a.check:
        stale = []
        for game in scanned_games:
            text = tables_text(game, [t for t in tables if t[0] == game])
            path = held_path(a.write or a.check, game)
            if a.write:
                os.makedirs(os.path.dirname(path), exist_ok=True)
                old = read_data(path)
                if old != text:
                    write_data(path, text)
                print('readable_held_gen: %s %s (%d bytes)' % (HELD_GAMES[game], 'written' if old != text else 'unchanged',
                                                              os.path.getsize(path)))
            elif read_data(path) != text:
                stale.append(HELD_GAMES[game])
        if stale:
            print('readable_held_gen: the committed held data of %s is not what the generated code makes: '
                  'readable_held_gen.py --write (the build\'s zx_held_write), then commit runtime/tools/held'
                  % ', '.join(stale))
            status = 1
        elif a.check:
            print('readable_held_gen: %s as the generated code makes them' % ', '.join(HELD_GAMES[g] for g in scanned_games))
    if a.out:
        text, chunks = render(tables, a.chunks)
        write_if_changed(a.out, text)
        # (the programs' sources beside it, readable_held_programs_NN.cpp: every one, empty past the last)
        for k, chunk_text in enumerate(chunks):
            write_if_changed(programs_path(a.out, k), chunk_text)
    if a.unknown:
        write_if_changed(a.unknown, '# game\tfunction\tsite\tregister\twhy (readable_held_gen.py)\n' + ''.join(
            '%d\t0x%08x\t0x%08x\t%s\t%s\n' % (gm, f, s, r, w) for gm, f, s, r, w in sorted(unknown_rows)))
    if a.depfile:
        # (a body of the original changed in place, the table with it: Ninja reads this after the step)
        # (its target: --out, or for --check the stamp beside the depfile, NAME.d -> NAME.stamp: game.cmake's)
        target = a.out or os.path.splitext(a.depfile)[0] + '.stamp'
        write_if_changed(a.depfile, '%s: \\\n%s\n' % (os.path.abspath(target).replace(' ', '\\ '), ' \\\n'.join(
            '  ' + os.path.abspath(f).replace(' ', '\\ ') for f in sorted(set(read)))))
    print('readable_held_gen: %d functions, %d values the table cannot give' % (len(tables), len(unknown_rows)))
    for game in sorted(proofs):
        print('readable_held_gen: game %d: values the site proves %d, behind argument checks %d (unchanged %d), behind the '
              'path %d (unchanged %d), loops proving their path %d, not given %d (unchanged %d), loops %d, path programs %d; '
              'cases a failed path condition rules out %d'
              % (game, proofs[game]['site'], proofs[game]['checked'], proofs[game]['checked carry'], proofs[game]['path'],
                 proofs[game]['path carry'], proofs[game]['loop path'], proofs[game]['dropped'], proofs[game]['dropped carry'],
                 proofs[game]['loop'], proofs[game]['path program'], proofs[game]['ruled out by the path']))
    if status:
        sys.exit(status)


if __name__ == '__main__':
    main()
