#!/usr/bin/env python3
"""A readable function made void whose original's result a caller reads.

A registered function whose C++ returns void leaves v0 and f0 as the C++ left them: the
entry wrapper writes no result (ee::ReturnKind::Void). A caller in the original that reads
the result after the call then reads whatever was there. The replay does not compare a void
function's result, and the A/B sees it only where a path changes. Lost this way:
shutdownPrimitiveRenderer and initPrimitiveRenderer, whose 1 shutdownEngine and startEngine
branch on (drafts-r; the void audit then counted a branch on v0 as writing it).

For every registered readable function (src/game, every status but Planned) in each game it
is bound for, whose C++ returns void: every caller in that game's original ELF (jal, a tail
call by j or branch, a jalr or jr whose callee is a constant or a recording saw:
readable_indirect_calls.tsv, AI's), and from each call every path of the caller's code until
v0 and f0 are both written or a call is reached. An instruction that reads either first
(branches, stores, jr, jalr, mtc1, ALU and FPU operations: every operand they read) is a hit.
A path that returns with the value unread hands it to the caller's callers: they are followed
the same way (`via`), up to a readable void function, which has its own entry here.

  AI    every registration of AI (READABLE_FUNCTION), at its address in AI's ELF.
  DM2   READABLE_FUNCTION_IN(dm2, ...), and the AI registrations of the sources DM2's build has
        an instance of (games/dm2/instance_sources.txt and game.cmake's default list) whose level
        there is 2 (its body runs there: readable_symbols.py's Levels over the bind table), at
        DM2's address in DM2's ELF. Skipped, with a line, when its ELF, Ghidra map or bind table
        is not there.

Unknown, listed with --unknowns and counted, not in the exit status: a jalr no constant and no
recording resolves whose code after the call reads v0 or f0 (it may call one of the void
functions whose address the image holds), and a path that leaves through a jr the tool cannot
resolve as a jump table.

  readable_voids.py [--build BUILD | --drafts DIR] [--games ai,dm2] [--unknowns] [--bind FILE] [--dm2-root DIR]
      exit 1 when any void function has a caller that reads its result; 2 when AI's ELF or Ghidra
      map is missing. --build: the sweep's drafts that build wrote (runtime/readable_sweep) are
      registrations too.

Each hit is one line: game, function, its registration, the caller (and the functions the
value passed through), the reading instruction, and what the original returns.
"""
import argparse
import array
import bisect
import csv
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.dont_write_bytecode = True
sys.path.insert(0, HERE)
import ztree  # noqa: E402  (the z-axis tree checked: ztree.py)
import zaxis_paths  # noqa: E402  (tools/paths.env: the workspaces, the data, the work database)
try:   # the scratch folder: the tooling's own paths.env (and a workshop's own layout, when there)
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    import zpaths  # noqa: E402
    SCRATCH_ROOT = zpaths.ZAXIS_SCRATCH
except Exception:   # (a worker's copy of the checks alone: no tooling around them)
    SCRATCH_ROOT = os.path.expanduser('~')
import readable_decls  # noqa: E402
import readable_digest  # noqa: E402
import readable_symbols  # noqa: E402

TREE = ztree.TREE
GAME_SRC = readable_symbols.TREE   # (the tree: its readable code by keys)
import zaxis_paths  # noqa: E402  (the tree's readable folders: walk, key_of, readable)
# game.cmake's ZX_GAME_INSTANCE_SOURCES_dm2: the default build's DM2 instance.
DEFAULT_INSTANCE_DM2 = ['zge/text/cstring.cpp', 'zge/container/heap.cpp', 'zge/container/tree.cpp',
                        'sce/libc.cpp', 'sce/libgcc.cpp']

V0, F0 = 1, 2           # the tracked registers, as bits of a path's state
NAMES = {V0: 'v0', F0: 'f0'}
GPR = ['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3', 't0', 't1', 't2', 't3', 't4', 't5', 't6', 't7',
       's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7', 't8', 't9', 'k0', 'k1', 'gp', 'sp', 'fp', 'ra']


# ---- the instructions ------------------------------------------------------------------------

class Ins:
    """One instruction: what it reads and writes of v0 and f0 (bits V0, F0), and its control."""
    __slots__ = ('reads', 'writes', 'ctl', 'target', 'rs', 'likely', 'text')

    def __init__(self, reads=0, writes=0, ctl=None, target=None, rs=0, likely=False, text=''):
        self.reads, self.writes, self.ctl, self.target, self.rs, self.likely, self.text = \
            reads, writes, ctl, target, rs, likely, text


def _g(*regs):
    return V0 if 2 in regs else 0


def _f(*regs):
    return F0 if 0 in regs else 0


SPECIAL_RD_RS_RT = {0x14: 'dsllv', 0x16: 'dsrlv', 0x17: 'dsrav', 0x04: 'sllv', 0x06: 'srlv', 0x07: 'srav',
                    0x20: 'add', 0x21: 'addu', 0x22: 'sub', 0x23: 'subu', 0x24: 'and', 0x25: 'or', 0x26: 'xor',
                    0x27: 'nor', 0x2a: 'slt', 0x2b: 'sltu', 0x2c: 'dadd', 0x2d: 'daddu', 0x2e: 'dsub', 0x2f: 'dsubu'}
