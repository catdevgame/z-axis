#!/usr/bin/env python3
"""Build-time step of the traced originals (src/game/game.cmake; docs/READABLE.md
"How a function is proven"):

1. For each traced function, a copy of its generated file with a block marker
   (FNCHECK_BLOCK(i)) at the start of every basic block, and the block table
   (the blocks' MIPS addresses, the generated file's sha256) before it:
   <out>/<folder>/blocks/<stem>.cpp, included by the traced source game.cmake
   writes. Block starts ("leaders"): the function's entry; the target of every
   branch and of every jump (j) inside the function; the instruction after
   every branch or jump (j, jr, eret; not the calls jal, jalr, bltzal,
   bgezal), so a jump table's cases start blocks too; and every entry block
   of the recompiler's (register_functions.cpp) inside the function, a case
   after a call among them. Leaders past the last
   instruction that is not a nop (the alignment after the return) are left
   out, as are those with no instruction of their own (a delay slot). The
   marker goes after the leader's label when it has one (where gotos land),
   else before its instruction (reached by falling through).
   The generated code is otherwise unchanged: the markers are statements of
   their own between its statements.

2. The callee tables (<out>/fncheck_stack_args.cpp). Stack arguments: every
   function of each generated folder whose code reads above its entry sp
   (loads based on sp, or on fp copied from sp, at an offset past its own
   frame, not stored by the function at a lower address first), with the
   byte range read. f0 readers: every call target some call site of the folder
   reads $f0 after (straight-line from the instruction after the delay slot to
   the first write of f0 or the next branch, jump or call), whose integer-typed
   readable version must leave f0 as the original does. Argument readers: the
   argument registers (a0-a3, t0-t3, f12-f19) each function reads before it sets
   them, on some path (a dataflow over its blocks; a call reads what its callee
   reads and then counts as setting every argument register, a jump out of the
   function what its target reads; jumps through a register are not followed),
   for ps2xFnCheck's check of the registers a readable call leaves out. Result
   readers: the registers (at, v0, v1, a0-a3, t0-t9; f0-f19) some call site of a
   function reads after the call before setting them (the same straight-line scan
   as f0's; a caller that returns with the register untouched, and a caller that
   tail-jumps to the function, hand on their own callers' reads), for ps2xFnCheck
   (f0 of an integer function and v0 of a void one fail, others are noted:
   regs_left) and readable_draft.py (drafts leave them: ee/leave.h). VU0 lanes:
   what crosses a function boundary in VU0's vector registers, by the same blocks:
   vu0_lanes below, for ps2xFnCheck --semantic. Untraced code: every function
   whose generated code goes into the runtime's own (a library function the
   runtime implements, a syscall or exception instruction: UNTRACED), which
   ps2xFnCheck's callee probes do not run.

    fncheck_traced_gen.py --list LIST --stack-args OUT.cpp [--scan GAME=DIR ...]
    fncheck_traced_gen.py --self-test

LIST: one function a line, tab-separated: game number, address (0x...),
generated file, output file, and for a function the recompiler wrote as
several entry_ chunks the later chunks' generated files ('|' between them;
game.cmake): one block table over them all. Files are written only when their
text changes.
"""
import argparse
import hashlib
import os
import re
import sys

INSN = re.compile(r'^\s*// 0x([0-9a-f]+): 0x([0-9a-f]+)\s+(\S+)\s*(.*?)\s*$')
LABEL = re.compile(r'^\s*label_([0-9a-f]+):')
RANGE = re.compile(r'^// Address: 0x([0-9a-f]+) - 0x([0-9a-f]+)', re.M)
LOADS = {'lb': 1, 'lbu': 1, 'lh': 2, 'lhu': 2, 'lw': 4, 'lwu': 4, 'lwl': 4, 'lwr': 4, 'lwc1': 4,
         'ld': 8, 'ldl': 8, 'ldr': 8, 'ldc1': 8, 'lq': 16, 'lqc2': 16}
STORES = {'sb': 1, 'sh': 2, 'sw': 4, 'swl': 4, 'swr': 4, 'swc1': 4, 'sd': 8, 'sdl': 8, 'sdr': 8, 'sdc1': 8,
          'sq': 16, 'sqc2': 16}
MEM = re.compile(r'^\$?(\w+),\s*(-?(?:0x)?[0-9A-Fa-f]+)\(\$(\w+)\)')


def write_if_changed(path, text):
    try:
        if open(path, encoding='utf-8', newline='').read() == text:
            return False
    except OSError:
        pass
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + '.tmp'
    with open(tmp, 'w', encoding='utf-8', newline='') as f:
        f.write(text)
    os.replace(tmp, path)
    return True


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


def control(addr, word):
    """(kind, target): kind 'branch' (conditional or not), 'jump', 'jr', 'call'
    or None; target for branches and j."""
    op, rs, rt = word >> 26, (word >> 21) & 31, (word >> 16) & 31
    if op in (4, 5, 6, 7, 20, 21, 22, 23):
        return 'branch', (addr + 4 + (s16(word & 0xFFFF) << 2)) & 0xFFFFFFFF
    if op == 1:
        if rt in (0, 1, 2, 3):
            return 'branch', (addr + 4 + (s16(word & 0xFFFF) << 2)) & 0xFFFFFFFF
        if rt in (16, 17, 18, 19):
            return 'call', None
        return None, None
    if op in (16, 17, 18) and rs == 8:
        return 'branch', (addr + 4 + (s16(word & 0xFFFF) << 2)) & 0xFFFFFFFF
    if op == 2:
        return 'jump', ((addr + 4) & 0xF0000000) | ((word & 0x3FFFFFF) << 2)
    if op == 3:
        return 'call', None
    if op == 0 and (word & 0x3F) == 8:
        return 'jr', None
    if op == 0 and (word & 0x3F) == 9:
        return 'call', None
    if word == 0x42000018:  # eret
        return 'jr', None
    return None, None


def always_taken(word):
    """A branch with no fall-through: `b` (beq zero, zero, any beq or beql of a
    register with itself), bgez, bgezl or blez of zero."""
    op, rs, rt = word >> 26, (word >> 21) & 31, (word >> 16) & 31
    return (op in (4, 20) and rs == rt) or (op == 1 and rs == 0 and rt in (1, 3)) or (op in (6, 22) and rs == 0)


def instructions(text):
    """{address: (word, mnemonic, operands)} from the instruction comments."""
    out = {}
    for line in text.split('\n'):
        m = INSN.match(line)
        if m:
            a = int(m.group(1), 16)
            ops = m.group(4)
            if ops.endswith('(Delay Slot)'):
                ops = ops[:-len('(Delay Slot)')].rstrip()
            out.setdefault(a, (int(m.group(2), 16), m.group(3), ops))
    return out


def leaders(start, end, insns, extra=()):
    lead = {start} | set(extra)
    for a, (w, _, _) in insns.items():
        kind, target = control(a, w)
        if kind in ('branch', 'jump', 'jr'):
            lead.add(a + 8)
            if target is not None and kind in ('branch', 'jump'):
                lead.add(target)
    real = [a for a, (w, _, _) in insns.items() if w != 0]
    last = max(real) if real else start
    return sorted(a for a in lead if start <= a < end and a <= last)


