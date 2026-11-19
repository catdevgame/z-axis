#!/usr/bin/env python3
"""The held table's programs as code (readable_held_gen.py's render; ee/held.h Value::program, ee/held_program.h).

The runtime works a held value out from its nodes (postfix, ee/held.h Op) with an interpreter (ee/held.cpp run). This
writes each value's program of more than one node as a C++ function instead: the same nodes in the same order, each
one a call of the helper the interpreter calls for it (ee/held_program.h), with the interpreter's stack as named
locals, a select (kSkipIf, kSkip) as an if, a loop program (kLoop) as a loop. So the function gives the value the
nodes give, is not known where they are not, and reads memory where they do (a load that fails at the same node).
Where the interpreter fails whatever the values (a stack too shallow or too deep, a malformed loop, a node it does
not know), the function returns false at that node. A program whose shape is not one of these (the arms of a select
leaving different depths, a kSkip not ending an arm, a kCarried past the loop's values) is left to the interpreter:
no function, Value::program 0.

Identical programs share one function. The functions go into CHUNKS sources (compiled in parallel), always all of
them (empty past the last), and the table's source declares them and lists them (kPrograms).

    readable_held_code.py --self-test
"""
import re
import sys

OP_CONST, OP_ENTRY, OP_PREV, OP_RESULT, OP_ARG, OP_FARG, OP_TARGET, OP_LOAD, OP_ALU, OP_ENTRY_LOAD, OP_RETURN_LOAD, \
    OP_SKIP_IF, OP_SKIP, OP_LOOP, OP_CARRIED, OP_LEAF, OP_LEAF_END = range(1, 18)
DEPTH = 32           # ee/held.cpp kDepth
CARRIED_VALUES = 16  # ee/held.h kCarriedValues
LOOP_DEPTH = 2       # loops in loops the interpreter runs (Scope::loopDepth)
CHUNKS = 16
# ee/held.h AluOp: kAluCount, and the forms of one and three operands (held_program.h aluOperands)
ALU_COUNT = 58
K_MOVN, K_MOVZ, K_ADDIU, K_DSRA32, K_NEGS, K_MFC1 = 16, 17, 18, 33, 40, 46
LEAF_CALLS = {OP_ENTRY: 'entry', OP_PREV: 'previous', OP_RESULT: 'result', OP_ARG: 'argument',
              OP_FARG: 'floatArgument', OP_ENTRY_LOAD: 'entryLoad', OP_RETURN_LOAD: 'returnLoad'}


def alu_operands(op):
    if op in (K_MOVN, K_MOVZ):
        return 3
    if K_ADDIU <= op <= K_DSRA32 or K_NEGS <= op <= K_MFC1:
        return 1
    return 2


class Unsupported(Exception):
    """A program shape left to the interpreter."""


DEAD = None  # (every path through the code emitted so far has returned false)


class Emitter:
    def __init__(self):
        self.lines = []

    def put(self, indent, text):
        self.lines.append('    ' * indent + text)


def i32(v):
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v >> 31 else v


def imm_literal(v):
    v = i32(v)
    return '%d' % v if v != -(1 << 31) else '(-0x7fffffff - 1)'


def run_code(nodes, lo, hi, prefix, loop, out, indent):
    """nodes[lo:hi] as one run() from an empty stack (its own count: hi - lo). Returns the depth left, or DEAD.
    loop: None outside a loop program, else (level, carried count)."""
    used = [0]
    body = Emitter()
    depth = seq_code(nodes, lo, hi, hi, 0, prefix, loop, body, indent, used)
    if used[0]:
        out.put(indent, 'uint64_t %s;' % ', '.join('%s%d' % (prefix, k) for k in range(used[0])))
    out.lines += body.lines
    return depth


