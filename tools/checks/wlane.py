#!/usr/bin/env python3
"""wlane.py: a leftover vector lane (the w of a 128-bit value, a quadword's upper half) followed through one game's
code, to where it is discarded or used. The proof a Tested function's A/B words need (docs/READABLE.md: a stale lane
the readable code leaves different, which the gate sees as RAM words differing).

  wlane.py trace --game dm2|bmx|ai SOURCE... [--depth N] [--fanout N] [--steps]
      static: the lane from its source through the recompiled code, registers, the frame, calls and returns, to
      every USE (a branch, an address, a call nothing here can see into, a hardware store) and every LANDING (a
      store of the lane into a record, a global, the scratchpad, or a frame word left below the stack pointer).
      SOURCE  after=PC LANE   the lane as instruction PC leaves it (PC in the function's code)
              entry=PC LANE   the lane as the function at PC is called
              ret=PC LANE     the lane as the call at site PC returns
              LANE: vf6.w  a3.hi  a3.2  f22  (as ZX_LANE_POKE's, runtime/lane_watch.h)
  wlane.py readers --game G WATCH            the lane watch's report (ZX_LANE_WATCH_OUT): each load of a watched
                                             word followed from its destination lane; each store and readable access listed
  wlane.py poke --game G --input I --to T --runner R --dir D --poke SPEC[,SPEC] --name NAME [--clock K] [--spec S]
                                             two runs with ZX_LANE_POKE forcing the lane to two values, compared
                                             (career_ab.py), the moved words watched in a third run and their readers
                                             followed: the verdict and the words, into D/NAME.lanes.txt
  wlane.py poke ... --diff                   the differential instead of the static follow's verdict (a ZX_LANE_WATCH_BUILD
                                             build whose generated code is wlane_branches.py's copy): the lane at
                                             DIFF_VALUES and the first value again, ZX_LANE_DIFF in every run; DEAD on
                                             the input when every run has the same control flow (each branch's
                                             directions, each register jump's targets), the same load and store
                                             addresses and the same calls as the first, nothing observable differs and
                                             the repeat is identical; the loads whose values differ (where the lane is
                                             read) head the lanes file as its proof. The static follow runs after it, as
                                             information.
  wlane.py case FILE                         a case file (`key value` lines: game, input, to, runner, dir, poke,
                                             trace, clock, spec, name): trace, then poke; the report into D/NAME.txt

The code is the recompiled functions' disassembly (the generated-ns folders' comments), cached under
$ZAXIS_SCRATCH/wlane/cache. What the model knows and what it does not is in MODEL below; every assumption the
verdict rests on is printed with it. A lane is dead only when nothing uses it and every landing is proven: a word
of a record is proven by its readers (the watch: every load that covers it, followed) and the poke (nothing
observable differs between the two values); a frame word left below sp by whoever reads it unset later (the same
walk, through the dead stack); a lane discarded in registers needs no more.
"""
import argparse
import collections
import glob
import hashlib
import os
import pickle
import re
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ztree  # noqa: E402  (the z-axis tree checked: ztree.py)
import zaxis_paths  # noqa: E402  (tools/paths.env: the workspaces, the data)

MODEL = """\
The model (what the verdict rests on):
- Registers: every GPR is four 32-bit words; 32- and 64-bit operations write words 0 and 1 and keep 2 and 3 (the
  R5900's, as the runtime's SET_GPR), lq/sq and the MMI operations move all four; VU0 registers are four lanes, an
  operation writes the lanes of its mask from its sources' lanes (a broadcast reads one lane of ft), ACC, Q, I, the
  MAC/status and clip flags are places too; FPU registers are 32-bit; HI/LO, the FPU condition likewise.
- Memory: a word of the function's frame is a place (sp-relative, through registers holding an address in the frame);
  a word reached through any other pointer is a LANDING (the lane left in a record, a global, the scratchpad) and,
  for loads through the same pointer value in the same function, a place. Two pointer values are two places: a
  record reached two ways in one function is not matched (its readers come from the watch, the dynamic side). A
  pointer argument reaches the words at and after it (a record's fields), never below it; memoryCopy and memcpy move
  the words within the size given (0x400 when the size is not a constant), memoryFill and memset clear them.
- Calls: a callee runs with the caller's registers and the frame words its pointer arguments reach (and the whole
  stack below the caller's sp: a callee reading a word of its own frame it never stored reads what the dead stack
  holds, the leftover's road); at its return every register is as it left it (a callee-saved register saved with sd
  and restored with ld keeps the upper half the callee left). A call to a function not in the game's table (the SDK's
  stubs, a pointer no table resolves) USES whatever reaches its argument registers' words 0 and 1 or a VU0 lane, and
  is noted for the upper halves. Recursion deeper than --depth and more callers than --fanout stop the walk: UNRESOLVED.
- USES: a branch or a conditional move on the lane, an address made from it, a jump through it, a syscall argument, a
  store to a hardware register (0x10000000..0x1FFFFFFF), a VU0 microprogram run (vcallms) while a VU0 place holds it,
  any instruction the model does not know reading it. A store is never a use by itself: it is a LANDING.
- Not modelled: pointers stored in memory and reloaded (the reload is a new value: the landing's readers are the
  watch's job), the VU0 data memory beyond one place (vsqi/vlqi), DMA (a landing in a packet is observable only
  through the probe's stream hashes, which the poke compares).
"""

LOADS = {'lb': 1, 'lbu': 1, 'lh': 2, 'lhu': 2, 'lw': 4, 'lwu': 4, 'ld': 8, 'lq': 16, 'lwc1': 4, 'lqc2': 16,
         'lwl': 4, 'lwr': 4, 'ldl': 8, 'ldr': 8}
STORES = {'sb': 1, 'sh': 2, 'sw': 4, 'sd': 8, 'sq': 16, 'swc1': 4, 'sqc2': 16, 'swl': 4, 'swr': 4, 'sdl': 8, 'sdr': 8}
CONDITIONAL = {'beq', 'bne', 'beqz', 'bnez', 'bltz', 'blez', 'bgez', 'bgtz', 'bc0f', 'bc0t', 'bc1f', 'bc1t', 'bc2f', 'bc2t'}
LIKELY = {'beql', 'bnel', 'beqzl', 'bnezl', 'bltzl', 'blezl', 'bgezl', 'bgtzl', 'bc0fl', 'bc0tl', 'bc1fl', 'bc1tl',
          'bc2fl', 'bc2tl'}
ABI = ['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3', 't0', 't1', 't2', 't3', 't4', 't5', 't6', 't7', 's0', 's1',
       's2', 's3', 's4', 's5', 's6', 's7', 't8', 't9', 'k0', 'k1', 'gp', 'sp', 'fp', 'ra']
REG = {'$' + n: i for i, n in enumerate(ABI)}
REG.update({'$r%d' % i: i for i in range(32)})
REG['$s8'] = 30
ARG_REGS = (4, 5, 6, 7, 8, 9, 10, 11)
# ALU operations on GPRs: words 0 and 1 of rd from words 0 and 1 of the sources.
ALU3 = {'addu', 'add', 'daddu', 'dadd', 'subu', 'sub', 'dsubu', 'dsub', 'and', 'or', 'xor', 'nor', 'slt', 'sltu',
        'sllv', 'srlv', 'srav', 'dsllv', 'dsrlv', 'dsrav', 'movz', 'movn', 'mult', 'multu', 'div', 'divu', 'mult1',
        'multu1', 'div1', 'divu1', 'madd', 'maddu', 'madd1', 'maddu1'}
ALU2I = {'addiu', 'addi', 'daddiu', 'daddi', 'andi', 'ori', 'xori', 'slti', 'sltiu', 'sll', 'srl', 'sra', 'dsll',
         'dsrl', 'dsra', 'dsll32', 'dsrl32', 'dsra32'}
MMI_LANEWISE = {'paddw', 'psubw', 'paddh', 'psubh', 'paddb', 'psubb', 'paddsw', 'paddsh', 'paddsb', 'padduw',
                'padduh', 'paddub', 'psubsw', 'psubsh', 'psubsb', 'psubuw', 'psubuh', 'psubub', 'pand', 'por', 'pxor',
                'pnor', 'pcgtw', 'pcgth', 'pcgtb', 'pceqw', 'pceqh', 'pceqb', 'pmaxw', 'pmaxh', 'pminw', 'pminh',
                'psllh', 'psrah', 'psrlh', 'psllw', 'psraw', 'psrlw', 'psllvw', 'psrlvw', 'psravw', 'pabsw', 'pabsh',
                'padsbh'}
VU_OPS = ['vopmula', 'vopmsub', 'vmadda', 'vmsuba', 'vmadd', 'vmsub', 'vadda', 'vsuba', 'vmula', 'vmul', 'vadd', 'vsub',
          'vmaxi', 'vmax', 'vmini', 'vmove', 'vmr32', 'vabs', 'vftoi0', 'vftoi4', 'vftoi12', 'vftoi15', 'vitof0',
          'vitof4', 'vitof12', 'vitof15', 'vclip', 'vlqi', 'vsqi', 'vlqd', 'vsqd', 'vmfir', 'vmtir', 'vdiv', 'vsqrt',
          'vrsqrt', 'vwaitq', 'vnop', 'viaddi', 'viadd', 'visub', 'viand', 'vior', 'vilwr', 'viswr', 'vrnext',
          'vrget', 'vrinit', 'vrxor', 'vcallmsr', 'vcallms']
VU_OPS.sort(key=len, reverse=True)
LANES = 'xyzw'
MEMORY = re.compile(r'^(-?(?:0x[0-9A-Fa-f]+|\d+))\((\$\w+)\)$')
NUMBER = re.compile(r'^-?(?:0x[0-9A-Fa-f]+|\d+)$')
HARDWARE = (0x10000000, 0x20000000)


def number(text):
    return int(text, 0)


# ---------------------------------------------------------------- the code

class Function:
    __slots__ = ('start', 'end', 'file', 'insns', 'index', 'tables', 'name', '_cfg', '_vals', '_size')

    def __init__(self, start, end, file, insns, tables):
        self.start, self.end, self.file = start, end, file
        self.insns = insns             # [(pc, mnemonic, [operands])]
        self.index = {pc: i for i, (pc, _, _) in enumerate(insns)}
        self.tables = tables           # {pc of a jr: [target pcs]}
        self.name = None
        self._cfg = self._vals = self._size = None


class Code:
    """One game's recompiled code: functions by start, instructions by pc, callers by callee."""

    def __init__(self, game, functions, names):
        self.game = game
        # The generated folders hold each function once and, for the points the runtime may resume it at, a copy
        # of its tail from there (entry_ files): those lie inside the function's range and are not functions.
        kept, fragments = {}, 0
        parent = None
        for start in sorted(functions):
            f = functions[start]
            if parent is not None and start < parent.end and f.end <= parent.end:
                fragments += 1
                continue
            kept[start] = f
            parent = f
        self.fragments = fragments
        self.functions = kept                           # {start: Function}
        functions = kept
        self.starts = sorted(functions)
        self.by_pc = {}
        for f in functions.values():
            for pc, _, _ in f.insns:
                self.by_pc[pc] = f
        self.names = names
        for start, f in functions.items():
            f.name = names.get(start) or re.sub(r'_0x[0-9a-f]+\.cpp$', '', f.file)
        self.callers = collections.defaultdict(list)    # callee start -> [(caller, pc of the jal)]
        for f in functions.values():
            for pc, mn, ops in f.insns:
                if mn == 'jal' and ops:
                    t = jal_target(ops[0])
                    if t is not None:
                        self.callers[t].append((f, pc))
        self.gp = None
        entry = functions.get(min(functions)) if functions else None
        if entry:
            hi = None
            for pc, mn, ops in entry.insns[:64]:
                if mn == 'lui' and ops[0] == '$gp':
                    hi = number(ops[1]) << 16
                elif mn in ('addiu', 'ori') and ops[0] == '$gp' and ops[1] == '$gp' and hi is not None:
                    self.gp = (hi + number(ops[2])) & 0xFFFFFFFF
                    break

    def add_callers(self, path):
        """The lane runtime's call graph (ZX_LANE_CALLERS_OUT: `C callee=0x.. from=0x.. n=..`): the callers no jal names."""
        added = 0
        for line in open(path):
            m = re.match(r'^C callee=0x([0-9a-f]+) from=0x([0-9a-f]+)', line)
            if not m:
                continue
            callee, site = int(m.group(1), 16), int(m.group(2), 16)
            caller = self.by_pc.get(site)
            if caller is None or callee not in self.functions:
                continue
            known = self.callers.get(callee, [])
            if not any(pc == site for _, pc in known):
                self.callers[callee].append((caller, site))
                added += 1
        return added

    def function_at(self, pc):
        return self.by_pc.get(pc)

    def fname(self, f):
        return '%s (0x%x)' % (f.name, f.start) if f else '?'

    def where(self, pc):
        f = self.by_pc.get(pc)
        return '%s+0x%x' % (f.name, pc - f.start) if f else '0x%x' % pc


def jal_target(text):
    m = re.fullmatch(r'func_([0-9A-Fa-f]+)', text)
    return int(m.group(1), 16) if m else None


def generated_dir(game):
    if game == 'dm2':
        return os.path.join(zaxis_paths.get('DM2_WORKSPACE'), 'recomp', 'generated-ns')
    if game == 'bmx':
        return os.path.join(zaxis_paths.get('BMX_RECOMP'), 'generated-ns')
    if game == 'ai':
        return os.path.join(zaxis_paths.get('AI_WORKSPACE'), 'recomp', 'generated')
    raise SystemExit('wlane: game dm2, bmx or ai, not %r' % game)


INSN = re.compile(r'^\s*// 0x([0-9a-f]+): 0x([0-9a-f]+)\s+(\S+)\s*(.*?)(?: \(Delay Slot\))?\s*$')
HEADER = re.compile(r'^// Address: 0x([0-9a-f]+) - 0x([0-9a-f]+)')
CASE = re.compile(r'^\s*case 0x([0-9a-fA-F]+)u: goto label_')
WORD_COMMENT = re.compile(r'^\.word\s+\S+\s+#\s+(\S+)\s*(.*?)\s*#')


