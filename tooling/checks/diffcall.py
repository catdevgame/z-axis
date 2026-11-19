#!/usr/bin/env python3
"""diffcall.py: evidence for functions no recording reaches, from generated calls.

A running game (PS2X_DIFFCALL, game/ee/diffcall.cpp) stops at snapshots; this
tool looks at the snapshot, makes plausible arguments for each target, and has
the game run each case in a forked copy of itself with the recorder on. The
recordings go through the usual ps2xFnCheck replay (fncheck_corpus.py run): the
traced original against the readable version, registers, memory, calls, cycles,
safe points, markers. Only that comparison decides a verdict; a case where the
original faults, hangs or waits is an invalid input and is dropped.

A pass from generated arguments is NOT Verified: `report` writes its own lower
tier (evidence.tsv: `generated` with the blocks run, `generated-shallow` when the
calls stayed near the entry) and nothing here turns anything on by default.

Arguments, best evidence first (a case keeps which kinds made it, `report` counts
the reach per kind):
  caller    what a static caller passes: a constant, a global's value, a field,
            or the caller's own parameter, whose real values the argument bank
            (the game's last values per function and register) holds
  callee    the real values of the parameters this one is handed on to
  sibling   the same parameter of the functions beside it in a function table
            (message handlers, script commands, chunk readers, class methods)
  type      the real values of parameters of the same struct type (ptype rows,
            hand-written signatures)
  shape     the real values of parameters whose fields are read at the same offsets
  buffer    a zeroed block (an out parameter), small integers, compared constants,
            usual floats
  nested    such a block with real objects in the fields it is read through (a
            message note holding the object)
  field     (--fields, for the functions that stay shallow) a kept case again with one
            field its function reads, or one global, set to another value in the
            case's copy of the game: not a state the game was in, so a weak kind
  object    (--object-budget) a seed again with another real object for a pointer parameter:
            not weak, nothing poked
  format, caller, repair, flip, force, havoc
            the search's (diffcall_search.py), which goes on from the cases above

  diffcall.py analyze --build BUILD --out ANALYSIS.json
  diffcall.py run --name NAME --build BUILD --targets FILE --vsyncs V1,V2 --pad career --scratch DIR [--budget N] [--new-blocks FILE] [--search N]
  diffcall.py upper --out FILE
  diffcall.py risky --unc UNC.tsv --out RISKY.tsv [--short-out FILE]
  diffcall.py gather --scratch DIR --runs NAME,... --corpus GEN
  diffcall.py check --build BUILD --corpus GEN --out RUN_DIR [--only @FILE] [--upper FILE]
  diffcall.py report --scratch DIR --runs NAME,... --check RUN_DIR [--out evidence.tsv] [--bytes targets.tsv] [--baseline LINES]
"""

import argparse
import collections
import glob
import json
import os
import random
import re
import shutil
import struct
import subprocess
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ztree  # noqa: E402  (the z-axis tree checked: ztree.py)
import zaxis_paths  # noqa: E402  (tooling/paths.env: the workspaces, the data, the work database)
try:   # the scratch folder: the tooling's own paths.env (and a workshop's own layout, when there)
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    import zpaths  # noqa: E402
    SCRATCH_ROOT = zpaths.ZAXIS_SCRATCH
except Exception:   # (a worker's copy of the checks alone: no tooling around them)
    SCRATCH_ROOT = os.path.expanduser('~')
import readable_symbols
import time

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import diffcall_search as search  # noqa: E402

ROOT = ztree.TREE                                          # the z-axis worktree
WORKSPACE = zaxis_paths.get('AI_WORKSPACE', required=False) or ''  # tools/, recomp/ (its data links)
ELF = os.path.join(WORKSPACE, 'recomp', 'disc-root', 'SLUS_203.27')
GAME = ztree.GAME
NAMES = os.path.join(ztree.TOOLS, 'readable_names.tsv')

ARG_REGS = list(range(4, 12))  # a0-a3, t0-t3
RAM_END = 0x02000000
ZERO, SP, GP, RA = 0, 29, 28, 31
ARENA_DROP = search.ARENA_DROP # game/ee/diffcall.cpp: the poke arena is [sp - ARENA_DROP, sp)


# ---- the ELF ----------------------------------------------------------------

class Elf:
    def __init__(self, path):
        self.data = open(path, 'rb').read()
        d = self.data
        self.entry = struct.unpack_from('<I', d, 24)[0]
        phoff, phsize, phnum = struct.unpack_from('<I', d, 28)[0], struct.unpack_from('<H', d, 42)[0], struct.unpack_from('<H', d, 44)[0]
        self.segments = []
        for i in range(phnum):
            p = phoff + i * phsize
            kind, off, vaddr, _, filesz = struct.unpack_from('<IIIII', d, p)
            if kind == 1 and filesz:
                self.segments.append((vaddr, off, filesz))
        # What the game never writes: code, read-only data and the compiler's literal pools. An input
        # there is no input: a constant the readable version holds as a literal differs for no reason.
        self.constants = []
        shoff, shsize, shnum, shstr = struct.unpack_from('<I', d, 32)[0], *struct.unpack_from('<HHH', d, 46)
        strings = struct.unpack_from('<I', d, shoff + shsize * shstr + 16)[0] if shoff else 0
        for i in range(shnum if shoff else 0):
            name, _, flags, addr, _, size = struct.unpack_from('<IIIIII', d, shoff + shsize * i)
            text = d[strings + name:d.index(b'\0', strings + name)]
            if flags & 2 and (not flags & 1 or text.startswith(b'.lit')):
                self.constants.append((addr, addr + size))

    def constant(self, address):
        a = address & 0x1FFFFFFF
        return any(s <= a < e for s, e in self.constants)

    def word(self, address):
        for v, o, n in self.segments:
            if v <= address and address + 4 <= v + n:
                return struct.unpack_from('<I', self.data, o + address - v)[0]
        return None


# ---- static analysis --------------------------------------------------------
#
# One linear pass over a function: what each argument register holds as a small
# expression (('arg', i), ('const', v), ('add', e, k), ('deref', e, off),
# ('sp', off)), through moves and stack spills, is good enough to read the
# offsets a parameter is used at and what call sites pass. It ignores branches;
# wrong guesses cost only a case that does not reach the body.

LOADS = {0x20: 1, 0x21: 2, 0x22: 4, 0x23: 4, 0x24: 1, 0x25: 2, 0x26: 4, 0x27: 4, 0x37: 8, 0x1E: 16, 0x31: 4, 0x36: 16, 0x1A: 8, 0x1B: 8}
STORES = {0x28: 1, 0x29: 2, 0x2A: 4, 0x2B: 4, 0x2C: 8, 0x2D: 8, 0x2E: 4, 0x3F: 8, 0x1F: 16, 0x39: 4, 0x3E: 16}


def sx(v):
    return v - 0x10000 if v & 0x8000 else v


def expr_add(e, k):
    if e and e[0] == 'const':
        return ('const', (e[1] + k) & 0xFFFFFFFF)
    if e and e[0] == 'sp':
        return ('sp', e[1] + k)
    if e and e[0] == 'add':
        return ('add', e[1], e[2] + k)
    return ('add', e, k) if e else None


