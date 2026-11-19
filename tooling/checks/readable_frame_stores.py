#!/usr/bin/env python3
"""The slots each function of the game stores in its own stack frame in the original and never reads before storing,
on any path, from the matching decomp's assembly: readable_frame_stores.tsv, read by PS2X_STACK_POISON_STORES and
PS2X_STACK_POISON_FRAMES (ee/abi.cpp, `career_ab.py stale`).

    readable_frame_stores.py [--asm DIR] [--out FILE] [--why ADDRESS]

A row is `ADDRESS<TAB>FRAME SIZE<TAB>OFFSET+BYTES,...`, one per function with a frame (the slots may be none).

The fill writes these slots at the function's entry, on both sides of the A/B. A slot is listed only if the function
stores it on some path and, on every path, never reads it before it has stored it there: so the fill never changes
what the original itself reads, and it shows a readable function that drops the store (the fill survives it, where
the original's value would be). A slot the function reads stale on some path is not listed: filling it would hide
the value an earlier function left there, the very chain a dropped store breaks (a later function that reads a slot
of its own frame stale on one path and stores it on another, bonePoint, drawSparks; and setupFireTextureEffect_'s
withObjects, which the original reads stale on its scroller path).

The analysis is per path: a control-flow graph of the function (branches and their delay slots, likely branches,
calls), and at each instruction the bytes of the frame stored on every path to it. Stores and loads count through sp
and through a register holding an address in the frame (addiu rX, sp, N; a move of one; forgotten when written and,
for the registers a call may change, after a call). Where an address in the frame leaves the function's sight
(passed to a callee, stored to memory, used in any other way), every byte at or above it not yet stored on every
path counts as read there: whoever holds it may read it. Not counted as stores: what a callee stores through a
pointer into the frame (a transpose fills a matrix but for its w lanes, and those lanes are reads this table must not
touch). A function whose stack pointer moves other than by its own frame is listed with no slots.
"""
import argparse
import glob
import os
import sys
import re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ztree  # noqa: E402  (the z-axis tree checked: ztree.py)
import zaxis_paths  # noqa: E402  (tooling/paths.env: the workspaces, the data, the work database)

STORES = {'sb': 1, 'sh': 2, 'sw': 4, 'sd': 8, 'sq': 16, 'swc1': 4, 'sqc2': 16, 'swl': 4, 'swr': 4, 'sdl': 8, 'sdr': 8}
LOADS = {'lb': 1, 'lbu': 1, 'lh': 2, 'lhu': 2, 'lw': 4, 'lwu': 4, 'ld': 8, 'lq': 16, 'lwc1': 4, 'lqc2': 16,
         'lwl': 4, 'lwr': 4, 'ldl': 8, 'ldr': 8}
UNALIGNED = {'swl', 'swr', 'sdl', 'sdr', 'lwl', 'lwr', 'ldl', 'ldr'}
CONDITIONAL = {'beq', 'bne', 'beqz', 'bnez', 'bltz', 'blez', 'bgez', 'bgtz', 'bc0f', 'bc0t', 'bc1f', 'bc1t', 'bc2f', 'bc2t'}
LIKELY = {'beql', 'bnel', 'beqzl', 'bnezl', 'bltzl', 'blezl', 'bgezl', 'bgtzl', 'bc0fl', 'bc0tl', 'bc1fl', 'bc1tl',
          'bc2fl', 'bc2tl'}
CALLS = {'jal', 'jalr'}
CALL_CLOBBERS = {'$v0', '$v1', '$a0', '$a1', '$a2', '$a3', '$t0', '$t1', '$t2', '$t3', '$t4', '$t5', '$t6', '$t7', '$t8', '$t9', '$at', '$ra'}
ARGUMENTS = ('$a0', '$a1', '$a2', '$a3', '$t0', '$t1', '$t2', '$t3')   # EE's EABI: eight argument registers
# Instructions whose first operand is read, not written.
FIRST_READ = re.compile(r'^(mt.*|ctc.*|qmtc2.*|mult.*|div.*|madd.*|msub.*|teq|tne|tge.*|tlt.*)$')
NUMBER = re.compile(r'-?(?:0x[0-9A-Fa-f]+|\d+)')
MEMORY = re.compile(r'(-?(?:0x[0-9A-Fa-f]+|\d+))\((\$\w+)\)')


def number(text):
    return int(text, 0)