def seq_code(nodes, i, end, count_end, depth, prefix, loop, out, indent, used):
    """The nodes from i to end of a run whose nodes end at count_end (the interpreter's bounds), the stack `depth`
    deep. Returns the depth after them, or DEAD."""
    def name(k):
        used[0] = max(used[0], k + 1)
        return '%s%d' % (prefix, k)

    def fail():
        out.put(indent, 'return false;')
        return DEAD

    while i < end:
        op, sub, imm = nodes[i]
        if op == OP_LOOP:
            carried, length = sub, i32(imm)
            if carried == 0 or carried > CARRIED_VALUES or depth < carried or length < 1 or i + length >= count_end or \
                    (loop[0] if loop else 0) >= LOOP_DEPTH:
                return fail()
            if i + 1 + length > end:
                raise Unsupported('a loop past its arm')
            loop_code(nodes, i + 1, length, carried, depth, prefix, (loop[0] if loop else 0) + 1, out, indent, used)
            depth = depth - carried + 1
            name(depth - 1)
            i += 1 + length
            continue
        if op in (OP_LEAF, OP_LEAF_END):
            return fail()
        if op == OP_LOAD:
            if depth < 1:
                return fail()
            top = name(depth - 1)
            out.put(indent, 'if (!load(c, %du, static_cast<uint32_t>(%s) + %du, %s)) return false;'
                    % (sub, top, imm & 0xFFFFFFFF, top))
        elif op == OP_SKIP_IF:
            a = i32(imm)
            if depth < 1 or a < 1 or i + a >= count_end:
                return fail()
            k = i + a
            if k >= end or nodes[k][0] != OP_SKIP:
                raise Unsupported('a select without its kSkip')
            b = i32(nodes[k][2])
            if b < 0 or k + b >= count_end or k + 1 + b > end:
                raise Unsupported('a select arm past its bounds')
            condition = name(depth - 1)
            out.put(indent, 'if (%s %s 0u)' % (condition, '!=' if sub == 0 else '=='))
            out.put(indent, '{')
            taken = seq_code(nodes, i + 1, k, count_end, depth - 1, prefix, loop, out, indent + 1, used)
            out.put(indent, '}')
            out.put(indent, 'else')
            out.put(indent, '{')
            other = seq_code(nodes, k + 1, k + 1 + b, count_end, depth - 1, prefix, loop, out, indent + 1, used)
            out.put(indent, '}')
            if taken is DEAD and other is DEAD:
                return DEAD
            if taken is not DEAD and other is not DEAD and taken != other:
                raise Unsupported('a select whose arms leave different depths')
            depth = other if taken is DEAD else taken
            i = k + 1 + b
            continue
        elif op == OP_SKIP:
            raise Unsupported('a kSkip outside a select')
        elif op == OP_ALU:
            k = alu_operands(sub)
            if depth < k or sub >= ALU_COUNT:
                return fail()
            ins = [name(depth - k + j) for j in range(k)] + ['0u'] * (3 - k)
            depth -= k
            out.put(indent, 'if (!alu(%du, %s, %s, %s, %s, %s)) return false;'
                    % (sub, imm_literal(imm), ins[0], ins[1], ins[2], name(depth)))
            depth += 1
        elif op in LEAF_CALLS or op in (OP_CONST, OP_TARGET, OP_CARRIED):
            # (a leaf: its value first, then the push, as the interpreter; a leaf has no effect to keep)
            if op == OP_CARRIED:
                if not loop or sub >= CARRIED_VALUES:
                    return fail()
                if sub >= loop[1]:
                    raise Unsupported('a kCarried past the loop\'s values')
            if depth >= DEPTH:
                return fail()
            top = name(depth)
            if op == OP_CONST:
                out.put(indent, '%s = constant(%s);' % (top, imm_literal(imm)))
            elif op == OP_TARGET:
                out.put(indent, 'target(c, %s);' % top)
            elif op == OP_CARRIED:
                out.put(indent, '%s = k%d_%d;' % (top, loop[0], sub))
            else:
                out.put(indent, 'if (!%s(c, %d, %s)) return false;' % (LEAF_CALLS[op], sub, top))
            depth += 1
        else:
            return fail()
        i += 1
    return depth