def parse_operands(mn, text):
    text = text.strip()
    if mn == '.word':
        m = WORD_COMMENT.match('.word ' + text) if text else None
        if m:
            return m.group(1), [o.strip() for o in m.group(2).split(',') if o.strip()]
        return mn, []
    if not text:
        return mn, []
    return mn, [o.strip() for o in text.split(',')]


def load_code(game, cache_dir=None):
    folder = generated_dir(game)
    files = sorted(f for f in os.listdir(folder) if f.endswith('.cpp') and f != 'register_functions.cpp')
    key = hashlib.sha1(('%s|%d|%s' % (folder, len(files), max(os.path.getmtime(os.path.join(folder, f)) for f in files[:50]))).encode()).hexdigest()[:12]
    cache_dir = cache_dir or os.path.join(zaxis_paths.get('ZAXIS_SCRATCH'), 'wlane', 'cache')
    os.makedirs(cache_dir, exist_ok=True)
    cached = os.path.join(cache_dir, '%s-%s.pickle' % (game, key))
    if os.path.exists(cached):
        with open(cached, 'rb') as f:
            functions = {start: Function(*row) for start, row in pickle.load(f).items()}
    else:
        print('wlane: reading %d generated files of %s (cached after)' % (len(files), game), file=sys.stderr)
        functions = {}
        for name in files:
            start = end = None
            insns, tables, seen = [], {}, set()
            table_of = None
            for line in open(os.path.join(folder, name), errors='replace'):
                m = HEADER.match(line)
                if m:
                    start, end = int(m.group(1), 16), int(m.group(2), 16)
                    continue
                m = INSN.match(line)
                if m:
                    pc = int(m.group(1), 16)
                    if pc in seen:
                        continue
                    seen.add(pc)
                    mn, ops = parse_operands(m.group(3), m.group(4))
                    insns.append((pc, mn, ops))
                    table_of = pc if mn == 'jr' and ops and ops[0] != '$ra' else None
                    continue
                if table_of is not None:
                    m = CASE.match(line)
                    if m:
                        tables.setdefault(table_of, []).append(int(m.group(1), 16))
            if not insns:
                continue
            if start is None:
                start, end = insns[0][0], insns[-1][0] + 4
            functions[start] = Function(start, end, name, insns, tables)
        with open(cached, 'wb') as f:
            pickle.dump({start: (fn.start, fn.end, fn.file, fn.insns, fn.tables) for start, fn in functions.items()}, f)
    names = {}
    for path in (os.path.join(cache_dir, '%s.names.csv' % game),
                 os.path.join(zaxis_paths.get('ZAXIS_SCRATCH'), 'wlane1', 'dis', '%s.names.csv' % game)):
        if os.path.exists(path):
            for line in open(path):
                parts = line.strip().split(',', 1)
                if len(parts) == 2 and re.fullmatch(r'[0-9a-f]+', parts[0]):
                    names[int(parts[0], 16)] = parts[1]
            break
    # the tree's own names (game/GAME/functions.tsv: start, end, layer, name; '-' unnamed) over the cached ones
    table = ztree.readable_path({'ai': 'aggressive'}.get(game, game) + '/functions.tsv')
    if os.path.exists(table):
        for line in open(table):
            parts = line.rstrip('\n').split('\t')
            if len(parts) >= 4 and parts[0].startswith('0x') and parts[3] not in ('', '-'):
                names[int(parts[0], 16)] = parts[3].rstrip('_')
    return Code(game, functions, names)


# ---------------------------------------------------------------- the control flow

def branch_target(pc, text):
    m = re.fullmatch(r'\. \+ 4 \+ \((-?0x[0-9A-Fa-f]+|-?\d+) << 2\)', text)
    if m:
        return pc + 4 + (number(m.group(1)) << 2)
    m = re.fullmatch(r'func_([0-9A-Fa-f]+)', text)
    if m:
        return int(m.group(1), 16)
    if NUMBER.match(text):
        return number(text)
    return None


def cfg(f):
    """successors[i] and call_after[slot index] = (call index), of f's instructions."""
    if f._cfg is not None:
        return f._cfg
    n = len(f.insns)
    succ = [[i + 1] if i + 1 < n else [] for i in range(n)]
    call_after = {}
    for i, (pc, mn, ops) in enumerate(f.insns):
        if i + 1 >= n:
            break
        slot, after = i + 1, ([i + 2] if i + 2 < n else [])
        if mn in ('b', 'j') or (mn == 'beq' and ops[:2] == ['$zero', '$zero']) or (mn == 'bgez' and ops[:1] == ['$zero']):
            t = f.index.get(branch_target(pc, ops[-1])) if ops else None
            if mn == 'j' and t is None and ops:
                target = branch_target(pc, ops[-1])
                if target is not None and f.index.get(target) is None:
                    call_after[slot] = ('tail', target)
            succ[i], succ[slot] = [slot], ([t] if t is not None else [])
        elif mn in CONDITIONAL:
            t = f.index.get(branch_target(pc, ops[-1])) if ops else None
            succ[i], succ[slot] = [slot], ([t] if t is not None else []) + after
        elif mn in LIKELY:
            t = f.index.get(branch_target(pc, ops[-1])) if ops else None
            succ[i], succ[slot] = [slot] + after, ([t] if t is not None else [])
        elif mn == 'jr':
            if ops and ops[0] == '$ra':
                succ[i], succ[slot] = [slot], []
            else:
                targets = [f.index[t] for t in f.tables.get(pc, []) if t in f.index]
                succ[i], succ[slot] = [slot], targets
                if not targets:
                    call_after[slot] = ('jr', pc)
        elif mn in ('jal', 'jalr'):
            succ[i] = [slot]
            succ[slot] = after
            call_after[slot] = ('call', i)
        elif mn == 'syscall':
            pass
    f._cfg = (succ, call_after)
    return f._cfg


# ---------------------------------------------------------------- symbolic values (addresses)

def frame_size(f):
    if f._size is None:
        size = 0
        for pc, mn, ops in f.insns:
            if mn in ('addiu', 'daddiu') and ops[:2] == ['$sp', '$sp'] and NUMBER.match(ops[2]) and number(ops[2]) < 0:
                size = max(size, -number(ops[2]))
        f._size = size
    return f._size


def values(code, f):
    """Per instruction index, the symbolic value of each GPR before it: {reg: (base, delta)} where base is 'sp0' (the
    entry sp), 'const', ('arg', k) (an argument register's entry value), ('gp',) or ('v', pc) (a value made at pc)."""
    if f._vals is not None:
        return f._vals
    succ, call_after = cfg(f)
    n = len(f.insns)
    entry = {29: ('sp0', 0), 28: ('const', code.gp) if code.gp else (('gp',), 0)}
    for k, r in enumerate(ARG_REGS):
        entry[r] = (('arg', k), 0)
    before = [None] * n
    before[0] = entry
    work = [0]

    def join(a, b, at):
        if a is None:
            return b
        out = {}
        for r in set(a) | set(b):
            va, vb = a.get(r), b.get(r)
            if va == vb:
                if va is not None:
                    out[r] = va
            else:
                out[r] = (('phi', at), 0)
        return out

    while work:
        i = work.pop()
        state = dict(before[i])
        pc, mn, ops = f.insns[i]
        transfer_value(state, pc, mn, ops)
        if i in call_after and call_after[i][0] == 'call':
            for r in (1, 2, 3) + ARG_REGS + (12, 13, 14, 15, 24, 25, 31):
                state[r] = (('v', f.insns[call_after[i][1]][0]), 0)
        for s in succ[i]:
            merged = join(before[s], state, f.insns[s][0])
            if merged != before[s]:
                before[s] = merged
                work.append(s)
    f._vals = before
    return before


def transfer_value(state, pc, mn, ops):
    def val(r):
        return state.get(r)

    def regs(texts):
        return [REG.get(t) for t in texts]

    if mn == 'lui' and len(ops) == 2 and ops[0] in REG:
        state[REG[ops[0]]] = ('const', (number(ops[1]) << 16) & 0xFFFFFFFF)
    elif mn in ('addiu', 'addi', 'daddiu', 'daddi', 'ori') and len(ops) == 3 and ops[0] in REG and ops[1] in REG and NUMBER.match(ops[2]):
        rd, rs = REG[ops[0]], REG[ops[1]]
        imm = number(ops[2])
        v = val(rs) if rs != 0 else ('const', 0)
        if v is not None and (mn != 'ori' or v[0] == 'const'):
            state[rd] = (v[0], (v[1] + imm) & 0xFFFFFFFF if v[0] == 'const' else v[1] + imm)
        elif rd != 0:
            state[rd] = (('v', pc), 0)
    elif mn in ('daddu', 'addu', 'or', 'dadd', 'add') and len(ops) == 3 and all(o in REG for o in ops):
        rd, rs, rt = regs(ops)
        if rt == 0 or rs == 0:
            src = rs if rt == 0 else rt
            state[rd] = val(src) if src != 0 and val(src) is not None else ('const', 0) if src == 0 else (('v', pc), 0)
        else:
            vs, vt = val(rs), val(rt)
            if vs is not None and vt is not None and vt[0] == 'const' and vs[0] != 'const':
                state[rd] = (vs[0], vs[1] + vt[1])
            elif vs is not None and vt is not None and vs[0] == 'const' and vt[0] != 'const':
                state[rd] = (vt[0], vt[1] + vs[1])
            elif vs is not None and vt is not None and vs[0] == 'const' and vt[0] == 'const':
                state[rd] = ('const', (vs[1] + vt[1]) & 0xFFFFFFFF)
            else:
                state[rd] = (('v', pc), 0)
    elif ops and ops[0] in REG and mn not in STORES and not mn.startswith(('b', 'j', 'mt', 'ctc', 'qmtc', 'mult', 'div', 'madd', 'msub', 't', 'sync', 'cache')) and mn not in ('mtsab', 'mtsah', 'c1', 'break'):
        rd = REG[ops[0]]
        if rd not in (0,):
            state[rd] = (('v', pc), 0)
    if mn in ('jalr',) and ops and REG.get(ops[0]) is not None and len(ops) == 1:
        pass


def values_after_slot(code, f, slot_index):
    """The symbolic values as the call after delay slot `slot_index` sees them (the slot has run)."""
    vals = values(code, f)
    v = dict(vals[slot_index] or {})
    pc, mn, ops = f.insns[slot_index]
    transfer_value(v, pc, mn, ops)
    return v


def prov(code, v, depth=0):
    """A value's provenance as text."""
    if v is None:
        return '?'
    base, delta = v
    d = ('+0x%x' % delta if delta > 0 else '-0x%x' % -delta if delta < 0 else '') if base != 'const' else ''
    if base == 'sp0':
        return 'sp0' + d
    if base == 'const':
        return '0x%x' % delta
    if isinstance(base, tuple):
        kind = base[0]
        if kind == 'arg':
            return 'arg%d' % base[1] + d
        if kind == 'gp':
            return 'gp' + d
        if kind == 'phi':
            return 'phi@%s' % code.where(base[1]) + d
        if kind == 'v':
            return 'v@%s' % code.where(base[1]) + d
    return repr(v)


# ---------------------------------------------------------------- places

def lane_place(text):
    """A LANE spec (vf6.w, a3.hi, a3.2, f22; sp+0x30 / sp-0xec: the word at that offset from sp at the point) as places."""
    m = re.fullmatch(r'sp([+-])(0x[0-9A-Fa-f]+|\d+)', text)
    if m:
        off = number(m.group(2)) * (1 if m.group(1) == '+' else -1)
        return {('sp', off)}   # (relative to sp at the point: parse_source turns it into the function's sp0 terms)
    m = re.fullmatch(r'vf(\d+)\.([xyzw])', text)
    if m:
        return {('vf', int(m.group(1)), LANES.index(m.group(2)))}
    m = re.fullmatch(r'f(\d+)', text)
    if m:
        return {('f', int(m.group(1)))}
    m = re.fullmatch(r'(\w+)\.(hi|lo|[0-3])', text)
    if m and '$' + m.group(1) in REG:
        r = REG['$' + m.group(1)]
        if m.group(2) == 'hi':
            return {('g', r, 2), ('g', r, 3)}
        if m.group(2) == 'lo':
            return {('g', r, 0), ('g', r, 1)}
        return {('g', r, int(m.group(2)))}
    raise SystemExit('wlane: a lane is vfN.x|y|z|w, REG.0..3, REG.hi, REG.lo or fN, not %r' % text)


def place_text(code, p):
    k = p[0]
    if k == 'g':
        return '%s.%d' % (ABI[p[1]], p[2])
    if k == 'vf':
        return 'vf%d.%s' % (p[1], LANES[p[2]])
    if k == 'f':
        return 'f%d' % p[1]
    if k == 'acc':
        return 'ACC.%s' % LANES[p[1]]
    if k == 'm':
        base, off = p[1], p[2]
        if base == 'sp0':
            return '[sp0%+#x]' % off if off else '[sp0]'
        return '[%s%+#x]' % (prov(code, (base, 0)), off) if off else '[%s]' % prov(code, (base, 0))
    return ':'.join(str(x) for x in p)


def vu_operand(text):
    """($vf5w) -> ('vf', 5, 3); $vf5 -> ('vf', 5, None); $ACC -> ('acc',); $Q -> ('q',); $I -> ('i',); ($vi1++) -> ('vi', 1)."""
    text = text.strip()
    m = re.fullmatch(r'\$vf(\d+)([xyzw]?)', text)
    if m:
        return ('vf', int(m.group(1)), LANES.index(m.group(2)) if m.group(2) else None)
    if text == '$ACC':
        return ('acc',)
    if text == '$Q':
        return ('q',)
    if text == '$I':
        return ('i',)
    m = re.fullmatch(r'\(?\$vi(\d+)(\+\+|--)?\)?', text)
    if m:
        return ('vi', int(m.group(1)))
    m = re.fullmatch(r'\$vi(\d+)', text)
    if m:
        return ('vi', int(m.group(1)))
    return None