def instrument(path, game, address, chunks=()):
    """The traced copy of the generated function at `path` (its block table,
    then its text with the markers). `chunks`: the generated files of its
    later entry_ chunks, for a function the recompiler wrote as several
    (game.cmake): one table over the whole function, each chunk's text after
    the one before."""
    paths = [path] + list(chunks)
    texts = [open(p, encoding='utf-8', newline='').read() for p in paths]
    digest = hashlib.sha256(''.join(texts).encode('utf-8')).hexdigest()[:16]
    m = RANGE.search(texts[0])
    insns = {}
    for text in texts:
        for a, insn in instructions(text).items():
            insns.setdefault(a, insn)
    start, end = (int(m.group(1), 16), int(m.group(2), 16)) if m else (address, max(insns, default=address) + 4)
    if m and chunks:
        last = RANGE.search(texts[-1])
        if last:
            end = int(last.group(2), 16)
    # Per text: its lines, the line of each instruction's first comment and
    # of each label, and the range it holds.
    places = []
    for text in texts:
        lines = text.split('\n')
        first_comment, label_line = {}, {}
        for i, line in enumerate(lines):
            mi = INSN.match(line)
            if mi and not line.rstrip().endswith('(Delay Slot)'):
                first_comment.setdefault(int(mi.group(1), 16), i)
                continue
            ml = LABEL.match(line)
            if ml:
                label_line.setdefault(int(ml.group(1), 16), i)
        r = RANGE.search(text)
        held = (int(r.group(1), 16), int(r.group(2), 16)) if r else (start, end)
        places.append((lines, first_comment, label_line, held, {}, {}))
    blocks = []
    # The recompiler's entry blocks start blocks too: a jump table's case
    # after a call (FUN_0018FA00's 0x18FB98, the return address of its jal
    # and a case of its jr) is one, and the safe-point checks (the markers'
    # order, --safepoints' slices) follow the original through them.
    entries = [a for a in entry_leaders(os.path.dirname(path)) if start < a < end]
    for a in leaders(start, end, insns, entries):
        # (the text whose range holds it first: a chunk's text has comments
        # of no other chunk's instructions)
        order = sorted(places, key=lambda p: not (p[3][0] <= a < p[3][1]))
        for lines, first_comment, label_line, _, after, before in order:
            if a in label_line:
                after.setdefault(label_line[a], []).append(len(blocks))
            elif a in first_comment:
                before.setdefault(first_comment[a], []).append(len(blocks))
            else:
                continue
            blocks.append(a)
            break
    body = []
    for lines, _, _, _, after, before in places:
        out = []
        for i, line in enumerate(lines):
            for b in before.get(i, ()):
                out.append('    FNCHECK_BLOCK(%du);' % b)
            out.append(line)
            for b in after.get(i, ()):
                out.append('    FNCHECK_BLOCK(%du);' % b)
        body.append('\n'.join(out))
    # The blocks that end with a JR through a jump table the recompiler made
    # a switch (`switch (jumpTarget)` after it): run in place in the whole
    # function, an IndirectJump dispatch from an entry block (ps2xFnCheck's
    # SliceEmu, ee::tableJump; drafter23).
    table_jumps = []
    for a, (w, _, _) in sorted(insns.items()):
        if control(a, w)[0] != 'jr' or w == 0x42000018 or (w >> 21) & 31 == 31:
            continue
        for lines, first_comment, _, _, _, _ in places:
            if a not in first_comment:
                continue
            i = first_comment[a]
            if any('switch (jumpTarget)' in l for l in lines[i:i + 16]):
                at = [b for b in blocks if b <= a]
                if at:
                    table_jumps.append((game, address, at[-1]))
            break
    starts = ', '.join('0x%xu' % b for b in blocks) or '0u'
    if chunks:
        source = '%s (%d entry_ chunks: %s)' % (os.path.basename(path), len(paths),
                                               ', '.join(os.path.basename(p) for p in paths[1:]))
        what = 'the function'
    else:
        source, what = os.path.basename(path), 'the generated function'
    head = ('// Generated by tools/fncheck_traced_gen.py from %s:\n'
            '// %s with a marker at each of its %d blocks (fncheck_build.h).\n'
            'namespace\n{\n    const uint32_t kFncheckBlockStarts[] = {%s};\n'
            '    const ::fncheck::BlockTable g_fncheckBlocks(0x%xu, static_cast<::game::Game>(%d), kFncheckBlockStarts, %du,\n'
            '                                                "%s");\n}\n'
            '#define FNCHECK_BLOCK(i) ::fncheck::blockHit(g_fncheckBlocks, (i))\n'
            % (source, what, len(blocks), starts, address, game, len(blocks), digest))
    return head + ''.join(body) + '\n#undef FNCHECK_BLOCK\n', len(blocks), table_jumps


def parse_int(s):
    return int(s, 0)


def stack_args(text):
    """(low, high) of the bytes above the entry sp the function reads, or None."""
    insns = instructions(text)
    sp = 0
    fp = None
    adjusted = False
    written = set()
    low, high = None, None
    for a in sorted(insns):
        w, mn, ops = insns[a]
        if mn in ('addiu', 'daddiu') and re.match(r'^\$sp,\s*\$sp,', ops):
            n = s16(w & 0xFFFF)
            if n < 0:
                if adjusted:
                    break  # a second frame (alloca): stop here
                adjusted = True
                sp += n
            elif not adjusted:
                return None  # a restore with no frame made: a part of another function's code
            continue  # the epilogue's restore: the frame stays for the code after it
        if mn in ('daddu', 'addu', 'or') and re.match(r'^\$fp,\s*\$sp,\s*\$zero$', ops):
            fp = sp
            continue
        if mn in ('addiu', 'daddiu') and re.match(r'^\$fp,\s*\$sp,', ops):
            fp = sp + s16(w & 0xFFFF)
            continue
        # The address of a stack slot handed on (daddu $a0, $sp, $zero; addiu
        # $a1, $sp, 0x10): a callee fills it, so reads from there on are not
        # the caller's arguments.
        m = re.match(r'^\$(\w+),\s*\$sp,\s*(\$zero|-?(?:0x)?[0-9A-Fa-f]+)$', ops)
        if m and m.group(1) != 'sp' and mn in ('daddu', 'addu', 'or', 'addiu', 'daddiu'):
            k = sp + (0 if m.group(2) == '$zero' else parse_int(m.group(2)))
            if k >= 0:
                written.update(range(k, k + 0x100))
            continue
        size = LOADS.get(mn) or STORES.get(mn)
        if not size:
            continue
        m = MEM.match(ops)
        if not m:
            continue
        base = m.group(3)
        off = parse_int(m.group(2))
        if base == 'sp':
            ea = sp + off
        elif base == 'fp' and fp is not None:
            ea = fp + off
        else:
            continue
        if ea < 0:
            continue
        if mn in STORES:
            written.update(range(ea, ea + size))
            continue
        if any(b in written for b in range(ea, ea + size)):
            continue
        low = ea if low is None else min(low, ea)
        high = ea + size if high is None else max(high, ea + size)
    if low is None or high > 0x400:
        return None
    return (low, high)


FUNC_FILE = re.compile(r'_0x([0-9a-f]+)\.cpp$')
FREG = re.compile(r'\$f(\d+)\b')
# FPU instructions whose first operand is only read (no FPU destination).
FPU_READ_ONLY = ('swc1', 'mfc1', 'adda.s', 'suba.s', 'mula.s', 'madda.s', 'msuba.s')


def f0_use(mn, ops):
    """'read' when the instruction reads $f0, 'write' when it sets it without reading
    it, else None."""
    regs = FREG.findall(ops)
    if '0' not in regs:
        return None
    if mn in ('mtc1', 'ctc1'):
        return 'write' if regs[-1] == '0' else None
    if mn == 'lwc1':
        return 'write' if regs[0] == '0' else None
    if mn in FPU_READ_ONLY or mn.startswith('c.') or mn.startswith('bc1'):
        return 'read'
    # fd, fs[, ft]: the destination first
    if regs[0] == '0' and '0' not in regs[1:]:
        return 'write'
    return 'read'