SPECIAL_SHIFT = {0x00: 'sll', 0x02: 'srl', 0x03: 'sra', 0x38: 'dsll', 0x3a: 'dsrl', 0x3b: 'dsra',
                 0x3c: 'dsll32', 0x3e: 'dsrl32', 0x3f: 'dsra32'}
IMM_OPS = {0x08: 'addi', 0x09: 'addiu', 0x0a: 'slti', 0x0b: 'sltiu', 0x0c: 'andi', 0x0d: 'ori', 0x0e: 'xori',
           0x18: 'daddi', 0x19: 'daddiu'}
LOADS = {0x20: 'lb', 0x21: 'lh', 0x22: 'lwl', 0x23: 'lw', 0x24: 'lbu', 0x25: 'lhu', 0x26: 'lwr', 0x27: 'lwu',
         0x1a: 'ldl', 0x1b: 'ldr', 0x37: 'ld', 0x1e: 'lq'}
STORES = {0x28: 'sb', 0x29: 'sh', 0x2a: 'swl', 0x2b: 'sw', 0x2c: 'sdl', 0x2d: 'sdr', 0x2e: 'swr', 0x3f: 'sd',
          0x1f: 'sq'}
BRANCHES = {0x04: 'beq', 0x05: 'bne', 0x06: 'blez', 0x07: 'bgtz', 0x14: 'beql', 0x15: 'bnel', 0x16: 'blezl',
            0x17: 'bgtzl'}
FPU_S = {0: 'add.s', 1: 'sub.s', 2: 'mul.s', 3: 'div.s', 4: 'sqrt.s', 5: 'abs.s', 6: 'mov.s', 7: 'neg.s',
         0x16: 'rsqrt.s', 0x18: 'adda.s', 0x19: 'suba.s', 0x1a: 'mula.s', 0x1c: 'madd.s', 0x1d: 'msub.s',
         0x1e: 'madda.s', 0x1f: 'msuba.s', 0x24: 'cvt.w.s', 0x28: 'max.s', 0x29: 'min.s'}


def _simm(imm):
    return imm - 0x10000 if imm & 0x8000 else imm