def functions(folder):
    """{address: ([(mnemonic, operands)], {label: index})} of the assembly files."""
    code = {}
    for path in sorted(glob.glob(os.path.join(folder, '*.s'))):
        current = None
        for line in open(path, errors='replace'):
            found = re.match(r'glabel func_([0-9A-Fa-f]{8})', line)
            if found:
                current = code.setdefault(int(found.group(1), 16), ([], {}))
                continue
            if line.startswith('endlabel'):
                current = None
                continue
            if current is None:
                continue
            text = re.sub(r'/\*.*?\*/', '', line).split('#', 1)[0].strip()
            label = re.fullmatch(r'(\.?L[0-9A-Fa-f_]\w*):', text) or re.fullmatch(r'jlabel\s+(\S+)', text)
            if label:   # (a branch's label, or a jump table's)
                current[1][label.group(1)] = len(current[0])
                continue
            parts = text.split(None, 1)
            if parts and not parts[0].endswith(':') and not parts[0].startswith('.'):
                current[0].append((parts[0], [o.strip() for o in parts[1].split(',')] if len(parts) > 1 else []))
    return code


def span(offset, bytes_):
    return ((1 << bytes_) - 1) << offset


class Function:
    """One function's frame, its control flow, and the stores and reads in it."""

    def __init__(self, insns, labels, arguments_read=None):
        self.insns = insns
        self.labels = labels
        self.arguments_read = arguments_read or {}
        self.every_path = False   # --every-path: also stored on every path to the function's return
        self.size = 0
        self.unknown_sp = False
        for mnemonic, ops in insns:
            if ops[:1] == ['$sp']:
                if mnemonic in ('addiu', 'daddiu') and ops[1:2] == ['$sp'] and NUMBER.fullmatch(ops[2]):
                    if number(ops[2]) < 0:
                        if self.size:
                            self.unknown_sp = True
                        self.size = -number(ops[2])
                elif not (mnemonic in ('daddu', 'addu', 'or') and '$zero' in ops[1:]):
                    self.unknown_sp = True   # (the stack set up from nothing, or moved by a computed amount)
        self.full = span(0, self.size)
        self.successors, self.call_after = self.flow()

    def call_reads(self, slot):
        """The argument registers the call after delay slot `slot` may read: its callee's, if known."""
        mnemonic, ops = self.call_after[slot]
        found = re.fullmatch(r'func_([0-9A-Fa-f]{8})', ops[-1]) if mnemonic in ('jal', 'j') and ops else None
        callee = self.arguments_read.get(int(found.group(1), 16)) if found else None
        return ARGUMENTS if callee is None else callee

    def target(self, ops):
        return self.labels.get(ops[-1]) if ops else None

    def flow(self):
        """successors[i]: the instructions after i; call_after[i]: the call made after delay slot i."""
        n = len(self.insns)
        successors = [[i + 1] if i + 1 < n else [] for i in range(n)]
        call_after = {}
        every_label = sorted(set(self.labels.values()))
        for i, (mnemonic, ops) in enumerate(self.insns):
            if i + 1 >= n:
                break
            slot, after = i + 1, [i + 2] if i + 2 < n else []
            target = self.target(ops)
            to = [target] if target is not None else []
            if mnemonic == 'b' or (mnemonic == 'beq' and ops[:2] == ['$zero', '$zero']):
                successors[i], successors[slot] = [slot], to
            elif mnemonic in CONDITIONAL:
                successors[i], successors[slot] = [slot], to + after
            elif mnemonic in LIKELY:
                successors[i], successors[slot] = [slot] + after, to
            elif mnemonic == 'j':
                successors[i], successors[slot] = [slot], to   # a tail call leaves the function
                call_after[slot] = (mnemonic, ops)
            elif mnemonic == 'jr':
                successors[i] = [slot]
                successors[slot] = [] if ops == ['$ra'] else every_label   # a jump table may go to any label
                if ops != ['$ra']:
                    call_after[slot] = (mnemonic, ops)
            elif mnemonic in CALLS:
                successors[i] = [slot]
                call_after[slot] = (mnemonic, ops)
        return successors, call_after

    def base(self, register, held):
        """The frame offset a register holds, or None. (held also keeps, under an int key, the frame offsets
        stored in the frame's own slots.)"""
        if register == '$sp':
            return 0
        return held.get(register) if register.startswith('$') else None

    def moved(self, ops, held):
        """The frame offset a move (addu rY, rX, zero) copies, or None."""
        if len(ops) != 3 or '$zero' not in ops[1:]:
            return None
        source = ops[2] if ops[1] == '$zero' else ops[1]
        return self.base(source, held)

    def step(self, i, written, held, events):
        """The state after instruction i (and the call after it, if any); events gets (kind, mask) of what it reads,
        stores or lets out of sight."""
        held = dict(held)
        mnemonic, ops = self.insns[i]
        if mnemonic in STORES or mnemonic in LOADS:
            found = MEMORY.fullmatch(ops[1]) if len(ops) == 2 else None
            at = self.base(found.group(2), held) if found else None
            value = self.base(ops[0], held) if mnemonic in STORES and ops else None
            kept = None   # the frame address a load takes back from a slot of the frame
            if at is not None:
                offset = number(found.group(1)) + at
                bytes_ = STORES.get(mnemonic) or LOADS[mnemonic]
                if mnemonic in UNALIGNED:
                    offset -= offset % bytes_
                elif mnemonic in ('sq', 'lq', 'sqc2', 'lqc2'):
                    offset -= offset % 16
                mask = span(offset, bytes_) & self.full if offset >= 0 else 0   # (below sp: not the frame)
                if mnemonic in STORES:
                    events.append(('store', mask))
                    written |= mask
                    for key in [k for k in held if isinstance(k, int) and k < offset + bytes_ and offset < k + 8]:
                        del held[key]
                    if value is not None and mask and mnemonic in ('sw', 'sd', 'sq'):
                        held[offset] = value   # an address in the frame kept in the frame: still in sight
                        value = None
                else:
                    events.append(('read', mask))
                    if mnemonic in ('lw', 'lwu', 'ld', 'lq'):
                        kept = held.get(offset)
            if value is not None:
                events.append(('escape', value))   # an address in the frame stored to memory
            if mnemonic in LOADS and ops:
                held.pop(ops[0], None)
                if kept is not None:
                    held[ops[0]] = kept
        elif mnemonic in ('addiu', 'daddiu') and len(ops) == 3 and ops[0] != '$sp' and self.base(ops[1], held) is not None:
            if NUMBER.fullmatch(ops[2]):
                held[ops[0]] = self.base(ops[1], held) + number(ops[2])
            else:   # (a symbol's low half the disassembler read into a frame offset)
                events.append(('escape', self.base(ops[1], held)))
                held.pop(ops[0], None)
        elif mnemonic in ('addu', 'daddu', 'or') and self.moved(ops, held) is not None:
            if ops[0] != '$sp':
                held[ops[0]] = self.moved(ops, held)
        elif mnemonic in ('addiu', 'daddiu') and ops[:2] == ['$sp', '$sp']:
            pass
        elif mnemonic in CONDITIONAL or mnemonic in LIKELY or mnemonic in ('b', 'j', 'jr', 'jal', 'jalr', 'nop'):
            pass   # (comparing an address reads nothing; calls are made after their delay slot)
        else:
            sources = ops if FIRST_READ.match(mnemonic) else ops[1:]
            for operand in sources:
                at = self.base(operand, held)
                if at is not None:
                    events.append(('escape', at))
            if ops and not FIRST_READ.match(mnemonic):
                held.pop(ops[0], None)
        if i in self.call_after:
            for register in self.call_reads(i):
                if register in held:
                    events.append(('escape', held[register]))
            held = {r: o for r, o in held.items() if r not in CALL_CLOBBERS}
        return written, held

    def argument_liveness(self):
        """The argument registers live at entry (backward, over the control flow and the calls)."""
        n = len(self.insns)
        if not n:
            return frozenset()
        args = frozenset(ARGUMENTS)
        live = [frozenset()] * n
        changed = True
        while changed:
            changed = False
            for i in range(n - 1, -1, -1):
                out = frozenset().union(*[live[s] for s in self.successors[i]]) if self.successors[i] else frozenset()
                if i in self.call_after:   # the call made after this delay slot
                    mnemonic, ops = self.call_after[i]
                    if mnemonic in ('jal', 'jalr'):
                        out = out - CALL_CLOBBERS
                    out = out | frozenset(self.call_reads(i))
                read, written = registers_of(*self.insns[i])
                new = ((out - written) | read) & args
                if new != live[i]:
                    live[i] = new
                    changed = True
        return live[0]

    def analyse(self):
        """(frame size, [(offset, bytes)] merged and sorted, {byte: why}) of the function."""
        if not self.size or self.unknown_sp:
            return self.size, [], {}
        n = len(self.insns)
        states = [None] * n   # (bytes stored on every path to here, held registers) at each instruction's entry
        states[0] = (0, {})
        work = [0]
        while work:
            i = work.pop()
            written, held = self.step(i, states[i][0], states[i][1], [])
            for s in self.successors[i]:
                old = states[s]
                if old is None:
                    states[s] = (written, held)
                    work.append(s)
                    continue
                merged = (old[0] & written, {r: o for r, o in old[1].items() if held.get(r) == o})
                if merged[0] != old[0] or len(merged[1]) != len(old[1]):
                    states[s] = merged
                    work.append(s)
        stored = 0
        stale = 0
        left = self.full
        why = {}
        for i in range(n):
            if states[i] is None:
                continue
            events = []
            after, _ = self.step(i, states[i][0], states[i][1], events)
            before = states[i][0]
            if self.every_path and not self.successors[i]:
                left &= after   # a return (or a tail call): what every path to it has stored
            for kind, value in events:
                if kind == 'store':
                    stored |= value
                    continue
                if kind == 'read':
                    mask = value
                else:   # whoever holds the address may read anything from it up
                    mask = self.full & ~span(0, max(0, value))
                mask &= ~before
                if mask & ~stale:
                    why.setdefault(i, (kind, mask & ~stale))
                stale |= mask
        runs = []
        listed = stored & ~stale & left
        offset = 0
        while listed >> offset:
            if (listed >> offset) & 1:
                start = offset
                while (listed >> offset) & 1:
                    offset += 1
                runs.append([start, offset - start])
            else:
                offset += 1
        return self.size, runs, why