def f0_readers(insns):
    """Call targets whose f0 a call site reads after the call returns (a straight-line
    scan from the instruction after the delay slot to the first write of f0 or the
    next branch, jump or call)."""
    out = set()
    order = sorted(insns)
    index = {a: i for i, a in enumerate(order)}
    for a in order:
        w, mn, _ = insns[a]
        if w >> 26 != 3:  # jal
            continue
        target = ((a + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
        i = index.get(a + 8)
        while i is not None and i < len(order):
            b = order[i]
            wb, mnb, opsb = insns[b]
            use = f0_use(mnb, opsb)
            if use == 'read':
                out.add(target)
                break
            if use == 'write':
                break
            kind, _ = control(b, wb)
            if kind is not None:
                # the branch's delay slot runs on either way
                d = insns.get(b + 4)
                if d and f0_use(d[1], d[2]) == 'read':
                    out.add(target)
                break
            i += 1
    return out


GPR_NAMES = ['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3', 't0', 't1', 't2', 't3', 't4', 't5', 't6', 't7',
             's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7', 't8', 't9', 'k0', 'k1', 'gp', 'sp', 'fp', 'ra']
GPR_NUM = {n: i for i, n in enumerate(GPR_NAMES)}
GPR_NUM['s8'] = 30
# The registers a callee may leave for its caller to read (caller-saved):
# at, v0, v1, a0-a3, t0-t7, t8, t9; f0-f19.
RESULT_GPRS = sum(1 << r for r in list(range(1, 16)) + [24, 25])
RESULT_FPRS = (1 << 20) - 1
FPU_MOVES_TO = ('mtc1', 'dmtc1', 'ctc1')       # rt -> fs
FPU_MOVES_FROM = ('mfc1', 'dmfc1', 'cfc1', 'qmfc2', 'cfc2', 'mfc0')   # fs -> rt


def reg_bits(names):
    """(GPR mask, FPR mask) of register names."""
    g = f = 0
    for n in names:
        if n in GPR_NUM:
            g |= 1 << GPR_NUM[n]
        elif n.startswith('f') and n[1:].isdigit():
            f |= 1 << int(n[1:])
    return g, f


UNALIGNED = {'lwl': ('lwr', 3), 'lwr': ('lwl', -3), 'ldl': ('ldr', 7), 'ldr': ('ldl', -7)}


def full_unaligned(insns):
    """The addresses of the first of an lwl/lwr (ldl/ldr) pair that loads a whole
    word (doubleword) into one register: the two halves at offsets o + 3 and o
    (o + 7 and o) of one base, next to each other (a struct copy's unaligned
    load). The register's old bytes are all replaced, so the first is a load,
    not a merge that reads the register."""
    out = set()
    for a, (w, mn, ops) in insns.items():
        pair = UNALIGNED.get(mn)
        nxt = insns.get(a + 4)
        if not pair or not nxt or nxt[1] != pair[0]:
            continue
        m1, m2 = MEM.match(ops), MEM.match(nxt[2])
        if not m1 or not m2 or m1.group(1) != m2.group(1) or m1.group(3) != m2.group(3) or \
                m1.group(1) == m1.group(3):
            continue
        try:
            o1, o2 = int(m1.group(2), 0), int(m2.group(2), 0)
        except ValueError:
            continue
        if o1 - o2 == pair[1]:
            out.add(a)
    return out


def reg_access(w, mn, ops, full=False):
    """((GPRs read, FPRs read), (GPRs written, FPRs written)) of one instruction;
    a register both read and written counts as read (it is read first). full:
    the first of a whole-word unaligned pair (full_unaligned), a plain load."""
    regs = REG.findall(ops)
    if not regs:
        return (0, 0), (0, 0)
    kind, _ = control(0, w)
    if kind in ('branch', 'jr'):
        return reg_bits(regs), (0, 0)
    if kind == 'call':   # jalr rd, rs / jalr rs
        return reg_bits(regs[-1:]), (0, 0)
    if mn in FPU_MOVES_TO:
        return reg_bits(regs[:1]), reg_bits(regs[1:2])
    if mn in FPU_MOVES_FROM:
        return reg_bits(regs[1:]), reg_bits(regs[:1])
    if mn in READ_ONLY or mn.startswith('c.') or (mn in MERGES and not full) or \
            (mn in TWO_OPERAND_READS and len(regs) == 2):
        return reg_bits(regs), (0, 0)
    return reg_bits(regs[1:]), reg_bits(regs[:1])


def result_reads(insns, start):
    """The registers call sites read after a call returns, before setting them: a
    straight-line scan from the instruction after the delay slot to the next
    branch, jump or call (its delay slot included). ({target: (GPRs, FPRs)},
    [(target, GPRs, FPRs)] handed on: the caller returns (jr $ra) with them
    untouched, so its own callers' reads count; start is the caller's start, None
    for an entry block's file, whose function is not known here)."""
    reads, handed = {}, []
    full = full_unaligned(insns)
    order = sorted(insns)
    index = {a: i for i, a in enumerate(order)}
    for a in order:
        w = insns[a][0]
        if w >> 26 != 3:  # jal
            continue
        target = ((a + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
        tg, tf = RESULT_GPRS, RESULT_FPRS
        rg = rf = 0
        i = index.get(a + 8)
        while i is not None and i < len(order) and order[i] == a + 8 + 4 * (i - index[a + 8]):
            b = order[i]
            wb, mnb, opsb = insns[b]
            (r1, r2), (w1, w2) = reg_access(wb, mnb, opsb, b in full)
            rg |= r1 & tg
            rf |= r2 & tf
            kind, _ = control(b, wb)
            if kind is not None:
                d = insns.get(b + 4)
                dw = (0, 0)
                if d:
                    (d1, d2), dw = reg_access(*d)
                    rg |= d1 & tg & ~r1
                    rf |= d2 & tf & ~r2
                if kind == 'jr' and REG.findall(opsb)[:1] == ['ra'] and start is not None:
                    handed.append((target, tg & ~rg & ~dw[0], tf & ~rf & ~dw[1]))
                break
            tg &= ~w1
            tf &= ~w2
            i += 1
        if rg or rf:
            g0, f0 = reads.get(target, (0, 0))
            reads[target] = (g0 | rg, f0 | rf)
    return reads, handed


def tail_targets(insns, start, end):
    """Functions the code jumps to (j) outside its own range: tail calls, which
    return to its caller."""
    out = set()
    for a, (w, mn, ops) in insns.items():
        kind, target = control(a, w)
        if kind == 'jump' and not (start <= target < end):
            out.add(target)
    return out


def result_readers(sites):
    """{(game, function): (GPRs, FPRs)} some caller reads after calling it, a
    wrapper's callers (a caller returning with the register untouched) and a tail
    caller's callers included."""
    reads, edges = {}, []
    for game, start, r, handed, tails in sites:
        for t, m in r.items():
            g0, f0 = reads.get((game, t), (0, 0))
            reads[(game, t)] = (g0 | m[0], f0 | m[1])
        for t, hg, hf in handed:
            edges.append(((game, start), (game, t), hg, hf))
        for t in tails:
            edges.append(((game, start), (game, t), RESULT_GPRS, RESULT_FPRS))
    for _ in range(16):
        changed = False
        for src, dst, hg, hf in edges:
            sg, sf = reads.get(src, (0, 0))
            g0, f0 = reads.get(dst, (0, 0))
            g1, f1 = g0 | (sg & hg), f0 | (sf & hf)
            if (g1, f1) != (g0, f0):
                reads[dst] = (g1, f1)
                changed = True
        if not changed:
            break
    return reads


def whole_text(files):
    """A function of several entry_ chunks as one generated text: the whole
    function's range first (RANGE finds it), then each chunk's text."""
    texts = [open(f, encoding='utf-8', errors='replace').read() for f in files]
    first, last = RANGE.search(texts[0]), RANGE.search(texts[-1])
    if not first or not last:
        return None
    return '// Address: 0x%s - 0x%s\n' % (first.group(1), last.group(2)) + '\n'.join(texts)


# Generated code that goes into the runtime's own code, which reads and writes
# guest memory or changes the kernel's state without telling a tracer: a
# library function the runtime implements (ps2_stubs, ps2_syscalls) and the
# syscall and exception instructions.
UNTRACED = re.compile(r'\bps2_stubs::|\bps2_syscalls::|runtime->handleSyscall\(|runtime->SignalException\(')


def scan(game, folder, readers, flows=None, sites=None, frames=None, calls=None, vu0=None, chunked=None, untraced=None,
         framed=None):
    """`chunked`: {address: [generated files]} of the functions the recompiler
    wrote as several entry_ chunks (the list's fifth column): their frame, call
    sites, argument flow and VU0 flow are the whole function's, from its chunks
    (other entry_ files, a function's slices, have none of their own).
    `framed`: the registered functions that are one entry_ chunk: their frame
    and call sites (as a function's) from it.
    `untraced`: a set the functions whose code runs the runtime's (UNTRACED)
    are added to, (game, address)."""
    chunked = chunked or {}
    framed = framed or set()
    out = []
    for name in sorted(os.listdir(folder)):
        m = FUNC_FILE.search(name)
        if not m:
            continue
        try:
            text = open(os.path.join(folder, name), encoding='utf-8', errors='replace').read()
        except OSError:
            continue
        insns = instructions(text)
        readers.update((game, t) for t in f0_readers(insns))
        if untraced is not None and UNTRACED.search(text):
            untraced.add((game, int(m.group(1), 16)))
        if sites is not None and insns:
            rm = RANGE.search(text)
            start = int(rm.group(1), 16) if rm and not name.startswith('entry_') else None
            r, handed = result_reads(insns, start)
            tails = tail_targets(insns, start, int(rm.group(2), 16)) if start is not None else set()
            sites.append((game, start, r, handed, tails))
        whole = chunked.get(int(m.group(1), 16))
        if frames is not None and insns and (not name.startswith('entry_') or whole or int(m.group(1), 16) in framed):
            rm = RANGE.search(text)
            if rm:
                end_whole = None
                if whole:
                    insns = {}
                    for chunk in whole:
                        chunk_text = open(chunk, encoding='utf-8', errors='replace').read()
                        for a, insn in instructions(chunk_text).items():
                            insns.setdefault(a, insn)
                    last = RANGE.search(chunk_text)
                    end_whole = int(last.group(2), 16) if last else None
                fr = prologue_frame(insns, int(rm.group(1), 16))
                if fr is not None:
                    frames.append((game, int(rm.group(1), 16), fr[0], fr[1]))
                if calls is not None:
                    start, end = int(rm.group(1), 16), end_whole or int(rm.group(2), 16)
                    # (a target called from more than one site: 0, so that
                    # its calls take no other site's return address)
                    for target, rets in sorted(call_sites(insns, start, end).items()):
                        calls.append((game, start, target, rets[0] if len(rets) == 1 else 0))
        if flows is not None and (not name.startswith('entry_') or whole):
            source = whole_text(whole) if whole else text
            flow = function_flow(source) if source else None
            if flow:
                flows[int(m.group(1), 16)] = flow
            if vu0 is not None:
                if name.startswith(VU0_ECOSSIN):
                    vu0[int(m.group(1), 16)] = 'ecossin'
                elif source:
                    flow = vu0_flow(source)
                    if flow:
                        vu0[int(m.group(1), 16)] = flow
        r = stack_args(text)
        # An entry into another function's code (entry_*: the recompiler's
        # split at a jump target) with no frame of its own runs in that
        # function's frame: its sp reads are the function's locals.
        if r and name.startswith('entry_') and not re.search(r'addiu\s+\$sp, \$sp, -', text):
            r = None
        if r:
            out.append((game, int(m.group(1), 16), r[0], r[1]))
    return out


HERE = os.path.dirname(os.path.abspath(__file__))
INDIRECT_CALLS = os.path.join(HERE, 'readable_indirect_calls.tsv')
DEAD_READS = os.path.join(HERE, 'readable_args_dead.tsv')
DEAD_READS_OTHER = {1: os.path.join(HERE, 'readable_args_dead_dm2.tsv'), 2: os.path.join(HERE, 'readable_args_dead_bmx.tsv')}
ARG_BITS = {'a0': 0, 'a1': 1, 'a2': 2, 'a3': 3, 't0': 4, 't1': 5, 't2': 6, 't3': 7}
ARG_BITS.update({'f%d' % (12 + i): 8 + i for i in range(8)})
CALLER_SAVED_ARGS = 0xFFFF  # a callee may change every argument register
REG = re.compile(r'\$(\w+)')
READ_ONLY = {'sb', 'sh', 'sw', 'sd', 'sq', 'swl', 'swr', 'sdl', 'sdr', 'swc1', 'sdc1', 'sqc2', 'jr',
             'ctc1', 'mthi', 'mtlo', 'mthi1', 'mtlo1', 'qmtc2', 'ctc2', 'mtsab', 'mtsah', 'mtsa', 'teq', 'tne', 'tge',
             'tgeu', 'tlt', 'tltu', 'mtc0', 'cache', 'pref'} | set(FPU_READ_ONLY) - {'mfc1'}
TWO_OPERAND_READS = {'mult', 'multu', 'div', 'divu', 'mult1', 'multu1', 'div1', 'divu1', 'madd', 'maddu', 'madd1',
                     'maddu1', 'dmult', 'dmultu', 'ddiv', 'ddivu'}
MERGES = {'movn', 'movz', 'lwl', 'lwr', 'ldl', 'ldr', 'movt', 'movf', 'movt.s', 'movf.s', 'movn.s', 'movz.s'}


# The callee-saved registers whose entry values a prologue may save: s0-s7, gp,
# fp, ra (GPR numbers) and f20-f31 (32 + n), as ee::EntrySave numbers them.
SAVED_GPRS = set(range(16, 24)) | {28, 30, 31}
# The argument registers (a0-a3, t0-t3; f12-f19 as 32 + n) whose entry values a prologue may store in its frame:
# a spill of an argument the function keeps there (BMX buildRigid's `sw a2, 0xC(sp)`, the materials table, which a
# later function's matrix copy reads as a w lane; frame1), or a variadic function's words. Part of the original's
# frame as its register saves are: ee::enterFrame writes them from the call's arguments.
ARG_GPRS = set(range(4, 12))
ARG_FPRS = set(range(12, 20))


def prologue_frame(insns, start):
    """The original's stack frame from its prologue: (size, [(reg, bytes, offset)])
    for the register saves (a callee-saved or argument register's entry value
    stored to the frame), or None when it cannot be told. The prologue is the straight-line code
    from the entry to the first branch, jump or call (with its delay slot): its
    frame adjustment (addiu sp, sp, -size; none: size 0, if the function has no
    frame adjustment anywhere) and the stores of callee-saved and argument
    registers not written before them. A frame made after a branch (an early exit first) or
    with another sp instruction is not told."""
    size = None
    saves = []
    written_g = written_f = 0
    a = start
    stop = None
    while a in insns:
        w, mn, ops = insns[a]
        if mn in ('addiu', 'daddiu') and re.match(r'^\$sp,\s*\$sp,', ops):
            n = s16(w & 0xFFFF)
            if n >= 0 or size is not None:
                break
            size = -n
        elif mn in STORES:
            m = MEM.match(ops)
            if m and m.group(3) == 'sp' and size is not None:
                name = m.group(1)
                if name in GPR_NUM and (GPR_NUM[name] in SAVED_GPRS or GPR_NUM[name] in ARG_GPRS) and \
                        not (written_g >> GPR_NUM[name]) & 1 and STORES[mn] in (4, 8, 16):
                    reg = GPR_NUM[name]
                elif name.startswith('f') and name[1:].isdigit() and \
                        (20 <= int(name[1:]) <= 31 or int(name[1:]) in ARG_FPRS) and \
                        not (written_f >> int(name[1:])) & 1 and STORES[mn] == 4:
                    reg = 32 + int(name[1:])
                else:
                    reg = None
                off = parse_int(m.group(2))
                if reg is not None and 0 <= off and off + STORES[mn] <= size and \
                        not any(r == reg for r, _, _ in saves):
                    saves.append((reg, STORES[mn], off))
        else:
            _, (wg, wf) = reg_access(w, mn, ops)
            if mn in MERGES:
                rs = REG.findall(ops)
                wg2, wf2 = reg_bits(rs[:1])
                wg |= wg2
                wf |= wf2
            if (wg >> 29) & 1:
                if size is None:
                    return None  # a frame made another way
                break  # the epilogue (daddu sp, fp, zero)
            written_g |= wg
            written_f |= wf
        if stop is not None and a >= stop:
            break
        kind, _ = control(a, w)
        if kind and stop is None:
            stop = a + 4
        a += 4
    if size is None:
        # No frame in the prologue: a frameless function, unless it makes one later.
        if any(mn in ('addiu', 'daddiu') and re.match(r'^\$sp,\s*\$sp,\s*-', ops) for _, mn, ops in insns.values()):
            return None
        return (0, [])
    return (size, saves)


def reg_mask(names):
    m = 0
    for n in names:
        b = ARG_BITS.get(n)
        if b is not None:
            m |= 1 << b
    return m


def insn_uses(w, mn, ops, full=False):
    """(read mask, written mask) of the argument registers (a0-a3, t0-t3, f12-f19);
    full: the first of a whole-word unaligned pair (full_unaligned)."""
    regs = REG.findall(ops)
    if not regs:
        return 0, 0
    kind, _ = control(0, w)
    if kind in ('branch', 'jr') or mn in READ_ONLY or mn.startswith('c.') or \
            (mn in TWO_OPERAND_READS and len(regs) == 2):
        return reg_mask(regs), 0
    if mn in ('mfc1', 'dmfc1', 'cfc1', 'qmfc2', 'cfc2', 'mfc0'):
        return reg_mask(regs[1:]), reg_mask(regs[:1])
    if mn in ('mtc1', 'dmtc1'):  # mtc1 rt, fs: fs is the one written
        return reg_mask(regs[:1]), reg_mask(regs[1:])
    if kind == 'call':  # jalr rd, rs / jalr rs (jal has no register operand)
        return reg_mask(regs[-1:]), 0
    if mn in MERGES and not full:
        return reg_mask(regs), 0
    return reg_mask(regs[1:]), reg_mask(regs[:1])


def function_flow(text, uses=None, flagged=None):
    """A generated function's argument flow: (start, blocks); blocks: {leader:
    (steps, succs)}, steps a list of ('use', read, written) and ('call', target or
    None, the call's address), the delay slot of a call before it; succs the blocks
    it goes on to, or ('tail', target, the jump's address) for a jump out of the
    function. uses: another insn_uses, for a flow of something other than the
    argument registers, and flagged: what picks the instructions its last
    argument is true for (vu0_flow)."""
    m = RANGE.search(text)
    insns = instructions(text)
    if not m or not insns:
        return None
    insn_uses = uses or globals()['insn_uses']
    start, end = int(m.group(1), 16), int(m.group(2), 16)
    lead = leaders(start, end, insns)
    lead_set = set(lead)
    full = flagged(insns) if flagged else {a for a in full_unaligned(insns) if a + 4 not in lead_set}
    # A variadic function's spill of its argument registers (sprintf's sd a1-a3,
    # t0-t3 into its frame, for va_arg): not reads of them (a caller passes only
    # what the format needs). Four or more argument registers stored to sp-based
    # slots in the first 32 instructions.
    spills = set()
    for a in range(start, min(end, start + 128), 4):
        w_mn_ops = insns.get(a)
        if not w_mn_ops:
            continue
        w, mn, ops = w_mn_ops
        mm = MEM.match(ops)
        if mn in ('sw', 'sd', 'sq') and mm and mm.group(3) == 'sp' and mm.group(1) in ARG_BITS and \
                mm.group(1) != 'a0':
            spills.add(a)
    if len(spills) < 4:
        spills = set()
    blocks = {}
    for i, b in enumerate(lead):
        stop = lead[i + 1] if i + 1 < len(lead) else end
        steps, succs = [], []
        a = b
        while a < stop and a in insns:
            w, mn, ops = insns[a]
            kind, target = control(a, w)
            if kind is None:
                if (w & 0xFC00003F) == 0x0000000C:
                    # SYSCALL: the kernel reads its arguments from registers
                    # no operand names (drafter25).
                    steps.append(('sys',))
                    a += 4
                    continue
                r, wr = insn_uses(w, mn, ops, a in full)
                if a in spills:
                    r = 0
                steps.append(('use', r, wr))
                a += 4
                continue
            delay = insns.get(a + 4)
            dr, dw = insn_uses(*delay) if delay else (0, 0)
            if a + 4 in spills:
                dr = 0
            likely = mn.endswith('l') and kind == 'branch' and mn not in ('bal',)
            r, _ = insn_uses(w, mn, ops)
            steps.append(('use', r, 0))
            if kind == 'call':
                steps.append(('use', dr, dw))
                steps.append(('call', ((a + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2) if w >> 26 == 3 else None, a))
                a += 8
                continue
            # A branch-likely's delay slot runs only when it is taken: its
            # reads and writes go on the taken edge (updatePlaying's a0, set in
            # the delay slot at 0x172724).
            if not likely:
                steps.append(('use', dr, dw))
            if kind == 'branch':
                if target in lead_set:
                    succs.append(('likely', target, dr, dw) if likely else target)
                if a + 8 in lead_set and not always_taken(w):
                    succs.append(a + 8)
            elif kind == 'jump':
                if start <= target < end and target in lead_set:
                    succs.append(target)
                elif not (start <= target < end):
                    succs.append(('tail', target, a))
            elif kind == 'jr' and w != 0x42000018 and (w >> 21) & 31 != 31:
                # A jump table's cases (the recompiler's switch after the JR),
                # else a tail call through a register: an unknown callee
                # (drafter25; both were left out).
                cases = jump_cases(text, a)
                if cases is None:
                    succs.append(('tail', None, a))
                else:
                    succs += [c for c in cases if c in lead_set]
            # jr ra: a return
            a = stop
            break
        else:
            if stop in lead_set:
                succs.append(stop)
        blocks[b] = (steps, succs)
    return start, blocks


def block_reads(steps, live_out, callee_reads):
    """The argument registers read before being written, from the block's start."""
    live = live_out
    for step in reversed(steps):
        if step[0] == 'use':
            live = (live & ~step[2]) | step[1]
        elif step[0] == 'sys':
            # A kernel call reads its arguments (callee_reads(None): an unknown
            # callee's) and leaves the argument registers as they were.
            live |= callee_reads(None)
        else:
            # A call: the callee reads its own arguments (as far as known) and
            # may change every argument register (they are caller-saved).
            live = (live & ~CALLER_SAVED_ARGS) | callee_reads(step[1], step[2])
    return live


def entry_reads(flow, callee_reads):
    start, blocks = flow
    live_in = {b: 0 for b in blocks}
    changed = True
    while changed:
        changed = False
        for b in sorted(blocks, reverse=True):
            steps, succs = blocks[b]
            out = 0
            for s in succs:
                if isinstance(s, tuple) and s[0] == 'likely':
                    out |= (live_in.get(s[1], 0) & ~s[3]) | s[2]
                else:
                    out |= callee_reads(s[1], s[2]) if isinstance(s, tuple) else live_in.get(s, 0)
            new = block_reads(steps, out, callee_reads)
            if new != live_in[b]:
                live_in[b] = new
                changed = True
    return live_in.get(start, 0)


def jump_cases(text, jr):
    """The case addresses of the recompiler's switch after the JR at `jr`, or None."""
    i = text.find('// 0x%x: ' % jr)
    if i < 0:
        return None
    m = re.match(r'[^\n]*\n(?:[^\n]*\n){0,15}?[^\n]*switch \(jumpTarget\) \{(.*?)\}', text[i:], re.S)
    if not m:
        return None
    return [int(c, 16) for c in re.findall(r'case 0x([0-9A-Fa-f]+)u:', m.group(1))]


def load_indirect(path=INDIRECT_CALLS):
    """{address of a jalr or an indirect jump: [the callees recordings saw there]}
    (readable_indirect_calls.tsv, written by readable_args.py indirect)."""
    calls = {}
    if os.path.exists(path):
        for line in open(path, encoding='utf-8'):
            if line.strip() and not line.startswith('#'):
                site, target = line.split()[:2]
                calls.setdefault(int(site, 16), []).append(int(target, 16))
    return calls


def load_dead(path=DEAD_READS):
    """{function: mask} of the reads a person has read and found dead: the scan's
    live range says the function reads the register, the code never uses the value
    (readable_args_dead.tsv: function, registers, why)."""
    dead = {}
    if os.path.exists(path):
        for line in open(path, encoding='utf-8'):
            if line.strip() and not line.startswith('#'):
                f = line.split('\t')
                dead[int(f[0], 16)] = dead.get(int(f[0], 16), 0) | reg_mask(f[1].split(','))
    return dead


def arg_readers(flows, unknown=0, indirect=None, dead=None):
    """{address: mask} of the argument registers each function reads before it sets
    them, its callees' (and tail calls') reads of registers it leaves included.
    unknown: what a callee the scan cannot name reads (a kernel call, an indirect call
    or tail call no recording saw): 0, or every argument register (CALLER_SAVED_ARGS:
    the registers a function hands on to one, as the drafter's variadic callees).
    indirect: load_indirect's: an indirect call reads what its recorded callees read.
    dead: load_dead's, taken out of a function's reads."""
    reads = {a: 0 for a in flows}
    indirect = indirect or {}
    dead = dead or {}

    def callee_reads(t, site=None):
        if t is not None:
            return reads.get(t, 0)
        seen = indirect.get(site)
        if not seen:
            return unknown
        out = 0
        for s in seen:
            out |= reads.get(s, 0)
        return out
    for _ in range(12):
        changed = False
        for a in sorted(flows):
            r = entry_reads(flows[a], callee_reads) & ~dead.get(a, 0)
            if r != reads[a]:
                reads[a] = r
                changed = True
        if not changed:
            break
    return reads


# ---- what crosses a function boundary in VU0's vector registers --------------
#
# Lane masks: bit r * 4 + l of a 128-bit number for vf<r>, l 0 x .. 3 w (vf0 is a
# constant: never). An instruction's reads and writes come from its word (the
# listing has no text for some, and no dest for others).

def vu0_lane(r, lanes):
    return 0 if r == 0 else lanes << (r * 4)


def vu0_dest(w):
    return ((w >> 24) & 1) | ((w >> 23) & 1) << 1 | ((w >> 22) & 1) << 2 | ((w >> 21) & 1) << 3


def vu0_uses(w):
    """(read lanes, written lanes) of a word: LQC2, SQC2, QMFC2, QMTC2 and COP2's macro
    instructions; VI registers, Q, ACC and the flags are not lanes."""
    L = vu0_lane
    op = w >> 26
    if op == 0x36:
        return 0, L((w >> 16) & 31, 15)
    if op == 0x3E:
        return L((w >> 16) & 31, 15), 0
    if op != 0x12:
        return 0, 0
    rs = (w >> 21) & 31
    if rs == 1:
        return L((w >> 11) & 31, 15), 0
    if rs == 5:
        return 0, L((w >> 11) & 31, 15)
    if not rs & 16:
        return 0, 0
    d = vu0_dest(w)
    ft, fs, fd = (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
    fn = w & 63
    if fn < 0x3C:
        if fn < 0x1C:                              # VADD .. VMULbc
            return L(fs, d) | L(ft, 1 << (fn & 3)), L(fd, d)
        if 0x1C <= fn <= 0x1F or 0x20 <= fn <= 0x27:  # the Q and I forms, VMAXi, VMINIi
            return L(fs, d), L(fd, d)
        if fn == 0x2C and fs == ft:                # VSUB fd, fs, fs: zero whatever fs holds
            return 0, L(fd, d)
        if fn in (0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2F):
            return L(fs, d) | L(ft, d), L(fd, d)
        if fn == 0x2E:                             # VOPMSUB
            return L(fs, 7) | L(ft, 7), L(fd, 7)
        return 0, 0                                # VI arithmetic, VCALLMS
    op2 = (fd << 2) | (w & 3)
    if op2 < 0x10 or 0x18 <= op2 < 0x1C:           # VADDAbc .. VMULAbc
        return L(fs, d) | L(ft, 1 << (op2 & 3)), 0
    if 0x10 <= op2 < 0x18 or op2 in (0x1D, 0x30):  # VITOF, VFTOI, VABS, VMOVE
        return L(fs, d), L(ft, d)
    if op2 in (0x1C, 0x1E) or 0x20 <= op2 <= 0x27: # the A forms of Q and I
        return L(fs, d), 0
    if op2 == 0x1F:                                # VCLIPw
        return L(fs, 7) | L(ft, 8), 0
    if op2 in (0x28, 0x29, 0x2A, 0x2C, 0x2D):
        return L(fs, d) | L(ft, d), 0
    if op2 == 0x2E:                                # VOPMULA
        return L(fs, 7) | L(ft, 7), 0
    if op2 == 0x31:                                # VMR32: x from y, y from z, z from w, w from x
        return L(fs, ((d & 7) << 1) | (d >> 3)), L(ft, d)
    if op2 in (0x34, 0x36, 0x3D, 0x40, 0x41):      # VLQI, VLQD, VMFIR, VRNEXT, VRGET
        return 0, L(ft, d)
    if op2 in (0x35, 0x37):                        # VSQI, VSQD
        return L(fs, d), 0
    fsf, ftf = (w >> 21) & 3, (w >> 23) & 3
    if op2 in (0x38, 0x3A):                        # VDIV, VRSQRT
        return L(fs, 1 << fsf) | L(ft, 1 << ftf), 0
    if op2 == 0x39:                                # VSQRT
        return L(ft, 1 << ftf), 0
    if op2 in (0x3C, 0x42, 0x43):                  # VMTIR, VRINIT, VRXOR
        return L(fs, 1 << fsf), 0
    return 0, 0                                    # VNOP, VWAITQ, VILWR, VISWR


# libvu0's cosine and sine: the runtime's replacement, so no listing in its file. Its
# argument in vf6.x; it writes vf4, vf5 and vf8 whole, and x and w of vf6 and vf7.
VU0_ECOSSIN = '_sceVu0ecossin_'
VU0_ECOSSIN_READS = vu0_lane(6, 1)
VU0_ECOSSIN_SETS = vu0_lane(4, 15) | vu0_lane(5, 15) | vu0_lane(8, 15) | vu0_lane(6, 9) | vu0_lane(7, 9)
VU0_ALL = (1 << 128) - 1


def vu0_low_word_reads(insns):
    """The QMFC2s whose register next goes to an MTC1: they read x only (the engine's
    macros take a float out of vf5 that way)."""
    out = set()
    for a, (w, mn, ops) in insns.items():
        if mn.startswith('qmfc2'):
            rt = ops.split(',')[0].strip()
            for k in range(1, 9):
                n = insns.get(a + 4 * k)
                if n is None:
                    break
                if rt in n[2]:
                    if n[1] == 'mtc1':
                        out.add(a)
                    break
    return out


def vu0_flow(text):
    """function_flow in VU0 lanes."""
    def uses(w, mn, ops, low_word=False):
        return (vu0_lane((w >> 11) & 31, 1), 0) if low_word else vu0_uses(w)
    return function_flow(text, uses, vu0_low_word_reads)


def vu0_lanes(flows):
    """({function: lanes it reads before setting them, its callees' included},
    {function: lanes live when it returns, at any of its call sites and through its
    callers' returns}, the lanes live after some indirect call: any function may be
    that callee). A call sets what its callee sets on every path to its return, so a
    lane a callee loads is not live in the caller."""
    known = {a for a, f in flows.items() if f == 'ecossin'}
    real = {a: f for a, f in flows.items() if a not in known}
    reads = {a: 0 for a in flows}
    sets = {a: VU0_ALL for a in real}
    for a in known:
        reads[a] = VU0_ECOSSIN_READS
        sets[a] = VU0_ECOSSIN_SETS

    def walk(flow, entry, at_call, at_tail):
        """Backward over the blocks: `entry` at a return, at_call(target, site, live
        after it) gives what is live before a call."""
        start, blocks = flow
        live = {b: 0 for b in blocks}
        changed = True
        while changed:
            changed = False
            for b in sorted(blocks, reverse=True):
                steps, succs = blocks[b]
                out = 0 if succs else entry
                for s in succs:
                    if isinstance(s, tuple) and s[0] == 'likely':
                        out |= (live.get(s[1], 0) & ~s[3]) | s[2]
                    elif isinstance(s, tuple):
                        out |= at_tail(s[1], s[2], entry)
                    else:
                        out |= live.get(s, 0)
                x = out
                for step in reversed(steps):
                    if step[0] == 'use':
                        x = (x & ~step[2]) | step[1]
                    elif step[0] == 'call':
                        x = at_call(step[1], step[2], x)
                if x != live[b]:
                    live[b] = x
                    changed = True
        return live.get(start, 0)

    def set_on_every_path(flow):
        start, blocks = flow
        must = {b: VU0_ALL for b in blocks}
        changed = True
        while changed:
            changed = False
            for b in sorted(blocks, reverse=True):
                steps, succs = blocks[b]
                out = VU0_ALL if succs else 0
                for s in succs:
                    if isinstance(s, tuple) and s[0] == 'likely':
                        out &= must.get(s[1], 0) | s[3]
                    elif isinstance(s, tuple):
                        out &= sets.get(s[1], 0)
                    else:
                        out &= must.get(s, 0)
                x = out
                for step in steps:
                    if step[0] == 'use':
                        x |= step[2]
                    elif step[0] == 'call':
                        x |= sets.get(step[1], 0)
                if x != must[b]:
                    must[b] = x
                    changed = True
        return must.get(start, 0)

    for _ in range(64):
        changed = False
        for a in sorted(real):
            n = set_on_every_path(real[a])
            if n != sets[a]:
                sets[a] = n
                changed = True
        if not changed:
            break

    def call(t, site, x):
        return (x & ~sets.get(t, 0)) | reads.get(t, 0)

    for _ in range(64):
        changed = False
        for a in sorted(real):
            r = walk(real[a], 0, call, lambda t, site, entry: reads.get(t, 0))
            if r != reads[a]:
                reads[a] = r
                changed = True
        if not changed:
            break

    after = {a: 0 for a in flows}
    indirect = [0, False]

    def note(t, x):
        if t in after:
            if x & ~after[t]:
                after[t] |= x
                indirect[1] = True
        elif x & ~indirect[0]:
            indirect[0] |= x
            indirect[1] = True

    def call_after(t, site, x):
        note(t, x)
        return call(t, site, x)

    def tail_after(t, site, entry):
        note(t, entry)
        return call(t, site, entry)
    for _ in range(64):
        indirect[1] = False
        for a in sorted(real):
            walk(real[a], after[a], call_after, tail_after)
        if not indirect[1]:
            break
    return reads, after, indirect[0]


SELF_TEST = """// Function: FUN_00100000
// Address: 0x100000 - 0x100030
void FUN_00100000_0x100000(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime) {
    switch (ctx->pc) {
        case 0x100018u: goto label_100018;
        default: break;
    }
    // 0x100000: 0x27bdfff0  addiu       $sp, $sp, -0x10
    // 0x100004: 0x8fa20010  lw          $v0, 0x10($sp)
    // 0x100008: 0x10400003  beqz        $v0, . + 4 + (0x3 << 2)
    {
        // 0x10000c: 0x0  nop (Delay Slot)
        if (taken) { goto label_100018; }
    }
    // 0x100010: 0x24420001  addiu       $v0, $v0, 0x1
    // 0x100014: 0xafa20000  sw          $v0, 0x0($sp)
label_100018:
    // 0x100018: 0x3e00008  jr          $ra
    {
        // 0x10001c: 0x27bd0010  addiu       $sp, $sp, 0x10 (Delay Slot)
    }
    // 0x100020: 0x0  nop
    // 0x100024: 0x0  nop
}
"""


def self_test():
    """The leaders and markers of a small function, and its stack-argument read."""
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, 'FUN_00100000_0x100000.cpp')
        open(path, 'w').write(SELF_TEST)
        text, n, _ = instrument(path, 0, 0x100000)
    lines = text.split('\n')
    def after(marker):
        i = lines.index('    FNCHECK_BLOCK(%du);' % marker)
        return lines[i - 1].strip(), lines[i + 1].strip()
    assert n == 3, n                                     # entry, fall-through 0x100010, target 0x100018
    assert 'kFncheckBlockStarts[] = {0x100000u, 0x100010u, 0x100018u}' in text
    assert after(0)[1].startswith('// 0x100000:')        # before the entry's instruction
    assert after(1)[1].startswith('// 0x100010:')        # before the fall-through's
    assert after(2)[0] == 'label_100018:'                # after the label gotos land on
    assert stack_args(SELF_TEST) == (0, 4), stack_args(SELF_TEST)   # lw 0x10($sp) past a 0x10 frame
    # mtc1 writes its FPR, an indirect call reads what its recorded callees read, a dead read goes.
    assert insn_uses(0x44806800, 'mtc1', '$zero, $f13') == (0, 1 << ARG_BITS['f13'])
    assert insn_uses(0x44856800, 'mtc1', '$a1, $f13') == (1 << ARG_BITS['a1'], 1 << ARG_BITS['f13'])
    caller = (0x100, {0x100: ([('call', None, 0x104)], [])})
    callee = (0x200, {0x200: ([('use', 1 << ARG_BITS['a1'], 0)], [])})
    flows = {0x100: caller, 0x200: callee}
    assert arg_readers(flows)[0x100] == 0
    assert arg_readers(flows, 0, {0x104: [0x200]})[0x100] == 1 << ARG_BITS['a1']
    assert arg_readers(flows, 0, {0x104: [0x200]}, {0x200: 1 << ARG_BITS['a1']})[0x100] == 0
    # A prologue's stores of entry values: callee-saved registers and argument registers (an argument kept in the
    # frame, buildRigid's `sw a2, 0xC(sp)`), not one written first (a0 below) nor one outside the frame.
    prologue = {0x100000: (0x27bdffc0, 'addiu', '$sp, $sp, -0x40'), 0x100004: (0xffbf0030, 'sd', '$ra, 0x30($sp)'),
                0x100008: (0xafa6000c, 'sw', '$a2, 0xC($sp)'), 0x10000c: (0x24840001, 'addiu', '$a0, $a0, 0x1'),
                0x100010: (0xafa40008, 'sw', '$a0, 0x8($sp)'), 0x100014: (0xe7ac0010, 'swc1', '$f12, 0x10($sp)'),
                0x100018: (0xafa70044, 'sw', '$a3, 0x44($sp)'), 0x10001c: (0x0c040000, 'jal', 'func_100000'),
                0x100020: (0x0, 'nop', '')}
    assert prologue_frame(prologue, 0x100000) == (0x40, [(31, 8, 0x30), (6, 4, 0xC), (44, 4, 0x10)]), \
        prologue_frame(prologue, 0x100000)
    print('fncheck_traced_gen: self-test passed')


ENTRY_ROW = re.compile(r'= (entry_\w+); // 0x([0-9a-f]+)$')


def entry_leaders(folder, cache={}):
    """The addresses register_functions.cpp of a generated folder maps to an
    entry_ function (the recompiler's entry blocks)."""
    if folder not in cache:
        found = set()
        try:
            for line in open(os.path.join(folder, 'register_functions.cpp'), encoding='utf-8', errors='replace'):
                m = ENTRY_ROW.search(line.rstrip())
                if m:
                    found.add(int(m.group(2), 16))
        except OSError:
            pass
        cache[folder] = found
    return cache[folder]


def safe_point_marks(game, address, gen, chunks=()):
    """[(game, function, address, flags)]: the traced function's entry blocks
    (flags 1), those a branch back of the function goes to (3: a loop head,
    which ee::loopBack marks), and the delay slots that end one (4).
    `chunks`: its later entry_ chunks' files (instrument)."""
    try:
        texts = [open(p, encoding='utf-8', errors='replace').read() for p in [gen] + list(chunks)]
    except OSError:
        return []
    text = ''.join(texts)
    m = RANGE.search(text)
    if not m:
        return []
    start, end = int(m.group(1), 16), int(m.group(2), 16)
    last = RANGE.search(texts[-1]) if chunks else None
    if last:
        end = int(last.group(2), 16)
    leaders = {a for a in entry_leaders(os.path.dirname(gen)) if start < a < end}
    heads, targets, delay_slots = set(), set(), set()
    for a, (w, mn, ops) in instructions(text).items():
        kind, target = control(a, w)
        if kind is not None:
            delay_slots.add(a + 4)
        if target is not None:
            targets.add(target)
        if kind in ('branch', 'jump') and target is not None and start <= target <= a:
            heads.add(target)
    # An entry block that starts in a delay slot no branch goes to is never
    # dispatched (the slot runs with its branch or call, whose return is the
    # address after it): not a mark the readable version owes (drafter20:
    # fn_1CFF58 0x1D0000, the slot of its jal at 0x1CFFFC).
    # It still ends the entry block before it, the slot run with it (flags 4,
    # for SliceEmu alone): that block's call returns to the scheduler, which
    # gives the whole function (diffcall4: fn_1CFF58's 0x1D0004).
    ends = {a for a in leaders if a in delay_slots and a not in targets}
    leaders -= ends
    return sorted([(game, address, a, 1 | (2 if a in heads else 0)) for a in leaders] +
                  [(game, address, a, 4) for a in ends], key=lambda m: m[2])


def call_sites(insns, start, end):
    """{target: [return address, ...]} of the function's calls: each jal TARGET,
    and each jalr linking ra (target 0: whatever its register holds); the return
    address is the jal's + 8 (drafter25)."""
    out = {}
    for a, (w, mn, ops) in insns.items():
        if not (start <= a < end):
            continue
        if w >> 26 == 3:
            t = ((a + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
        elif w >> 26 == 0 and (w & 0x3F) == 9 and (w >> 11) & 31 == 31:
            t = 0
        else:
            continue
        out.setdefault(t, []).append(a + 8)
    return out


def load_locals(path=None):
    """{(game, address): [offset, ...]}: tools/readable_locals.tsv, the places of the ee::Local
    objects the original does not keep in declaration order (ee/plain.h), one per ordinal (consecutive rows of one
    ordinal, Locals of sibling blocks, give the same place)."""
    path = path or os.path.join(os.path.dirname(os.path.abspath(__file__)), 'readable_locals.tsv')
    locals_of = {}
    others = [os.path.splitext(path)[0] + s for s in ('_dm2.tsv', '_bmx.tsv')]   # (another game's rows: read when its frames are scanned, frame_table)
    for file in [path] + [f for f in others if os.path.exists(f)]:
        for line in open(file, encoding='utf-8'):
            if line.startswith('#') or not line.strip():
                continue
            game, address, _name, ordinal, local, offset = line.rstrip('\n').split('\t')
            places = locals_of.setdefault((int(game), int(address, 16)), [])
            if places and int(ordinal) == len(places) - 1:
                # (another Local at the same ordinal, one of sibling blocks: the ordinal has one place)
                if int(offset, 16) != places[-1]:
                    raise SystemExit('readable_locals.tsv: %s %s: %s at ordinal %s, at 0x%x where the ordinal\'s other '
                                     'rows have 0x%x: Locals the original keeps apart need different ordinals' %
                                     (game, address, local, ordinal, int(offset, 16), places[-1]))
                continue
            if int(ordinal) != len(places):
                raise SystemExit('readable_locals.tsv: %s %s: ordinal %s out of order' % (game, address, ordinal))
            places.append(int(offset, 16))
    return locals_of


def frame_table(frames, calls=(), locals_of=None):
    """The frame table's source (ee/abi.h FrameRow; registered at static
    initialisation, so a build without it runs with none)."""
    locals_of = load_locals() if locals_of is None else locals_of
    # (a game's rows where its generated code was scanned: a build with none of a game's code, AI's included (a
    # contributor's first build, before any disc is set up), has no frame for any of them and no traced original to
    # need one)
    locals_of = {key: value for key, value in locals_of.items() if any(f[0] == key[0] for f in frames)}
    rows, saves, places = [], [], []
    for game, address, size, regs in sorted(frames):
        here = locals_of.pop((game, address), [])
        if any(offset >= size for offset in here):
            raise SystemExit('readable_locals.tsv: 0x%08x: a place outside its 0x%x frame' % (address, size))
        rows.append('    {%du, 0x%08xu, 0x%xu, %du, %du, %du, %du},\n' % (game, address, size, len(saves), len(regs), len(places), len(here)))
        saves += ['{%d, %d, 0x%x}' % r for r in regs]
        places += here
    if locals_of:  # (only the games scanned here; a row of a function with no frame is an error)
        raise SystemExit('readable_locals.tsv: no frame for %s' % sorted(locals_of))
    sbody = ''.join('    %s,\n' % ', '.join(saves[i:i + 8]) for i in range(0, len(saves), 8))
    cbody = ''.join('    {%du, 0x%08xu, 0x%08xu, 0x%08xu},\n' % c for c in sorted(set(calls)))
    return ('// Generated by tools/fncheck_traced_gen.py: each scanned function\'s stack frame from its\n'
            '// prologue (size, register saves; %d functions, %d saves), for the readable functions\'\n'
            '// frames (ee/abi.h, ee::frameLayout), and each function\'s call targets with the return\n'
            '// address of the one site that calls it, 0 for several (%d; ee::siteReturn). Built from\n'
            '// generated code: never committed.\n'
            '#include "ee/abi.h"\n\n'
            'namespace\n{\n'
            '    const ee::EntrySave kSaves[] = {\n%s    {0, 0, 0}};\n'
            '    const uint16_t kLocals[] = {%s0};\n'
            '    const ee::FrameRow kRows[] = {\n%s    {0u, 0u, 0u, 0u, 0u, 0u, 0u}};\n'
            '    const ee::FrameTableRegistration g_frames{kRows, %du, kSaves, kLocals};\n'
            '    const ee::CallSite kCallSites[] = {\n%s    {0u, 0u, 0u, 0u}};\n'
            '    const ee::CallSiteRegistration g_callSites{kCallSites, %du};\n}\n'
            % (len(rows), len(saves), len(set(calls)), sbody, ''.join('0x%x, ' % p for p in places), ''.join(rows), len(rows), cbody, len(set(calls))))


def main():
    if sys.argv[1:] == ['--self-test']:
        return self_test()
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--list', required=True)
    p.add_argument('--stack-args', required=True)
    p.add_argument('--scan', action='append', default=[], help='GAME=DIR: a generated folder to scan')
    p.add_argument('--frames', help='OUT.cpp: the frame table (every scanned function\'s frame size and register saves)')
    a = p.parse_args()
    changed = total = blocks = 0
    marks = []
    table_jumps = []
    chunked = {}  # {game: {address: [generated files]}}: functions of several entry_ chunks
    framed = {}   # {game: {address}}: registered functions of one entry_ chunk (their frame rows)
    for line in open(a.list, encoding='utf-8'):
        line = line.rstrip('\n')
        if not line or line.startswith('#'):
            continue
        game, addr, gen, out, *rest = line.split('\t')
        chunks = rest[0].split('|') if rest and rest[0] else []
        if chunks:
            chunked.setdefault(int(game), {})[int(addr, 16)] = [gen] + chunks
        elif FUNC_FILE.search(os.path.basename(gen)) and os.path.basename(gen).startswith('entry_'):
            # A registered function that is one entry_ chunk (BMX updateEditObjectMatrix, 0x214BF8, which the
            # functions list ran on from setEditObjectPosition): its frame row from that chunk (frame1).
            framed.setdefault(int(game), set()).add(int(addr, 16))
        marks += safe_point_marks(int(game), int(addr, 16), gen, chunks)
        text, n, tj = instrument(gen, int(game), int(addr, 16), chunks)
        table_jumps += tj
        changed += write_if_changed(out, text)
        total += 1
        blocks += n
    rows, readers, arg_rows, sites, frames, calls, vu0_rows, untraced = [], set(), [], [], [], [], [], set()
    for item in a.scan:
        game, folder = item.split('=', 1)
        flows, vu0_flows = {}, {}
        rows += scan(int(game), folder, readers, flows, sites, frames, calls, vu0_flows, chunked.get(int(game)), untraced,
                     framed.get(int(game)))
        # A function of several chunks runs the runtime's code if one of its chunks does.
        for start, files in (chunked.get(int(game)) or {}).items():
            for f in files[1:]:
                fm = FUNC_FILE.search(os.path.basename(f))
                if fm and (int(game), int(fm.group(1), 16)) in untraced:
                    untraced.add((int(game), start))
        lanes_read, lanes_after, lanes_indirect = vu0_lanes(vu0_flows)
        vu0_rows.append((int(game), 0, 0, lanes_indirect))  # function 0: any function's return
        vu0_rows += [(int(game), f, lanes_read[f], lanes_after[f]) for f in sorted(vu0_flows) if lanes_read[f] or lanes_after[f]]
        # (game 0, Aggressive Inline: the tables are its; another game's guest keeps the upper bound)
        indirect, dead = (load_indirect(), load_dead()) if int(game) == 0 else (None, load_dead(DEAD_READS_OTHER[int(game)]))
        reads = arg_readers(flows, 0, indirect, dead)
        handed = arg_readers(flows, CALLER_SAVED_ARGS, indirect, dead)
        arg_rows += [(int(game), f, r, handed[f]) for f, r in sorted(reads.items()) if r or handed[f]]
    result_rows = sorted((g, f, m[0], m[1]) for (g, f), m in result_readers(sites).items() if m[0] or m[1])
    rbody = ''.join('    {%du, 0x%08xu, 0x%08xu, 0x%05xu},\n' % r for r in result_rows)
    mbody = ''.join('    {%du, 0x%08xu, 0x%08xu, %du},\n' % r for r in sorted(set(marks)))
    body = ''.join('    {%du, 0x%08xu, %du, %du},\n' % r for r in sorted(set(rows)))
    fbody = ''.join('    {%du, 0x%08xu},\n' % r for r in sorted(readers))
    abody = ''.join('    {%du, 0x%08xu, 0x%04xu, 0x%04xu},\n' % r for r in arg_rows)
    jbody = ''.join('    {%du, 0x%08xu, 0x%08xu},\n' % r for r in sorted(set(table_jumps)))
    ubody = ''.join('    {%du, 0x%08xu},\n' % r for r in sorted(untraced))
    vbody = ''.join('    {%du, 0x%08xu, {0x%016xull, 0x%016xull}, {0x%016xull, 0x%016xull}},\n' % (
        g, f, r & (2 ** 64 - 1), r >> 64, t & (2 ** 64 - 1), t >> 64) for g, f, r, t in vu0_rows)
    text = ('// Generated by tools/fncheck_traced_gen.py: the functions that read stack arguments\n'
            '// (%d) and those a caller reads f0 after (%d; fncheck_build.h, fncheck_tables.h).\n'
            '#include "ee/fncheck_tables.h"\n#include "ee/fncheck_jumps.h"\n\n'
            'namespace fncheck\n{\n'
            '    const StackArgs g_stackArgs[] = {\n%s        {0u, 0u, 0u, 0u}};\n'
            '    const size_t g_stackArgCount = %d;\n'
            '    const F0Reader g_f0Readers[] = {\n%s        {0u, 0u}};\n'
            '    const size_t g_f0ReaderCount = %d;\n'
            '    const ArgReader g_argReaders[] = {\n%s        {0u, 0u, 0u, 0u}};\n'
            '    const size_t g_argReaderCount = %d;\n'
            '    const ResultReader g_resultReaders[] = {\n%s        {0u, 0u, 0u, 0u}};\n'
            '    const size_t g_resultReaderCount = %d;\n'
            '    const SafePointMark g_safePointMarks[] = {\n%s        {0u, 0u, 0u, 0u}};\n'
            '    const size_t g_safePointMarkCount = %d;\n'
            '    const TableJump g_tableJumps[] = {\n%s        {0u, 0u, 0u}};\n'
            '    const size_t g_tableJumpCount = %d;\n'
            '    const Vu0Lanes g_vu0Lanes[] = {\n%s        {0u, 0u, {0ull, 0ull}, {0ull, 0ull}}};\n'
            '    const size_t g_vu0LanesCount = %d;\n'
            '    const UntracedCode g_untracedCode[] = {\n%s        {0u, 0u}};\n'
            '    const size_t g_untracedCodeCount = %d;\n}\n'
            % (len(rows), len(readers), body, len(rows), fbody, len(readers), abody, len(arg_rows), rbody,
               len(result_rows), mbody, len(set(marks)), jbody, len(set(table_jumps)), vbody, len(vu0_rows),
               ubody, len(untraced)))
    write_if_changed(a.stack_args, text)
    if a.frames:
        write_if_changed(a.frames, frame_table(frames, calls))
    print('fncheck_traced_gen: %d traced originals (%d rewritten), %d blocks; %d functions read stack arguments, '
          '%d have a caller that reads f0 after the call, %d read argument registers, %d have callers that read '
          'registers they leave'
          % (total, changed, blocks, len(rows), len(readers), len(arg_rows), len(result_rows)))


if __name__ == '__main__':
    main()