def decode(w, pc):
    op, rs, rt = w >> 26, (w >> 21) & 31, (w >> 16) & 31
    rd, sa, fn, imm = (w >> 11) & 31, (w >> 6) & 31, w & 63, w & 0xFFFF
    branch_to = (pc + 4 + (_simm(imm) << 2)) & 0xFFFFFFFF
    if op == 0:
        if fn in SPECIAL_SHIFT:
            if w == 0:
                return Ins(text='nop')
            return Ins(_g(rt), _g(rd), text='%s %s,%s,%d' % (SPECIAL_SHIFT[fn], GPR[rd], GPR[rt], sa))
        if fn in SPECIAL_RD_RS_RT:
            return Ins(_g(rs, rt), _g(rd), text='%s %s,%s,%s' % (SPECIAL_RD_RS_RT[fn], GPR[rd], GPR[rs], GPR[rt]))
        if fn == 0x08:
            return Ins(_g(rs), ctl='jr', rs=rs, text='jr %s' % GPR[rs])
        if fn == 0x09:
            return Ins(_g(rs), _g(rd), ctl='jalr', rs=rs, text='jalr %s' % GPR[rs])
        if fn in (0x0a, 0x0b):   # movz/movn: rd keeps its value when the condition fails
            return Ins(_g(rs, rt), 0, text='%s %s,%s,%s' % ('movz' if fn == 0x0a else 'movn', GPR[rd], GPR[rs], GPR[rt]))
        if fn == 0x0c:           # the kernel's result is in v0
            return Ins(0, V0, text='syscall')
        if fn in (0x0d, 0x0f):
            return Ins(text='break' if fn == 0x0d else 'sync')
        if fn in (0x10, 0x12, 0x28):
            return Ins(0, _g(rd), text='%s %s' % ({0x10: 'mfhi', 0x12: 'mflo', 0x28: 'mfsa'}[fn], GPR[rd]))
        if fn in (0x11, 0x13, 0x29):
            return Ins(_g(rs), 0, text='%s %s' % ({0x11: 'mthi', 0x13: 'mtlo', 0x29: 'mtsa'}[fn], GPR[rs]))
        if fn in (0x18, 0x19):
            return Ins(_g(rs, rt), _g(rd), text='%s %s,%s,%s' % ('mult' if fn == 0x18 else 'multu', GPR[rd], GPR[rs], GPR[rt]))
        if fn in (0x1a, 0x1b):
            return Ins(_g(rs, rt), 0, text='%s %s,%s' % ('div' if fn == 0x1a else 'divu', GPR[rs], GPR[rt]))
        if 0x30 <= fn <= 0x36:
            return Ins(_g(rs, rt), 0, text='trap %s,%s' % (GPR[rs], GPR[rt]))
        return Ins(_g(rs, rt), _g(rd), text='special.%02x %s,%s,%s' % (fn, GPR[rd], GPR[rs], GPR[rt]))
    if op == 1:
        if rt in (0, 1, 2, 3):
            name = ['bltz', 'bgez', 'bltzl', 'bgezl'][rt]
            always = rt == 1 and rs == 0
            return Ins(_g(rs), ctl='b' if always else 'br', target=branch_to, likely=rt in (2, 3),
                       text='%s %s,0x%x' % (name, GPR[rs], branch_to))
        if rt in (0x10, 0x11, 0x12, 0x13):
            return Ins(_g(rs), V0, ctl='jal', target=branch_to, text='bal 0x%x' % branch_to)
        return Ins(_g(rs), 0, text='regimm.%02x %s' % (rt, GPR[rs]))
    if op in (2, 3):
        target = ((pc + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
        return Ins(ctl='j' if op == 2 else 'jal', target=target, text='%s 0x%x' % ('j' if op == 2 else 'jal', target))
    if op in BRANCHES:
        two = op in (0x04, 0x05, 0x14, 0x15)
        always = op == 0x04 and rs == 0 and rt == 0
        reads = _g(rs, rt) if two else _g(rs)
        args = '%s,%s' % (GPR[rs], GPR[rt]) if two else GPR[rs]
        return Ins(reads, ctl='b' if always else 'br', target=branch_to, likely=op >= 0x14,
                   text='%s %s,0x%x' % (BRANCHES[op], args, branch_to))
    if op in IMM_OPS:
        return Ins(_g(rs), _g(rt), text='%s %s,%s,%d' % (IMM_OPS[op], GPR[rt], GPR[rs], _simm(imm)))
    if op == 0x0f:
        return Ins(0, _g(rt), text='lui %s,0x%x' % (GPR[rt], imm))
    if op in LOADS:
        return Ins(_g(rs), _g(rt), text='%s %s,%d(%s)' % (LOADS[op], GPR[rt], _simm(imm), GPR[rs]))
    if op in STORES:
        return Ins(_g(rs, rt), 0, text='%s %s,%d(%s)' % (STORES[op], GPR[rt], _simm(imm), GPR[rs]))
    if op in (0x2f, 0x33, 0x36, 0x3e):   # cache, pref, lqc2, sqc2: the base
        return Ins(_g(rs), 0, text='op.%02x %d(%s)' % (op, _simm(imm), GPR[rs]))
    if op == 0x31:
        return Ins(_g(rs), _f(rt), text='lwc1 $f%d,%d(%s)' % (rt, _simm(imm), GPR[rs]))
    if op == 0x39:
        return Ins(_g(rs) | _f(rt), 0, text='swc1 $f%d,%d(%s)' % (rt, _simm(imm), GPR[rs]))
    if op == 0x10:   # COP0
        if rs == 0:
            return Ins(0, _g(rt), text='mfc0 %s' % GPR[rt])
        if rs == 4:
            return Ins(_g(rt), 0, text='mtc0 %s' % GPR[rt])
        if rs == 8:
            return Ins(ctl='br', target=branch_to, likely=bool(rt & 2), text='bc0 0x%x' % branch_to)
        if rs == 0x10 and fn == 0x18:
            return Ins(ctl='stop', text='eret')
        return Ins(text='cop0')
    if op == 0x11:   # COP1
        fs, fd, ft = rd, sa, rt
        if rs == 0:
            return Ins(_f(fs), _g(rt), text='mfc1 %s,$f%d' % (GPR[rt], fs))
        if rs == 2:
            return Ins(0, _g(rt), text='cfc1 %s' % GPR[rt])
        if rs == 4:
            return Ins(_g(rt), _f(fs), text='mtc1 %s,$f%d' % (GPR[rt], fs))
        if rs == 6:
            return Ins(_g(rt), 0, text='ctc1 %s' % GPR[rt])
        if rs == 8:
            return Ins(ctl='br', target=branch_to, likely=bool(rt & 2),
                       text='bc1%s%s 0x%x' % ('t' if rt & 1 else 'f', 'l' if rt & 2 else '', branch_to))
        if rs == 0x10:
            if fn >= 0x30:
                return Ins(_f(fs, ft), 0, text='c.s $f%d,$f%d' % (fs, ft))
            name = FPU_S.get(fn, 'cop1.s.%02x' % fn)
            if fn == 4:
                return Ins(_f(ft), _f(fd), text='%s $f%d,$f%d' % (name, fd, ft))
            if fn in (5, 6, 7, 0x24):
                return Ins(_f(fs), _f(fd), text='%s $f%d,$f%d' % (name, fd, fs))
            if fn in (0x18, 0x19, 0x1a, 0x1e, 0x1f):
                return Ins(_f(fs, ft), 0, text='%s $f%d,$f%d' % (name, fs, ft))
            return Ins(_f(fs, ft), _f(fd), text='%s $f%d,$f%d,$f%d' % (name, fd, fs, ft))
        if rs == 0x14:
            return Ins(_f(fs), _f(fd), text='cvt.s.w $f%d,$f%d' % (fd, fs))
        return Ins(_f(fs, ft), _f(fd), text='cop1.%02x' % rs)
    if op == 0x12:   # COP2 (VU0 macro)
        if rs in (1, 2):
            return Ins(0, _g(rt), text='%s %s' % ('qmfc2' if rs == 1 else 'cfc2', GPR[rt]))
        if rs in (5, 6):
            return Ins(_g(rt), 0, text='%s %s' % ('qmtc2' if rs == 5 else 'ctc2', GPR[rt]))
        if rs == 8:
            return Ins(ctl='br', target=branch_to, likely=bool(rt & 2), text='bc2 0x%x' % branch_to)
        return Ins(text='vu0')
    if op == 0x1c:   # MMI: rd from rs and rt, the few that only move hi/lo aside
        if fn in (0x10, 0x12, 0x30):
            return Ins(0, _g(rd), text='mmi.%02x %s' % (fn, GPR[rd]))
        if fn in (0x11, 0x13, 0x31):
            return Ins(_g(rs), 0, text='mmi.%02x %s' % (fn, GPR[rs]))
        if fn in (0x1a, 0x1b):
            return Ins(_g(rs, rt), 0, text='mmi.%02x %s,%s' % (fn, GPR[rs], GPR[rt]))
        return Ins(_g(rs, rt), _g(rd), text='mmi.%02x.%02x %s,%s,%s' % (fn, sa, GPR[rd], GPR[rs], GPR[rt]))
    return Ins(ctl='bad', text='.word 0x%08x' % w)


# ---- a game's image --------------------------------------------------------------------------

class Image:
    def __init__(self, game, elf_path, ghidra_csv, names, indirect=None):
        self.game = game
        self.elf = readable_symbols.Elf(elf_path)
        addr, off, size, _ = self.elf.sections['.text']
        self.lo, self.hi = addr, addr + size
        self.words = array.array('I')
        self.words.frombytes(self.elf.bytes[off:off + size])
        if sys.byteorder != 'little':
            self.words.byteswap()
        self._dec = {}
        # functions: Ghidra's map without its entry_ rows (switch labels and split blocks)
        self.funcs = {}
        with open(ghidra_csv) as f:
            for row in csv.DictReader(f):
                if row['Name'].startswith('entry_'):
                    continue
                s, e = int(row['Start'], 16), int(row['End'], 16)
                self.funcs[s] = (e, row['Name'])
        self.starts = sorted(self.funcs)
        self.names = names
        self._index(indirect or {})

    def word(self, pc):
        if self.lo <= pc < self.hi:
            return self.words[(pc - self.lo) >> 2]
        return None

    def ins(self, pc):
        d = self._dec.get(pc)
        if d is None:
            w = self.word(pc)
            d = decode(w, pc) if w is not None else Ins(ctl='bad', text='outside .text')
            self._dec[pc] = d
        return d

    def function_of(self, pc):
        i = bisect.bisect_right(self.starts, pc) - 1
        return self.starts[i] if i >= 0 else None

    def end_of(self, start):
        return self.funcs.get(start, (start + 4, ''))[0]

    def name(self, address):
        n = self.names(address)
        if n:
            return n
        f = self.funcs.get(address)
        return f[1] if f else '0x%06X' % address

    def where(self, pc):
        f = self.function_of(pc)
        return '%s+0x%x' % (self.name(f), pc - f) if f is not None else '0x%06X' % pc

    def _index(self, indirect):
        """calls[target] = [jal or resolved jalr sites], tails[target] = [j, branch or jr sites from
        another function]; unresolved: the jalr sites nothing resolves; taken: addresses an
        instruction pair or a data word holds."""
        self.calls, self.tails, self.unresolved = {}, {}, []
        self.taken = set()
        hi = {}
        words = self.words
        for i in range(len(words)):
            w = words[i]
            pc = self.lo + 4 * i
            op = w >> 26
            if op in (2, 3):
                target = ((pc + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
                if op == 3:
                    self.calls.setdefault(target, []).append(pc)
                elif target in self.funcs and self.function_of(pc) != target:
                    self.tails.setdefault(target, []).append(pc)
                hi.clear()
                continue
            if op == 0x0f:
                hi[(w >> 16) & 31] = (w & 0xFFFF) << 16
                continue
            if op in (0x09, 0x0d) and ((w >> 21) & 31) in hi:
                rs = (w >> 21) & 31
                v = (hi[rs] + (_simm(w & 0xFFFF) if op == 0x09 else (w & 0xFFFF))) & 0xFFFFFFFF
                if v in self.funcs:
                    self.taken.add(v)
            if op in (0x04, 0x05, 0x14, 0x15) and (w >> 16) & 31 == 0 and (w >> 21) & 31 == 0:
                target = (pc + 4 + (_simm(w & 0xFFFF) << 2)) & 0xFFFFFFFF
                if target in self.funcs and self.function_of(pc) != target:
                    self.tails.setdefault(target, []).append(pc)
            if op == 0 and (w & 63) in (8, 9) and ((w >> 21) & 31) != 31:
                link = (w & 63) == 9
                seen = indirect.get(pc)
                constant = self._constant(pc, (w >> 21) & 31)
                targets = set(seen or ()) | ({constant} if constant is not None else set())
                for t in targets:
                    (self.calls if link else self.tails).setdefault(t, []).append(pc)
                if link and not targets:
                    self.unresolved.append(pc)
        # data words that are function starts
        for name in ('.data', '.rodata', '.sdata'):
            sec = self.elf.sections.get(name)
            if not sec:
                continue
            a = array.array('I')
            a.frombytes(self.elf.bytes[sec[1]:sec[1] + (sec[2] & ~3)])
            if sys.byteorder != 'little':
                a.byteswap()
            self.taken.update(v for v in a if v in self.funcs)

    def _constant(self, pc, reg):
        """The address a jalr's register holds when lui and addiu or ori set it just before."""
        value, low = None, None
        for k in range(1, 12):
            w = self.word(pc - 4 * k)
            if w is None:
                return None
            op = w >> 26
            rt = (w >> 16) & 31
            if op in (0x09, 0x0d) and rt == reg and (w >> 21) & 31 == reg and low is None:
                low = _simm(w & 0xFFFF) if op == 0x09 else (w & 0xFFFF)
                continue
            if op == 0x0f and rt == reg:
                if low is None:
                    return None
                value = (((w & 0xFFFF) << 16) + low) & 0xFFFFFFFF
                return value if value in self.funcs else None
            d = decode(w, pc - 4 * k)
            if d.ctl is not None or (op == 0 and (w >> 11) & 31 == reg) or (op not in (0, 0x1c) and rt == reg and op not in STORES):
                return None
        return None

    def jump_table(self, pc, reg):
        """The cases of a switch's jr: lui r,hi; [addiu r,r,lo;] addu x,idx,r; lw reg,lo(x); jr reg
        (the table's length from the sltiu before it when there is one), or None."""
        f = self.function_of(pc)
        if f is None:
            return None
        end = self.end_of(f)
        loaded, regs, hi, low, bound = False, set(), None, 0, None
        for k in range(1, 24):
            q = pc - 4 * k
            if q < f:
                break
            w = self.word(q)
            op, rs, rt, rd = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
            if not loaded:
                if op == 0x23 and rt == reg:
                    loaded, regs, low = True, {rs}, _simm(w & 0xFFFF)
                continue
            if hi is not None:
                if op == 0x0b and bound is None:   # sltiu x, idx, N
                    bound = w & 0xFFFF
                continue
            if op == 0 and (w & 63) == 0 and rd in regs:   # sll idx: the index, not the table
                regs.discard(rd)
            elif op == 0 and (w & 63) in (0x21, 0x2d) and rd in regs:
                regs |= {rs, rt}
            elif op == 0x09 and rt in regs:
                low += _simm(w & 0xFFFF)
                regs.add(rs)
            elif op == 0x0f and rt in regs and hi is None:
                hi = (w & 0xFFFF) << 16
            elif op == 0x0b and bound is None:   # sltiu x, idx, N
                bound = w & 0xFFFF
        if hi is None:
            return None
        table = (hi + low) & 0xFFFFFFFF
        out = []
        for i in range(bound if bound else 1024):
            d = self.elf.data(table + 4 * i, 4)
            if d is None:
                break
            v = int.from_bytes(d, 'little')
            if not (f <= v < end if bound is None else self.lo <= v < self.hi):
                break
            out.append(v)
        return out or None


# ---- the dataflow ----------------------------------------------------------------------------

class Outcome:
    __slots__ = ('reads', 'returns', 'unknown')

    def __init__(self):
        self.reads = []      # (bit, pc, text)
        self.returns = 0     # bits still unread at a return
        self.unknown = []    # (pc, why)


def after_call(img, start, state=V0 | F0, limit=6000):
    """From the instruction after a call's delay slot: every path until v0 and f0 are written,
    a call is reached, or the function returns."""
    out = Outcome()
    seen = set()
    todo = [(start, state)]
    steps = 0

    def slot(pc, s):
        d = img.ins(pc + 4)
        r = d.reads & s
        if r:
            for b in (V0, F0):
                if r & b:
                    out.reads.append((b, pc + 4, d.text))
            s &= ~r
        return s & ~d.writes

    while todo:
        pc, s = todo.pop()
        if not s or (pc, s) in seen:
            continue
        seen.add((pc, s))
        steps += 1
        if steps > limit:
            out.unknown.append((pc, 'too long'))
            break
        d = img.ins(pc)
        r = d.reads & s
        if r:
            for b in (V0, F0):
                if r & b:
                    out.reads.append((b, pc, d.text))
            s &= ~r
            if not s:
                continue
        c = d.ctl
        if c is None:
            todo.append((pc + 4, s & ~d.writes))
        elif c == 'br':
            after = slot(pc, s)
            todo.append((d.target, after))
            todo.append((pc + 8, s if d.likely else after))
        elif c in ('b', 'j'):
            todo.append((d.target, slot(pc, s)))
        elif c in ('jal', 'jalr'):
            slot(pc, s)
        elif c == 'jr':
            after = slot(pc, s)
            if d.rs == 31:
                out.returns |= after
            else:
                cases = img.jump_table(pc, d.rs)
                if cases is None:
                    if after:
                        out.unknown.append((pc, 'jr %s with no table' % GPR[d.rs]))
                else:
                    todo.extend((t, after) for t in cases)
        elif c == 'stop':
            continue
        else:
            out.unknown.append((pc, d.text))
    return out


class Game:
    def __init__(self, img, regs):
        self.img = img
        self.regs = regs           # address -> [registration dict]
        self._passes = {}

    def readable_void(self, address):
        rs = self.regs.get(address)
        return bool(rs) and all(r['void'] for r in rs)

    def site_reads(self, pc, bits, tail=False):
        """[(bit, caller function, call site, read pc, text, chain)], [(pc, why)] for one call site;
        chain: the functions that returned the value unread on the way, nearest first."""
        img = self.img
        caller = img.function_of(pc)
        hits, unknown = [], []
        returns = bits
        if not tail:
            o = after_call(img, pc + 8, bits)
            hits = [(b, caller, pc, rpc, text, ()) for b, rpc, text in o.reads]
            unknown = [(pc, upc, why) for upc, why in o.unknown]
            returns = o.returns
        if returns:
            h, u = self.passes(caller, returns)
            hits += [x[:5] + ((caller,) + x[5],) for x in h]
            unknown += u
        return hits, unknown

    def passes(self, function, bits):
        """What the callers of a function that returns the value unread read of it."""
        if function is None or self.readable_void(function):
            return [], []     # (a readable void function: its own entry lists them)
        key = (function, bits)
        if key in self._passes:
            return self._passes[key]
        self._passes[key] = ([], [])     # (a cycle reads nothing new)
        self._passes[key] = self.callers(function, bits)
        return self._passes[key]

    def callers(self, address, bits):
        hits, unknown = [], []
        for pc in self.img.calls.get(address, ()):
            h, u = self.site_reads(pc, bits)
            hits += h
            unknown += u
        for pc in self.img.tails.get(address, ()):
            h, u = self.site_reads(pc, bits, tail=True)
            hits += h
            unknown += u
        return hits, unknown


# ---- the registrations -----------------------------------------------------------------------

def source_files(drafts=None):
    """[(path, label)]: src/game's sources, and the sweep's drafts a build wrote."""
    out = []
    for root, tag in ((GAME_SRC, ''), (drafts, 'sweep: ')):
        if not root:
            continue
        for d, dirs, files in ztree.walk(root):     # (the tree: its readable code)
            for f in sorted(files):
                if f.endswith(('.cpp', '.h')):
                    p = os.path.join(d, f)
                    rel = ztree.key_of(p, root) if ztree.is_tree(root) else os.path.relpath(p, root)
                    out.append((p, tag + rel.replace(os.sep, '/')))
    return out


def return_types(files):
    """{(file, short name): set of normalised return types} of the definitions in the sources."""
    scan = readable_decls.Scan()
    for p, label in files:
        scan.file(p, p, True)
    norm = readable_decls.Norm(scan.aliases)
    out, anywhere = {}, {}
    for e in scan.entries:
        if not e['body']:
            continue
        short = e['name'].split('::')[-1]
        r = norm.ret(e['ret'])
        out.setdefault((e['file'], short), set()).add(r)
        anywhere.setdefault(short, set()).add(r)
    return out, anywhere


PREFIX_DROP = {'static', 'inline', 'constexpr', 'extern', 'virtual', 'explicit', 'friend', 'FORCE_INLINE',
               '__forceinline', 'nodiscard', 'maybe_unused', 'noinline', 'public', 'private', 'protected'}


def fallback_return(path, short):
    """The return type of a definition readable_decls leaves out (static, a member): the words
    before its name back to the last ; { or }."""
    code = readable_digest.strip(open(path, encoding='utf-8', errors='replace').read())
    found = set()
    for m in re.finditer(r'\b%s\s*\(' % re.escape(short), code):
        q = readable_digest.match(code, m.end() - 1, '(', ')')
        if q < 0 or not re.match(r'\s*(?:const\s*|noexcept\s*|override\s*|->\s*[\w:<>,\s*&]+?\s*)*\{', code[q:q + 200]):
            continue
        before = code[max(0, m.start() - 300):m.start()]
        before = re.split(r'[;{}]', before)[-1]
        before = '\n'.join(l for l in before.split('\n') if not l.lstrip().startswith('#'))
        before = re.sub(r'(\w+::)+$', '', before.strip())
        words = [x for x in re.findall(r'[A-Za-z_][\w:]*|[*&<>]', before) if x not in PREFIX_DROP and x != ':']
        if words == ['auto']:
            t = re.match(r'\s*->\s*([\w:<>\s*&]+?)\s*\{', code[q:q + 200])
            words = t.group(1).split() if t else words
        if words:
            found.add(' '.join(words))
    return found


def registrations(files, rets, anywhere):
    """[{game, addr (that game's), name, file, status, void, type}] of every registration but Planned."""
    rows = []
    for p, label in files:
        if not p.endswith('.cpp'):
            continue
        for r in readable_digest.file_digests(p):
            short = r['name'].split('::')[-1]
            types = rets.get((p, short))
            if not types:
                types = fallback_return(p, short) or anywhere.get(short, set())
            r = dict(r, file=label, types=types,
                     void=types == {'void'}, type=' | '.join(sorted(types)) or '?')
            rows.append(r)
    return rows


def dm2_rows(rows, bind_path):
    """DM2's registrations at DM2's addresses: its own, and AI's whose body runs there."""
    out = [dict(r) for r in rows if r['game'] == 'dm2']
    t = readable_symbols.table()
    bind = readable_symbols.Bind(bind_path)
    levels = readable_symbols.Levels(bind, t, GAME_SRC, 'dm2')
    instance = set(DEFAULT_INSTANCE_DM2)
    listed = ztree.readable(GAME_SRC, 'dm2', 'instance_sources.txt')
    if os.path.exists(listed):
        instance |= {l.strip() for l in open(listed, encoding='utf-8') if re.match(r'^[a-z]', l)}
    by_ai = {}
    for r in rows:
        if r['game'] == 'ai':
            by_ai.setdefault((r['file'], r['addr']), r)
    for name, level in levels.level.items():
        rel = levels.files.get(name)
        if level != 2 or rel not in instance or name not in t.syms:
            continue
        ai = t.syms[name][1]
        r = by_ai.get((rel, ai))
        address = readable_symbols.resolve(bind, t, name)
        if r is None or address is None:
            continue
        out.append(dict(r, game='dm2', addr=address, ai=ai))
    return out


def indirect_table(path):
    out = {}
    if os.path.exists(path):
        for line in open(path, encoding='utf-8'):
            p = line.split()
            if len(p) >= 2 and not line.startswith('#'):
                out.setdefault(int(p[0], 16), set()).add(int(p[1], 16))
    return out


def what_it_returns(img, address, bit):
    """The instructions that set the result on the original's returns."""
    found = []
    end = img.end_of(address)
    pc = address
    while pc < end:
        d = img.ins(pc)
        if d.ctl == 'jr' and d.rs == 31:
            slot = img.ins(pc + 4)
            text = None
            if slot.writes & bit:
                text = slot.text
            else:
                q = pc - 4
                while q >= address and q > pc - 4 * 40:
                    e = img.ins(q)
                    if e.ctl in ('jal', 'jalr'):
                        text = '%s\'s result' % (img.name(e.target) if e.target is not None else e.text)
                        break
                    if e.writes & bit:
                        text = e.text
                        break
                    q -= 4
            found.append(text or 'set before a branch')
        elif d.ctl == 'j' and d.target in img.funcs and d.target != address:
            found.append('tail call %s' % img.name(d.target))
        pc += 4
    seen, out = set(), []
    for t in found:
        if t not in seen:
            seen.add(t)
            out.append(t)
    return out or ['no return (falls through or never returns)']


def check_game(game, img, regs, args, report):
    by_addr = {}
    for r in regs:
        by_addr.setdefault(r['addr'], []).append(r)
    g = Game(img, by_addr)
    voids = sorted(a for a, rs in by_addr.items() if all(r['void'] for r in rs))
    hits = 0
    functions = 0
    unknown, unknown_seen = [], set()
    for a in voids:
        r = by_addr[a][0]
        h, u = g.callers(a, V0 | F0)
        for x in u:
            if (a,) + x not in unknown_seen:
                unknown_seen.add((a,) + x)
                unknown.append((a,) + x)
        if not h:
            continue
        functions += 1
        uniq = {}
        for b, caller, site, rpc, text, chain in h:
            uniq.setdefault((b, site, rpc), (b, caller, site, rpc, text, chain))
        bits = 0
        for b, *_ in uniq.values():
            bits |= b
        returns = '; '.join('%s: %s' % (NAMES[b], ', '.join(what_it_returns(img, a, b))) for b in (V0, F0) if bits & b)
        report.append('%s %s (0x%06X, %s, %s): %d reading site(s); the original returns %s' % (
            game, r['name'], a, r['status'], r['file'], len(uniq), returns))
        for b, caller, site, rpc, text, chain in sorted(uniq.values(), key=lambda x: (x[2], x[3])):
            hits += 1
            creg = g.regs.get(caller)
            cstat = ' [readable %s %s]' % (creg[0]['name'], creg[0]['status']) if creg else ''
            via = (' via ' + ' > '.join(img.name(x) for x in chain)) if chain else ''
            report.append('    caller %s%s calls at 0x%06X%s, reads %s at 0x%06X: %s' % (
                img.name(caller), cstat, site, via, NAMES[b], rpc, text))
    # unresolved jalr sites that read the result: they may call a void function whose address is taken
    taken = [a for a in voids if a in img.taken]
    blind = []
    if taken:
        for pc in img.unresolved:
            o = after_call(img, pc + 8)
            if o.reads:
                b, rpc, text = o.reads[0]
                blind.append((pc, rpc, NAMES[b], text))
    return functions, hits, voids, unknown, taken, blind


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    workspace = zaxis_paths.get('AI_WORKSPACE', required=False)
    ap.add_argument('--games', default='ai,dm2')
    ap.add_argument('--workspace', default=workspace, help='the bring-up workspace (recomp/, extracted/)')
    ap.add_argument('--elf', help="AI's ELF (default: recomp/config.game.toml's input)")
    ap.add_argument('--dm2-root', help="DM2's workspace (default: $ZX_DM2_ROOT, else dm2 beside "
                    "the workspace)")
    ap.add_argument('--bind', help="DM2's bind table (default: $ZX_BIND_DIR/dm2.tsv, else the tree's "
                    "tools/readable/bind/dm2.tsv, else $ZAXIS_SCRATCH/workdb/bind/dm2.tsv)")
    ap.add_argument('--build', help="a build dir: its sweep drafts (runtime/readable_sweep) are checked too")
    ap.add_argument('--drafts', help='the sweep drafts folder itself')
    ap.add_argument('--unknowns', action='store_true', help='list the unknowns')
    a = ap.parse_args()
    games = a.games.split(',')
    a.dm2_root = a.dm2_root or os.environ.get('ZX_DM2_ROOT') or \
        os.path.join(os.path.dirname(os.path.abspath(a.workspace)), 'dm2')
    drafts = a.drafts or (os.path.join(a.build, 'runtime', 'readable_sweep') if a.build else None)
    if drafts and not os.path.isdir(drafts):
        print('readable_voids: no sweep drafts at %s' % drafts)
        return 2
    if drafts and os.path.exists(os.path.join(drafts, readable_symbols.EXTRA_ROWS)):
        os.environ['READABLE_SYMBOLS_EXTRA'] = os.path.join(drafts, readable_symbols.EXTRA_ROWS)
    files = source_files(drafts)
    rets, anywhere = return_types(files)
    rows = registrations(files, rets, anywhere)
    status = 0
    for game in games:
        report = []
        if game == 'ai':
            elf = a.elf
            if not elf:
                cfg = os.path.join(a.workspace, 'recomp', 'config.game.toml')
                if os.path.exists(cfg):
                    m = re.search(r'^input\s*=\s*"(.*)"', open(cfg).read(), re.M)
                    elf = m.group(1) if m else None
                elf = elf or os.path.join(a.workspace, 'extracted', 'SLUS_203.27')
            ghidra = os.path.join(a.workspace, 'recomp', 'ghidra-functions.csv')
            if not (os.path.exists(elf) and os.path.exists(ghidra)):
                print('readable_voids: ai: no ELF or Ghidra map (%s, %s)' % (elf, ghidra))
                return 2
            t = readable_symbols.table()
            img = Image('ai', elf, ghidra, lambda x: t.by_address.get(('function', x)),
                        indirect_table(os.path.join(ztree.TOOLS, 'readable_indirect_calls.tsv')))
            regs = [r for r in rows if r['game'] == 'ai']
        elif game == 'dm2':
            elf = os.path.join(a.dm2_root, 'extracted', 'SLUS_201.59')
            ghidra = os.path.join(a.dm2_root, 'recomp', 'ghidra-functions.csv')
            # (the tree's own table, tools/readable/bind since tools39; the shared folder's for a tree from before)
            own = os.path.join(ztree.TOOLS, 'bind')
            bind = a.bind or os.path.join(os.environ.get('ZX_BIND_DIR') or (own if os.path.isfile(os.path.join(own, 'dm2.tsv'))
                                          else os.path.join(SCRATCH_ROOT, 'workdb', 'bind')), 'dm2.tsv')
            missing = [p for p in (elf, ghidra, bind) if not os.path.exists(p)]
            if missing:
                print('readable_voids: dm2: skipped, missing %s' % ', '.join(missing))
                continue
            regs = dm2_rows(rows, bind)
            t = readable_symbols.table()
            own = t.own('dm2')
            back = {}
            for ai, g in readable_symbols.Bind(bind).entries.items():
                back.setdefault(g, ai)

            def dm2_name(x, own=own, back=back, t=t):
                n = own.by_address.get(('function', x))
                if n:
                    return n
                ai = back.get(x)
                return t.by_address.get(('function', ai)) if ai is not None else None
            img = Image('dm2', elf, ghidra, dm2_name)
        else:
            print('readable_voids: no game %s' % game)
            return 2
        functions, hits, voids, unknown, taken, blind = check_game(game, img, regs, a, report)
        for line in report:
            print(line)
        print('readable_voids: %s: %d registrations, %d void; %d void function(s) with a reading caller, %d reading '
              'site(s); unknown: %d unresolved path(s) after a call, %d unresolved jalr site(s) that read the result '
              '(%d void function(s) have their address taken)' % (
                  game, len(regs), len(voids), functions, hits, len(unknown), len(blind), len(taken)))
        if a.unknowns:
            for f, site, pc, why in sorted(unknown):
                print('    unknown %s %s: the call at 0x%06X (%s) reaches 0x%06X: %s' % (
                    game, img.name(f), site, img.where(site), pc, why))
            for pc, rpc, reg, text in blind:
                print('    unknown %s jalr at 0x%06X (%s) reads %s at 0x%06X: %s' % (game, pc, img.where(pc), reg, rpc, text))
            for f in taken:
                print('    taken %s %s (0x%06X)' % (game, img.name(f), f))
        if hits:
            status = 1
    return status


if __name__ == '__main__':
    sys.exit(main())