def parse_vu(mn):
    """'vmaddax.xyz' -> ('vmadda', 'x', 'xyz'); 'vdiv' -> ('vdiv', None, '')."""
    base, _, mask = mn.partition('.')
    for op in VU_OPS:
        if base == op:
            return op, None, mask
        if base.startswith(op) and len(base) == len(op) + 1 and base[-1] in 'xyzwqi':
            return op, base[-1], mask
    return None


# ---------------------------------------------------------------- what a function reads at its entry

def insn_rw(mn, ops):
    """(reads, writes) of one instruction as register places; ('memread',) / ('memwrite',) for memory through a
    pointer, ('frame', off, size) entries are returned apart by frame_access."""
    reads, writes = set(), set()

    def gw(r, words=(0, 1)):
        return {('g', r, w) for w in words} if r else set()

    def vf_all(o):
        v = vu_operand(o)
        return {('vf', v[1], l) for l in range(4)} if v and v[0] == 'vf' and v[1] else set()

    if mn in CONDITIONAL or mn in LIKELY:
        if mn.startswith('bc1'):
            reads.add(('fcc',))
        elif mn.startswith('bc2'):
            reads |= {('mac',), ('clip',)}
        else:
            for o in ops[:-1]:
                if o in REG:
                    reads |= gw(REG[o])
        return reads, writes
    if mn in LOADS:
        m = MEMORY.match(ops[1]) if len(ops) == 2 else None
        if m:
            reads |= gw(REG.get(m.group(2), 0))
        if mn == 'lqc2':
            writes |= vf_all(ops[0])
        elif mn == 'lwc1':
            writes.add(('f', int(ops[0][2:])))
        elif mn == 'lq':
            writes |= gw(REG[ops[0]], (0, 1, 2, 3))
        else:
            writes |= gw(REG[ops[0]])
            if mn in ('lwl', 'lwr', 'ldl', 'ldr'):
                reads |= gw(REG[ops[0]])
        return reads, writes
    if mn in STORES:
        m = MEMORY.match(ops[1]) if len(ops) == 2 else None
        if m:
            reads |= gw(REG.get(m.group(2), 0))
        if mn == 'sqc2':
            reads |= vf_all(ops[0])
        elif mn == 'swc1':
            reads.add(('f', int(ops[0][2:])))
        elif mn == 'sq':
            reads |= gw(REG[ops[0]], (0, 1, 2, 3))
        else:
            reads |= gw(REG[ops[0]])
        return reads, writes
    if mn == 'syscall':
        reads |= {('g', r, w) for r in (2, 4, 5, 6, 7, 8, 9, 10, 11) for w in (0, 1)}
        writes |= gw(2) | gw(3)
        return reads, writes
    if mn in ('jr', 'jalr'):
        for o in ops:
            if o in REG and o != '$ra':
                reads |= gw(REG[o])
        if mn == 'jalr':
            writes |= gw(31)
        return reads, writes
    if mn == 'jal':
        writes |= gw(31)
        return reads, writes
    if mn == 'mtc1':
        reads |= gw(REG[ops[0]])
        writes.add(('f', int(ops[1][2:])))
        return reads, writes
    if mn == 'mfc1':
        reads.add(('f', int(ops[1][2:])))
        writes |= gw(REG[ops[0]])
        return reads, writes
    if mn.endswith('.s') or mn in ('cvt.s.w', 'cvt.w.s'):
        fregs = [int(o[2:]) for o in ops if o.startswith('$f')]
        if mn.startswith('c.'):
            reads |= {('f', r) for r in fregs}
            writes.add(('fcc',))
        elif mn in ('adda.s', 'suba.s', 'mula.s', 'madda.s', 'msuba.s'):
            reads |= {('f', r) for r in fregs}
            writes.add(('facc',))
            if mn[0] == 'm' and 'add' in mn or 'sub' in mn[1:]:
                reads.add(('facc',))
        elif fregs:
            writes.add(('f', fregs[0]))
            reads |= {('f', r) for r in fregs[1:]}
            if mn in ('madd.s', 'msub.s'):
                reads.add(('facc',))
        return reads, writes
    if mn.startswith('qmfc2'):
        reads |= vf_all(ops[1])
        writes |= gw(REG[ops[0]], (0, 1, 2, 3))
        return reads, writes
    if mn.startswith('qmtc2'):
        reads |= gw(REG[ops[0]], (0, 1, 2, 3))
        writes |= vf_all(ops[1])
        return reads, writes
    if mn.startswith('cfc2'):
        vi = vu_operand(ops[1])
        reads.add({16: ('mac',), 17: ('mac',), 18: ('clip',), 22: ('q',), 21: ('i',)}.get(vi[1], ('vi', vi[1])) if vi else ('vi', 0))
        writes |= gw(REG[ops[0]])
        return reads, writes
    if mn.startswith('ctc2'):
        vi = vu_operand(ops[1])
        reads |= gw(REG[ops[0]])
        writes.add({16: ('mac',), 17: ('mac',), 18: ('clip',), 22: ('q',), 21: ('i',)}.get(vi[1], ('vi', vi[1])) if vi else ('vi', 0))
        return reads, writes
    if mn.startswith('v'):
        parsed = parse_vu(mn)
        if parsed is None:
            return reads, writes
        op, bc, mask = parsed
        lanes = [LANES.index(c) for c in mask] if mask else list(range(4))
        operands = [vu_operand(o) for o in ops]

        def of(o, lane):
            if o is None:
                return set()
            if o[0] == 'vf':
                return {('vf', o[1], o[2] if o[2] is not None else lane)} if o[1] else set()
            if o[0] == 'acc':
                return {('acc', lane)}
            return {o}

        def bcof(o, lane):
            if bc and bc in 'xyzw' and o and o[0] == 'vf':
                return {('vf', o[1], LANES.index(bc))} if o[1] else set()
            if bc == 'q':
                return {('q',)}
            if bc == 'i':
                return {('i',)}
            return of(o, lane)

        if op in ('vadd', 'vsub', 'vmul', 'vmax', 'vmini', 'vmaxi', 'vmadd', 'vmsub', 'vadda', 'vsuba', 'vmula', 'vmadda', 'vmsuba'):
            fd, fs, ft = operands[0], operands[1], operands[2] if len(operands) > 2 else None
            for lane in lanes:
                reads |= of(fs, lane) | bcof(ft, lane) | ({('acc', lane)} if op in ('vmadd', 'vmsub', 'vmadda', 'vmsuba') else set())
                writes |= of(fd, lane)
            writes.add(('mac',))
        elif op in ('vopmula', 'vopmsub'):
            fd, fs, ft = operands[0], operands[1], operands[2]
            for lane, (a, b) in {0: (1, 2), 1: (2, 0), 2: (0, 1)}.items():
                reads |= of(fs, a) | of(ft, b) | ({('acc', lane)} if op == 'vopmsub' else set())
                writes |= of(fd, lane)
            writes.add(('mac',))
        elif op in ('vmove', 'vabs', 'vftoi0', 'vftoi4', 'vftoi12', 'vftoi15', 'vitof0', 'vitof4', 'vitof12', 'vitof15'):
            for lane in lanes:
                reads |= of(operands[1], lane)
                writes |= of(operands[0], lane)
        elif op == 'vmr32':
            for lane in lanes:
                reads |= of(operands[1], (lane + 1) % 4)
                writes |= of(operands[0], lane)
        elif op in ('vdiv', 'vsqrt', 'vrsqrt'):
            for o in operands[1:]:
                if o and o[0] == 'vf' and o[2] is not None:
                    reads |= of(o, 0)
            writes.add(('q',))
        elif op == 'vclip':
            fs, ft = operands[0], operands[1]
            reads |= {('vf', fs[1], l) for l in (0, 1, 2)} if fs and fs[1] else set()
            reads |= {('vf', ft[1], 3)} if ft and ft[1] else set()
            writes.add(('clip',))
        elif op in ('vlqi', 'vlqd'):
            reads.add(('vu0mem',))
            for lane in lanes:
                writes |= of(operands[0], lane)
        elif op in ('vsqi', 'vsqd'):
            for lane in lanes:
                reads |= of(operands[0], lane)
            writes.add(('vu0mem',))
        elif op in ('vcallms', 'vcallmsr'):
            reads |= {('vf', n, l) for n in range(1, 32) for l in range(4)} | {('acc', l) for l in range(4)} | {('q',), ('i',), ('vu0mem',)}
        elif op == 'vmfir':
            for lane in lanes:
                writes |= of(operands[0], lane)
            reads.add(operands[1] if operands[1] else ('vi', 0))
        elif op == 'vmtir':
            if operands[1] and operands[1][0] == 'vf' and operands[1][2] is not None:
                reads |= of(operands[1], 0)
            writes.add(operands[0] if operands[0] else ('vi', 0))
        return reads, writes
    if mn == 'lui':
        writes |= gw(REG[ops[0]])
        return reads, writes
    if mn in ('mfhi', 'mflo', 'mfhi1', 'mflo1'):
        reads.add(('hi1' if mn.endswith('1') else 'hi',) if 'hi' in mn else ('lo1' if mn.endswith('1') else 'lo',))
        writes |= gw(REG[ops[0]])
        return reads, writes
    if mn in ('mthi', 'mtlo', 'mthi1', 'mtlo1'):
        writes.add(('hi1' if mn.endswith('1') else 'hi',) if 'hi' in mn else ('lo1' if mn.endswith('1') else 'lo',))
        reads |= gw(REG[ops[0]])
        return reads, writes
    regs = [REG[o] for o in ops if o in REG]
    if not regs:
        return reads, writes
    if mn in ('mult', 'multu', 'div', 'divu', 'madd', 'maddu', 'mult1', 'multu1', 'div1', 'divu1', 'madd1', 'maddu1') and len(regs) == 2:
        reads |= gw(regs[0]) | gw(regs[1])
        writes |= {('hi1',), ('lo1',)} if mn.endswith('1') else {('hi',), ('lo',)}
        return reads, writes
    wide = mn.startswith('p') or mn in ('qfsrv',)
    words = (0, 1, 2, 3) if wide else (0, 1)
    writes |= gw(regs[0], words)
    for r in regs[1:]:
        reads |= gw(r, words)
    if mn in ('movz', 'movn'):
        reads |= gw(regs[0])
    if mn in ('mult', 'multu', 'div', 'divu', 'mult1', 'multu1', 'div1', 'divu1', 'madd', 'maddu'):
        writes |= {('hi',), ('lo',)}
    if mn in ('mtsab', 'mtsah'):
        writes.add(('sa',))
        reads |= gw(regs[0])
        writes -= gw(regs[0])
    return reads, writes


def frame_access(mn, ops, v):
    """A load's or store's frame words (sp0-relative) when through the frame, else None; (kind, [offsets], through_pointer)."""
    if mn not in LOADS and mn not in STORES:
        return None
    m = MEMORY.match(ops[1]) if len(ops) == 2 else None
    if not m:
        return None
    off, base_r = number(m.group(1)), REG.get(m.group(2), 0)
    size = LOADS.get(mn) or STORES[mn]
    if mn in ('lq', 'sq', 'lqc2', 'sqc2'):
        off &= ~15
    b = v.get(base_r) if base_r else ('const', 0)
    if b is None:
        return ('mem', [], True)
    if b[0] == 'sp0':
        a = b[1] + off
        return ('frame', [(a + w) & ~3 for w in range(0, size, 4)], False)
    if b[0] == 'const':
        return ('global', [], False)
    return ('mem', [], True)


class EntryReads:
    """Per function, what it may read before writing at its entry (registers as places, 'deadstack' for a word of
    its own frame read before stored on some path, 'argmem' for memory through a pointer), its callees' included.
    What a walk uses to skip a callee the lane cannot reach."""

    def __init__(self, code):
        self.code = code
        self.local = {}      # start -> (reads, deadstack offsets, argmem, {call index: written-before set})
        self.total = {}      # start -> (reads, deadstack bool, argmem bool)
        self.pending = set()

    def local_of(self, f):
        if f.start in self.local:
            return self.local[f.start]
        succ, call_after = cfg(f)
        vals = values(self.code, f)
        n = len(f.insns)
        reads, dead = set(), set()
        argmem = False
        written_at_call = {}
        before = [None] * n
        before[0] = frozenset()
        work = [0]
        while work:
            i = work.pop()
            w = before[i]
            pc, mn, ops = f.insns[i]
            r, wr = insn_rw(mn, ops)
            reads |= {p for p in r if p not in w}
            fa = frame_access(mn, ops, vals[i] or {})
            if fa is not None:
                kind, offs, pointer = fa
                if kind == 'frame':
                    if mn in LOADS:
                        dead |= {o for o in offs if ('m', 'sp0', o) not in w and o < 0}
                    else:
                        wr = set(wr) | {('m', 'sp0', o) for o in offs}
                elif pointer:
                    argmem = True
            after = w | frozenset(wr)
            if i in call_after and call_after[i][0] == 'call':
                written_at_call[call_after[i][1]] = after
                # a callee clobbers the caller-saved registers (words 0..3 unknown: not written for us)
            for s_ in succ[i]:
                prev = before[s_]
                new = after if prev is None else prev & after
                if prev is None or new != prev:
                    before[s_] = new
                    work.append(s_)
        self.local[f.start] = (frozenset(reads), frozenset(dead), argmem, written_at_call)
        return self.local[f.start]

    def of(self, f, depth=0):
        if f.start in self.total:
            return self.total[f.start]
        if f.start in self.pending or depth > 60:
            reads, dead, argmem, _ = self.local_of(f)
            return (reads, dead, argmem)
        self.pending.add(f.start)
        reads, dead, argmem, written = self.local_of(f)
        reads, deadoffs = set(reads), set(dead)   # (dead stack read: offsets from this function's sp0, its callees' shifted)
        for i, (pc, mn, ops) in enumerate(f.insns):
            if mn == 'jal':
                t = jal_target(ops[0])
                callee = self.code.functions.get(t) if t is not None else None
                if callee is None:
                    reads |= {('g', r, w) for r in (4, 5, 6, 7) for w in (0, 1)} | {('f', 12), ('f', 13), ('f', 14)}
                    argmem = True
                    continue
                cr, cd, ca = self.of(callee, depth + 1)
                reads |= {p for p in cr if p not in written.get(i, frozenset())}
                if cd and i + 1 < len(f.insns):
                    sp = values_after_slot(self.code, f, i + 1).get(29)
                    d = sp[1] if sp and sp[0] == 'sp0' else 0
                    deadoffs |= {o + d for o in cd}
                argmem = argmem or ca
            elif mn == 'jalr':
                reads |= {('g', r, w) for r in ARG_REGS for w in (0, 1)} | {('f', 12), ('f', 13), ('f', 14), ('f', 15)}
                argmem = True
        self.pending.discard(f.start)
        self.total[f.start] = (frozenset(reads), frozenset(deadoffs), argmem)
        return self.total[f.start]