def loop_code(nodes, first, length, carried, depth, prefix, level, out, indent, used):
    """The kLoop whose program is nodes[first:first + length] (ee/held.cpp runLoop), its carried values the top
    `carried` of the stack (`depth` deep), its result left in their first's place."""
    stack = ['%s%d' % (prefix, depth - carried + j) for j in range(carried)]
    out.put(indent, '{')
    out.put(indent + 1, 'uint64_t %s;' % ', '.join('k%d_%d = %s' % (level, j, stack[j]) for j in range(carried)))
    out.put(indent + 1, 'for (uint32_t i%d = 0;; ++i%d)' % (level, level))
    out.put(indent + 1, '{')
    inner = indent + 2
    out.put(inner, 'if (i%d == kLoopIterations) return false;' % level)
    j = 0
    while True:
        if j >= length:
            out.put(inner, 'return false; // (no leaf\'s path)')
            break
        op, kind, condition_length = nodes[first + j]
        condition_length = i32(condition_length)
        if op != OP_LEAF or condition_length < 1 or j + 1 + condition_length >= length or \
                nodes[first + j + 1 + condition_length][0] != OP_LEAF_END:
            out.put(inner, 'return false; // (a malformed leaf)')
            break
        values_length = i32(nodes[first + j + 1 + condition_length][2])
        if values_length < 0:
            raise Unsupported('a leaf of negative length')
        following = j + 2 + condition_length + values_length
        if following > length:
            out.put(inner, 'return false; // (a malformed leaf)')
            break
        out.put(inner, '{')
        cond_at = first + j + 1
        got = run_code(nodes, cond_at, cond_at + condition_length, 'c%d_' % level, (level, carried), out, inner + 1)
        if got is DEAD:
            out.put(inner, '}')
            break  # (every path returned: the leaves after it are not reached)
        if got != 1:
            out.put(inner + 1, 'return false;')
            out.put(inner, '}')
            break
        out.put(inner + 1, 'if (c%d_0 != 0u)' % level)
        out.put(inner + 1, '{')
        values_at = cond_at + condition_length + 1
        given = run_code(nodes, values_at, values_at + values_length, 'w%d_' % level, (level, carried), out, inner + 2)
        if given is not DEAD:
            if kind == 1:
                if given != 1:
                    out.put(inner + 2, 'return false;')
                else:
                    out.put(inner + 2, '%s = w%d_0;' % (stack[0], level))
                    out.put(inner + 2, 'break;')
            elif given != carried:
                out.put(inner + 2, 'return false;')
            else:
                for k in range(carried):
                    out.put(inner + 2, 'k%d_%d = w%d_%d;' % (level, k, level, k))
                out.put(inner + 2, 'continue;')
        out.put(inner + 1, '}')
        out.put(inner, '}')
        j = following
    out.put(indent + 1, '}')
    out.put(indent, '}')


def program_code(nodes, name):
    """The C++ function `name` of a whole program (ee/held.cpp evaluate), or None if left to the interpreter."""
    out = Emitter()
    try:
        depth = run_code(nodes, 0, len(nodes), 's', None, out, 1)
    except Unsupported:
        return None
    lines = ['bool %s(const Context &c, uint64_t &out)' % name, '{'] + out.lines
    if depth is not DEAD:
        if depth == 1:
            lines += ['    out = s0;', '    return true;']
        else:
            lines.append('    return false;')
    lines.append('}')
    return '\n'.join(lines) + '\n'


def programs_of(nodes, values):
    """Program numbers for `values` ((slot, count, first) rows; 0: none) and the distinct programs' code, in order
    of number (from 1)."""
    numbers, codes, numbered = [], [], {}
    for _, count, first in values:
        if count < 2:
            numbers.append(0)
            continue
        key = tuple(nodes[first:first + count])
        number = numbered.get(key)
        if number is None:
            code = program_code(key, 'p%d' % (len(codes) + 1))
            number = 0 if code is None else len(codes) + 1
            if code is not None:
                codes.append(code)
            numbered[key] = number
        numbers.append(number)
    return numbers, codes


SLOT_MASK = 0x7F  # (Value::slot less kChecked)
CHECKED = 0x80


def value_call(nodes, count, first, number, ctx, var):
    """The C++ condition that works one table value out into `var` (true: known), as ee/held.cpp evaluateValue
    does: a leaf alone in place, a program as its code. 'false' where the interpreter can only fail (no nodes, one
    node that is no leaf); None where it is left to the interpreter (a program with no code)."""
    if count == 0:
        return 'false'
    if count == 1:
        op, sub, imm = nodes[first]
        if op == OP_CONST:
            return '(%s = constant(%s), true)' % (var, imm_literal(imm))
        if op == OP_TARGET:
            return 'target(%s, %s)' % (ctx, var)
        if op in LEAF_CALLS:
            return '%s(%s, %d, %s)' % (LEAF_CALLS[op], ctx, sub, var)
        return 'false'  # (kCarried outside a loop, or a node that needs a stack)
    if not number:
        return None
    return 'p%d(%s, %s)' % (number, ctx, var)