def analyze_function(elf, start, end, gp):
    regs = {ZERO: ('const', 0), GP: ('const', gp), SP: ('sp', 0)}
    for i, r in enumerate(ARG_REGS):
        regs[r] = ('arg', i)
    fregs = {12 + i: ('farg', i) for i in range(8)}
    slots = {}
    p = {i: dict(reads={}, writes={}, chain={}, cmps=set(), chars=set(), passed=[], used=False, called=False) for i in range(8)}
    fused = set()
    globals_read = {}
    sites = []

    def base_of(e):
        if not e:
            return None, 0
        if e[0] == 'arg':
            return e[1], 0
        if e[0] == 'add' and e[1] and e[1][0] == 'arg':
            return e[1][1], e[2]
        return None, 0

    def use(r):
        e = regs.get(r)
        if e and e[0] == 'arg':
            p[e[1]]['used'] = True

    def cmp_const(r, value):
        e = regs.get(r)
        if e and e[0] == 'arg':
            p[e[1]]['cmps'].add(value & 0xFFFFFFFF)
        elif e and e[0] == 'deref' and len(e) > 3 and 0 < value < 0x100:   # a byte read through the parameter
            i, _ = base_of(e[1])
            if i is not None:
                p[i]['chars'].add(value)

    def fuse(r):
        e = fregs.get(r)
        if e:
            fused.add(e[1])

    pc = start
    pending = None      # a jal's site, taken after its delay slot
    while pc < end:
        w = elf.word(pc)
        if w is None:
            break
        op, rs, rt, rd, sa, fn = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31, w & 63
        imm = sx(w & 0xFFFF)
        next_pending = None
        if op == 0:
            if fn in (0x21, 0x2D, 0x25) and (rs == ZERO or rt == ZERO):      # move
                src = rt if rs == ZERO else rs
                regs[rd] = regs.get(src)
            elif fn in (0x08, 0x09):                                         # jr, jalr
                if rs != RA:
                    e = regs.get(rs)
                    if e and e[0] == 'arg':
                        p[e[1]]['called'] = True
                    use(rs)
                if fn == 0x09:
                    next_pending = ('indirect', pc)
            elif fn in (0x0C, 0x0D, 0x0F):                                   # syscall, break, sync
                pass
            else:
                use(rs)
                use(rt)
                if rd:
                    regs[rd] = None
        elif op == 3:                                                        # jal
            target = (pc & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
            next_pending = ('jal', target, pc)
        elif op == 2:                                                        # j (a tail call out of the function)
            target = (pc & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
            if not start <= target < end:
                next_pending = ('jal', target, pc)
        elif op == 0x0F:
            regs[rt] = ('const', (imm << 16) & 0xFFFFFFFF)
        elif op in (0x09, 0x19):                                             # addiu, daddiu
            if rt == SP:
                pass                                                         # (the frame: offsets are from the sp after it)
            elif rs == SP:
                regs[rt] = ('sp', imm)
            else:
                regs[rt] = expr_add(regs.get(rs), imm)
                use(rs)
        elif op == 0x0D:                                                     # ori
            e = regs.get(rs)
            regs[rt] = ('const', e[1] | (w & 0xFFFF)) if e and e[0] == 'const' else None
        elif op in (0x0A, 0x0B):                                             # slti, sltiu
            cmp_const(rs, imm)
            cmp_const(rs, imm - 1)
            use(rs)
            regs[rt] = None
        elif op == 0x0C:                                                     # andi
            cmp_const(rs, w & 0xFFFF)
            use(rs)
            regs[rt] = None
        elif op in (4, 5, 0x14, 0x15):                                       # beq, bne (l)
            use(rs)
            use(rt)
            a, b = regs.get(rs), regs.get(rt)
            if b and b[0] == 'const':
                cmp_const(rs, b[1])
            if a and a[0] == 'const':
                cmp_const(rt, a[1])
        elif op in (1, 6, 7, 0x16, 0x17):
            use(rs)
        elif op in LOADS:
            base = regs.get(rs)
            size = LOADS[op]
            i, k = base_of(base)
            if i is not None:
                p[i]['used'] = True
                p[i]['reads'][k + imm] = size
            elif base and base[0] == 'deref' and base[1] and base[1][0] == 'arg':
                p[base[1][1]]['chain'].setdefault(base[2], set()).add(imm)
            elif base and base[0] == 'const' and rs != ZERO and size <= 8:
                globals_read[(base[1] + imm) & 0xFFFFFFFF] = size
            if op in (0x31, 0x36):
                if op == 0x31:
                    fregs.pop(rt, None)
            elif base and base[0] == 'sp':
                regs[rt] = slots.get(base[1] + imm)
            else:
                regs[rt] = (('deref', base, imm, 'byte') if size == 1 else ('deref', base, imm)) if base else None
        elif op in STORES:
            size = STORES[op]
            base = regs.get(rs)
            i, k = base_of(base)
            if base and base[0] == 'sp':
                off = base[1] + imm
                slots[off] = regs.get(rt) if op not in (0x39, 0x3E) else None
                if op == 0x39:
                    fuse(rt)
            else:
                if i is not None:
                    p[i]['used'] = True
                    p[i]['writes'][k + imm] = size
                use(rt)
                if op == 0x39:
                    fuse(rt)
        elif op == 0x11:                                                     # COP1
            if rs == 0:                                                      # mfc1
                fuse(rd)
                regs[rt] = None
            elif rs == 4:                                                    # mtc1
                fregs.pop(rd, None)
                use(rt)
            elif rs == 16:                                                   # S format
                fuse(rd)
                fuse(rt)
                fregs.pop(sa, None)
            elif rs in (8,):                                                 # bc1
                pass
        if pending:
            kind = pending[0]
            args = {i: regs.get(r) for i, r in enumerate(ARG_REGS)}
            fargs = {i: fregs.get(12 + i) for i in range(4)}
            if kind == 'jal':
                sites.append((pending[1], args, pending[2], fargs))
                for i, e in args.items():
                    if e and e[0] == 'arg':
                        p[e[1]]['passed'].append((pending[1], i))
                    b, k = base_of(e) if e else (None, 0)
                    if b is not None and e[0] == 'add':
                        p[b]['passed'].append((pending[1], i))
            for r in (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 24, 25, 31):
                regs[r] = None
            for r in range(0, 20):
                fregs.pop(r, None)
        pending = next_pending
        pc += 4
    return dict(params=p, fused=sorted(fused), sites=sites, globals=globals_read)


# ---- the kinds of code readable versions got wrong ----------------------------
#
# Verified proves the recorded paths only. These scans list where a readable version
# is likelier to differ on a path no recording ran (quar1's two): `wide_scan` a call's
# v0 used as 64 bits (__svfscanf's %ld) and a 32-bit result used as 64 bits (its sign
# extension is part of the value); `short_circuits` a boolean operator with a call on
# its right side in hand-written code, which the original may make whatever the left
# side says (trickRelease).

WIDE_SHIFTS = {0x38, 0x3A, 0x3B, 0x3C, 0x3E, 0x3F, 0x14, 0x16, 0x17}   # dsll dsrl dsra, +32, and the v forms
OP32 = {0x21, 0x23, 0x00, 0x02, 0x03, 0x04, 0x06, 0x07}                 # addu subu sll srl sra sllv srlv srav


def wide_scan(elf, start, end):
    """(calls, extensions): the pcs where a call's v0, or a 32-bit result, reaches a 64-bit use."""
    src = {}            # register: 'call', 'icall' or '32'
    calls, ext = [], []

    def consume(r, pc):
        k = src.get(r)
        if k in ('call', 'icall'):
            calls.append((pc, k))
        elif k == '32':
            ext.append(pc)

    pc = start
    pending = None
    while pc < end:
        w = elf.word(pc)
        if w is None:
            break
        op, rs, rt, rd, sa, fn = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31, w & 63
        next_pending = None
        if op == 0:
            if fn in (0x2D, 0x25, 0x21) and (rs == ZERO or rt == ZERO):
                from_reg = rt if rs == ZERO else rs
                if fn == 0x21:
                    src[rd] = '32' if src.get(from_reg) in (None, '32') and from_reg else src.get(from_reg)
                else:
                    src[rd] = src.get(from_reg)
            elif fn in (0x2D, 0x2F):                                        # daddu, dsubu
                consume(rs, pc)
                consume(rt, pc)
                src.pop(rd, None)
            elif fn in WIDE_SHIFTS:
                consume(rt, pc)
                src.pop(rd, None)
            elif fn in OP32 and w != 0:
                src[rd] = '32'
            elif fn == 0x09:
                next_pending = 'icall'
            elif fn in (0x08,):
                pass
            elif rd:
                src.pop(rd, None)
        elif op == 3:
            next_pending = 'call'
        elif op == 0x09 and rt != SP:                                       # addiu
            if w & 0xFFFF:
                src[rt] = '32'
            else:
                src[rt] = '32' if rs else None
        elif op == 0x23:                                                    # lw
            src[rt] = '32'
        elif op == 0x19:                                                    # daddiu
            if w & 0xFFFF:
                consume(rs, pc)
            src[rt] = src.get(rs) if not w & 0xFFFF else None
        elif op == 0x3F:                                                    # sd
            consume(rt, pc)
        elif op in LOADS:
            src.pop(rt, None)
        elif op in (0x0C, 0x0D, 0x0E, 0x0A, 0x0B, 0x0F):
            src.pop(rt, None)
        if pending:
            for r in (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 24, 25):
                src.pop(r, None)
            src[2] = pending
        pending = next_pending
        pc += 4
    return calls, ext


CALL_ON_RIGHT = re.compile(r'(\|\||&&)[^;{}]*?\b(?:game::call(?:At)?<|ee::call<|[a-z]\w*\()')


def short_circuits():
    """{function name: [source lines]}: hand-written functions with a call right of || or &&."""
    out = collections.defaultdict(list)
    for root, _, files in os.walk(GAME):
        if os.sep + 'sweep' in root or (os.sep + 'ee') in root.replace(GAME, ''):
            continue
        for name in files:
            if not name.endswith('.cpp'):
                continue
            text = readable_symbols.literal_view(open(os.path.join(root, name), errors='replace').read())
            reg = {m.group(1): m.start() for m in re.finditer(r'READABLE_FUNCTION\w*\(\s*0x[0-9A-Fa-f]+,\s*(\w+)', text)}
            for fn, at in reg.items():
                head = None
                for m in re.finditer(r'^\s*(?:[\w:<>\*&]+\s+)+\*?' + re.escape(fn) + r'\s*\([^;{]*\)\s*\{', text[:at], re.M):
                    head = m
                if not head:
                    continue
                body = text[head.end():at]
                for line in body.split('\n'):
                    code = line.split('//')[0]
                    if CALL_ON_RIGHT.search(code):
                        out[fn].append(code.strip()[:120])
    return out


def cmd_risky(a):
    """The Verified functions with blocks no recording ran, the kinds that failed first."""
    elf = Elf(ELF)
    rows = {int(r['addr']): int(r['end_addr']) for r in query("SELECT addr, end_addr FROM functions WHERE build='ai_ps2'")}
    registry = {int(r['addr']): r for r in query("SELECT addr, name, file, status FROM readable")}
    shorts = short_circuits()
    out = []
    for line in open(a.unc):
        c = line.rstrip('\n').split('\t')
        address, status, name, blocks, hit = int(c[0], 16), c[1], c[2], int(c[3]), int(c[4])
        if status != 'verified' or address not in rows:
            continue
        missing = [int(x, 16) for x in c[5].split(',')] if len(c) > 5 and c[5] else []
        calls, ext = wide_scan(elf, address, rows[address])
        reg = registry.get(address)
        hand = bool(reg and reg['file'] and 'sweep/' not in reg['file'])
        short = shorts.get(name.split('::')[-1], [])
        out.append(dict(address=address, name=name, hand=hand, blocks=blocks, missing=len(missing), short=short,
                        icalls=sum(1 for _, k in calls if k == 'icall'), calls=len(calls), ext=len(ext)))
    def rank(r):
        return (-(bool(r['short']) and r['missing'] > 0), -(r['icalls'] > 0 and r['missing'] > 0), -(r['hand'] and r['ext'] > 0 and r['missing'] > 0),
                -(r['ext'] > 0 and r['missing'] > 0), -r['missing'])
    out.sort(key=rank)
    with open(a.out, 'w') as f:
        for r in out:
            f.write('0x%08X\t%s\t%s\tblocks=%d\tmissing=%d\tshort=%d\ticalls=%d\tcalls=%d\text=%d\n' % (
                r['address'], r['name'], 'hand' if r['hand'] else 'swept', r['blocks'], r['missing'], len(r['short']), r['icalls'], r['calls'], r['ext']))
    have = [r for r in out if r['missing']]
    print('%d Verified, %d with blocks no recording ran: short circuits %d, icall v0 used wide %d, 32-bit used wide %d (hand %d), direct call v0 %d' % (
        len(out), len(have), sum(1 for r in have if r['short']), sum(1 for r in have if r['icalls']), sum(1 for r in have if r['ext']),
        sum(1 for r in have if r['ext'] and r['hand']), sum(1 for r in have if r['calls'])))
    if a.short_out:
        with open(a.short_out, 'w') as f:
            for r in out:
                for line in r['short']:
                    f.write('0x%08X\t%s\tmissing=%d\t%s\n' % (r['address'], r['name'], r['missing'], line))


def enc(e):
    """An expression as JSON."""
    if e is None or isinstance(e, (int, str)):
        return e
    return [enc(x) for x in e]


def dec(e):
    if isinstance(e, list):
        return tuple(dec(x) for x in e)
    return e


def gp_value(elf):
    """The value crt0 sets gp to, or hands SetupThread in a0 (lui/addiu at the entry point)."""
    pc, hi = elf.entry, None
    for _ in range(64):
        w = elf.word(pc)
        if w is None:
            break
        op, rs, rt = w >> 26, (w >> 21) & 31, (w >> 16) & 31
        if op == 0x0F and rt in (GP, 4):
            hi = (w & 0xFFFF) << 16
        elif op in (0x09, 0x0D, 0x19) and rt in (GP, 4) and hi is not None:
            return (hi + sx(w & 0xFFFF)) & 0xFFFFFFFF if op != 0x0D else hi | (w & 0xFFFF)
        pc += 4
    return 0


def query(sql):
    db = os.path.join(SCRATCH_ROOT, 'workdb', 'work.duckdb')
    r = subprocess.run(['duckdb', '-no-agent', '-readonly', '-json', db, sql], capture_output=True, text=True)
    if r.returncode:
        sys.exit('query failed: ' + r.stderr)
    return json.loads(r.stdout or '[]')


def signature_types(registry):
    """{(address, parameter): struct type} from the ptype rows and the hand-written signatures."""
    types = {}
    for line in open(NAMES, errors='replace'):
        c = line.rstrip('\n').split('\t')
        if len(c) > 2 and c[0] == 'ptype':
            a, _, n = c[1].partition(':')
            if n.isdigit():
                types[(int(a, 16), int(n) - 1)] = c[2].split('::')[-1]
    done = {}
    for address, (name, path) in registry.items():
        if not path or 'sweep/' in path:
            continue
        text = done.get(path)
        if text is None:
            try:
                text = done[path] = readable_symbols.literal_view(open(os.path.join(GAME, path), errors='replace').read())
            except OSError:
                text = done[path] = ''
        m = re.search(r'\b' + re.escape(name) + r'\s*\(([^;{}()]*)\)\s*(?:\n\s*)?\{', text)
        if not m or not m.group(1).strip():
            continue
        i = 0
        for param in m.group(1).split(','):
            param = param.strip()
            t = re.search(r'ee::Ptr<\s*(?:[\w:]*::)?(\w+)\s*>|(?:^|\s)(?:const\s+)?(?:[\w:]*::)?(\w+)\s*\*', param)
            if 'Float' in param or re.match(r'float\b', param):
                continue            # (floats are counted apart, in f12..)
            if t and (t.group(1) or t.group(2)) not in ('uint8_t', 'char', 'void', 'uint32_t', 'int32_t', 'uint16_t'):
                types[(address, i)] = t.group(1) or t.group(2)
            i += 1
    return types


def string_at(elf, address):
    """The string literal at an address of the ELF, or None."""
    out = bytearray()
    while len(out) < 240:
        w = elf.word(address & ~3)
        if w is None:
            return None
        c = (w >> (8 * (address & 3))) & 0xFF
        if c == 0:
            return out.decode('ascii') if len(out) >= 2 else None
        if not (0x20 <= c < 0x7F or c in (9, 10, 13)):
            return None
        out.append(c)
        address += 1
    return None


def string_parameters(elf, raw, registry):
    """{address: {parameter: 'format' or 'text'}}: char pointers by their signatures (the manifests'
    Ghidra lines, the hand-written ones), their use (bytes read through them and compared) or what
    static callers pass (a string literal; a format if it has a conversion in it). A wrapper's format
    is its callee's."""
    found = collections.defaultdict(dict)
    conversion = re.compile(r'%[-+ #0]*\d*(\.\d*)?[hlLq]*[diouxXcspnfeEgG]')
    for s, f in raw.items():
        for target, args, _, _ in f['sites']:
            for i, e in args.items():
                if e and e[0] == 'const':
                    text = string_at(elf, e[1])
                    if text is not None:
                        role = 'format' if conversion.search(text) else 'text'
                        if found[target].get(i) != 'format':
                            found[target][i] = role
    for path in glob.glob(os.path.join(GAME, '*', 'sweep', 'manifest.txt')):
        for line in open(path, errors='replace'):
            c = line.rstrip('\n').split('\t')
            if len(c) < 8 or '(' not in c[7]:
                continue
            address = int(c[0], 16)
            i = 0
            for param in c[7][c[7].index('(') + 1:c[7].rindex(')')].split(','):
                if param.strip() in ('void', '...', '') or re.match(r'\s*(float|double)\b', param):
                    continue
                if re.search(r'\b(char|byte)\s*\*', param):
                    found[address][i] = 'format' if re.search(r'fmt|format', param) else 'text'
                i += 1
    done = {}
    for address, (name, path) in registry.items():
        if not path or 'sweep/' in path:
            continue
        text = done.get(path)
        if text is None:
            try:
                text = done[path] = readable_symbols.literal_view(open(os.path.join(GAME, path), errors='replace').read())
            except OSError:
                text = done[path] = ''
        m = re.search(r'\b' + re.escape(name) + r'\s*\(([^;{}()]*)\)\s*(?:\n\s*)?\{', text)
        if m:
            for i, param in enumerate(m.group(1).split(',')):
                if re.search(r'Ptr<\s*(const\s+)?char|\bchar\s*\*', param):
                    found[address][i] = 'format' if re.search(r'fmt|format', param) else 'text'
    for s, f in raw.items():
        for i, q in f['params'].items():
            if q['chars']:
                found[s][i] = 'format' if 0x25 in q['chars'] else found[s].get(i, 'text')
    fresh = True
    while fresh:
        fresh = False
        for s, f in raw.items():
            for i, q in f['params'].items():
                role = found.get(s, {}).get(i)
                if role != 'format':
                    continue
                for t, j in q['passed']:
                    if found[t].get(j) != 'format':
                        found[t][j] = 'format'
                        fresh = True
    return found


def address_takers(elf, ends, starts):
    """{function: the functions whose code loads its address (lui, then addiu or ori on that register)}: a
    function no jal reaches may be registered as a handler or a callback from there."""
    out = collections.defaultdict(set)
    for start, end in ends.items():
        pc = start
        while pc < end:
            w = elf.word(pc)
            if w is not None and w >> 26 == 0x0F:
                rt, high = (w >> 16) & 31, (w & 0xFFFF) << 16
                for k in range(1, 12):
                    x = elf.word(pc + 4 * k)
                    if x is None:
                        break
                    if x >> 26 in (0x09, 0x19, 0x0D) and (x >> 21) & 31 == rt:
                        low = x & 0xFFFF if x >> 26 == 0x0D else sx(x & 0xFFFF)
                        target = (high + low) & 0xFFFFFFFF
                        if target in starts:
                            out[target].add(start)
                        break
            pc += 4
    return out


def cmd_analyze(a):
    elf = Elf(ELF)
    gp = gp_value(elf)
    rows = query("SELECT addr, end_addr, bytes FROM functions WHERE build='ai_ps2' ORDER BY addr")
    starts = {int(r['addr']) for r in rows}
    ends = {int(r['addr']): int(r['end_addr']) for r in rows}
    registry = {int(r['addr']): (r['name'], r['file']) for r in query("SELECT addr, name, file FROM readable")}
    raw = {}
    for r in rows:
        s, e = int(r['addr']), int(r['end_addr'])
        raw[s] = analyze_function(elf, s, e, gp)
    # A parameter is live if the function or something it hands it on to uses it: every argument register
    # still unset at a call looks handed on, most of them only left there.
    live = {s: {i for i, q in f['params'].items() if q['used']} for s, f in raw.items()}
    fresh = True
    while fresh:
        fresh = False
        for s, f in raw.items():
            for i, q in f['params'].items():
                if i not in live[s] and any(j in live.get(t, ()) for t, j in q['passed']):
                    live[s].add(i)
                    fresh = True
    # A pointer: its fields are used, or it is handed on to a parameter that is one, or its type says so.
    typed = signature_types(registry)
    pointers = {s: {i for i, q in f['params'].items() if q['reads'] or q['writes'] or q['chain'] or (s, i) in typed}
                for s, f in raw.items()}
    fresh = True
    while fresh:
        fresh = False
        for s, f in raw.items():
            for i, q in f['params'].items():
                if i not in pointers[s] and any(j in pointers.get(t, ()) for t, j in q['passed']):
                    pointers[s].add(i)
                    fresh = True
    strings = string_parameters(elf, raw, registry)
    fns = {}
    for s, f in raw.items():
        params = {}
        for i, q in f['params'].items():
            if i in live[s]:
                params[i] = dict(pointer=i in pointers[s], reads={str(k): v for k, v in q['reads'].items()},
                                 writes={str(k): v for k, v in q['writes'].items()},
                                 chain={str(k): sorted(v) for k, v in q['chain'].items()},
                                 cmps=sorted(q['cmps']), passed=[x for x in q['passed'] if x[1] in live.get(x[0], ())],
                                 called=q['called'])
        fns[s] = dict(params=params, fused=f['fused'], globals={str(k): v for k, v in f['globals'].items()},
                      end=ends[s], strings={str(i): r for i, r in strings.get(s, {}).items()},
                      sites=[[t, {i: enc(e) for i, e in args.items() if e is not None and i in live.get(t, ())}, pc]
                             for t, args, pc, _ in f['sites']])
    # Who calls whom with what (the reverse of `sites`).
    callers = collections.defaultdict(list)
    for s, f in fns.items():
        for t, args, pc in f['sites']:
            callers[t].append((s, pc, args))
    # Function tables: data words that hold function starts, and their neighbours.
    words = {}
    for v, o, n in elf.segments:
        for off in range(0, n - 3, 4):
            w = struct.unpack_from('<I', elf.data, o + off)[0]
            if w in starts:
                words[v + off] = w
    siblings = collections.defaultdict(set)
    for at, fn in words.items():
        for d in range(-0x40, 0x44, 4):
            n = words.get(at + d)
            if n is not None and n != fn:
                siblings[fn].add(n)
    types = typed
    takers = address_takers(elf, ends, starts)
    out = dict(gp=gp, fns={str(k): v for k, v in fns.items()}, callers={str(k): v for k, v in callers.items()},
               siblings={str(k): sorted(v) for k, v in siblings.items()}, takers={str(k): sorted(v) for k, v in takers.items()},
               types={'%x:%d' % k: v for k, v in types.items()})
    with open(a.out, 'w') as f:
        json.dump(out, f)
    print('analyzed %d functions, gp %#x, %d with table neighbours, %d typed parameters -> %s' % (len(fns), gp, len(siblings), len(types), a.out))


# ---- arguments --------------------------------------------------------------

class Snapshot:
    def __init__(self, folder):
        self.folder = folder
        self.ram = open(os.path.join(folder, 'ram.bin'), 'rb').read()
        self.regs = {}
        for line in open(os.path.join(folder, 'regs.txt')):
            k, v = line.split()
            self.regs[k] = int(v, 16) if k.startswith('r') else int(v)
        self.bank = collections.defaultdict(list)       # (address, reg): [(tick, value)]
        path = os.path.join(folder, 'bank.tsv')
        if os.path.exists(path):
            for line in open(path):
                c = line.split('\t')
                self.bank[(int(c[0], 16), int(c[1]))].append((int(c[3]), int(c[2], 16)))
            for rows in self.bank.values():
                rows.sort(reverse=True)

    def word(self, address):
        a = address & 0x1FFFFFFF
        if a + 4 > len(self.ram):
            return None
        return struct.unpack_from('<I', self.ram, a)[0]

    def valid(self, v):
        v &= 0xFFFFFFFF
        return 0x00100000 <= v < RAM_END or 0x70000000 <= v < 0x70004000

    def values(self, address, reg, limit=8):
        return [v for _, v in self.bank.get((address, reg), [])[:limit]]

    def live(self, address, window):
        """The function ran in the last `window` vsyncs before this snapshot (the bank's ticks)."""
        rows = self.bank.get((address, 0))
        return bool(rows) and rows[0][0] >= self.regs['vsync'] - window


class Maker:
    """Candidates per parameter of a target, each with the kind of evidence."""

    def __init__(self, analysis, snap, rng):
        self.a = analysis
        self.s = snap
        self.rng = rng
        self.fns = analysis['fns']
        self.shape_index = None
        self.pool = {}

    def evaluate(self, e, caller, depth):
        """The values expression e has at a call in `caller`, from the bank and the snapshot."""
        if e is None:
            return []
        kind = e[0]
        if kind == 'const':
            return [e[1]]
        if kind == 'arg':
            found = self.s.values(caller, e[1])
            if found or depth <= 0:
                return found
            out = []
            for c, _, args in self.a['callers'].get(str(caller), [])[:6]:
                inner = args.get(str(e[1]))
                if inner is not None:
                    out += self.evaluate(dec(inner), c, depth - 1)
            return out
        if kind == 'add':
            return [(v + e[2]) & 0xFFFFFFFF for v in self.evaluate(e[1], caller, depth)]
        if kind == 'deref':
            out = []
            for v in self.evaluate(e[1], caller, depth):
                w = self.s.word((v + e[2]) & 0xFFFFFFFF) if self.s.valid(v + e[2]) else None
                if w is not None:
                    out.append(w)
            return out
        return []

    def nested(self, target, q):
        """For a made-up block: real objects for the pointer fields it is read through,
        {offset: [values]}, from parameters whose fields are read at the same offsets (a
        message's note, whose +4 is the real object its handler works on)."""
        out = {}
        for off, reads in q.get('chain', {}).items():
            found = []
            for _, k, j in self.shape_donors(target, None, dict(reads={str(x): 4 for x in reads}), least=1):
                found += [v for v in self.s.values(k, j, 2) if self.s.valid(v) and not v & 3]
            if found and 0 <= int(off) < 0x380:
                out[int(off)] = found[:12]
        return out

    def shape_donors(self, target, i, params, least=2):
        reads = set(int(k) for k in params.get('reads', {})) | set(int(k) for k in params.get('writes', {}))
        if len(reads) < least:
            return []
        if self.shape_index is None:
            index = []
            for k, f in self.fns.items():
                for j, q in f['params'].items():
                    off = set(int(x) for x in q['reads']) | set(int(x) for x in q['writes'])
                    if len(off) >= 2 and self.s.bank.get((int(k), int(j))):
                        index.append((int(k), int(j), off))
            self.shape_index = index
        scored = []
        for k, j, off in self.shape_index:
            if k == target:
                continue
            common = len(reads & off)
            if common * 2 >= len(reads):
                scored.append((common / len(reads | off), k, j))
        scored.sort(reverse=True)
        return scored[:6]

    def candidates(self, target, i, params, limit=24, per_donor=2):
        """[(value, kind)], best kind first, no repeats; within a kind in this snapshot's own order: a
        fixed one would try the same few objects at every snapshot and seed (a function may need the
        one object of its type that has some field set)."""
        out, seen = [], set()
        count = collections.Counter()

        def add(v, kind):
            v &= 0xFFFFFFFFFFFFFFFF
            if v not in seen and len(out) < limit:
                seen.add(v)
                out.append((v, kind))
                count[kind] += 1

        def donors(rows):
            rows = list(rows)
            self.rng.shuffle(rows)
            return rows

        key = str(target)
        for c, _, args in self.a['callers'].get(key, []):
            e = args.get(str(i))
            if e is not None:
                for v in self.evaluate(dec(e), c, 2):
                    add(v, 'caller')
        for t, j in params.get('passed', []):
            for v in self.s.values(t, j, 4):
                add(v, 'callee')
        for n in donors(self.a['siblings'].get(key, []))[:24]:
            for v in self.s.values(n, i, per_donor):
                add(v, 'sibling')
        t = self.a['types'].get('%x:%d' % (target, i))
        if t:
            room = len(out) + max(8, (limit - len(out)) // 2)    # (the shape's donors keep their part of the list)
            for k in donors(k for k, name in self.a['types'].items() if name == t):
                a, _, j = k.partition(':')
                for v in self.s.values(int(a, 16), int(j), per_donor):
                    if len(out) < room:
                        add(v, 'type')
        for _, k, j in self.shape_donors(target, i, params):
            for v in self.s.values(k, j, max(3, per_donor)):
                add(v, 'shape')
        return out

    def objects(self, target, i, params):
        """Every real object this snapshot's bank has for a pointer parameter (aligned, in RAM)."""
        key = (target, i)
        if key not in self.pool:
            self.pool[key] = [v for v, _ in self.candidates(target, i, params, 512, 8)
                              if self.s.valid(v) and not v & 3]
        return self.pool[key]

    def plain(self, params, block):
        """For a parameter no real value reached: what its use suggests."""
        out = []
        if params.get('pointer'):
            return [(block, 'buffer'), (0, 'buffer')]
        for c in params.get('cmps', [])[:8]:
            out.append((c & 0xFFFFFFFF, 'buffer'))
        for v in (0, 1, 2, 3, 0xFFFFFFFF):
            out.append((v, 'buffer'))
        return out


USUAL_FLOATS = [0.0, 1.0, -1.0, 0.5, 2.0, 100.0]


def sext32(v):
    v &= 0xFFFFFFFF
    return v | 0xFFFFFFFF00000000 if v & 0x80000000 else v


def make_cases(maker, target, budget, first_id):
    """Case lines for one target at one snapshot, and {id: kinds}."""
    s, rng = maker.s, maker.rng
    f = maker.fns.get(str(target))
    sp = s.regs['r29'] & ~15
    arena = sp - ARENA_DROP + search.ARENA_SKIP
    # A zeroed block for a pointer nothing better fits, a second one its pointer fields point to.
    pokes = ['%x*%x' % (arena, search.ARENA_SIZE)]
    lists = []
    params = f['params'] if f else {}
    fields = {}                 # block address: {offset: [real objects]}
    for i in range(8):
        q = params.get(str(i))
        if q is None:
            lists.append([(0, 'unused')])
            continue
        got = maker.candidates(target, i, q)
        block = arena + 0x700 * i
        for off in q.get('chain', {}):
            if 0 <= int(off) < 0x380 and q.get('pointer'):
                pokes.append('%x=%s' % (block + int(off), struct.pack('<I', block + 0x380).hex()))
        if q.get('pointer'):
            fields[block] = maker.nested(target, q)
        lists.append(got + maker.plain(q, block) if got else maker.plain(q, block))
    fused = set(f['fused']) if f else set()
    flists = []
    for i in range(8):
        if i not in fused:
            flists.append([(0, 'unused')])
            continue
        real = [(v & 0xFFFFFFFF, 'callee') for v in s.values(target, 8 + i)]
        for c, _, _ in maker.a['callers'].get(str(target), [])[:6]:
            real += [(v & 0xFFFFFFFF, 'caller') for v in s.values(c, 8 + i, 2)]
        for x in USUAL_FLOATS:
            real.append((struct.unpack('<I', struct.pack('<f', x))[0], 'buffer'))
        flists.append(real)
    live = [i for i in range(8) if lists[i][0][1] != 'unused']
    flive = [i for i in range(8) if flists[i][0][1] != 'unused']
    if not live and not flive:
        count = 1
    else:
        total = 1
        for i in live:
            total *= len(lists[i])
        for i in flive:
            total *= len(flists[i])
        count = min(budget, total)
    many = any(fields.values())      # (each case draws its own objects for the fields)
    if many:
        count = budget
    lines, kinds, seen = [], {}, set()
    base = [0] * 8
    fbase = [0] * 8
    n = 0
    attempts = 0
    while n < count and attempts < count * 6:
        attempts += 1
        pick, fpick = list(base), list(fbase)
        if attempts > 1:
            # One parameter at a time through its list first, then mixed.
            k = attempts - 2
            order = [(0, i, j) for i in live for j in range(1, len(lists[i]))] + [(1, i, j) for i in flive for j in range(1, len(flists[i]))]
            if k < len(order):
                w, i, j = order[k]
                (fpick if w else pick)[i] = j
            else:
                for i in live:
                    pick[i] = min(rng.randrange(len(lists[i])), rng.randrange(len(lists[i])))
                for i in flive:
                    fpick[i] = rng.randrange(len(flists[i]))
        key = tuple(pick) + tuple(fpick)
        if key in seen and not many:
            continue
        seen.add(key)
        ints = [lists[i][pick[i]][0] if lists[i][pick[i]][1] != 'unused' else 0 for i in range(8)]
        floats = [flists[i][fpick[i]][0] if flists[i][fpick[i]][1] != 'unused' else 0 for i in range(8)]
        made = sorted({lists[i][pick[i]][1] for i in live} | {flists[i][fpick[i]][1] for i in flive}) or ['none']
        cid = first_id + n
        extra = []
        for block, offsets in fields.items():
            if offsets and block in ints:
                for off, values in offsets.items():
                    extra.append('%x=%s' % (block + off, struct.pack('<I', rng.choice(values)).hex()))
                made = sorted(set(made) | {'nested'})
        lines.append('%d %x %s %s %s' % (cid, target, ','.join('%x' % sext32(v) if v < (1 << 32) else '%x' % v for v in ints),
                                         ','.join('%x' % v for v in floats), ','.join(pokes + extra)))
        kinds[cid] = made
        n += 1
    return lines, kinds


# ---- the run ----------------------------------------------------------------

def wait_for(path, seconds, proc=None):
    t0 = time.time()
    while time.time() - t0 < seconds:
        if os.path.exists(path):
            return True
        if proc is not None and proc.poll() is not None:
            return False
        time.sleep(0.05)
    return False


def read_targets(spec):
    out = []
    for line in open(spec):
        line = line.split('#')[0].strip()
        if line:
            out.append(int(line.split()[0], 16))
    return out


def start_runner(a, work, env_extra):
    build = a.build
    runner = os.path.join(build, 'ps2xRuntime', 'ps2EntryRunner')
    for sub in ('home', 'cwd', 'app', 'shots'):
        os.makedirs(os.path.join(work, sub), exist_ok=True)
    card = os.path.join(work, 'mc0')
    if not os.path.isdir(card):
        shutil.copytree(os.path.join(WORKSPACE, 'recomp', 'run', 'mc0'), card)
    env = dict(os.environ)
    env.update({'HOME': os.path.join(work, 'home'), 'PS2X_APP_SUPPORT_DIR': os.path.join(work, 'app'),
                'PS2X_APP_LOG_DIR': os.path.join(work, 'app'), 'PS2X_SCREENSHOT_DIR': os.path.join(work, 'shots'),
                'PS2X_MC_ROOT': card, 'PS2X_GS_BACKEND': 'cpu', 'PS2X_DEBUG_UI': '0',
                'PS2X_UNPACED': '1', 'PS2X_GS_NULL': '1', 'PS2X_VU1_SKIP': '1', 'PS2X_SAVES': 'card',
                'PS2X_READABLE': 'off', 'PS2X_DEV_UNLOCK_ALL': '1', 'PS2X_DIFFCALL_BANK': '1'})
    env.update(env_extra)
    log = open(os.path.join(work, 'runner.log'), 'wb')
    return subprocess.Popen([runner, ELF], env=env, cwd=os.path.join(work, 'cwd'), stdout=log, stderr=subprocess.STDOUT), env


class Batches:
    """The cases of one snapshot, batch by batch: the runner takes a batch-<n>.cases as it appears."""

    def __init__(self, a, folder, proc):
        self.a, self.folder, self.proc = a, folder, proc
        self.n = 0
        self.total = collections.Counter()

    def run(self, cases, kinds):
        """cases: {id: the line after the id}; {id: (status, blocks)}, or None if the runner stopped."""
        stem = os.path.join(self.folder, 'batch-%d' % self.n)
        self.n += 1
        with open(stem + '.cases.tmp', 'w') as f:
            f.write(''.join('%s %s\n' % kv for kv in cases.items()))
        with open(stem + '.kinds.json', 'w') as f:
            json.dump(kinds, f)
        os.rename(stem + '.cases.tmp', stem + '.cases')
        if not wait_for(stem + '.done', self.a.max_secs, self.proc):
            return None
        out = {}
        for line in open(stem + '.out'):
            c = line.split()
            blocks = frozenset(int(x, 16) for x in c[4].split(',')) if len(c) > 4 and c[4] else frozenset()
            out[c[0]] = (c[1], blocks)
            self.total[c[1]] += 1
        return out


def first_cases(a, maker, targets, next_id, cold=()):
    cases, kinds = {}, {}
    for t in targets:
        got, k = make_cases(maker, t, a.cold_budget if t in cold else a.budget, next_id)
        for line in got:
            cid, rest = line.split(None, 1)
            cases[cid] = rest
        kinds.update({str(i): v for i, v in k.items()})
        next_id += len(got)
    return cases, kinds, next_id


FIELD_VALUES = {1: [0, 1, 2, 0xFF], 2: [0, 1, 2, 0xFFFF], 4: [0, 1, 2, 0xFFFFFFFF, 0x3F800000], 8: [0, 1, 0xFFFFFFFFFFFFFFFF]}


def field_cases(a, analysis, cases, kinds, results, next_id, hunts, cold=()):
    """Cases of the --fields targets live here with blocks left that returned, again with one field they read (or one global) at another value."""
    wanted = {t for t in read_targets(a.fields) if t not in cold and (t not in hunts or hunts[t].missing())}
    kept = collections.defaultdict(list)
    for cid, (status, _) in results.items():
        if status in ('kept', 'notkept'):
            target, rest = cases[cid].split(None, 1)
            address = int(target.lstrip('~').split('@')[0], 16)
            if address in wanted and '@' not in target:
                kept[address].append((cid, target, rest))
    out, out_kinds = {}, {}
    rng = random.Random(next_id)
    for t, rows in kept.items():
        f = analysis['fns'].get(str(t))
        if not f:
            continue
        rng.shuffle(rows)
        made = []
        for cid, target, rest in rows[:a.field_bases]:
            ints, floats, pokes = rest.split(' ')
            values = [int(v, 16) for v in ints.split(',')]
            places = []
            for i, q in f['params'].items():
                base = values[int(i)] & 0xFFFFFFFF
                if not 0x00100000 <= base < RAM_END:
                    continue
                for off, size in sorted(q['reads'].items(), key=lambda kv: int(kv[0]))[:12]:
                    places.append(((base + int(off)) & 0xFFFFFFFF, size))
            places += [(int(k), v) for k, v in sorted(f.get('globals', {}).items())[:12]]
            for at, size in places:
                if analysis['elf'].constant(at):
                    continue
                for v in FIELD_VALUES.get(size, []):
                    hexed = v.to_bytes(size, 'little').hex()
                    made.append((rest_with_poke(ints, floats, pokes, at, hexed), cid, target))
        rng.shuffle(made)
        for rest, cid, target in made[:a.field_budget]:
            out[str(next_id)] = '%s %s' % (target, rest)
            out_kinds[str(next_id)] = kinds.get(cid, []) + ['field']
            next_id += 1
    return out, out_kinds, next_id


def rest_with_poke(ints, floats, pokes, at, hexed):
    return '%s %s %s' % (ints, floats, '%x=%s' % (at, hexed) if pokes == '-' else '%s,%x=%s' % (pokes, at, hexed))


def caller_chain(analysis, target, depth=2, limit=4):
    """Static callers of the target, nearest first, as far as `depth` calls up."""
    out, level, seen = [], [target], {target}
    for _ in range(depth):
        up = []
        for t in level:
            for c, _, _ in analysis['callers'].get(str(t), []):
                if c not in seen:
                    seen.add(c)
                    out.append(c)
                    up.append(c)
        level = up
    return out[:limit]


def structured_lines(a, maker, analysis, target, entry, rng, formats, next_id):
    """Cases of `entry` with a built format string or texts in its string parameters, recording the target."""
    f = analysis['fns'].get(str(entry))
    roles = {int(i): r for i, r in (f or {}).get('strings', {}).items()}
    if not roles:
        return {}, {}, next_id
    params = f['params']

    def base(e):
        lines, _ = make_cases(maker, e, 1, 0)
        one = search.Case.parse(lines[0].split(None, 1)[1], [], (maker.s.regs['r29'] & ~15) - ARENA_DROP)
        return one.ints, one.floats, one.pokes, one.sp

    cases, kinds = {}, {}
    name = '%x@%x' % (target, entry) if entry != target else '%x' % target
    if not any(r == 'format' for r in roles.values()):
        # Texts in the string parameters.
        for _ in range(min(a.format_cases, 16)):
            ints, floats, pokes, sp = base(entry)
            arena = sp + search.ARENA_SKIP
            pokes = list(pokes)
            for k, i in enumerate(sorted(roles)):
                at = arena + search.TEXT_AT + 0x40 * k
                ints[i] = at
                pokes.append(search.text_poke(at, rng.choice(search.TEXTS)))
            cases[str(next_id)] = '%s %s %s %s' % (name, ','.join('%x' % v for v in ints), ','.join('%x' % v for v in floats), ','.join(pokes))
            kinds[str(next_id)] = ['format']
            next_id += 1
        return cases, kinds, next_id
    made = search.structured_cases(entry, roles, a.format_cases, rng, formats, base)
    fmt_at = next(i for i in sorted(roles) if roles[i] == 'format')
    va_list = str(fmt_at + 1) in params and params[str(fmt_at + 1)].get('pointer')
    for ints, floats, pokes, sp, _, _ in made:
        if va_list:
            ints[fmt_at + 1] = sp + search.ARENA_SKIP + search.VA_AT
        cases[str(next_id)] = '%s %s %s %s' % (name, ','.join('%x' % (v & 0xFFFFFFFFFFFFFFFF) for v in ints), ','.join('%x' % v for v in floats), ','.join(pokes))
        kinds[str(next_id)] = ['format']
        next_id += 1
    return cases, kinds, next_id


def caller_lines(a, maker, analysis, target, next_id):
    """Cases of the target's callers, recording the target, for one that stays shallow."""
    cases, kinds = {}, {}
    for c in caller_chain(analysis, target):
        got, k = make_cases(maker, c, a.caller_cases, next_id)
        for line in got:
            cid, rest = line.split(None, 1)
            word, rest = rest.split(None, 1)
            cases[cid] = '%x@%x %s' % (target, c, rest)
            kinds[cid] = sorted(set(k[int(cid)]) | {'caller'})
        next_id += len(got)
    return cases, kinds, next_id


def feed(hunts, stem, cases, kinds, results, sp):
    """A batch's feedback into the hunts: the new paths become seeds, the faults wait for a repair."""
    parsed = {}
    for cid, (status, blocks) in results.items():
        if status not in ('kept', 'notkept', 'fault'):
            continue
        case = search.Case.parse(cases[cid], kinds.get(cid, []), sp)
        if case.target in hunts:
            case.blocks = blocks
            parsed[cid] = case
    search.parse_reads(stem + '.reads', parsed)
    for cid, case in parsed.items():
        hunt = hunts[case.target]
        status = results[cid][0]
        if status == 'fault':
            if case.wild and len(hunt.faults) < 32:
                hunt.faults.append(case)
                if case.wild[:2] not in hunt.fault_sites:      # (a fault further in: a repair worked)
                    hunt.fault_sites.add(case.wild[:2])
                    hunt.idle = 0
            continue
        hunt.cases += 1
        case.wild = None
        if hunt.take(case) or (case.logged and case.blocks):
            hunt.seeds.append(case)
            hunt.idle = 0


def object_cases(maker, hunt, rng, budget):
    """The seeds again with another real object of the parameter: a branch that wants a field set is
    often taken by an object that has it, with no poke at all. Pokes of the old object go with it."""
    f = maker.fns.get(str(hunt.target))
    seeds = [c for c in hunt.seeds[-8:] if c.entry == c.target and not c.forces]
    if not f or not seeds:
        return []
    out = []
    pointers = [int(i) for i, q in f['params'].items() if q.get('pointer')]
    for _ in range(budget * 4):
        if len(out) >= budget or not pointers:
            break
        i = rng.choice(pointers)
        pool = maker.objects(hunt.target, i, f['params'][str(i)])
        if not pool:
            pointers.remove(i)
            continue
        seed = rng.choice(seeds)
        c = seed.copy('object')
        c.ints[i] = rng.choice(pool)
        arena = seed.arena()
        c.pokes = [p for p in seed.pokes if arena <= int(re.split(r'[=*]', p)[0], 16) < arena + search.ARENA_SIZE]
        text = c.text()
        if text not in hunt.tried:
            hunt.tried.add(text)
            out.append(c)
    return out


def formatted(analysis, t):
    """A format string decides what the target does, whatever state the game is in: not a cold case."""
    return 'format' in (analysis['fns'].get(str(t)) or {}).get('strings', {}).values()


def search_rounds(a, b, analysis, hunts, maker, targets, next_id, formats, deadline, cold=()):
    """The rounds of the search at this snapshot (diffcall_search.py)."""
    elf = maker.elf
    sp = (maker.s.regs['r29'] & ~15) - ARENA_DROP
    rng = random.Random(a.seed ^ maker.s.regs['vsync'] ^ 0x51)
    cases, kinds = {}, {}
    for t in targets:
        # Structured inputs at the target or a caller that takes the format, callers for what stays shallow.
        for entry in [t] + (caller_chain(analysis, t) if t not in cold or formatted(analysis, t) else []):
            got, k, next_id = structured_lines(a, maker, analysis, t, entry, rng, formats, next_id)
            cases.update(got)
            kinds.update(k)
        hunt = hunts[t]
        if a.caller_cases and (not hunt.seeds or (t not in cold and (len(hunt.seeds) < 3 or hunt.missing()))):
            got, k, next_id = caller_lines(a, maker, analysis, t, next_id)
            cases.update(got)
            kinds.update(k)
    for number in range(a.search + 1):
        if number:
            cases, kinds = {}, {}
            order = sorted(hunts.values(), key=lambda h: -len(h.missing()))
            for hunt in order:
                if hunt.idle >= (3 if hunt.missing() else 2):
                    continue
                made = hunt.propose(elf, rng, a.round_budget)
                if hunt.missing() and a.object_budget:
                    made += object_cases(maker, hunt, rng, a.object_budget)
                for case in made:
                    cid = str(next_id)
                    next_id += 1
                    cases[cid] = case.text()
                    kinds[cid] = case.kinds
                hunt.idle += 1
        if not cases:
            break
        results = b.run(cases, kinds)
        if results is None:
            return None
        feed(hunts, os.path.join(b.folder, 'batch-%d' % (b.n - 1)), cases, kinds, results, sp)
        if not a.keep_ram and os.path.exists(os.path.join(b.folder, 'batch-%d.reads' % (b.n - 1))):
            os.remove(os.path.join(b.folder, 'batch-%d.reads' % (b.n - 1)))
        print('    round %d: %d cases, %s; missing blocks left %d' % (
            number, len(cases), dict(collections.Counter(v[0] for v in results.values())), sum(len(h.missing()) for h in hunts.values())), flush=True)
        if time.time() > deadline:
            print('    snapshot time up', flush=True)
            break
    return next_id


def run_snapshot(a, folder, analysis, targets, next_id, proc=None, hunts=None):
    """A snapshot's first cases, the ones that waited again as invocations (they may wait), the field
    stage, then the search's rounds."""
    t0 = time.time()
    snap = Snapshot(folder)
    # A target whose blocks have all run stays for the first snapshots only (for more paths).
    targets = [t for t in targets if t not in hunts or hunts[t].missing() or hunts[t].snapshots < a.full_snapshots]
    for hunt in hunts.values():
        hunt.snapshots += 1
    cold = set()
    if a.live_window:
        # Only what this game state runs, or calls next; anything else would run on made-up objects. A
        # function no jal reaches may be reached by a table or a registered pointer: a neighbour in the
        # table, or the code that takes its address, running counts. (Of 1,233 functions without a
        # recording 816 have no static caller.) --cold-budget keeps the others, with fewer cases.
        had = len(targets)
        here = lambda f: snap.live(f, a.live_window)
        live = [t for t in targets if here(t) or any(here(c) for c in caller_chain(analysis, t))
                or any(here(n) for n in analysis['siblings'].get(str(t), [])) or any(here(n) for n in analysis.get('takers', {}).get(str(t), []))]
        print('%s: %d of %d targets live' % (os.path.basename(folder), len(live), had), flush=True)
        if a.cold_budget:
            cold = set(targets) - set(live)
        else:
            targets = live
    rng = random.Random(a.seed ^ snap.regs['vsync'])
    maker = Maker(analysis, snap, rng)
    maker.elf = analysis['elf']
    b = Batches(a, folder, proc)
    cases, kinds, next_id = first_cases(a, maker, targets, next_id, cold)
    print('%s: %d cases over %d targets' % (os.path.basename(folder), len(cases), len(targets)), flush=True)
    for hunt in hunts.values():
        hunt.begin_snapshot()
    hunts = {t: hunts[t] for t in targets if t in hunts}
    sp = (snap.regs['r29'] & ~15) - ARENA_DROP
    results = b.run(cases, kinds)
    if results is None:
        return None, next_id
    feed(hunts, os.path.join(folder, 'batch-0'), cases, kinds, results, sp)
    per = collections.Counter()
    again, again_kinds = {}, {}
    for cid, (status, _) in results.items():
        if status == 'waited':
            target, rest = cases[cid].split(None, 1)
            per[target] += 1
            if per[target] <= a.wait_cases:
                again[str(next_id)] = '~%s %s' % (target, rest)
                again_kinds[str(next_id)] = kinds.get(cid, []) + ['wait']
                next_id += 1
    if again:
        got = b.run(again, again_kinds)
        if got is None:
            return None, next_id
        feed(hunts, os.path.join(folder, 'batch-%d' % (b.n - 1)), again, again_kinds, got, sp)
    if a.fields:
        more, more_kinds, next_id = field_cases(a, analysis, cases, kinds, results, next_id, hunts, cold)
        if more:
            got = b.run(more, more_kinds)
            if got is None:
                return None, next_id
            feed(hunts, os.path.join(folder, 'batch-%d' % (b.n - 1)), more, more_kinds, got, sp)
    if a.search:
        next_id = search_rounds(a, b, analysis, hunts, maker, targets, next_id, analysis['formats'], t0 + a.snapshot_secs, cold)
        if next_id is None:
            return None, 0
    for name in ('ram.bin', 'scratch.bin'):      # 32 MB each snapshot; the recordings hold what they read
        if not a.keep_ram and os.path.exists(os.path.join(folder, name)):
            os.remove(os.path.join(folder, name))
    return b.total, next_id


def prepare_hunts(a, analysis):
    """The search's state over a run: a Hunt per target, the corpus's blocks (--new-blocks), the game's format strings."""
    elf = Elf(ELF)
    analysis['elf'] = elf
    analysis['formats'] = search.real_formats(elf)
    corpus = collections.defaultdict(set)
    if a.new_blocks:
        for line in open(a.new_blocks):
            c = line.split()
            if len(c) > 1 and c[1] != '-':
                corpus[int(c[0], 16)] = {int(x, 16) for x in c[1].split(',')}
    hunts = {}
    stub = search.find_stub(elf, sorted(int(k) for k in analysis['fns']))
    for t in read_targets(a.targets):
        f = analysis['fns'].get(str(t))
        if f:
            hunts[t] = search.Hunt(t, search.Listing(elf, t, f['end']), corpus[t], stub, elf.constant)
    return hunts


def write_hunts(path, hunts):
    with open(path, 'w') as f:
        for t, h in sorted(hunts.items()):
            f.write('0x%08X\t%d blocks\t%d corpus\t%d ran\t%d gained\t%d paths\t%d returned\t%d fault places\n' % (
                t, len(h.listing.leaders), len(h.corpus), len(h.ran), h.gained, len(h.paths), h.cases, len(h.fault_sites)))


def cmd_steps(a):
    """A closed-loop route with snapshots: a steps file of `route CMDS` and `snap NAME` lines."""
    scratch = os.path.abspath(a.scratch)
    run = os.path.join(scratch, 'runs', a.name)
    shutil.rmtree(run, ignore_errors=True)
    os.makedirs(run)
    analysis = json.load(open(a.analysis))
    targets = read_targets(a.targets)
    hunts = prepare_hunts(a, analysis)
    dirn = os.path.join(run, 'dc')
    os.makedirs(dirn)
    routes = os.path.join(WORKSPACE, 'tools', 'routes.py')
    runner = os.path.join(a.build, 'ps2xRuntime', 'ps2EntryRunner')
    env = dict(os.environ, RUNNER=runner, ROUTES_DIR=os.path.join(run, 'routes'), PYTHONPATH=os.path.join(WORKSPACE, 'tools'),
               ROUTES_SLOW='8')
    keys = {'PS2X_UNPACED': '1', 'PS2X_GS_NULL': '1', 'PS2X_VU1_SKIP': '1', 'PS2X_SAVES': 'card', 'PS2X_READABLE': 'off',
            'PS2X_DEV_UNLOCK_ALL': '1', 'PS2X_DEBUG_UI': '0', 'PS2X_DIFFCALL': a.hook + '@trigger', 'PS2X_DIFFCALL_DIR': dirn,
            'PS2X_DIFFCALL_BANK': '1', 'PS2X_DIFFCALL_PATHS': '1'}
    if a.case_ms:
        keys['PS2X_DIFFCALL_CASE_MS'] = str(a.case_ms)
    for line in a.env or []:
        k, _, v = line.partition('=')
        keys[k] = v
    name = 'dc-' + a.name
    subprocess.run([sys.executable, routes, 'start', name] + ['%s=%s' % kv for kv in keys.items()], capture_output=True, text=True,
                   env=env, cwd=WORKSPACE)
    pid = None
    try:
        pid = int(open(os.path.join(WORKSPACE, 'recomp', 'run', 'ctl-' + name, 'pid')).read())
    except (OSError, ValueError):
        pass
    t0 = time.time()
    next_id = a.first_id
    try:
        for line in open(a.steps):
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            kind, _, rest = line.partition(' ')
            if kind == 'route':
                p = subprocess.run([sys.executable, routes, name, rest], capture_output=True, text=True, env=env, cwd=WORKSPACE,
                                   timeout=a.max_secs)
                print('route %s: %s, %.0f s' % (rest[:60], (p.stdout.strip().split('\n') or [''])[-1][:90], time.time() - t0), flush=True)
            elif kind == 'snap':
                folder = os.path.join(dirn, 'snap-' + rest)
                with open(os.path.join(dirn, 'trigger.tmp'), 'w') as f:
                    f.write(rest + '\n')
                os.rename(os.path.join(dirn, 'trigger.tmp'), os.path.join(dirn, 'trigger'))
                if not wait_for(os.path.join(folder, 'ready'), 300):
                    print('no snapshot %s' % rest, flush=True)
                    continue
                counts, next_id = run_snapshot(a, folder, analysis, targets, next_id, hunts=hunts)
                if counts is None:
                    print('batch not done at snapshot %s' % rest, flush=True)
                    break
                open(os.path.join(folder, 'finish'), 'w').close()
                print('  %s: %s, %.0f s' % (rest, dict(counts), time.time() - t0), flush=True)
                write_hunts(os.path.join(run, 'hunts.tsv'), hunts)
    finally:
        subprocess.run([sys.executable, routes, 'stop', name], capture_output=True, text=True, env=env, cwd=WORKSPACE, timeout=60)
        if pid:
            try:
                os.kill(pid, 15)
            except OSError:
                pass
    print('run %s: %.0f s, %s' % (a.name, time.time() - t0, run))


def cmd_run(a):
    if a.steps:
        return cmd_steps(a)
    scratch = os.path.abspath(a.scratch)
    run = os.path.join(scratch, 'runs', a.name)
    shutil.rmtree(run, ignore_errors=True)
    os.makedirs(run)
    analysis = json.load(open(a.analysis))
    targets = read_targets(a.targets)
    hunts = prepare_hunts(a, analysis)
    dirn = os.path.join(run, 'dc')
    os.makedirs(dirn)
    env = {'PS2X_DIFFCALL_DIR': dirn, 'PS2X_DIFFCALL_PATHS': '1'}
    if a.case_ms:
        env['PS2X_DIFFCALL_CASE_MS'] = str(a.case_ms)
    if a.vsyncs:
        env['PS2X_DIFFCALL'] = '%s@%s' % (a.hook, a.vsyncs)
    names = [str(i) for i in range(len(a.vsyncs.split(',')))] if a.vsyncs else []
    if a.pad:
        tool = {'career': 'careercheck', 'lc': 'logiccheck'}[a.pad]
        env['PS2X_PAD_SCRIPT'] = subprocess.run([sys.executable, os.path.join(WORKSPACE, 'tools', tool + '.py'), 'script'],
                                                capture_output=True, text=True, cwd=WORKSPACE).stdout.strip()
        env['PS2X_PAD_SCRIPT_CLOCK'] = 'vsync'
    proc, _ = start_runner(a, run, env)
    t0 = time.time()
    next_id = a.first_id
    try:
        for name in names:
            folder = os.path.join(dirn, 'snap-' + name)
            if not wait_for(os.path.join(folder, 'ready'), a.max_secs, proc):
                print('no snapshot %s: runner %s' % (name, proc.poll()))
                break
            counts, next_id = run_snapshot(a, folder, analysis, targets, next_id, proc, hunts)
            if counts is None:
                print('batch not done at snapshot %s: runner %s' % (name, proc.poll()))
                break
            open(os.path.join(folder, 'finish'), 'w').close()
            print('  %s: %s, %.0f s' % (name, dict(counts), time.time() - t0), flush=True)
            write_hunts(os.path.join(run, 'hunts.tsv'), hunts)
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
    print('run %s: %.0f s, %s' % (a.name, time.time() - t0, run))


def split_records(path):
    """A recording as (header, [(sequence, bytes)]), its format as fncheck.cpp writes it."""
    d = open(path, 'rb').read()
    context = struct.unpack_from('<I', d, 16)[0]
    out = []
    o = 24

    def lines(o):
        return o + 4 + 24 * struct.unpack_from('<I', d, o)[0]

    while o < len(d):
        start = o
        flags, sequence = struct.unpack_from('<IQ', d, o + 4)
        o = lines(lines(o + 36 + 2 * context))
        calls = struct.unpack_from('<I', d, o)[0]
        o += 4
        for _ in range(calls):
            o = lines(o + 12 + 64 + 64 + 32 + context)
        if flags & 2:
            o += 4 + 24 * struct.unpack_from('<I', d, o)[0]
        out.append((sequence, d[start:o]))
    return d[:24], out


def drop_crashing_calls(fncheck, path, log):
    """A call that ends the checker (the readable version crashed) hides the file's other calls: they go on
    without it, and it is listed: a difference to look at, with the original's replay good."""
    dropped = 0
    while dropped < 256:
        r = subprocess.run([fncheck, '--cycles', '--show', '0', path], capture_output=True, text=True, timeout=600)
        m = re.search(r'^\[fncheck:fault\] address=(\S+) file="[^"]*" call=(\d+) phase="([^"]*)" touched=(\S+)', r.stdout, re.M)
        if not m:
            break
        header, records = split_records(path)
        keep = [b for sequence, b in records if sequence != int(m.group(2))]
        log.write(json.dumps(dict(address=m.group(1), file=path, call=int(m.group(2)), phase=m.group(3), touched=m.group(4))) + '\n')
        log.flush()
        dropped += 1
        if len(keep) == len(records):
            break
        with open(path + '.tmp', 'wb') as f:
            f.write(header + b''.join(keep))
        os.replace(path + '.tmp', path)         # (a new file: the run's rec folder keeps its link to the old one)
    return dropped


def cmd_check(a):
    fncheck = os.path.join(a.build, 'ps2xRuntime', 'ps2xFnCheck')
    os.makedirs(a.out, exist_ok=True)
    cmd = [sys.executable, '-I', os.path.join(HERE, 'fncheck_corpus.py'), 'run', '--fncheck', fncheck, '--out', a.out,
           '--corpus', a.corpus, '--jobs', str(a.jobs), '--max-mem-gb', '4', '--no-cache', '--build', a.build,
           '--fncheck-arg', '--safepoints 32 3 --safepoints-paths %d --markers --call-blocks' % a.safepoint_paths + (' --upper ' + os.path.abspath(a.upper) if a.upper else '')]
    if a.only:
        cmd += ['--only', a.only]
    print(' '.join(cmd))
    for attempt in range(3):
        status = subprocess.call(cmd)
        faulted = sorted({json.loads(l)['file'] for l in open(os.path.join(a.out, 'lines.jsonl')) if '"kind": "fault"' in l})
        if not faulted or attempt == 2:
            break
        with open(os.path.join(a.out + '.dropped.jsonl'), 'a') as log:
            total = sum(drop_crashing_calls(fncheck, f, log) for f in faulted)
        print('%d files faulted, %d calls taken out (%s)' % (len(faulted), total, a.out + '.dropped.jsonl'), flush=True)
    return status


# ---- the verdicts -----------------------------------------------------------

DEEP_SHARE = 0.4        # a call reached the body: it ran this share of the function's blocks (or 4 of them)
TIER_SHARE = 0.5        # `generated`: the passing calls ran this share of the blocks; below it `generated-shallow`


def case_statuses(scratch, runs):
    """{id: (target, status, kinds, snapshot)} of every generated case of the runs."""
    out = {}
    for run in runs:
        for folder in glob.glob(os.path.join(scratch, 'runs', run, 'dc', 'snap-*')):
            snap = run + '/' + os.path.basename(folder)[5:]
            for cases in glob.glob(os.path.join(folder, 'batch-*.cases')):
                stem = cases[:-len('.cases')]
                kinds = json.load(open(stem + '.kinds.json')) if os.path.exists(stem + '.kinds.json') else {}
                targets = {}
                for line in open(cases):
                    c = line.split(None, 2)
                    if len(c) >= 2:
                        targets[c[0]] = int(c[1].lstrip('~').split('@')[0], 16)
                if os.path.exists(stem + '.out'):
                    for line in open(stem + '.out'):
                        c = line.split()
                        if len(c) >= 2 and c[0] in targets:
                            out[(snap, int(c[0]))] = (targets[c[0]], c[1], kinds.get(c[0], []), snap)
    return out


def gather_corpus(scratch, runs, corpus):
    """Every run's snap-*/rec folder hard-linked as <corpus>/<run>-<snap>, a corpus fncheck_corpus.py reads."""
    os.makedirs(corpus, exist_ok=True)
    made = 0
    for run in runs:
        for folder in glob.glob(os.path.join(scratch, 'runs', run, 'dc', 'snap-*', 'rec')):
            snap = os.path.basename(os.path.dirname(folder))[5:]
            to = os.path.join(corpus, '%s-%s' % (run, snap))
            shutil.rmtree(to, ignore_errors=True)
            os.makedirs(to)
            for name in os.listdir(folder):
                os.link(os.path.join(folder, name), os.path.join(to, name))
                made += 1
    return made


def load_check(check):
    """The replay's lines per function: results, safe points, markers, blocks per call, faults."""
    per = collections.defaultdict(lambda: dict(results=[], sp=[], markers=[], calls={}, faults=[], blocks=None, hits=set(), upper=[]))
    for line in open(os.path.join(check, 'lines.jsonl')):
        j = json.loads(line)
        kind = j.get('kind')
        address = j.get('address')
        if address is None and kind == 'fault':         # (the checker died: the file names the function)
            address = '0x' + os.path.basename(j.get('file', ''))[:8]
        if address is None:
            continue
        p = per[int(address, 16)]
        if kind == 'result':
            p['results'].append(j)
        elif kind == 'safepoints':
            p['sp'].append(j)
        elif kind == 'upper':
            if int(j.get('upper', 0)):
                p['upper'].append(j)
        elif kind == 'markers':
            p['markers'].append(j)
        elif kind == 'callblocks':
            blocks = [] if j.get('blocks') in ('-', None) else j['blocks'].split(',')
            p['calls'][(j.get('route'), int(j.get('call', 0)))] = (int(j.get('ok', 0)), len(blocks))
        elif kind == 'fault':
            p['faults'].append(j)
        elif kind == 'coverage':
            all_blocks = j.get('blocks', '-')
            p['blocks'] = 0 if all_blocks == '-' else len(all_blocks.split(','))
            hits = j.get('hits', '-')
            if hits != '-':
                p['hits'] |= {h.split(':')[0] for h in hits.split(',')}
    return per


def mismatch_class(why):
    """The kind of difference a first failure names: what the report counts by."""
    table = (('vu0_left', 'stale VU0 lanes'), ('safe points', 'safe point'), ('markers', 'marker'), ('entry blocks', 'marker'), ('faulted', 'readable fault'),
             ('with another fill', 'read of other memory'), ('upper half or VU0', 'upper half or VU0'), ('left: ', 'register left'), ('bytes of RAM', 'memory'), ('goes to', 'call order'),
             ('not in the recording', 'call order'), ('calls recorded', 'call order'), ('recorded calls', 'call order'),
             ('quadword', 'quadword argument'), (': a', 'call argument'), (': f', 'call argument'), ('VU0', 'VU0'),
             ('not preserved', 'saved register'), ('COP0', 'status'), ('left =', 'register left'), ('v0 =', 'result'),
             ('f0 =', 'result'), ('cycles', 'cycles'), ('I/O', 'I/O'))
    for key, name in table:
        if key in why:
            return name
    return 'other'


def verdict_of(p, known=frozenset()):
    """The function's tier from its replay lines, and why. `known`: the notes the function's
    recordings have too (--baseline): a difference the checker only notes counts when it is new."""
    if not p['results'] and not p['faults']:
        return 'noreach', ''
    recorded = sum(int(r.get('recorded', 0)) for r in p['results'])
    bad = sum(int(r.get('readable_bad', 0)) for r in p['results'])
    original_bad = sum(int(r.get('original_bad', 0)) for r in p['results'])
    ok = sum(int(r.get('readable_ok', 0)) for r in p['results'])
    sp_bad = sum(int(s.get('differ', 0)) + int(s.get('count_differ', 0)) for s in p['sp'])
    order_bad = sum(int(m.get('order_bad', 0)) for m in p['markers'])
    if bad:
        first = next((r.get('first_readable') for r in p['results'] if r.get('first_readable')), '')
        return 'mismatch', first
    if sp_bad:
        return 'mismatch', 'safe points: ' + next((s.get('first', '') for s in p['sp'] if int(s.get('differ', 0)) + int(s.get('count_differ', 0))), '')
    if order_bad:
        return 'mismatch', 'markers: ' + next((m.get('first_order', '') for m in p['markers'] if int(m.get('order_bad', 0))), '')
    unmarked = sorted({x for m in p['markers'] for f in ('unmarked_blocks', 'unmarked_loops') for x in m.get(f, '-').split(',') if x != '-'})
    marked = {x for m in p['markers'] for f in ('marked_blocks_at', 'marked_loops_at') for x in m.get(f, '-').split(',') if x != '-'}
    unmarked = [x for x in unmarked if x not in marked]      # (a block another snapshot's replay marked is marked)
    if unmarked:
        return 'mismatch', 'entry blocks the original reaches and the readable never marks: ' + ','.join(unmarked)
    # (vu0_left: a lane nothing reads does no harm, but a later VMULx.xyz keeps a stale lane and an SQC2
    # stores it: abdiff1's career A/B differences.)
    for field, note in (('regs_left', 'first_regs_left'), ('args_left', 'first_args_left'), ('wide', 'first_wide'),
                        ('vu0_left', 'first_vu0_left')):
        for r in p['results']:
            if int(r.get(field, 0)) and field not in known:
                return 'mismatch', 'left: %s: %s' % (field, r.get(note, ''))
    for u in p['upper']:
        return 'mismatch', 'upper half or VU0 left: %s' % u.get('first_upper', '')
    if p['faults']:
        # The original replayed and the readable version touched memory it did not, or crashed the checker
        # (a host index out of RAM): the rest of that file's calls went unreplayed.
        f = p['faults'][0]
        return 'mismatch', 'the readable version faulted (%s, %s, %d files)' % (f.get('phase', ''), f.get('touched', '-'), len(p['faults']))
    if not ok:
        return 'noreplay', next((r.get('first_original') for r in p['results'] if r.get('first_original')), '') if original_bad else ''
    share = len(p['hits']) / p['blocks'] if p['blocks'] else 1.0
    return ('generated' if share >= TIER_SHARE else 'generated-shallow'), '%d of %s blocks, %d calls' % (len(p['hits']), p['blocks'], ok)


def cmd_report(a):
    scratch = os.path.abspath(a.scratch)
    runs = a.runs.split(',')
    cases = case_statuses(scratch, runs)
    per = load_check(a.check)
    sizes, names = {}, {}
    if a.bytes:
        for line in open(a.bytes):
            c = line.rstrip('\n').split('\t')
            sizes[int(c[0], 16)] = int(c[1])
            names[int(c[0], 16)] = c[3]
    targets = sorted(sizes) if sizes else sorted({v[0] for v in cases.values()})   # (the list: a function recorded since is off it)
    status = collections.Counter(v[1] for v in cases.values())
    total = sum(status.values())
    print('cases %d: %s' % (total, ', '.join('%s %d' % kv for kv in status.most_common())))
    kept, deep, shallow = 0, 0, 0
    kind_seen, kind_deep = collections.Counter(), collections.Counter()
    per_target_kinds = collections.defaultdict(collections.Counter)
    per_target_deep = collections.Counter()
    for (snap, cid), (t, st, kinds, _) in cases.items():
        if st != 'kept':
            continue
        kept += 1
        p = per.get(t)
        got = p['calls'].get((snap.replace('/', '-'), cid)) if p else None
        for k in kinds:
            kind_seen[k] += 1
        if not got or not got[0]:
            continue
        total_blocks = p['blocks'] or 1
        if got[1] >= min(4, total_blocks) or got[1] >= DEEP_SHARE * total_blocks:
            deep += 1
            per_target_deep[t] += 1
            for k in kinds:
                kind_deep[k] += 1
                per_target_kinds[t][k] += 1
        else:
            shallow += 1
    print('kept %d of %d generated calls (%.1f%%); reached the body %d (%.1f%% of all, %.1f%% of kept), early out %d' % (
        kept, total, 100.0 * kept / max(total, 1), deep, 100.0 * deep / max(total, 1), 100.0 * deep / max(kept, 1), shallow))
    print('reach by argument kind (kept, reached): ' + ', '.join('%s %d %d' % (k, kind_seen[k], kind_deep[k]) for k, _ in kind_seen.most_common()))
    tiers = collections.Counter()
    tier_bytes = collections.Counter()
    rows = []
    baseline = collections.defaultdict(set)
    if a.baseline:
        for line in open(a.baseline):
            if '"kind": "result"' in line:
                j = json.loads(line)
                for field in ('regs_left', 'args_left', 'wide', 'vu0_left'):
                    if int(j.get(field, 0)):
                        baseline[int(j['address'], 16)].add(field)
    for t in targets:
        tier, why = verdict_of(per[t], baseline[t]) if t in per else ('noreach', '')
        tiers[tier] += 1
        tier_bytes[tier] += sizes.get(t, 0)
        statuses = collections.Counter(v[1] for v in cases.values() if v[0] == t)
        rows.append((t, tier, why, statuses))
    print('tiers: ' + ', '.join('%s %d (%d bytes)' % (k, tiers[k], tier_bytes[k]) for k, _ in tiers.most_common()))
    classes = collections.Counter(mismatch_class(why) for _, tier, why, _ in rows if tier == 'mismatch')
    print('mismatches by class: ' + (', '.join('%s %d' % kv for kv in classes.most_common()) or 'none'))
    if a.out:
        with open(a.out, 'w') as f:
            f.write('# address\ttier\tname\tbytes\twhy\tcase statuses\tcalls that reached the body\ttheir argument kinds\n')
            for t, tier, why, st in rows:
                f.write('0x%08X\t%s\t%s\t%d\t%s\t%s\t%d\t%s\n' % (
                    t, tier, names.get(t, ''), sizes.get(t, 0), (why or '').replace('\t', ' ')[:300],
                    ','.join('%s=%d' % kv for kv in st.most_common()), per_target_deep[t],
                    ','.join('%s=%d' % kv for kv in per_target_kinds[t].most_common())))
    if a.notes:
        workdb = zaxis_paths.get('WORKDB_PY')
        python = os.path.join(SCRATCH_ROOT, 'venv', 'bin', 'python')
        made = 0
        for t, tier, why, st in rows:
            if not tier.startswith('generated') and tier != 'mismatch':
                continue
            text = 'generated-call tier %s: %s (diffcall: generated arguments from real game states, not a route; never Verified)' % (tier, why)
            subprocess.run([python, '-I', workdb, 'emit', a.notes, 'note', 'addr=0x%08X' % t, 'topic=evidence', 'text=' + text,
                            'run=' + a.note_run, 'generator=diffcall', 'agent=diffcall', 'source=' + a.note_source], check=True,
                           capture_output=True)
            made += 1
        print('%d notes in %s' % (made, a.notes))
    for t, tier, why, st in rows:
        if a.list or tier == 'mismatch':
            print('0x%08X %-18s %-34s %s | %s' % (t, tier, names.get(t, ''), (why or '')[:90], ','.join('%s=%d' % kv for kv in st.most_common())))


def cmd_upper(a):
    """The file ps2xFnCheck --upper reads."""
    elf = Elf(ELF)
    rows = [(int(r['addr']), int(r['end_addr'])) for r in query("SELECT addr, end_addr FROM functions WHERE build='ai_ps2'")]
    got = search.upper_reads(elf, rows)
    with open(a.out, 'w') as f:
        for t, (args, returns, vu0) in sorted(got.items()):
            if args or returns or vu0:
                f.write('%x %x %x %x\n' % (t, args, returns, vu0))
    print('%d functions: %d with an argument upper half read, %d with a result upper half read, %d with VU0 registers read after them' % (
        len(got), sum(1 for v in got.values() if v[0]), sum(1 for v in got.values() if v[1]), sum(1 for v in got.values() if v[2])))


def cmd_gather(a):
    print(gather_corpus(os.path.abspath(a.scratch), a.runs.split(','), a.corpus), 'files linked into', a.corpus)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('analyze')
    p.add_argument('--out', required=True)
    p = sub.add_parser('run')
    p.add_argument('--name', required=True)
    p.add_argument('--build', required=True)
    p.add_argument('--targets', required=True)
    p.add_argument('--analysis', required=True)
    p.add_argument('--scratch', required=True)
    p.add_argument('--vsyncs')
    p.add_argument('--hook', default='0x100968')
    p.add_argument('--pad', choices=['career', 'lc'])
    p.add_argument('--steps', help='a closed-loop route: lines `route CMDS` (routes.py) and `snap NAME`')
    p.add_argument('--env', action='append', help='KEY=VALUE for the runner (--steps)')
    p.add_argument('--budget', type=int, default=32)
    p.add_argument('--new-blocks', help='fncheck_corpus.py coverage --covered-out file: the blocks the search goes for lie outside it')
    p.add_argument('--seed', type=lambda v: int(v, 0), default=0x5EED, help='the run\'s random numbers')
    p.add_argument('--search', type=int, default=6, help='rounds of the search per snapshot (0: the generated cases only)')
    p.add_argument('--round-budget', type=int, default=24, help='a target\'s cases per round of the search')
    p.add_argument('--format-cases', type=int, default=48, help='structured cases per entry and snapshot')
    p.add_argument('--caller-cases', type=int, default=8, help='cases per caller of a target that stays shallow')
    p.add_argument('--snapshot-secs', type=int, default=1200, help='no new round of the search after this long at a snapshot')
    p.add_argument('--live-window', type=int, default=0, help='a snapshot takes only targets that it, a caller, a table neighbour or code that takes their address ran in this many vsyncs (0: all)')
    p.add_argument('--cold-budget', type=int, default=0, help='with --live-window: the others stay, with this many first cases')
    p.add_argument('--object-budget', type=int, default=0, help='cases per round that swap a pointer parameter for another real object, for targets with blocks left')
    p.add_argument('--full-snapshots', type=int, default=3, help='snapshots a target with no block left to run still gets')
    p.add_argument('--keep-ram', action='store_true', help='keep each snapshot\'s memory files')
    p.add_argument('--fields', help='targets (a file) that get the `field` stage after their first cases')
    p.add_argument('--field-bases', type=int, default=6, help='kept cases a target\'s fields are varied from')
    p.add_argument('--field-budget', type=int, default=96, help='field cases per target and snapshot')
    p.add_argument('--wait-cases', type=int, default=4, help='cases a target that waited gets again as invocations')
    p.add_argument('--first-id', type=int, default=1)
    p.add_argument('--case-ms', type=int, default=0)
    p.add_argument('--max-secs', type=int, default=900)
    p = sub.add_parser('check')
    p.add_argument('--build', required=True)
    p.add_argument('--corpus', required=True)
    p.add_argument('--out', required=True)
    p.add_argument('--only')
    p.add_argument('--jobs', type=int, default=2)
    p.add_argument('--upper', help='diffcall.py upper\'s file: the upper halves and VU0 registers later code reads count too')
    p.add_argument('--safepoints-paths', dest='safepoint_paths', type=int, default=16,
                   help='calls of a file, past its first three, that get interrupted because they go another way')
    p = sub.add_parser('risky')
    p.add_argument('--unc', required=True, help='address, status, name, blocks, covered, missing blocks (tab-separated)')
    p.add_argument('--out', required=True)
    p.add_argument('--short-out')
    p = sub.add_parser('gather')
    p.add_argument('--scratch', required=True)
    p.add_argument('--runs', required=True)
    p.add_argument('--corpus', required=True)
    p = sub.add_parser('upper')
    p.add_argument('--out', required=True)
    p = sub.add_parser('report')
    p.add_argument('--scratch', required=True)
    p.add_argument('--runs', required=True)
    p.add_argument('--check', required=True)
    p.add_argument('--bytes')
    p.add_argument('--out')
    p.add_argument('--list', action='store_true')
    p.add_argument('--baseline', help='a corpus replay\'s lines.jsonl: the notes its recordings have are not new')
    p.add_argument('--notes', help='the tiers as workdb evidence notes, in this results file')
    p.add_argument('--note-run', default='diffcall')
    p.add_argument('--note-source', default='tooling/checks/diffcall.py')
    a = ap.parse_args()
    {'analyze': cmd_analyze, 'run': cmd_run, 'check': cmd_check, 'gather': cmd_gather, 'report': cmd_report, 'risky': cmd_risky, 'upper': cmd_upper}[a.cmd](a)


if __name__ == '__main__':
    main()