# Library functions the runtime answers itself (no body in the table): what each reads and leaves.
STUBS = {
    'memoryCopy': 'copy', 'memoryFill': 'fill', 'memoryMove': 'copy',
    'memcpy': 'copy', 'memmove': 'copy', 'bcopy': 'copy', '_memcpy': 'copy', 'memset': 'fill', 'bzero': 'fill',
    'memset_0x': 'fill', 'kFlushCache': 'none', 'FlushCache': 'none', 'sqrtf': 'f12', 'sinf': 'f12', 'cosf': 'f12',
    'tanf': 'f12', 'atanf': 'f12', 'atan2f': 'f12', 'asinf': 'f12', 'acosf': 'f12', 'fabsf': 'f12', 'powf': 'f12',
    'expf': 'f12', 'logf': 'f12', 'fmodf': 'f12', 'floorf': 'f12', 'ceilf': 'f12', 'sqrt': 'f12', 'sin': 'f12',
    'cos': 'f12',
}


# ---------------------------------------------------------------- the lane's flow

class Walk:
    """One lane's walk from a source through the code."""

    def __init__(self, code, depth=6, fanout=40, budget=600, steps=False):
        self.code = code
        self.depth = depth
        self.fanout = fanout
        self.budget = budget
        self.steps = steps
        self.uses = []          # (pc, kind, text)
        self.landings = []      # (pc, kind, text, places)
        self.notes = []         # assumptions and limits met
        self.visited = set()    # (f.start, start index, frozenset) analysed
        self.memo = {}          # (f.start, entry taint) -> return taint
        self.functions_seen = set()
        self.unresolved = []
        self.trail = []
        self.entry_reads = EntryReads(code)
        self.skipped = 0
        self.runs = collections.Counter()   # function start -> runs of it (one per distinct entry state and start)
        self.ascended = set()               # (function, returned taint) already carried into its callers
        self.max_runs = 4000
        self.stopped = False

    def note(self, text):
        if text not in self.notes:
            self.notes.append(text)

    def use(self, pc, kind, text):
        self.uses.append((pc, kind, text))

    def land(self, pc, kind, text, places):
        self.landings.append((pc, kind, text, places))

    # -- the transfer of one instruction over a taint set --
    def transfer(self, f, i, taint, vals, call_depth):
        """taint after instruction i given taint before; uses and landings recorded. Returns the new set."""
        pc, mn, ops = f.insns[i]
        code = self.code
        T = set(taint)
        v = vals[i] or {}

        def gpr_words(r, words=(0, 1)):
            return {('g', r, w) for w in words} if r != 0 else set()

        def tainted(places):
            return any(p in T for p in places)

        def reads_tainted(places):
            return {p for p in places if p in T}

        def kill(places):
            T.difference_update(places)

        def taint_from(dests, sources):
            if any(p in T for p in sources):
                T.update(dests)
            else:
                T.difference_update(dests)

        def address_places(base_r, off, size):
            """The memory places of an access through base_r + off (word places), or None when unknown."""
            if base_r == 0:
                return ('const', [(('m', ('const', (off + w) & ~3), 0)) for w in range(0, size, 4)])
            b = v.get(base_r)
            if b is None:
                return (('v', pc), [('m', (('v', pc, base_r)), (off + w) & ~3) for w in range(0, size, 4)])
            base, delta = b
            a = delta + off
            if base == 'sp0':
                return ('sp0', [('m', 'sp0', (a + w) & ~3) for w in range(0, size, 4)])
            if base == 'const':
                return ('const', [('m', ('const', (a + w) & 0xFFFFFFFC), 0) for w in range(0, size, 4)])
            return (base, [('m', base, (a + w) & ~3) for w in range(0, size, 4)])

        def use_if(places, kind, text):
            hit = reads_tainted(places)
            if hit:
                self.use(pc, kind, '%s %s %s: %s' % (code.where(pc), mn, ' '.join(ops), ', '.join(sorted(place_text(code, p) for p in hit))) + (' ' + text if text else ''))

        def mem_base_use(base_r):
            if base_r:
                use_if(gpr_words(base_r), 'address', '(the base register)')

        # ---- branches
        if mn in CONDITIONAL or mn in LIKELY:
            if mn.startswith('bc1'):
                use_if({('fcc',)}, 'branch', '')
            elif mn.startswith('bc2'):
                use_if({('mac',), ('clip',)}, 'branch', '')
            elif not mn.startswith('bc0'):
                srcs = set()
                for o in ops[:-1]:
                    if o in REG:
                        srcs |= gpr_words(REG[o])
                use_if(srcs, 'branch', '')
            return T
        if mn in ('b', 'j', 'nop', 'sync', 'sync.p', 'sync.l', 'ei', 'di', 'cache', 'vnop', 'vwaitq', 'break', 'mfc0', 'mtc0', 'tlbwi', 'eret', 'c1'):
            if mn == 'mtc0' and ops and ops[0] in REG:
                use_if(gpr_words(REG[ops[0]]), 'cop0', '')
            if mn == 'mfc0' and ops and ops[0] in REG:
                kill(gpr_words(REG[ops[0]]))
            return T
        if mn == 'syscall':
            use_if({('g', r, w) for r in (4, 5, 6, 7, 8, 9, 10, 11, 2) for w in (0, 1)}, 'syscall', '')
            kill(gpr_words(2) | gpr_words(3))
            return T
        if mn in ('jr',):
            if ops and ops[0] != '$ra':
                use_if(gpr_words(REG[ops[0]]), 'jump', '')
            return T
        if mn == 'jal':
            kill({('g', 31, 0), ('g', 31, 1)})
            return T
        if mn == 'jalr':
            target = ops[-1]
            if target in REG:
                use_if(gpr_words(REG[target]), 'jump', '(the call through a register)')
            kill({('g', 31, 0), ('g', 31, 1)})
            return T
        # ---- loads and stores
        if mn in LOADS:
            size = LOADS[mn]
            m = MEMORY.match(ops[1]) if len(ops) == 2 else None
            if not m:
                self.use(pc, 'unmodelled', '%s %s %s' % (code.where(pc), mn, ' '.join(ops)))
                return T
            off, base_r = number(m.group(1)), REG.get(m.group(2), 0)
            mem_base_use(base_r)
            if mn in ('lwl', 'lwr', 'ldl', 'ldr'):
                off = off - size + 1 if mn in ('lwl', 'ldl') else off
            kind, places = address_places(base_r, off if mn not in ('lq', 'lqc2') else off & ~15, size if mn not in ('lwl', 'lwr', 'ldl', 'ldr') else size)
            if mn == 'lqc2':
                vf = vu_operand(ops[0])
                for lane in range(4):
                    taint_from({('vf', vf[1], lane)}, {places[lane]})
            elif mn == 'lwc1':
                taint_from({('f', int(ops[0][2:]) if ops[0].startswith('$f') else 0)}, set(places))
            elif mn == 'lq':
                rt = REG[ops[0]]
                for w in range(4):
                    taint_from({('g', rt, w)}, {places[w]})
            elif mn in ('ld', 'ldl', 'ldr'):
                rt = REG[ops[0]]
                if mn == 'ld':
                    taint_from({('g', rt, 0)}, {places[0]})
                    taint_from({('g', rt, 1)}, {places[1]})
                else:
                    taint_from({('g', rt, 0), ('g', rt, 1)}, set(places) | gpr_words(rt))
            else:
                rt = REG[ops[0]]
                taint_from(gpr_words(rt), set(places) | (gpr_words(rt) if mn in ('lwl', 'lwr') else set()))
            return T
        if mn in STORES:
            size = STORES[mn]
            m = MEMORY.match(ops[1]) if len(ops) == 2 else None
            if not m:
                self.use(pc, 'unmodelled', '%s %s %s' % (code.where(pc), mn, ' '.join(ops)))
                return T
            off, base_r = number(m.group(1)), REG.get(m.group(2), 0)
            mem_base_use(base_r)
            aligned = off & ~15 if mn in ('sq', 'sqc2') else off
            kind, places = address_places(base_r, aligned, size)
            if mn == 'sqc2':
                vf = vu_operand(ops[0])
                srcs = [{('vf', vf[1], lane)} for lane in range(4)]
            elif mn == 'swc1':
                srcs = [{('f', int(ops[0][2:]))}]
            else:
                rt = REG[ops[0]]
                if mn == 'sq':
                    srcs = [{('g', rt, w)} for w in range(4)]
                elif mn == 'sd':
                    srcs = [{('g', rt, 0)}, {('g', rt, 1)}]
                else:
                    srcs = [gpr_words(rt)]
            partial = mn in ('sb', 'sh', 'swl', 'swr', 'sdl', 'sdr')
            stored = []
            for k, place in enumerate(places[:len(srcs)]):
                if tainted(srcs[k]):
                    T.add(place)
                    stored.append((place, srcs[k]))
                elif not partial:
                    T.discard(place)
            if stored:
                if kind == 'const':
                    address = stored[0][0][1][1]
                    if HARDWARE[0] <= address < HARDWARE[1]:
                        self.use(pc, 'hardware', '%s %s %s -> 0x%x' % (code.where(pc), mn, ' '.join(ops), address))
                    else:
                        self.land(pc, 'scratchpad' if address >= 0x70000000 else 'global', '%s %s %s: %s' % (code.where(pc), mn, ' '.join(ops), ', '.join('0x%x <- %s' % (p[1][1], place_text(code, next(iter(s)))) for p, s in stored)), [p for p, _ in stored])
                elif kind != 'sp0':
                    b = v.get(base_r)
                    self.land(pc, 'record', '%s %s %s: %s' % (code.where(pc), mn, ' '.join(ops), ', '.join('[%s%+#x] <- %s' % (prov(code, (b[0], 0)) if b else '?', p[2], place_text(code, next(iter(s)))) for p, s in stored)), [p for p, _ in stored])
            return T
        # ---- FPU
        if mn in ('mtc1',):
            taint_from({('f', int(ops[1][2:]))}, gpr_words(REG[ops[0]]))
            return T
        if mn in ('mfc1',):
            taint_from(gpr_words(REG[ops[0]]), {('f', int(ops[1][2:]))})
            return T
        if mn.endswith('.s') or mn in ('cvt.s.w', 'cvt.w.s'):
            fregs = [int(o[2:]) for o in ops if o.startswith('$f')]
            if mn.startswith('c.'):
                taint_from({('fcc',)}, {('f', r) for r in fregs})
            elif mn in ('adda.s', 'suba.s', 'mula.s', 'madda.s', 'msuba.s'):
                taint_from({('facc',)}, {('f', r) for r in fregs} | ({('facc',)} if mn.startswith('m') and 'a' in mn[1:3] else set()))
            elif mn in ('madd.s', 'msub.s'):
                taint_from({('f', fregs[0])}, {('f', r) for r in fregs[1:]} | {('facc',)})
            elif fregs:
                taint_from({('f', fregs[0])}, {('f', r) for r in fregs[1:]})
            return T
        # ---- COP2 macro
        if mn.startswith(('qmfc2', 'qmtc2', 'cfc2', 'ctc2')):
            if mn.startswith('qmfc2'):
                rt, vf = REG[ops[0]], vu_operand(ops[1])
                for w in range(4):
                    taint_from({('g', rt, w)}, {('vf', vf[1], w)})
            elif mn.startswith('qmtc2'):
                rt, vf = REG[ops[0]], vu_operand(ops[1])
                for w in range(4):
                    taint_from({('vf', vf[1], w)}, {('g', rt, w)})
            elif mn.startswith('cfc2'):
                rt, vi = REG[ops[0]], vu_operand(ops[1])
                src = {('mac',)} if vi and vi[1] in (16, 17) else {('clip',)} if vi and vi[1] == 18 else {('q',)} if vi and vi[1] == 22 else {('i',)} if vi and vi[1] == 21 else {('vi', vi[1])} if vi else set()
                taint_from(gpr_words(rt), src)
            else:
                rt, vi = REG[ops[0]], vu_operand(ops[1])
                dst = {('mac',)} if vi and vi[1] in (16, 17) else {('clip',)} if vi and vi[1] == 18 else {('q',)} if vi and vi[1] == 22 else {('i',)} if vi and vi[1] == 21 else {('vi', vi[1])} if vi else set()
                taint_from(dst, gpr_words(rt))
            return T
        if mn.startswith('v'):
            parsed = parse_vu(mn)
            if parsed is None:
                self.use(pc, 'unmodelled', '%s %s %s' % (code.where(pc), mn, ' '.join(ops)))
                return T
            op, bc, mask = parsed
            lanes = [LANES.index(c) for c in mask] if mask else list(range(4))
            operands = [vu_operand(o) for o in ops]

            def lanes_of(o, lane):
                if o is None:
                    return set()
                if o[0] == 'vf':
                    if o[1] == 0:
                        return set()
                    return {('vf', o[1], o[2] if o[2] is not None else lane)}
                if o[0] == 'acc':
                    return {('acc', lane)}
                return {o}

            def bc_lanes(o, lane):
                if bc and bc in 'xyzw' and o and o[0] == 'vf':
                    return {('vf', o[1], LANES.index(bc))} if o[1] != 0 else set()
                if bc == 'q':
                    return {('q',)}
                if bc == 'i':
                    return {('i',)}
                return lanes_of(o, lane)

            if op in ('vadd', 'vsub', 'vmul', 'vmax', 'vmini', 'vmaxi', 'vmadd', 'vmsub', 'vadda', 'vsuba', 'vmula', 'vmadda', 'vmsuba'):
                fd, fs, ft = operands[0], operands[1], operands[2] if len(operands) > 2 else None
                acc_read = op in ('vmadd', 'vmsub', 'vmadda', 'vmsuba')
                # x - x is +0 whatever x holds (the cross product's `vsub.w vf6, vf6, vf6`): the VU has no infinity or
                # NaN, an exponent-255 lane is a large number and an exponent-0 lane is zero (the runtime's ps2_vsub:
                # ps2_vdaz, then ps2_fsub_slow's double for exponent 255), so the result and its MAC flags hold nothing
                # of the operand.
                self_minus = (op in ('vsub', 'vsuba') and not bc and fs and ft and fs[0] == 'vf' and ft[0] == 'vf'
                              and fs[1] == ft[1] and fs[2] is None and ft[2] is None)
                result_tainted = set()
                for lane in lanes:
                    srcs = set() if self_minus else lanes_of(fs, lane) | bc_lanes(ft, lane) | ({('acc', lane)} if acc_read else set())
                    dests = lanes_of(fd, lane) if fd and fd[0] != 'vf' or (fd and fd[1] != 0) else set()
                    if tainted(srcs):
                        T.update(dests)
                        result_tainted.add(lane)
                    else:
                        T.difference_update(dests)
                if result_tainted:
                    T.add(('mac',))
                else:
                    T.discard(('mac',))
                return T
            if op in ('vopmula', 'vopmsub'):
                fd, fs, ft = operands[0], operands[1], operands[2]
                pairs = {0: (1, 2), 1: (2, 0), 2: (0, 1)}
                any_t = False
                for lane in (0, 1, 2):
                    a, b = pairs[lane]
                    srcs = lanes_of(fs, a) | lanes_of(ft, b) | ({('acc', lane)} if op == 'vopmsub' else set())
                    dests = lanes_of(fd, lane)
                    if tainted(srcs):
                        T.update(dests)
                        any_t = True
                    else:
                        T.difference_update(dests)
                if any_t:
                    T.add(('mac',))
                else:
                    T.discard(('mac',))
                return T
            if op in ('vmove', 'vabs', 'vftoi0', 'vftoi4', 'vftoi12', 'vftoi15', 'vitof0', 'vitof4', 'vitof12', 'vitof15'):
                fd, fs = operands[0], operands[1]
                for lane in lanes:
                    taint_from(lanes_of(fd, lane), lanes_of(fs, lane))
                return T
            if op == 'vmr32':
                fd, fs = operands[0], operands[1]
                for lane in lanes:
                    taint_from(lanes_of(fd, lane), lanes_of(fs, (lane + 1) % 4))
                return T
            if op in ('vdiv', 'vsqrt', 'vrsqrt'):
                srcs = set()
                for o in operands[1:]:
                    srcs |= lanes_of(o, 0) if o and o[2] is not None else set()
                taint_from({('q',)}, srcs)
                return T
            if op == 'vclip':
                fs, ft = operands[0], operands[1]
                srcs = {('vf', fs[1], l) for l in (0, 1, 2)} if fs[1] else set()
                srcs |= {('vf', ft[1], 3)} if ft and ft[1] else set()
                taint_from({('clip',)}, srcs)
                return T
            if op in ('vlqi', 'vlqd'):
                fd = operands[0]
                for lane in lanes:
                    taint_from(lanes_of(fd, lane), {('vu0mem',)})
                return T
            if op in ('vsqi', 'vsqd'):
                fs = operands[0]
                if any(tainted(lanes_of(fs, lane)) for lane in lanes):
                    T.add(('vu0mem',))
                    self.land(pc, 'vu0mem', '%s %s %s' % (code.where(pc), mn, ' '.join(ops)), [('vu0mem',)])
                return T
            if op == 'vmfir':
                fd, vi = operands[0], operands[1]
                for lane in lanes:
                    taint_from(lanes_of(fd, lane), {vi} if vi else set())
                return T
            if op == 'vmtir':
                vi, fs = operands[0], operands[1]
                taint_from({vi}, lanes_of(fs, 0) if fs and fs[2] is not None else set())
                return T
            if op in ('viadd', 'visub', 'viand', 'vior', 'viaddi'):
                dst = operands[0]
                srcs = {o for o in operands[1:] if o and o[0] == 'vi'}
                taint_from({dst}, srcs)
                return T
            if op in ('vcallms', 'vcallmsr'):
                vu = {p for p in T if p[0] in ('vf', 'acc', 'q', 'i', 'vi', 'vu0mem', 'mac', 'clip')}
                if vu:
                    self.use(pc, 'vcallms', '%s %s %s: %s' % (code.where(pc), mn, ' '.join(ops), ', '.join(sorted(place_text(code, p) for p in vu))))
                return T
            if op in ('vilwr', 'viswr', 'vrnext', 'vrget', 'vrinit', 'vrxor'):
                return T
            self.use(pc, 'unmodelled', '%s %s %s' % (code.where(pc), mn, ' '.join(ops)))
            return T
        # ---- GPR integer
        if mn == 'lui':
            kill(gpr_words(REG[ops[0]]))
            return T
        if mn in ALU2I:
            rd, rs = REG[ops[0]], REG[ops[1]]
            taint_from(gpr_words(rd), gpr_words(rs))
            return T
        if mn in ALU3 or mn in ('negu', 'dnegu', 'neg'):
            regs = [REG[o] for o in ops if o in REG]
            if mn in ('mult', 'multu', 'div', 'divu', 'madd', 'maddu') and len(regs) == 2:
                taint_from({('hi',), ('lo',)}, gpr_words(regs[0]) | gpr_words(regs[1]) | ({('hi',), ('lo',)} if mn.startswith('madd') else set()))
                return T
            if mn in ('mult1', 'multu1', 'div1', 'divu1', 'madd1', 'maddu1') and len(regs) == 2:
                taint_from({('hi1',), ('lo1',)}, gpr_words(regs[0]) | gpr_words(regs[1]))
                return T
            rd, srcs = regs[0], regs[1:]
            src_places = set()
            for r in srcs:
                src_places |= gpr_words(r)
            if mn in ('mult', 'multu', 'div', 'divu', 'mult1', 'multu1', 'div1', 'divu1', 'madd', 'maddu', 'madd1', 'maddu1'):
                hl = {('hi1',), ('lo1',)} if mn.endswith('1') else {('hi',), ('lo',)}
                taint_from(hl | gpr_words(rd), src_places | (hl if mn.startswith('madd') else set()))
                return T
            if mn in ('movz', 'movn'):
                taint_from(gpr_words(rd), src_places | gpr_words(rd))
                return T
            taint_from(gpr_words(rd), src_places)
            return T
        if mn in ('mfhi', 'mflo', 'mfhi1', 'mflo1'):
            src = {('hi1' if mn.endswith('1') else 'hi',)} if 'hi' in mn else {('lo1' if mn.endswith('1') else 'lo',)}
            taint_from(gpr_words(REG[ops[0]]), src)
            return T
        if mn in ('mthi', 'mtlo', 'mthi1', 'mtlo1'):
            dst = {('hi1' if mn.endswith('1') else 'hi',)} if 'hi' in mn else {('lo1' if mn.endswith('1') else 'lo',)}
            taint_from(dst, gpr_words(REG[ops[0]]))
            return T
        if mn in ('mtsab', 'mtsah'):
            taint_from({('sa',)}, gpr_words(REG[ops[0]]))
            return T
        if mn == 'pcpyld':
            rd, rs, rt = (REG[o] for o in ops)
            taint_from({('g', rd, 0)}, {('g', rt, 0)})
            taint_from({('g', rd, 1)}, {('g', rt, 1)})
            taint_from({('g', rd, 2)}, {('g', rs, 0)})
            taint_from({('g', rd, 3)}, {('g', rs, 1)})
            return T
        if mn == 'pcpyud':
            rd, rs, rt = (REG[o] for o in ops)
            taint_from({('g', rd, 0)}, {('g', rs, 2)})
            taint_from({('g', rd, 1)}, {('g', rs, 3)})
            taint_from({('g', rd, 2)}, {('g', rt, 2)})
            taint_from({('g', rd, 3)}, {('g', rt, 3)})
            return T
        if mn == 'pextlw':
            rd, rs, rt = (REG[o] for o in ops)
            taint_from({('g', rd, 0)}, {('g', rt, 0)})
            taint_from({('g', rd, 1)}, {('g', rs, 0)})
            taint_from({('g', rd, 2)}, {('g', rt, 1)})
            taint_from({('g', rd, 3)}, {('g', rs, 1)})
            return T
        if mn == 'pextuw':
            rd, rs, rt = (REG[o] for o in ops)
            taint_from({('g', rd, 0)}, {('g', rt, 2)})
            taint_from({('g', rd, 1)}, {('g', rs, 2)})
            taint_from({('g', rd, 2)}, {('g', rt, 3)})
            taint_from({('g', rd, 3)}, {('g', rs, 3)})
            return T
        if mn in MMI_LANEWISE:
            regs = [REG[o] for o in ops if o in REG]
            rd, srcs = regs[0], regs[1:]
            for w in range(4):
                taint_from({('g', rd, w)}, {('g', r, w) for r in srcs})
            return T
        if mn.startswith('p') or mn in ('qfsrv',):
            regs = [REG[o] for o in ops if o in REG]
            if regs:
                rd, srcs = regs[0], regs[1:]
                allsrc = {('g', r, w) for r in srcs for w in range(4)} | ({('sa',)} if mn == 'qfsrv' else set()) | ({('hi',), ('lo',)} if mn in ('pmfhl', 'pmfhi', 'pmflo') else set())
                if mn in ('pmthl', 'pmthi', 'pmtlo'):
                    taint_from({('hi',), ('lo',)}, {('g', rd, w) for w in range(4)})
                else:
                    taint_from({('g', rd, w) for w in range(4)}, allsrc)
            return T
        # anything else: reads tainted -> use; dest killed
        regs = [REG[o] for o in ops if o in REG]
        srcs = set()
        for r in regs[1:]:
            srcs |= gpr_words(r)
        use_if(srcs, 'unmodelled', '(%s)' % mn)
        if regs:
            kill(gpr_words(regs[0]))
        return T

    # -- running a function --
    def run(self, f, start_index, taint, call_depth, label):
        """Walk f from instruction start_index with the taint set; returns the taint at f's returns (union), in f's
        own terms (frame words sp0-relative)."""
        key = (f.start, start_index, frozenset(taint))
        if key in self.memo:
            return self.memo[key]
        if key in self.visited:
            return set()   # (recursion met: the second round's effects are not added)
        self.visited.add(key)
        self.functions_seen.add(f.start)
        self.runs[f.start] += 1
        if len(self.functions_seen) > self.budget or sum(self.runs.values()) > self.max_runs:
            if not self.stopped:
                self.unresolved.append('the walk visited %d functions in %d runs (--budget %d, %d runs): stopped in %s; most run: %s' % (
                    len(self.functions_seen), sum(self.runs.values()), self.budget, self.max_runs, self.code.fname(f),
                    ', '.join('%s %d' % (self.code.functions[a].name, n) for a, n in self.runs.most_common(6))))
                self.stopped = True
            return set()
        succ, call_after = cfg(f)
        vals = values(self.code, f)
        n = len(f.insns)
        before = {start_index: frozenset(taint)}
        work = [start_index]
        returns = set()
        while work:
            i = work.pop()
            state = self.transfer(f, i, before[i], vals, call_depth)
            pc, mn, ops = f.insns[i]
            if self.steps and state & before[i] != state or (self.steps and state != before[i]):
                self.trail.append('%s%s %s %s: %s' % ('  ' * call_depth, self.code.where(pc), mn, ' '.join(ops), ', '.join(sorted(place_text(self.code, p) for p in state - before[i])) if state - before[i] else '-' + ', '.join(sorted(place_text(self.code, p) for p in before[i] - state))))
            if i in call_after:
                kind, what = call_after[i]
                if kind == 'call':
                    state = self.call(f, what, i, state, vals, call_depth)
                elif kind == 'tail':
                    target = self.code.functions.get(what)
                    if target is not None:
                        returns |= self.enter(target, state, vals[i] or {}, 0, call_depth + 1, 'tail call from %s' % self.code.where(pc))
                    else:
                        self.unknown_callee(pc, state, 'a tail call to 0x%x, not in the table' % what)
                    continue
                elif kind == 'jr':
                    self.unknown_callee(pc, state, 'a jump through a register at %s with no table' % self.code.where(pc))
                    returns |= state
                    continue
            if not succ[i]:
                if mn == 'jr' and ops and ops[0] == '$ra' or (i > 0 and f.insns[i - 1][1] == 'jr' and f.insns[i - 1][2] == ['$ra']):
                    returns |= state
                elif state:
                    returns |= state
                continue
            for s in succ[i]:
                merged = before.get(s)
                new = frozenset(state) if merged is None else merged | state
                if merged is None or new != merged:
                    before[s] = new
                    work.append(s)
        # (a record's word stays a place inside the function alone: at the return it is the landing already recorded)
        returns = {p for p in returns if p[0] != 'm' or p[1] == 'sp0' or (isinstance(p[1], tuple) and p[1][0] == 'arg')}
        self.memo[key] = returns
        return returns

    def unknown_callee(self, pc, state, why, target=None, v=None):
        """A call the table has no body for: the runtime's own library functions read their argument registers (words
        0 and 1) and FPU arguments, never VU0; memcpy carries memory, memset clears it (STUBS by name)."""
        name = self.code.names.get(target, '') if target is not None else ''
        kind = STUBS.get(name) or ('f12' if name.endswith('f') and name[:-1] in ('sqrt', 'sin', 'cos') else None)
        if kind == 'none':
            return state
        if kind == 'copy' and v is not None:
            # memcpy(dst = a0, src = a1, n): the words tainted under a1's base, at [0, n) from it, move under a0's; a landing
            src, dst, size = v.get(5), v.get(4), v.get(6)
            n = size[1] if size is not None and size[0] == 'const' else 0x400
            moved = set()
            if src is not None and dst is not None:
                for p in list(state):
                    if p[0] == 'm' and p[1] == src[0] and 0 <= p[2] - src[1] < n:
                        moved.add(('m', dst[0], p[2] - src[1] + dst[1]))
            if moved:
                state = set(state) | moved
                self.land(pc, 'copy', '%s memcpy: %s' % (self.code.where(pc), ', '.join(sorted(place_text(self.code, p) for p in moved)[:8])), sorted(moved))
            reads = {p for p in state if p[0] == 'g' and p[1] in (4, 5, 6) and p[2] in (0, 1)}
        elif kind == 'fill':
            reads = {p for p in state if p[0] == 'g' and p[1] in (4, 5, 6) and p[2] in (0, 1)}
            if v is not None and v.get(4) is not None:
                base, d = v[4]
                size = v.get(6)
                n = size[1] if size is not None and size[0] == 'const' else 0
                state = {p for p in state if not (p[0] == 'm' and p[1] == base and 0 <= p[2] - d < n)}
        elif kind == 'f12':
            reads = {p for p in state if p[0] == 'f' and p[1] in (12, 13)}
            if reads:
                state = set(state) | {('f', 0)}
                reads = set()
            else:
                state = set(state) - {('f', 0)}
        else:
            reads = {p for p in state if (p[0] == 'g' and p[1] in ARG_REGS and p[2] in (0, 1)) or (p[0] == 'f' and p[1] in (12, 13, 14, 15))}
        if reads:
            self.use(pc, 'unknown callee', '%s: %s reaches %s' % (self.code.where(pc), ', '.join(sorted(place_text(self.code, p) for p in reads)), why))
        return state
        upper = {p for p in state if p[0] == 'g' and p[1] in ARG_REGS and p[2] in (2, 3)}
        if upper:
            self.note('%s: the upper half of an argument register (%s) reaches %s: assumed not read (no table function spills an argument register whole)' % (self.code.where(pc), ', '.join(sorted(place_text(self.code, p) for p in upper)), why))

    def call(self, f, call_index, slot_index, state, vals, call_depth):
        pc, mn, ops = f.insns[call_index]
        v = values_after_slot(self.code, f, slot_index)
        target = jal_target(ops[-1]) if mn == 'jal' else None
        if mn == 'jalr':
            state = self.unknown_callee(pc, state, 'a call through a register at %s' % self.code.where(pc), None, v)
            after = {p for p in state if not (p[0] == 'g' and p[1] in (1, 2, 3) + ARG_REGS + (12, 13, 14, 15, 24, 25))}
            return after
        callee = self.code.functions.get(target) if target is not None else None
        if callee is not None and STUBS.get(callee.name) in ('copy', 'fill'):
            # the game's own memoryCopy / memoryFill: the copy itself, not its loop (whose pointers the values cannot follow)
            return self.unknown_callee(pc, state, 'the call of %s at %s' % (callee.name, self.code.where(pc)), target, v)
        if callee is None:
            state = self.unknown_callee(pc, state, 'the call of %s at %s, not in the table' % (self.code.names.get(target) or '0x%x' % (target or 0), self.code.where(pc)), target, v)
            return {p for p in state if not (p[0] == 'g' and p[1] in (1, 2, 3) + ARG_REGS + (12, 13, 14, 15, 24, 25))}
        # the callee is entered only if the lane can reach something it reads
        reads, deadstack, argmem = self.entry_reads.of(callee)
        sp_delta = v.get(29, ('sp0', 0))[1]
        arg_bases = {v[r][0] for r in ARG_REGS if v.get(r) is not None and v[r][0] != 'const'}
        reach = {p for p in state if p[0] != 'm' and p in reads}
        if deadstack:
            # a word below our sp the callee (or its callees) reads of its frame before storing it
            reach |= {p for p in state if p[0] == 'm' and p[1] == 'sp0' and p[2] < sp_delta and (p[2] - sp_delta) in deadstack}
        if argmem:
            reach |= {p for p in state if p[0] == 'm' and (p[1] in arg_bases or (p[1] == 'sp0' and 'sp0' in arg_bases))}
        if not reach:
            self.skipped += 1
            return state
        if call_depth >= self.depth:
            reach = {p for p in state if p[0] != 'm' or p[1] == 'sp0' or any(v.get(r) and v[r][0] == p[1] for r in ARG_REGS)}
            if reach:
                self.unresolved.append('%s: the call of %s is deeper than --depth %d with %s in reach' % (self.code.where(pc), self.code.fname(callee), self.depth, ', '.join(sorted(place_text(self.code, p) for p in list(reach)[:8]))))
            return state
        # only what the callee can reach goes in (the rest it neither reads nor, for us, kills): small entry states
        # are what lets one analysis of the callee serve every call of it
        rest = set(state) - reach
        after = self.enter(callee, reach, v, sp_delta, call_depth + 1, 'call at %s' % self.code.where(pc), caller=f, back=True) | rest
        if self.steps and after != state:
            self.trail.append('%s%s <- %s returned: +%s -%s' % ('  ' * call_depth, self.code.where(pc), self.code.fname(callee),
                              ', '.join(sorted(place_text(self.code, p) for p in after - state)) or '-', ', '.join(sorted(place_text(self.code, p) for p in state - after)) or '-'))
        return after

    def map_into(self, state, v, sp_delta):
        """The caller's taint as the callee sees it at entry: registers as they are, the caller's frame words relative
        to the callee's sp0 (= the caller's sp at the call), records through the argument registers' values."""
        out = set()
        bases = {}
        for k, r in enumerate(ARG_REGS):
            b = v.get(r)
            if b is not None and b[0] != 'const':
                bases.setdefault(b[0], []).append((('arg', k), b[1]))
        for p in state:
            if p[0] != 'm':
                out.add(p)
                continue
            base, off = p[1], p[2]
            if base == 'sp0':
                out.add(('m', 'sp0', (off - sp_delta) & ~3))
                for argbase, d in bases.get('sp0', []):
                    if 0 <= off - d < 0x4000:   # (a pointer argument reaches the words at and after it: a record's fields)
                        out.add(('m', argbase, off - d))
            elif base in bases:
                for argbase, d in bases[base]:
                    if 0 <= off - d < 0x4000:
                        out.add(('m', argbase, off - d))
            else:
                out.add(p)   # (kept: a record the callee cannot reach through its arguments)
        return out

    def map_back(self, state, v, sp_delta):
        out = set()
        for p in state:
            if p[0] != 'm':
                out.add(p)
                continue
            base, off = p[1], p[2]
            if base == 'sp0':
                out.add(('m', 'sp0', (off + sp_delta) & ~3))
            elif isinstance(base, tuple) and base[0] == 'arg':
                r = ARG_REGS[base[1]]
                b = v.get(r)
                if b is not None and b[0] != 'const':
                    out.add(('m', b[0], (off + b[1]) & ~3 if b[0] == 'sp0' else off + b[1]))
                else:
                    out.add(p)
            else:
                out.add(p)
        return out

    def enter(self, callee, state, v, sp_delta, call_depth, label, caller=None, back=False):
        entry = self.map_into(state, v, sp_delta)
        returned = self.run(callee, 0, entry, call_depth, label)
        # A frame word the callee also reaches through a pointer argument went in twice (sp-relative and arg-relative);
        # a store through the pointer killed the arg-relative one alone: the word is dead when either copy is gone.
        pointers = [(k, v[r][1]) for k, r in enumerate(ARG_REGS) if v.get(r) is not None and v[r][0] == 'sp0']
        if pointers:
            kept = set()
            for p in returned:
                if p[0] == 'm' and p[1] == 'sp0':
                    ours = p[2] + sp_delta
                    if any(0 <= ours - d < 0x4000 and ('m', ('arg', k), ours - d) not in returned for k, d in pointers):
                        continue
                kept.add(p)
            returned = kept
        return self.map_back(returned, v, sp_delta)

    # -- the source and the callers --
    def from_source(self, kind, pc, places):
        """places: lane places, or ('sp', off): the frame word at sp+off as sp is at the point (entry, exit: as called)."""
        code = self.code

        def frame_words(sp_delta):
            return {('m', 'sp0', (sp_delta + p[1]) & ~3) if p[0] == 'sp' else p for p in places}

        if kind in ('entry', 'exit'):
            f = code.functions.get(pc)
            if f is None:
                raise SystemExit('wlane: no function starts at 0x%x' % pc)
            if kind == 'exit':
                self.ascend(f, frame_words(0), 1)    # (what f leaves as it returns: its sp0 is the caller's sp at the call)
                return
            returned = self.run(f, 0, frame_words(0), 0, 'source')
            self.ascend(f, returned, 1)
            return
        f = code.function_at(pc)
        if f is None:
            raise SystemExit('wlane: 0x%x is in no function of the table' % pc)
        i = f.index[pc]
        if kind == 'ret':
            succ, call_after = cfg(f)
            if f.insns[i][1] not in ('jal', 'jalr'):
                raise SystemExit('wlane: ret=0x%x is not a call' % pc)
            start = i + 2
        else:
            start = i + 1
        if start >= len(f.insns):
            return
        v = values(code, f)[start] or {}
        sp = v.get(29, ('sp0', 0))
        returned = self.run(f, start, frame_words(sp[1] if sp[0] == 'sp0' else 0), 0, 'source')
        self.ascend(f, returned, 1)

    def ascend(self, f, returned, level):
        """The taint f leaves at its return, carried into each caller after its call of f."""
        if not returned:
            return
        key = (f.start, frozenset(returned))
        if key in self.ascended or level > 40:
            if level > 40:
                self.unresolved.append('%s: the return chain is more than 40 deep (a cycle of callers?): not followed further' % self.code.fname(f))
            return
        self.ascended.add(key)
        callers = self.code.callers.get(f.start, [])
        regs = {p for p in returned if p[0] != 'm'}
        mem = {p for p in returned if p[0] == 'm'}
        if not callers:
            if regs or mem:
                self.notes.append('%s returns with %s and has no jal caller in the table (a task, a callback, a pointer): the walk ends there' % (self.code.fname(f), ', '.join(sorted(place_text(self.code, p) for p in list(returned)[:10]))))
                for p in mem:
                    if p[1] == 'sp0':
                        self.land(f.start, 'dead stack', '%s leaves %s below its caller\'s sp (no caller in the table to follow)' % (self.code.fname(f), place_text(self.code, p)), [p])
            return
        if len(callers) > self.fanout:
            self.unresolved.append('%s returns with %s to %d callers (--fanout %d): not followed' % (self.code.fname(f), ', '.join(sorted(place_text(self.code, p) for p in list(returned)[:8])), len(callers), self.fanout))
            return
        for caller, pc in callers:
            succ, call_after = cfg(caller)
            i = caller.index[pc]
            v = values_after_slot(self.code, caller, i + 1)
            sp_delta = v.get(29, ('sp0', 0))[1]
            state = self.map_back(returned, v, sp_delta)
            if i + 2 >= len(caller.insns):
                continue
            if self.steps:
                self.trail.append('=> %s returns into %s (sp0%+#x there): %s' % (self.code.fname(f), self.code.where(pc), sp_delta, ', '.join(sorted(place_text(self.code, p) for p in state))))
            up = self.run(caller, i + 2, state, 0, 'return into %s' % self.code.where(pc))
            if self.steps:
                self.trail.append('-> returned into %s with %s' % (self.code.where(pc), ', '.join(sorted(place_text(self.code, p) for p in state))))
            self.ascend(caller, up, level + 1)