def case_values_code(name, rows, nodes):
    """A case's values (ee/held.cpp caseValues): each one worked out into w.value and w.known, or w.cleared; a
    kChecked one only when `confirmed`. rows: (slot, count, first, program). None: left to the interpreter."""
    lines, slots = [], set()
    for slot, count, first, number in rows:
        bit = slot & SLOT_MASK
        if bit in slots or bit >= 32:
            return None  # (a slot twice: the interpreter's order decides)
        slots.add(bit)
        call = value_call(nodes, count, first, number, 'c', 'x')
        if call is None:
            return None
        if slot & CHECKED:
            call = 'confirmed && ' + call if call != 'false' else 'false'
        if call == 'false':
            lines.append('    w.cleared |= 1u << %d;' % bit)
        else:
            lines.append('    if (%s) { w.value[%d] = x; w.known |= 1u << %d; } else w.cleared |= 1u << %d;'
                         % (call, bit, bit, bit))
    return ('void %s(const Context &c, bool confirmed, Worked &w)\n{\n    uint64_t x;\n    (void)confirmed;\n%s\n}\n'
            % (name, '\n'.join(lines)))


def case_checks_code(name, rows, nodes):
    """A case's checks (ee/held.cpp matches): false when the call does not fit them; `confirmed` when one was worked
    out and held. A kind 18 check only when `several`. None: left to the interpreter."""
    lines = []
    for slot, count, first, number in rows:
        call = value_call(nodes, count, first, number, 'k', 'want')
        if call is None:
            return None
        if call == 'false':
            continue  # (not known, and no load made: the next check)
        body = ['unreadable = false;',
                'if (!%s)' % call,
                '{',
                '    if (unreadable) return false;',
                '}']
        if slot < 8:
            body += ['else if (%d < c.regs.intCount)' % slot, '{',
                     '    if (static_cast<uint32_t>(c.regs.a[%d]) != static_cast<uint32_t>(want)) return false;' % slot,
                     '    confirmed = true;', '}']
        elif slot < 16:
            body += ['else if (%d < c.regs.floatCount)' % (slot - 8), '{',
                     '    if (bitsOf(c.regs.f[%d]) != static_cast<uint32_t>(want)) return false;' % (slot - 8),
                     '    confirmed = true;', '}']
        elif slot == 17:
            body += ['else', '{', '    if (want == 0u) return false;', '    confirmed = true;', '}']
        elif slot == 18:
            body += ['else if (want == 0u) return false;']
        else:
            body += ['else', '{',
                     '    if ((c.regs.target & 0x1FFFFFFFu) != (static_cast<uint32_t>(want) & 0x1FFFFFFFu)) return false;',
                     '    confirmed = true;', '}']
        if slot == 18:
            lines.append('    if (several)')
            lines.append('    {')
            lines += ['        ' + t for t in body]
            lines.append('    }')
        else:
            lines += ['    ' + t for t in body]
    return ('bool %s(const Context &c, bool several, bool &confirmed)\n{\n    (void)several;\n    bool unreadable = false;\n'
            '    const Context k{c.frame, c.state, c.regs, true, &unreadable, c.replaying};\n    uint64_t want = 0;\n'
            '    (void)k;\n    (void)want;\n    confirmed = false;\n%s\n    return true;\n}\n' % (name, '\n'.join(lines)))


def cases_of(nodes, cases, values, checks):
    """Per case (prev, first value, first check, values, checks) its values' and its checks' code numbers (0:
    none, the interpreter's), and the distinct ones' code (v1 .., k1 ..). values, checks: (slot, count, first,
    program) rows."""
    value_numbers, check_numbers, value_codes, check_codes = [], [], [], []
    seen_values, seen_checks = {}, {}
    for _, first_value, first_check, value_count, check_count in cases:
        for rows, count, numbers, codes, seen, make, prefix in (
                (values[first_value:first_value + value_count], value_count, value_numbers, value_codes, seen_values,
                 case_values_code, 'v'),
                (checks[first_check:first_check + check_count], check_count, check_numbers, check_codes, seen_checks,
                 case_checks_code, 'k')):
            if not count:
                numbers.append(0)
                continue
            key = tuple((r[0], r[1], r[3], nodes[r[2]] if r[1] == 1 else None) for r in rows)
            number = seen.get(key)
            if number is None:
                code = make('%s%d' % (prefix, len(codes) + 1), rows, nodes)
                number = 0 if code is None else len(codes) + 1
                if code is not None:
                    codes.append(code)
                seen[key] = number
            numbers.append(number)
    return value_numbers, check_numbers, value_codes, check_codes