# Which argument registers a function reads before writing them (itself, or a callee it passes them on to).
PASSES_FIRST = re.compile(r'^(movn|movz|lwl|lwr|ldl|ldr)$')   # (their target keeps part of its old value: read too)


def registers_of(mnemonic, ops):
    """(read, written) registers of an instruction, read erring on the side of more."""
    regs = [o for o in ops if o.startswith('$')]
    found = [MEMORY.fullmatch(o) for o in ops]
    bases = [f.group(2) for f in found if f]
    if mnemonic in STORES or mnemonic in CONDITIONAL or mnemonic in LIKELY or mnemonic in ('jr', 'jalr') \
            or FIRST_READ.match(mnemonic):
        return set(regs) | set(bases), set()
    if not ops:
        return set(), set()
    read = set(regs[1:] if regs[:1] == [ops[0]] else regs) | set(bases)
    if PASSES_FIRST.match(mnemonic):
        read.add(ops[0])
    return read, {ops[0]} if ops[0].startswith('$') else set()


def arguments_read(code):
    """{address: the argument registers it may read as it receives them}, by liveness to a fixed point over the
    whole program (a call reads its callee's; a call through a register, or to a function not in the assembly,
    every argument register)."""
    result = {address: frozenset() for address in code}
    flows = {address: Function(insns, labels) for address, (insns, labels) in code.items()}
    changed = True
    while changed:
        changed = False
        for address, function in flows.items():
            function.arguments_read = result
            live = function.argument_liveness()
            if live != result[address]:
                result[address] = live
                changed = True
    return result