def report_walk(code, walk, out):
    def w(text=''):
        out.append(text)
    uses = walk.uses
    landings = walk.landings
    w('USES: %d' % len(uses))
    for pc, kind, text in uses[:200]:
        w('  use %-14s %s' % (kind, text))
    if len(uses) > 200:
        w('  ... %d more' % (len(uses) - 200))
    w('LANDINGS: %d' % len(landings))
    by_kind = collections.Counter(k for _, k, _, _ in landings)
    for pc, kind, text, places in landings[:400]:
        w('  land %-11s %s' % (kind, text))
    if len(landings) > 400:
        w('  ... %d more' % (len(landings) - 400))
    if walk.unresolved:
        w('UNRESOLVED: %d' % len(walk.unresolved))
        for t in walk.unresolved[:50]:
            w('  ' + t)
    if walk.notes:
        w('NOTES: %d' % len(walk.notes))
        for t in walk.notes[:50]:
            w('  ' + t)
    w('functions walked: %d' % len(walk.functions_seen))
    if uses:
        verdict = 'LIVE: the lane reaches %d use%s (%s)' % (len(uses), 's' if len(uses) > 1 else '', ', '.join('%s %d' % (k, n) for k, n in collections.Counter(k for _, k, _ in uses).most_common()))
    elif walk.unresolved:
        verdict = 'UNRESOLVED: no use seen, but the walk was cut (%d places)' % len(walk.unresolved)
    elif landings:
        verdict = 'LANDS: no use in the code; %s to prove by their readers (the watch) and the poke' % ', '.join('%d %s' % (n, k) for k, n in by_kind.most_common())
    else:
        verdict = 'DEAD: discarded in registers, nothing stored, nothing used'
    w('VERDICT: ' + verdict)
    return verdict