def chunk_sources(codes, header, chunks=CHUNKS):
    """`chunks` sources of the code (the functions in order, about as much in each), every one written, empty
    past the last; each declares the programs (p1 ..) it calls that another chunk defines."""
    total = sum(len(c) for c in codes)
    parts, size = [[] for _ in range(chunks)], 0
    for code in codes:
        parts[min(chunks - 1, size * chunks // total if total else 0)].append(code)
        size += len(code)
    out = []
    for part in parts:
        text = ''.join(part)
        defined = set(re.findall(r'^bool (p\d+)\(', text, re.M))
        called = sorted(set(re.findall(r'\b(p\d+)\((?:c|k), ', text)) - defined, key=lambda n: int(n[1:]))
        decl = ''.join('bool %s(const Context &, uint64_t &);\n' % n for n in called)
        out.append(header + '#include "ee/held_program.h"\n\nnamespace ee::held::program\n{\n' + decl + text + '}\n')
    return out


def declarations(programs, case_values, case_checks):
    """The table source's declarations of the code and its lists (Table::programs, caseValues, caseChecks)."""
    def listing(kind, names):
        rows = ''.join('        %s,\n' % ', '.join('ee::held::program::' + n for n in names[i:i + 6])
                       for i in range(0, len(names), 6))
        return '    const ee::held::program::%s k%s[] = {\n        nullptr,\n%s    };\n' % (kind[0], kind[1], rows)
    p = ['p%d' % k for k in range(1, programs + 1)]
    v = ['v%d' % k for k in range(1, case_values + 1)]
    k = ['k%d' % n for n in range(1, case_checks + 1)]
    decl = ''.join('    bool %s(const Context &, uint64_t &);\n' % n for n in p)
    decl += ''.join('    void %s(const Context &, bool, Worked &);\n' % n for n in v)
    decl += ''.join('    bool %s(const Context &, bool, bool &);\n' % n for n in k)
    return ('namespace ee::held::program\n{\n%s}\n\nnamespace\n{\n%s%s%s}\n'
            % (decl, listing(('Code', 'Programs'), p), listing(('CaseValuesCode', 'CaseValues'), v),
               listing(('CaseChecksCode', 'CaseChecks'), k)))


def self_test():
    # sll(a0 + 4): straight code
    code = program_code([(OP_ARG, 0, 0), (OP_ALU, K_ADDIU, 4)], 'p1')
    assert 'argument(c, 0, s0)' in code and 'alu(18u, 4, s0, 0u, 0u, s0)' in code and 'out = s0;' in code, code
    # a select: the taken arm when not 0
    code = program_code([(OP_CONST, 0, 1), (OP_SKIP_IF, 0, 2), (OP_CONST, 0, 7), (OP_SKIP, 0, 1), (OP_CONST, 0, 9)],
                        'p2')
    assert 'if (s0 != 0u)' in code and 'else' in code, code
    # arms of different depths: the interpreter's
    assert program_code([(OP_CONST, 0, 1), (OP_SKIP_IF, 0, 3), (OP_CONST, 0, 7), (OP_CONST, 0, 8), (OP_SKIP, 0, 1),
                         (OP_CONST, 0, 9)], 'p3') is None
    # a loop program (the unit test's count to 3)
    loop = [(OP_CONST, 0, 0), (OP_LOOP, 1, 14),
            (OP_LEAF, 1, 4), (OP_CARRIED, 0, 0), (OP_CONST, 0, 3), (OP_ALU, 9, 0), (OP_ALU, 24, 1), (OP_LEAF_END, 0, 1),
            (OP_CARRIED, 0, 0),
            (OP_LEAF, 0, 3), (OP_CARRIED, 0, 0), (OP_CONST, 0, 3), (OP_ALU, 9, 0), (OP_LEAF_END, 0, 2), (OP_CARRIED, 0, 0),
            (OP_ALU, K_ADDIU, 1)]
    code = program_code(loop, 'p4')
    assert 'k1_0 = s0' in code and 'break;' in code and 'continue;' in code, code
    # a stack left two deep: the nodes run, then not known
    code = program_code([(OP_CONST, 0, 1), (OP_CONST, 0, 2)], 'p5')
    assert code.endswith('    return false;\n}\n'), code
    numbers, codes = programs_of([(OP_ARG, 0, 0), (OP_ALU, K_ADDIU, 4), (OP_ARG, 0, 0), (OP_ALU, K_ADDIU, 4)],
                                 [(0, 2, 0), (1, 2, 2), (2, 1, 0)])
    assert numbers == [1, 1, 0] and len(codes) == 1, (numbers, codes)
    print('readable_held_code self-test: ok')


if __name__ == '__main__':
    if sys.argv[1:] == ['--self-test']:
        self_test()
    else:
        print(__doc__)