def frame_stores(insns, labels=None, arguments=None, every_path=False):
    """(frame size, [(offset, bytes)] merged and sorted) of one function."""
    function = Function(insns, labels or {}, arguments)
    function.every_path = every_path
    size, runs, _ = function.analyse()
    return size, runs


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--asm', default=os.path.join(zaxis_paths.get('AI_WORKSPACE', required=False) or '', 'decomp', 'asm', 'cod'))
    parser.add_argument('--out', default=os.path.join(here, 'readable_frame_stores.tsv'))
    parser.add_argument('--every-path', action='store_true', help='list only slots also stored on every path to the '
                        'return: the fill is then never seen by the original at all, and no fill writes over a value '
                        'left for a later reader by an earlier function')
    parser.add_argument('--why', help='a function\'s address: the reads and escapes that keep its stored slots out')
    args = parser.parse_args()
    code = functions(args.asm)
    if args.why:
        insns, labels = code[int(args.why, 16)]
        function = Function(insns, labels, arguments_read(code))
        function.every_path = args.every_path
        size, runs, why = function.analyse()
        print('frame %d, listed %s' % (size, ','.join('%d+%d' % tuple(r) for r in runs) or 'none'))
        for i, (kind, mask) in sorted(why.items()):
            bytes_ = [b for b in range(size) if (mask >> b) & 1]
            print('  insn %d %-32s %s bytes %d..%d (%d)' % (i, ' '.join([function.insns[i][0]] + function.insns[i][1]),
                                                           kind, bytes_[0], bytes_[-1], len(bytes_)))
        return
    rows = []
    arguments = arguments_read(code)
    for address in sorted(code):
        size, runs = frame_stores(*code[address], arguments, args.every_path)
        if size:
            rows.append('0x%08X\t%d\t%s\n' % (address, size, ','.join('%d+%d' % tuple(r) for r in runs)))
    with open(args.out, 'w') as f:
        f.write('# readable_frame_stores.py: function, the size of its frame and the slots of it it stores in the '
                'original and never reads before storing, on any path (OFFSET+BYTES)\n')
        f.writelines(rows)
    print('%d of %d functions have a frame, %d slots stored' % (len(rows), len(code), sum(r.count('+') for r in rows)))


if __name__ == '__main__':
    main()