def parse_source(text, lane):
    m = re.fullmatch(r'(after|entry|ret|exit)=(0x[0-9A-Fa-f]+|[0-9A-Fa-f]+)', text)
    if not m:
        raise SystemExit('wlane: a source is after=PC, entry=PC, ret=PC or exit=PC, then the lane; not %r' % text)
    return m.group(1), int(m.group(2), 16), lane_place(lane)


def with_callers(code, args):
    for path in getattr(args, 'callers', None) or []:
        n = code.add_callers(path)
        print('wlane: %d call edges added from %s' % (n, path), file=sys.stderr)
    return code


def cmd_trace(args):
    code = with_callers(load_code(args.game), args)
    if len(args.source) % 2:
        raise SystemExit('wlane trace: SOURCE pairs: after=PC LANE ...')
    out = []
    for k in range(0, len(args.source), 2):
        kind, pc, places = parse_source(args.source[k], args.source[k + 1])
        walk = Walk(code, depth=args.depth, fanout=args.fanout, budget=args.budget, steps=args.steps)
        walk.from_source(kind, pc, places)
        out.append('SOURCE %s=0x%x %s (%s)' % (kind, pc, args.source[k + 1], code.where(pc)))
        if args.steps:
            out.extend('  ' + t for t in walk.trail[:3000])
        report_walk(code, walk, out)
        out.append('')
    print('\n'.join(out))
    if args.model:
        print(MODEL)


# ---------------------------------------------------------------- readers (the watch's loads followed)

WATCH_LINE = re.compile(r'^([RWA]) (?:pc=0x([0-9a-f]+)|fn=(\S+)) at=0x([0-9a-f]+) size=(\d+) n=(\d+) first=(\d+) last=(\d+)$')


def follow_load(code, pc, at, size, depth, fanout, budget):
    """A load at pc covering the word `at`: the destination lanes that hold it, followed. Returns (places, walk)."""
    f = code.function_at(pc)
    if f is None:
        return None, None
    i = f.index[pc]
    _, mn, ops = f.insns[i]
    if mn not in LOADS:
        return None, None
    places = set()
    if mn == 'lqc2':
        vf = vu_operand(ops[0])
        places.add(('vf', vf[1], (at & 15) // 4))
    elif mn == 'lq':
        places.add(('g', REG[ops[0]], (at & 15) // 4))
    elif mn == 'lwc1':
        places.add(('f', int(ops[0][2:])))
    elif mn in ('ld', 'ldl', 'ldr'):
        places |= {('g', REG[ops[0]], (at & 7) // 4)} if mn == 'ld' else {('g', REG[ops[0]], 0), ('g', REG[ops[0]], 1)}
    else:
        places |= {('g', REG[ops[0]], 0), ('g', REG[ops[0]], 1)}
    walk = Walk(code, depth=depth, fanout=fanout, budget=budget)
    walk.from_source('after', pc, places)
    return places, walk


def readers(code, lines, depth, fanout, budget, out):
    """The watch report's lines: each R followed; returns (all_dead, summary lines)."""
    loads, stores, readable = [], [], []
    for line in lines:
        m = WATCH_LINE.match(line.strip())
        if not m:
            continue
        kind, pc, fn, at, size, n, first, last = m.groups()
        rec = (kind, int(pc, 16) if pc else None, fn, int(at, 16), int(size), int(n), int(first), int(last))
        (loads if kind == 'R' else stores if kind == 'W' else readable).append(rec)
    live = 0
    unresolved = 0
    by_site = {}
    words_of = collections.defaultdict(list)
    for kind, pc, fn, at, size, n, first, last in loads:
        key = (pc, at & 15 if size == 16 else at & 7 if size == 8 else 0, size)
        if key not in by_site:
            places, walk = follow_load(code, pc, at, size, depth, fanout, budget)
            by_site[key] = (places, walk)
        words_of[key].append((at, n, first, last))
    for key in sorted(by_site, key=lambda k: (k[0], k[1])):
        pc, lane, size = key
        places, walk = by_site[key]
        words = words_of[key]
        span = '%d word%s, %d loads, t%d..t%d' % (len(words), 's' if len(words) > 1 else '', sum(w[1] for w in words), min(w[2] for w in words), max(w[3] for w in words))
        if walk is None:
            out.append('  R %s at 0x%x size %d: not a load of the table (%s)' % (code.where(pc), words[0][0], size, span))
            unresolved += 1
            continue
        verdict = 'live' if walk.uses else 'unresolved' if walk.unresolved else 'lands' if walk.landings else 'dead'
        live += bool(walk.uses)
        unresolved += bool(walk.unresolved) and not walk.uses
        f = code.function_at(pc)
        out.append('  R %s %s: %s -> %s (%s; e.g. 0x%x)' % (code.where(pc), f.insns[f.index[pc]][1], ', '.join(sorted(place_text(code, p) for p in places)), verdict, span, words[0][0]))
        if walk.uses:
            kinds = collections.Counter(k for _, k, _ in walk.uses)
            firsts = collections.Counter(code.where(upc).split('+')[0] for upc, _, _ in walk.uses)
            out.append('      %d uses (%s) in %s' % (len(walk.uses), ', '.join('%s %d' % kv for kv in kinds.most_common()), ', '.join('%s %d' % kv for kv in firsts.most_common(6))))
            for upc, ukind, utext in walk.uses[:3]:
                out.append('      use %s %s' % (ukind, utext))
        if walk.landings and not walk.uses:
            kinds = collections.Counter(k for _, k, _, _ in walk.landings)
            out.append('      %d landings (%s)' % (len(walk.landings), ', '.join('%s %d' % kv for kv in kinds.most_common())))
        for t in walk.unresolved[:2]:
            out.append('      unresolved ' + t)
    for kind, pc, fn, at, size, n, first, last in stores:
        out.append('  W %s %s: word 0x%x size %d, %d stores, t%d..t%d' % (code.where(pc), code.function_at(pc).insns[code.function_at(pc).index[pc]][1] if code.function_at(pc) else '?', at, size, n, first, last))
    for kind, pc, fn, at, size, n, first, last in readable:
        out.append('  A %s: word 0x%x size %d, %d accesses, t%d..t%d (a readable body: not followed, the poke decides)' % (fn, at, size, n, first, last))
    return live == 0 and unresolved == 0, len(loads), live, unresolved, len(stores), len(readable)


def cmd_readers(args):
    code = with_callers(load_code(args.game), args)
    lines = open(args.watch).read().split('\n')
    out = []
    all_dead, nloads, live, unresolved, nstores, nreadable = readers(code, lines, args.depth, args.fanout, args.budget, out)
    print('\n'.join(out))
    print('loads %d (live %d, unresolved %d), stores %d, readable accesses %d: %s' % (nloads, live, unresolved, nstores, nreadable, 'every load dead' if all_dead else 'NOT proven'))


# ---------------------------------------------------------------- the poke

POKE_VALUES = ('3F8CCCCD', 'C2F6E979')   # 1.1 and -123.456: ordinary floats, no NaN, no denormal, no zero
# The differential's: those two, then the largest positive and negative bit patterns (an exponent-255 lane, the VU's
# huge numbers: what the gate's stale lanes hold).
DIFF_VALUES = ('3F8CCCCD', 'C2F6E979', '7FFFFFFF', 'FFFFFFFF')
DIFF_LINE = re.compile(r'^D pc=0x([0-9a-f]+) k=(\w) n=(\d+) a=([0-9a-f]+) v=([0-9a-f]+)$')


def career_ab_tool():
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), 'career_ab.py')


def run_side(args, name, env_extra):
    env = dict(os.environ)
    env.update(env_extra)
    cmd = [sys.executable, '-I', career_ab_tool(), 'run', name, '--dir', args.dir, '--input', args.input, '--to', str(args.to),
           '--runner', args.runner, '--clock', str(args.clock), '--game', args.game]
    if args.spec:
        cmd += ['--spec', args.spec]
    if args.secs:
        cmd += ['--secs', str(args.secs)]
    log = open(os.path.join(args.dir, name + '.run.out'), 'w')
    r = subprocess.run(cmd, env=env, stdout=log, stderr=subprocess.STDOUT)
    log.close()
    return r.returncode


def compare_runs(args, a, b, words):
    cmd = [sys.executable, '-I', career_ab_tool(), 'compare', os.path.join(args.dir, a), os.path.join(args.dir, b), '--out', words, '--list', '20']
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.stdout + r.stderr


def parse_words(path):
    """{tick: {addr: (va, vb, region)}} of a compare --out file."""
    out = collections.defaultdict(dict)
    if not os.path.exists(path):
        return out
    for line in open(path):
        m = re.match(r'^t(\d+) 0x([0-9A-Fa-f]+) ([0-9A-Fa-f]+) ([0-9A-Fa-f]+) (\S+)', line)
        if m:
            out[int(m.group(1))][int(m.group(2), 16)] = (m.group(3), m.group(4), m.group(5))
    return out


def observable_differences(compare_text):
    """The compare's problems that are observable (not RAM words, not the VU0 register hashes)."""
    m = re.search(r'^RESULT: (.*)$', compare_text, re.M)
    if not m or m.group(1).startswith('identical'):
        return [], [], []
    problems = [p.strip() for p in m.group(1).split(':', 1)[1].split(';')] if ':' in m.group(1) else [m.group(1)]
    observable, registers, streams = [], [], []
    for p in problems:
        if p.startswith('RAM '):
            continue
        if 'VU0' in p:
            registers.append(p)
        elif p.startswith('vif1 stream') or p.startswith('vif0 stream'):
            # the lane reaches a VIF stream in words the probe's kDeadWords do not cover: the VU1 side's question
            # (zx-vu1check --chain --poke); not observable by itself, not proven here either
            streams.append(p)
        else:
            observable.append(p)
    return observable, registers, streams


def read_diff(path):
    """A ZX_LANE_DIFF report: {(kind, pc): (n, a, v)} (kind C: a callee, its call sites)."""
    out = {}
    for line in open(path):
        m = DIFF_LINE.match(line.strip())
        if m:
            out[(m.group(2) if m.group(2) == 'C' else 'I', int(m.group(1), 16))] = (m.group(2), int(m.group(3)), m.group(4), m.group(5))
    return out


def diff_ticks(args):
    """The ticks the differential reports at: the career A/B's dumps (every 3,000) and --to."""
    return sorted(set(range(3000, args.to + 1, 3000)) | {args.to})


def cmd_diff(args, code, pokes, out):
    """The differential (see the top). Returns (verdict, words, read lines) or the verdict alone when the runs fail."""
    ticks = diff_ticks(args)
    values = list(DIFF_VALUES) + [DIFF_VALUES[0]]
    names = []
    for k, value in enumerate(values):
        spec = ';'.join(p.replace('VALUE', value) for p in pokes)
        name = '%s-d%d' % (args.name, k)
        names.append(name)
        diff_out = os.path.join(args.dir, name + '.diff')
        if not os.path.exists(diff_out + '.t%d' % ticks[-1]) or args.rerun:
            out.append('run %s: ZX_LANE_POKE=%s, ZX_LANE_DIFF at %s' % (name, spec, ','.join(map(str, ticks))))
            rc = run_side(args, name, {'ZX_LANE_POKE': spec, 'ZX_LANE_DIFF': diff_out,
                                       'ZX_LANE_DIFF_AT': ','.join(map(str, ticks))})
            applied = ''
            for log in glob.glob(os.path.join(args.dir, name, '*.log')):
                applied += ''.join(l for l in open(log, errors='replace') if '[lane-poke]' in l or '[lane-diff]' in l)
            out.append('  exit %d; %s' % (rc, applied.strip().replace('\n', '; ') or 'no [lane-poke] line in the run log'))
        if not os.path.exists(diff_out + '.t%d' % ticks[-1]):
            return 'NOT PROVEN: run %s left no differential at tick %d (a ZX_LANE_WATCH_BUILD build is needed)' % (name, ticks[-1]), {}, []
    # RAM and everything observable, each run against the first
    words = collections.defaultdict(dict)
    observable_all = []
    for k in range(1, len(names)):
        words_path = os.path.join(args.dir, '%s.d%d.words.txt' % (args.name, k))
        text = compare_runs(args, names[0], names[k], words_path)
        open(os.path.join(args.dir, '%s.d%d.compare.txt' % (args.name, k)), 'w').write(text)
        result = re.search(r'^RESULT: (.*)$', text, re.M)
        out.append('compare %s against %s (%s): %s' % (names[k], names[0], values[k], result.group(1) if result else text[-300:]))
        observable, registers, streams = observable_differences(text)
        if k == len(names) - 1:
            if not result or not result.group(1).startswith('identical'):
                return 'NOT PROVEN: the repeat of %s differs from its first run (the runs are not deterministic)' % values[0], {}, []
            continue
        observable_all += ['%s: %s' % (values[k], o) for o in observable + streams]
        for t, ws in parse_words(words_path).items():
            for a, w in ws.items():
                words[t].setdefault(a, w)
    # the differential, tick by tick
    control, read, wrote = [], collections.defaultdict(set), collections.defaultdict(set)
    for t in ticks:
        first = read_diff(os.path.join(args.dir, names[0] + '.diff.t%d' % t))
        for k in range(1, len(names)):
            other = read_diff(os.path.join(args.dir, names[k] + '.diff.t%d' % t))
            for key in set(first) | set(other):
                x, y = first.get(key), other.get(key)
                if x is None or y is None or x[1] != y[1] or x[2] != y[2]:
                    if k == len(names) - 1 or len(control) < 200:
                        control.append('t%d %s %s: %s in %s, %s in %s' % (t, key[0] if key[0] == 'C' else (x or y)[0], code.where(key[1]) if key[0] != 'C' else 'calls of ' + code.where(key[1]),
                                       'n=%d a=%s' % (x[1], x[2]) if x else 'absent', names[0], 'n=%d a=%s' % (y[1], y[2]) if y else 'absent', names[k]))
                elif x[3] != y[3]:
                    (read if x[0] == 'L' else wrote)[key[1]].add(values[k])
    out.append('differential at %s: %d instructions or callees whose control, addresses or calls differ; %d loads and %d stores whose values differ'
               % (','.join('t%d' % t for t in ticks), len(control), len(read), len(wrote)))
    for line in control[:40]:
        out.append('  CONTROL ' + line)
    lines = []
    for pc in sorted(read):
        lines.append('reads the lane: %s %s (values %s)' % (code.where(pc), (code.function_at(pc).insns[code.function_at(pc).index[pc]][1] if code.function_at(pc) and pc in code.function_at(pc).index else '?'), ', '.join(sorted(read[pc]))))
    for pc in sorted(wrote):
        lines.append('stores it: %s %s' % (code.where(pc), (code.function_at(pc).insns[code.function_at(pc).index[pc]][1] if code.function_at(pc) and pc in code.function_at(pc).index else '?')))
    out.extend('  ' + l for l in lines)
    if observable_all:
        return 'LIVE: observable differences: %s' % '; '.join(observable_all), words, lines
    if control:
        return 'LIVE: the lane changes control flow, an address or a call (%d places, CONTROL above)' % len(control), words, lines
    moved = len({a for w in words.values() for a in w})
    return ('DEAD on %s (differential): at %s the lane changes no branch, jump, load or store address or call, and nothing '
            'observable; it moves %d RAM words (%s); it is read by the %d loads listed' % (
                args.input, '/'.join(values[:-1]), moved, ', '.join('t%d %d' % (t, len(words[t])) for t in sorted(words)), len(read))), words, lines


def cmd_poke(args):
    code = with_callers(load_code(args.game), args)
    if getattr(args, 'diff', False):
        return cmd_poke_diff(args, code)
    os.makedirs(args.dir, exist_ok=True)
    out = []
    pokes = args.poke.split(',')
    names = []
    for k, value in enumerate(POKE_VALUES):
        spec = ';'.join(p.replace('VALUE', value) for p in pokes)
        name = '%s-p%d' % (args.name, k)
        names.append(name)
        if not os.path.exists(os.path.join(args.dir, name, 'run.txt')) or args.rerun:
            out.append('run %s: ZX_LANE_POKE=%s' % (name, spec))
            rc = run_side(args, name, {'ZX_LANE_POKE': spec})
            applied = ''
            for log in glob.glob(os.path.join(args.dir, name, '*.log')):
                applied += ''.join(l for l in open(log, errors='replace') if '[lane-poke]' in l)
            out.append('  exit %d; %s' % (rc, applied.strip().replace('\n', '; ') or 'no [lane-poke] line in the run log'))
    words_path = os.path.join(args.dir, args.name + '.poke.words.txt')
    text = compare_runs(args, names[0], names[1], words_path)
    open(os.path.join(args.dir, args.name + '.poke.compare.txt'), 'w').write(text)
    observable, registers, streams = observable_differences(text)
    words = parse_words(words_path)
    moved = sorted({a for t in words for a in words[t]})
    out.append('poke compare: %s' % (re.search(r'^RESULT: (.*)$', text, re.M).group(1) if re.search(r'^RESULT: (.*)$', text, re.M) else text[-400:]))
    out.append('moved words: %d (%s)' % (len(moved), ', '.join('t%d %d' % (t, len(words[t])) for t in sorted(words))))
    if registers:
        out.append('VU0 registers at a vsync differ (%s): register residue, not observable' % '; '.join(registers))
    verdict = None
    if observable:
        verdict = 'LIVE: observable differences between the two values: %s' % '; '.join(observable)
    elif streams:
        out.append('the lane reaches a VIF stream in words outside the probe\'s kDeadWords (%s): the VU1 side decides those (zx-vu1check --chain --poke)' % '; '.join(streams))
    # the watch runs over the moved words: the original code (off) and the readable set the gate runs (on)
    readers_ok = True
    if moved and not observable:
        chunks = ','.join('%x' % a for a in moved)
        spec_poke = ';'.join(p.replace('VALUE', POKE_VALUES[0]) for p in pokes)
        for side, side_spec in (('off', 'off'), ('on', args.on_spec)):
            name = '%s-watch-%s' % (args.name, side)
            watch_out = os.path.join(args.dir, '%s.watch-%s.txt' % (args.name, side))
            if not os.path.exists(watch_out) or args.rerun:
                out.append('run %s: ZX_LANE_WATCH over %d words, readable %s' % (name, len(moved), side_spec or 'defaults'))
                saved = args.spec
                args.spec = side_spec
                rc = run_side(args, name, {'ZX_LANE_POKE': spec_poke, 'ZX_LANE_WATCH': chunks, 'ZX_LANE_WATCH_OUT': watch_out,
                                           'ZX_LANE_CALLERS': '1', 'ZX_LANE_CALLERS_OUT': os.path.join(args.dir, '%s.callers-%s.txt' % (args.name, side))})
                args.spec = saved
                out.append('  exit %d' % rc)
            callers_path = os.path.join(args.dir, '%s.callers-%s.txt' % (args.name, side))
            if os.path.exists(callers_path):
                code.add_callers(callers_path)
            lines = open(watch_out).read().split('\n') if os.path.exists(watch_out) else []
            out.append('readers of the moved words with readable %s (the watch, followed):' % side)
            all_dead, nloads, live, unresolved, nstores, nreadable = readers(code, lines, args.depth, args.fanout, args.budget, out)
            out.append('  loads %d (live %d, unresolved %d), stores %d, readable accesses %d' % (nloads, live, unresolved, nstores, nreadable))
            readers_ok = readers_ok and all_dead and nreadable == 0
            if nreadable:
                out.append('  readable bodies access %d of the words: the poke runs decide those; listed above' % nreadable)
            if not all_dead:
                verdict = verdict or 'NOT PROVEN: a reader of a moved word uses its lane (readable %s, above)' % side
    if verdict is None and streams:
        verdict = 'NOT PROVEN HERE: nothing the probe counts observable differs and every RAM reader is followed, but the lane reaches %s: the VU1 side (zx-vu1check) must prove those words' % '; '.join(streams)
    if verdict is None:
        if moved:
            verdict = 'DEAD on %s: the two values move %d RAM words and nothing observable; every load of them is followed to a discard%s' % (
                args.input, len(moved), '' if readers_ok else ' (readable accesses noted)')
        else:
            verdict = 'DEAD on %s: the two values move nothing in RAM and nothing observable' % args.input
    out.append('VERDICT: ' + verdict)
    # the allow the checker reads
    lanes_path = os.path.join(args.dir, args.name + '.lanes.txt')
    with open(lanes_path, 'w') as f:
        f.write('# wlane.py poke: %s, input %s, to tick %d, clock %d, poke %s\n' % (args.game, args.input, args.to, args.clock, args.poke))
        f.write('# %s\n' % verdict)
        f.write('# words the lane moves between the two values, per dump tick (career_ab.py: `lanes=FILE reason`)\n')
        for t in sorted(words):
            for a in sorted(words[t]):
                va, vb, region = words[t][a]
                f.write('t%d 0x%08X %s %s %s\n' % (t, a, va, vb, region))
    out.append('lanes file: %s' % lanes_path)
    print('\n'.join(out))
    open(os.path.join(args.dir, args.name + '.poke.txt'), 'w').write('\n'.join(out) + '\n')
    return 0 if verdict.startswith('DEAD') else 1


def cmd_poke_diff(args, code):
    """poke --diff: the differential's verdict, then the static follow (two runs at POKE_VALUES and the watch) as
    information; the lanes file has the differential's words and its proof."""
    os.makedirs(args.dir, exist_ok=True)
    out = []
    pokes = args.poke.split(',')
    verdict, words, lines = cmd_diff(args, code, pokes, out)
    out.append('VERDICT: ' + verdict)
    info = []
    if not getattr(args, 'no_static', False) and verdict.startswith('DEAD'):
        saved = args.name
        args.name = saved + '-static'
        args.diff = False
        try:
            cmd_poke(args)
        except SystemExit:
            pass
        args.name, args.diff = saved, True
    report = os.path.join(args.dir, args.name + '-static.poke.txt')
    if verdict.startswith('DEAD') and os.path.exists(report):  # (--no-static: an earlier static follow's report, kept)
        info = [l for l in open(report).read().split('\n') if l.startswith('VERDICT') or l.startswith('  loads ')]
        out.append('static follow (information, %s): %s' % (report, ' | '.join(info)))
    lanes_path = os.path.join(args.dir, args.name + '.lanes.txt')
    with open(lanes_path, 'w') as f:
        f.write('# wlane.py poke --diff: %s, input %s, to tick %d, clock %d, poke %s, values %s\n' % (args.game, args.input, args.to, args.clock, args.poke, ', '.join(DIFF_VALUES)))
        f.write('# %s\n' % verdict)
        for l in lines:
            f.write('# %s\n' % l)
        for l in info:
            f.write('# static follow (information): %s\n' % l.strip())
        f.write('# words the lane moves between the values, per dump tick (career_ab.py: `lanes=FILE input=NAME reason`)\n')
        for t in sorted(words):
            for a in sorted(words[t]):
                va, vb, region = words[t][a]
                f.write('t%d 0x%08X %s %s %s\n' % (t, a, va, vb, region))
    out.append('lanes file: %s' % lanes_path)
    print('\n'.join(out))
    open(os.path.join(args.dir, args.name + '.poke.txt'), 'w').write('\n'.join(out) + '\n')
    return 0 if verdict.startswith('DEAD') else 1


def cmd_case(args):
    spec = {}
    for line in open(args.file):
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        key, _, value = line.partition(' ')
        spec.setdefault(key.strip(), []).append(value.strip())
    one = {k: v[-1] for k, v in spec.items()}
    ns = argparse.Namespace(game=one['game'], input=one.get('input'), to=int(one.get('to', '0')), runner=one.get('runner'),
                            dir=one.get('dir', '.'), poke=one.get('poke'), name=one.get('name', os.path.basename(args.file).rsplit('.', 1)[0]),
                            clock=int(one.get('clock', '4096')), spec=one.get('spec', 'off'), on_spec=one.get('on-spec'), secs=int(one.get('secs', '0')) or None,
                            depth=int(one.get('depth', '16')), fanout=int(one.get('fanout', '200')), budget=int(one.get('budget', '600')),
                            rerun=args.rerun, steps=False, model=False, callers=spec.get('callers'))
    code = load_code(ns.game)
    out = ['CASE %s (%s)' % (ns.name, args.file)]
    for t in spec.get('trace', []):
        parts = t.split()
        for k in range(0, len(parts) - 1, 2):
            kind, pc, places = parse_source(parts[k], parts[k + 1])
            walk = Walk(code, depth=ns.depth, fanout=ns.fanout, budget=ns.budget)
            walk.from_source(kind, pc, places)
            out.append('TRACE %s %s (%s)' % (parts[k], parts[k + 1], code.where(pc)))
            report_walk(code, walk, out)
    print('\n'.join(out))
    os.makedirs(ns.dir, exist_ok=True)
    rc = 0
    if ns.poke and ns.input and ns.runner:
        rc = cmd_poke(ns)
    with open(os.path.join(ns.dir, ns.name + '.txt'), 'w') as f:
        f.write('\n'.join(out) + '\n')
        poke_report = os.path.join(ns.dir, ns.name + '.poke.txt')
        if os.path.exists(poke_report):
            f.write(open(poke_report).read())
        f.write('\n' + MODEL)
    return rc


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('trace')
    p.add_argument('--game', required=True)
    p.add_argument('--callers', action='append', help="a run's call graph (ZX_LANE_CALLERS_OUT), for the callers no jal names")
    p.add_argument('source', nargs='+')
    p.add_argument('--depth', type=int, default=16)
    p.add_argument('--fanout', type=int, default=200)
    p.add_argument('--budget', type=int, default=600)
    p.add_argument('--steps', action='store_true', help='every step of the walk')
    p.add_argument('--model', action='store_true', help='print the model')
    p.set_defaults(func=cmd_trace)
    p = sub.add_parser('readers')
    p.add_argument('--game', required=True)
    p.add_argument('--callers', action='append', help="a run's call graph (ZX_LANE_CALLERS_OUT), for the callers no jal names")
    p.add_argument('watch')
    p.add_argument('--depth', type=int, default=16)
    p.add_argument('--fanout', type=int, default=200)
    p.add_argument('--budget', type=int, default=600)
    p.set_defaults(func=cmd_readers)
    p = sub.add_parser('poke')
    p.add_argument('--game', required=True)
    p.add_argument('--callers', action='append', help="a run's call graph (ZX_LANE_CALLERS_OUT), for the callers no jal names")
    p.add_argument('--input', required=True)
    p.add_argument('--to', type=int, required=True)
    p.add_argument('--runner', required=True)
    p.add_argument('--dir', required=True)
    p.add_argument('--poke', required=True, help="ZX_LANE_POKE items with VALUE for the value, ',' between items")
    p.add_argument('--name', required=True)
    p.add_argument('--clock', type=int, default=4096)
    p.add_argument('--spec', default='off', help='the readable set of the two poked runs (off: the original code, where the leftover is)')
    p.add_argument('--on-spec', default=None, help="the readable set of the second watch run (the gate's defaults)")
    p.add_argument('--secs', type=int, default=None)
    p.add_argument('--depth', type=int, default=16)
    p.add_argument('--fanout', type=int, default=200)
    p.add_argument('--budget', type=int, default=600)
    p.add_argument('--rerun', action='store_true')
    p.add_argument('--diff', action='store_true', help='the differential (see the top)')
    p.add_argument('--no-static', action='store_true', help='with --diff: no static follow after it')
    p.set_defaults(func=cmd_poke)
    p = sub.add_parser('case')
    p.add_argument('file')
    p.add_argument('--rerun', action='store_true')
    p.set_defaults(func=cmd_case)
    args = parser.parse_args()
    rc = args.func(args)
    sys.exit(rc or 0)


if __name__ == '__main__':
    main()
