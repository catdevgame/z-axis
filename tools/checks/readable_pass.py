#!/usr/bin/env python3
"""The bulk step from proven code to readable code (docs/READABLE.md): passes over the hand-written
sources of engine/zge, games/aggressive and ps2/sce that take out what only served the old proof and put
in what the approved target looks like (ee/plain.h, zge/math/vec.h).

  readable_pass.py run PASS[,PASS...] [--tree WORKTREE] [--build DIR] [--report FILE] [--only NAME,...] [--files TEXT,...]
                       [--jobs N] [--budget SECONDS]
  readable_pass.py report [--report FILE]                the sites a run left, by reason
  readable_pass.py list                                  the passes
  readable_pass.py audit --out FILE                      every function's callees in source order, by game address
  readable_pass.py audit-compare --against FILE --out FILE   two of those: a run must leave them equal

The tree is the input: a run rewrites what it can prove, nothing is stored between runs, and running it
again finds nothing more to do. Literal addresses and `sym::` / `points::` names read the same. Everything
it knows it asks clang (readable_pass_ast.py); DIR is a configured build, for its compile_commands.json and
its frame table (fncheck_build/readable_frames.cpp). After every round each changed file is compiled; one
that stops compiling goes back and what broke it is left out and reported.

--budget stops at the end of the round that passes it, the tree compiling, and exits 3: the same command goes on
from there (FILE.state keeps the counts and what was left out). --files limits a pass to files whose path has TEXT.

The passes, in the order they are meant to run; each runs alone, and each is followed by `dead` and `tidy`:

  marks    ee::block, ee::afterCall, ee::loopBack, ee::tableJump, ee::jumpDispatch, ee::chargeLoop and the like:
           they charge emulated time and offer a safe point, no more (ee/abi.h). Calls keep their safe point
           (ee::call). A polling loop, with no call and no local moved, keeps its loopBack and is reported: nothing
           else lets the EE out of it, and it needs a named wait.
  saves    ee::calleeSaves, StackFrame::saveEntry, SavedRegisters and whatever publishes them: they feed the dead
           stack a callee's prologue writes. The values computed only for them go with them, a read of a callee-saved
           register as the caller left it included; a call among them stays.
  frames   ee::StackFrame as ee::Local at the same offsets, or a plain value when the address goes nowhere, nothing
           overlaps it and a whole write always comes first (a read of what the stack held stays on the guest stack).
           Offsets go in readable_locals.tsv unless declaration order gives them.
  aliases  `const uint32_t x = pointer.address;`: x is the pointer, so what follows sees its type.
  routes   a readable function a typed call's unit declares has its plain name taken by the definition, and a plain
           call must not reach the body (it would skip hooks, the replay's answers, a function switched off). So its
           body takes a marker, a trailing underscore on the C++ name, at every declaration, registration and
           reference. Nothing else changes. A plain call of a body that was direct stays direct, as name_(...).
  calls    ee::call(kConstant, ...), ee::tailCall and game::call as name(...). A body the unit declares:
           GAME_CALL(name) after its declaration (ee/readable.h), one line for the function, no line per caller, the call
           qualified when the body is another namespace's. Anything else (a recompiled callee, a function the unit does
           not declare): one typed line in namespace { } above the first function of the file that needs it, over ee::game
           or game::call; it takes what the sites pass: the register a callee gets is the one it got. A call whose
           callee's leftovers are read after it stays a table call. A tail call keeps its form, as tail::name when name
           is taken. Names and
           parameter names: the readable function's own, else readable_names.tsv, else the constant's comment, else
           what the first site hands it. Indirect calls stay.
  fields   *ee::at<T>(pointer.address + 0x20) as pointer->field, when clang's layout of the struct has a member of
           that type there, and x + offsetof(S, f) as ee::Ptr<S>(x)->f. A padding field never counts.
  vectors  ee::vu0::Machine's macros on plain Vec4s as vec.h writes them.
  handons  an argument register a function reads as it came and hands on, as a parameter, where a table call reaches it
           and nothing runs before the read. Never dropped: the rest is listed.
  mirrors  ee::callerRegister, ee::callerFloat and SavedRegisters::keepCallers of a callee-saved register the original never
           reads as it found it (readable_entry_reads.tsv lists the ones it does): the read goes, `dead` takes what carried it.
  comments comments about what the passes took out; a file that keeps some of it keeps them.

Every pass leaves what it cannot prove and says why (--report, one row a site: pass, file, line, function, reason).
"""
import argparse
import collections
import json
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ztree  # noqa: E402  (the z-axis tree checked: ztree.py)
try:   # the scratch folder: the tooling's own paths.env (and a workshop's own layout, when there)
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    import zpaths  # noqa: E402
    SCRATCH_ROOT = zpaths.ZAXIS_SCRATCH
except Exception:   # (a worker's copy of the checks alone: no tooling around them)
    SCRATCH_ROOT = os.path.expanduser('~')
sys.dont_write_bytecode = True
import readable_pass_ast as A  # noqa: E402

SCRATCH = os.path.join(SCRATCH_ROOT, 'pass1')
MARKS = {'block', 'afterCall', 'loopBack', 'loopBackWhole', 'blockWhole', 'tableJump', 'jumpDispatch', 'chargeLoop', 'chargeCycles'}
SAVES = {'calleeSaves', 'saveEntry'}
SAVED_REGISTERS = 'aggressive::SavedRegisters'
GUEST_CALLS = {'call', 'tailCall', 'kernelCall', 'callKeepingVu0', 'callAt', 'tailCallAt', 'game', 'selfCall'}
# Calls that only read: what a pure expression may contain.
PURE_CALLS = {
    'at', 'addressOf', 'pointerAt', 'get', 'operator->', 'operator*', 'operator[]', 'operator T *', 'bits', 'raw', 'fromBits', 'cast',
    'Ptr', 'Float', 'toFloat', 'toInt', 'abs', 'min', 'max', 'sqrt', 'rsqrt', 'callerRegister', 'callerRegisterHigh', 'callerFloat',
    'leftByCall', 'leftByCallHigh', 'leftByCallFloat', 'operator+', 'operator-', 'operator/', 'operator==', 'operator!=', 'operator<',
    'operator<=', 'operator>', 'operator>=', 'operator bool', 'high', 'uncached', 'size', 'data', 'empty', 'address',
}
WRITE_OPERATORS = {'operator=', 'operator+=', 'operator-=', 'operator*=', 'operator/=', 'operator++', 'operator--'}
ASSIGN = {'=', '+=', '-=', '*=', '/=', '%=', '&=', '|=', '^=', '<<=', '>>='}
FULL_EXPRESSION = {'ExprWithCleanups'}
SCAFFOLD_COMMENT = re.compile(r'\b(?:entry block|branch back|the original\'s (?:loop )?(?:branch|block|s\d|ra\b|fp\b|f2\d|f3\d)|'
                              r'callee-saved|calleeSaves|published?|its s\d|s[0-7] \.\.|\(?s[0-7], s[0-7]|ee::block|ee::loopBack|ee::afterCall|'
                              r'safe point|delay slot|return address|What the original keeps|The registers the loop uses)\b')


LEAVE_ALONE = {'aggressive/saved_registers.h'}  # (the scaffolding itself)


class Report:
    def __init__(self):
        self.rows = []
        self.changed = collections.Counter()      # (pass, function key)
        self.sites = collections.Counter()        # pass -> sites rewritten
        self.functions = collections.defaultdict(set)

    def keep(self, name, fn, node, reason):
        line = fn.text.line_of(A.begin(node) or A.begin(fn.decl))
        self.rows.append((name, fn.rel, line, fn.name, reason))

    def did(self, name, fn, count=1):
        self.sites[name] += count
        self.functions[name].add((fn.rel, fn.name))


class Fn:
    """A function of the tree, with its parents."""

    def __init__(self, tree, path, decl):
        self.tree, self.path, self.decl = tree, path, decl
        self.name = decl.get('name', '')
        self.text = tree.texts[path]
        self.rel = tree.rel(path)
        self.body = next(c for c in A.children(decl) if c.get('kind') == 'CompoundStmt')
        self.parent = {}
        for n, p in A.walk(decl):
            self.parent[id(n)] = p

    def up(self, node):
        return self.parent.get(id(node))

    def enclosing(self, node, kinds):
        p = self.up(node)
        while p is not None and p.get('kind') not in kinds:
            p = self.up(p)
        return p

    def source(self, node):
        return self.text.raw[A.source_begin(node):A.source_end(node, self.text.raw)]


def callee_name(call):
    """The name a CallExpr or CXXMemberCallExpr calls, with what it is qualified by when that is known."""
    inner = A.children(call)
    if not inner:
        return None
    x = A.strip_implicit(inner[0])
    if x.get('kind') == 'DeclRefExpr':
        return (x.get('referencedDecl') or {}).get('name')
    if x.get('kind') == 'MemberExpr':
        return x.get('name')
    if x.get('kind') in ('UnresolvedLookupExpr', 'UnresolvedMemberExpr'):
        return x.get('name')
    return None


def member_base_type(call):
    x = A.strip_implicit(A.children(call)[0])
    if x.get('kind') != 'MemberExpr' or not A.children(x):
        return ''
    return A.desugar(A.children(x)[0].get('type', {}))


def is_ee_mark(fn, call, names):
    """A call of ee::<name>, by the text it was written in (ee::block, not a lambda called block)."""
    if call.get('kind') != 'CallExpr':
        return False
    name = callee_name(call)
    if name not in names:
        return False
    begin = A.begin(call)
    return begin is not None and fn.text.raw[begin:begin + 4 + len(name)] == 'ee::' + name


PURE_HELPERS = set()  # functions of the tree that only compute (is_pure_function)


def is_pure(fn, node):
    """No write and no call that could do anything: safe to evaluate less often."""
    for n, _ in A.walk(node):
        kind = n.get('kind')
        if kind in ('CallExpr', 'CXXMemberCallExpr', 'CXXOperatorCallExpr'):
            name = callee_name(n)
            if name == 'operator()':
                if not pure_lambda_call(fn, n):
                    return False
            elif name not in PURE_CALLS and name not in PURE_HELPERS:
                return False
        elif kind == 'BinaryOperator' and n.get('opcode') in ASSIGN:
            return False
        elif kind == 'CompoundAssignOperator':
            return False
        elif kind == 'UnaryOperator' and n.get('opcode') in ('++', '--'):
            return False
        elif kind in ('CXXNewExpr', 'CXXDeleteExpr', 'CXXThrowExpr', 'StmtExpr', 'LambdaExpr'):
            return False
    return True


def pure_lambda_call(fn, call):
    """A call of a lambda of this function with a body that only computes."""
    f = A.strip_implicit(A.children(call)[1]) if len(A.children(call)) > 1 else {}
    if f.get('kind') != 'DeclRefExpr':
        return False
    wanted = (f.get('referencedDecl') or {}).get('id')
    for var, _ in local_vars(fn):
        if var.get('id') == wanted:
            lam = lambda_of(var)
            if lam is None:
                return False
            return statements_pure(fn, [c for c in A.children(lam) if c.get('kind') == 'CompoundStmt'][-1])
    return False


def statements_pure(fn, body):
    inside = A.children(body)
    if not inside or len(inside) > 4:
        return False
    for st in inside:
        if st.get('kind') == 'ReturnStmt':
            if not all(is_pure(fn, c) for c in A.children(st)):
                return False
        elif st.get('kind') == 'DeclStmt':
            for d in A.children(st):
                if d.get('kind') != 'VarDecl' or not all(is_pure(fn, c) for c in A.children(d)):
                    return False
        else:
            return False
    return True


def find_pure_helpers(fns):
    """The functions that only compute: short bodies of declarations and a return, every same-named one too."""
    while True:
        verdicts = {}
        for fn in fns:
            if fn.name:
                verdicts[fn.name] = verdicts.get(fn.name, True) and statements_pure(fn, fn.body)
        fresh = {n for n, ok in verdicts.items() if ok} - PURE_HELPERS
        if not fresh:
            return
        PURE_HELPERS.update(fresh)


class Rewriter:
    """Taking a statement out of its context, whatever that context is."""

    def __init__(self, tree):
        self.tree = tree
        self.edits = A.Edits()

    def lines_only(self, text, start, stop):
        """Whether [start, stop) is all that is on its lines, a trailing comment apart: (first line's start, past the
        last line's newline), or None."""
        first = text.line_start(start)
        if text.raw[first:start].strip():
            return None
        end = text.line_end(stop)
        tail = text.raw[stop:end].strip()
        if tail and not tail.startswith('//'):
            return None
        return first, min(end + 1, len(text.raw))

    def past_semicolon(self, text, stop):
        i = stop
        while i < len(text.raw) and text.raw[i] in ' \t\r\n':
            i += 1
        return i + 1 if text.raw[i:i + 1] == ';' else stop

    def scaffold_comment_above(self, text, first):
        """The comment block right above a line about to go, when it only talks about what goes."""
        i = first
        start = None
        block = []
        while i > 0:
            prev = text.raw.rfind('\n', 0, i - 1) + 1
            line = text.raw[prev:i].strip()
            if not line.startswith('//'):
                break
            block.append(line)
            start = prev
            i = prev
        if start is not None and SCAFFOLD_COMMENT.search(' '.join(block)):
            return start
        return first

    def remove_lines(self, fn, start, stop):
        """Takes [start, stop) out; whole lines when it has its lines to itself."""
        text = fn.text
        whole = self.lines_only(text, start, stop)
        if whole is None:
            self.edits.add(fn.path, start, stop, '')
            return
        first, last = whole
        first = self.scaffold_comment_above(text, first)
        # No blank line left at the top or bottom of a block, or doubled.
        before = text.raw[:first].rstrip(' \t')
        above_blank = before.endswith('\n\n') or before.endswith('{\n')
        below = text.raw[last:text.line_end(last)].strip()
        if (not below) and above_blank:
            last = text.line_end(last) + 1
        elif below.startswith('}') and before.endswith('\n\n'):
            first = text.line_start(first - 2) if text.raw[first - 2:first - 1] == '\n' else first - 1
        self.edits.add(fn.path, first, last, '')

    def negated(self, fn, cond):
        c = A.strip_implicit(cond)
        src = fn.source(cond)
        if c.get('kind') == 'BinaryOperator' and c.get('opcode') in ('==', '!='):
            l, r = A.children(c)
            lt = A.desugar(A.strip_implicit(l).get('type', {}))
            if 'float' not in lt.lower():
                return '%s %s %s' % (fn.source(l), '!=' if c.get('opcode') == '==' else '==', fn.source(r))
        if c.get('kind') == 'UnaryOperator' and c.get('opcode') == '!':
            inner = fn.source(A.children(c)[0])
            return inner[1:-1] if inner.startswith('(') and inner.endswith(')') and balanced(inner[1:-1]) else inner
        if re.fullmatch(r'[\w:.>\-\[\]]+(\([^()]*\))?', src):
            return '!' + src
        return '!(%s)' % src

    def remove(self, fn, node):
        """Takes a statement or an expression statement out. False: its context is not one this knows."""
        n, p = node, fn.up(node)
        while p is not None and p.get('kind') in FULL_EXPRESSION | {'ParenExpr'}:
            n, p = p, fn.up(p)
        if p is None:
            return False
        kind = p.get('kind')
        text = fn.text
        start, stop = A.begin(n), A.end(n)
        if start is None or stop is None or A.in_macro(n):
            return False
        if kind == 'CompoundStmt':
            self.remove_lines(fn, start, self.past_semicolon(text, stop))
            return True
        if kind == 'IfStmt':
            return self.remove_branch(fn, p, n)
        if kind == 'ForStmt':
            inner = p.get('inner', [])
            # [init, condition variable, condition, increment, body]
            if len(inner) == 5 and inner[3] is n:
                while text.raw[start - 1] in ' \t':
                    start -= 1
                self.edits.add(fn.path, start, stop, '')
                return True
            if len(inner) == 5 and inner[4] is n:
                self.edits.add(fn.path, start, self.past_semicolon(text, stop), '{\n%s}' % indent_of(text, p))
                return True
            return False
        if kind in ('WhileStmt', 'DoStmt'):
            self.edits.add(fn.path, start, self.past_semicolon(text, stop), '{\n%s}' % indent_of(text, p))
            return True
        if kind in ('CaseStmt', 'DefaultStmt', 'LabelStmt'):
            stop2 = self.past_semicolon(text, stop)
            scope = fn.up(p)
            siblings = A.children(scope) if scope and scope.get('kind') == 'CompoundStmt' else []
            last = bool(siblings) and siblings[-1] is p
            if last or kind == 'LabelStmt':
                self.edits.add(fn.path, start, stop2, 'break;' if last and kind != 'LabelStmt' else ';')
            else:
                # The statement's own line goes; the label stays.
                whole = self.lines_only(text, start, stop2)
                if whole is None:
                    while text.raw[stop2:stop2 + 1] == ' ':
                        stop2 += 1
                    self.edits.add(fn.path, start, stop2, '')
                else:
                    self.edits.add(fn.path, whole[0], whole[1], '')
            return True
        if kind == 'BinaryOperator' and p.get('opcode') == ',':
            l, r = A.children(p)
            if l is n:
                other = A.begin(r)
                self.edits.add(fn.path, start, other, '')
            else:
                self.edits.add(fn.path, A.end(l), stop, '')
            return True
        if kind == 'ReturnStmt':
            self.edits.add(fn.path, start, stop, '')
            # (`return ;`)
            self.edits.add(fn.path, start - 1, start, '') if text.raw[start - 1] == ' ' else None
            return True
        return False

    def remove_branch(self, fn, stmt, branch):
        text = fn.text
        inner = A.children(stmt)
        has_else = bool(stmt.get('hasElse'))
        then = inner[-2] if has_else else inner[-1]
        otherwise = inner[-1] if has_else else None
        cond = inner[-3] if has_else else inner[-2]
        if len(inner) != (3 if has_else else 2):
            return False  # (an init statement or a condition variable)
        if branch is otherwise:
            # `else` and what follows it.
            i = text.raw.rfind('else', A.end(then), A.begin(otherwise))
            if i < 0:
                return False
            stop = self.past_semicolon(text, A.end(otherwise))
            whole = self.lines_only(text, i, stop)
            if whole:
                self.edits.add(fn.path, whole[0], whole[1], '')
            else:
                self.edits.add(fn.path, i, stop, '')
            return True
        stop = self.past_semicolon(text, A.end(then))
        if otherwise is None:
            if is_pure(fn, cond):
                self.edits_remove_whole_if(fn, stmt, stop)
            else:
                # The condition still runs (a call): it stays as a statement.
                self.edits.add(fn.path, A.begin(stmt), stop, fn.source(cond) + ';')
            return True
        # `if (c) MARK; else X;`: if (!c) X;
        else_at = text.raw.rfind('else', A.end(then), A.begin(otherwise))
        if else_at < 0:
            return False
        head = 'if (%s)' % self.negated(fn, cond)
        if otherwise.get('kind') == 'IfStmt':
            # `if (c) MARK; else if ...`: the rest of the chain goes inside.
            pad = indent_of(text, stmt)
            last = text.line_end(self.statement_end(fn, otherwise))
            rest = text.raw[text.line_start(A.begin(otherwise)):last] if not text.raw[text.line_start(A.begin(otherwise)):A.begin(otherwise)].strip() \
                else pad + text.raw[A.begin(otherwise):last]
            inner_text = '\n'.join(('    ' + l if l.strip() else l) for l in rest.split('\n'))
            self.edits.add(fn.path, A.begin(stmt), last, '%s\n%s{\n%s\n%s}' % (head, pad, inner_text, pad))
            return True
        self.edits.add(fn.path, A.begin(stmt), text.line_end(else_at) + 1 if not text.raw[else_at + 4:text.line_end(else_at)].strip() else else_at + 4,
                       head + ('\n' if not text.raw[else_at + 4:text.line_end(else_at)].strip() else ''))
        return True

    def statement_end(self, fn, node):
        """Past the last token of a statement, its semicolon included, through an `else` chain."""
        return self.past_semicolon(fn.text, A.end(node))

    def edits_remove_whole_if(self, fn, stmt, stop):
        parent = fn.up(stmt)
        if parent is not None and parent.get('kind') == 'CompoundStmt':
            self.remove_lines(fn, A.begin(stmt), stop)
        else:
            self.remove(fn, stmt)


def balanced(s):
    depth = 0
    for c in s:
        depth += c == '('
        depth -= c == ')'
        if depth < 0:
            return False
    return depth == 0


def indent_of(text, node):
    first = text.line_start(A.begin(node))
    return re.match(r'[ \t]*', text.raw[first:]).group(0)


# ---- the passes ---------------------------------------------------------------------------------


def loop_of(fn, node):
    return fn.enclosing(node, ('ForStmt', 'WhileStmt', 'DoStmt', 'CXXForRangeStmt'))


def moves_local(fn, loop):
    """Whether a loop changes one of the function's locals: it counts or walks something, so it ends by itself."""
    for n, _ in A.walk(loop):
        kind = n.get('kind')
        target = None
        if kind == 'BinaryOperator' and n.get('opcode') == '=' or kind == 'CompoundAssignOperator':
            target = A.children(n)[0]
        elif kind == 'UnaryOperator' and n.get('opcode') in ('++', '--'):
            target = A.children(n)[0]
        elif kind == 'CXXOperatorCallExpr' and callee_name(n) in WRITE_OPERATORS:
            target = A.children(n)[1]
        if target is not None:
            t = A.strip_implicit(target)
            if t.get('kind') == 'DeclRefExpr' and (t.get('referencedDecl') or {}).get('kind') in ('VarDecl', 'ParmVarDecl'):
                return True
    return False


def calls_guest(fn, node):
    for n, _ in A.walk(node):
        if n.get('kind') == 'CallExpr':
            name = callee_name(n)
            begin = A.begin(n)
            if name in GUEST_CALLS and begin is not None and fn.text.raw[begin:begin + 4] in ('ee::', 'game'):
                return True
            # A helper of the file or another readable function: it can reach a call.
            if name and name not in PURE_CALLS and name not in MARKS and name not in SAVES:
                return True
    return False


def pass_marks(ctx, fn):
    gotos = None
    for n, p in A.walk_code(fn.body):
        if n.get('kind') != 'CallExpr' or not is_ee_mark(fn, n, MARKS):
            continue
        name = callee_name(n)
        if name in ('loopBack', 'loopBackWhole'):
            loop = loop_of(fn, n)
            scope = loop if loop is not None else fn.body
            if loop is None and gotos is None:
                gotos = any(x.get('kind') == 'GotoStmt' for x, _ in A.walk(fn.body))
            if not calls_guest(fn, scope) and not moves_local(fn, scope):
                ctx.report.keep('marks', fn, n, 'poll: a loop with no call and no local moved keeps its loopBack; it needs a named wait')
                continue
        if ctx.rw.remove(fn, n):
            if ctx.rw.edits.commit():
                ctx.report.did('marks', fn)
        else:
            ctx.rw.edits.rollback()
            ctx.report.keep('marks', fn, n, 'mark in a context the pass does not know (%s)' % (fn.up(n) or {}).get('kind'))


def saved_registers_method(n):
    if n.get('kind') != 'CXXMemberCallExpr':
        return None
    if SAVED_REGISTERS in member_base_type(n):
        return callee_name(n)
    return None


def pass_saves(ctx, fn):
    for n, p in A.walk_code(fn.body):
        if n.get('kind') == 'CallExpr' and is_ee_mark(fn, n, {'calleeSaves'}):
            arguments = A.children(n)[1:]
            if not all(is_pure(fn, a) for a in arguments):
                ctx.report.keep('saves', fn, n, 'a save line with a call in it: the call is real')
            elif ctx.rw.remove(fn, n):
                if ctx.rw.edits.commit():
                    ctx.report.did('saves', fn)
            else:
                ctx.rw.edits.rollback()
                ctx.report.keep('saves', fn, n, 'save in a context the pass does not know (%s)' % (fn.up(n) or {}).get('kind'))
        elif n.get('kind') == 'CXXMemberCallExpr' and callee_name(n) == 'saveEntry':
            if ctx.rw.remove(fn, n) and ctx.rw.edits.commit():
                ctx.report.did('saves', fn)
            ctx.rw.edits.rollback()
        else:
            method = saved_registers_method(n)
            if method in ('publish', 'set', 'setWhole', 'setFloat', 'keepCallers', 'keepCallersFloat'):
                if all(is_pure(fn, a) for a in A.children(n)[1:]) and ctx.rw.remove(fn, n):
                    if ctx.rw.edits.commit():
                        ctx.report.did('saves', fn)
                else:
                    ctx.rw.edits.rollback()
                    ctx.report.keep('saves', fn, n, 'SavedRegisters.%s in a context the pass does not know' % method)


def refs_of(fn):
    """{declaration id: [(DeclRefExpr, parent)]} of the function, each source place once (a lambda's body is in
    the AST twice)."""
    refs = collections.defaultdict(dict)
    for n, p in A.walk_code(fn.decl):
        if n.get('kind') == 'DeclRefExpr':
            d = n.get('referencedDecl') or {}
            refs[d.get('id')].setdefault(A.begin(n), (n, p))
    return {k: list(v.values()) for k, v in refs.items()}


def written_only(fn, ref, parent):
    """The statement a reference only writes to: (statement, right-hand side or None), or None if it reads."""
    p = parent
    # Through the implicit steps between the name and what uses it.
    node = ref
    while p is not None and p.get('kind') in ('ImplicitCastExpr', 'ParenExpr') and p.get('castKind') != 'LValueToRValue':
        node, p = p, fn.up(p)
    if p is None:
        return None
    kind = p.get('kind')
    inner = A.children(p)
    if kind == 'BinaryOperator' and p.get('opcode') == '=' and inner[0] is node:
        return p, inner[1]
    if kind == 'CompoundAssignOperator' and inner[0] is node:
        return p, inner[1]
    if kind == 'UnaryOperator' and p.get('opcode') in ('++', '--'):
        return p, None
    if kind == 'CXXOperatorCallExpr' and callee_name(p) in WRITE_OPERATORS and len(inner) >= 2:
        if A.strip_implicit(inner[1]) is ref:
            return p, inner[2] if len(inner) > 2 else None
    if kind == 'CStyleCastExpr' and A.desugar(p.get('type', {})) == 'void':
        return p, None
    return None


def statement_of(fn, expression):
    """The expression when it stands as a statement of its own."""
    n, p = expression, fn.up(expression)
    while p is not None and p.get('kind') in FULL_EXPRESSION | {'ParenExpr'}:
        n, p = p, fn.up(p)
    if p is None:
        return None
    if p.get('kind') in ('CompoundStmt', 'CaseStmt', 'DefaultStmt', 'LabelStmt'):
        return n
    if p.get('kind') == 'IfStmt' and n is not A.children(p)[0]:
        return n
    if p.get('kind') == 'WhileStmt':
        return n if n is A.children(p)[-1] else None
    if p.get('kind') == 'DoStmt':
        return n if n is A.children(p)[0] else None
    if p.get('kind') == 'ForStmt':
        return n if n is p.get('inner', [None] * 4)[3] or n is p.get('inner', [None] * 5)[4] else None
    return None


def local_vars(fn):
    """The local variables declared by a DeclStmt of their own, with that statement."""
    out = []
    for n, p in A.walk_code(fn.body):
        if n.get('kind') == 'DeclStmt' and p is not None and p.get('kind') == 'CompoundStmt':
            decls = A.children(n)
            if len(decls) == 1 and decls[0].get('kind') == 'VarDecl':
                out.append((decls[0], n))
    return out


def initializer(var):
    inner = A.children(var)
    return inner[-1] if inner and var.get('init') else None


def pass_dead(ctx, fn):
    """A local nobody reads goes, with the writes to it; a call among them stays as a statement."""
    refs = refs_of(fn)
    for var, stmt in local_vars(fn):
        if A.in_macro(stmt):
            continue
        t = var.get('type', {}).get('qualType', '')
        if t.endswith('&') and 'auto' not in t and refs.get(var.get('id')):
            continue
        init = initializer(var)
        # A lambda, an object with a constructor that does something: only the kinds we know.
        kind_ok = init is None and A.desugar(var.get('type', {})) in SCALAR or init is not None and \
            (init.get('kind') == 'LambdaExpr' or A.desugar(var.get('type', {})).split('<')[0] in SCALAR_OBJECTS or
             re.fullmatch(r'(const )?(auto|unsigned (int|long long|char|short)|int|short|char|signed char|float|long long|bool|ee::Float|ee::Ptr<.*>|.*\*( const)?)', A.desugar(var.get('type', {}))) or
             'SavedRegisters' in A.desugar(var.get('type', {})))
        if not kind_ok and 'SavedRegisters' not in A.desugar(var.get('type', {})) and A.desugar(var.get('type', {})) != 'ee::vu0::Machine':
            continue
        uses = refs.get(var.get('id'), [])
        writes = []
        reading = False
        for ref, parent in uses:
            w = written_only(fn, ref, parent)
            if w is None or statement_of(fn, w[0]) is None:
                reading = True
                break
            writes.append(w)
        if reading:
            continue
        ok = True
        plan = []
        for expression, value in writes:
            if value is None or is_pure(fn, value):
                plan.append((expression, None))
            elif expression.get('kind') == 'BinaryOperator' and expression.get('opcode') == '=':
                plan.append((expression, value))
            else:
                ok = False
        if init is not None and not is_pure(fn, init) and init.get('kind') != 'LambdaExpr':
            # The initializer's call stays.
            if A.in_macro(init):
                continue
            if writes:
                continue
            ctx.rw.edits.add(fn.path, A.begin(stmt), A.end(stmt) - 1, fn.source(init))
            if ctx.rw.edits.commit():
                ctx.report.did('dead', fn)
            continue
        if not ok:
            continue
        for expression, value in plan:
            if value is not None:
                ctx.rw.edits.add(fn.path, A.begin(expression), A.end(expression), fn.source(value))
            elif not ctx.rw.remove(fn, expression):
                ok = False
        if ok:
            ctx.rw.remove_lines(fn, A.begin(stmt), A.end(stmt))
            if ctx.rw.edits.commit():
                ctx.report.did('dead', fn)
        else:
            ctx.rw.edits.rollback()
    # A namespace alias nothing uses (`namespace here = points::x;`).
    for n, p in A.walk(fn.body):
        if n.get('kind') == 'DeclStmt' and A.children(n) and A.children(n)[0].get('kind') == 'NamespaceAliasDecl':
            alias = A.children(n)[0].get('name')
            text = fn.source(fn.body)
            if len(re.findall(r'\b%s::' % re.escape(alias), text)) == 0 and not A.in_macro(n):
                ctx.rw.remove_lines(fn, A.begin(n), A.end(n))
                if ctx.rw.edits.commit():
                    ctx.report.did('dead', fn)
    pass_entry_values(ctx, fn)
    pass_empty_lambdas(ctx, fn)
    pass_empty_calls(ctx, fn)


SCALAR = {'unsigned int', 'int', 'unsigned long long', 'long long', 'unsigned short', 'short', 'unsigned char', 'signed char', 'char',
          'float', 'bool', 'ee::Float'}
SCALAR_OBJECTS = {'ee::Ptr', 'ee::Float', 'std::function'}


def lambda_of(var):
    init = initializer(var)
    while init is not None and init.get('kind') in ('ExprWithCleanups', 'ImplicitCastExpr', 'MaterializeTemporaryExpr', 'CXXConstructExpr',
                                                     'CXXBindTemporaryExpr') and A.children(init):
        init = A.children(init)[0]
    return init if init is not None and init.get('kind') == 'LambdaExpr' else None


ENTRY_REGISTER = re.compile(r'\bee::caller(?:Register|Float)\(')


def statement_in(fn, node, block):
    """The child of `block` a node is under, or None."""
    n = node
    while n is not None:
        p = fn.up(n)
        if p is block:
            return n
        n = p
    return None


def always_set_first(fn, var, stmt):
    """Whether every read of a local comes after a plain write that always runs before it, in the structured code
    around it: the value it is declared with is never seen. No goto, no read in a lambda."""
    writes = []
    reads = []
    for ref, parent in var_refs(fn, var):
        if fn.enclosing(ref, ('LambdaExpr',)) is not None:
            return False
        w = written_only(fn, ref, parent)
        if w is not None and w[0].get('kind') == 'BinaryOperator' and w[0].get('opcode') == '=':
            s = statement_of(fn, w[0])
            block = fn.up(s) if s is not None else None
            if block is not None and block.get('kind') == 'CompoundStmt':
                # What the write reads runs before it.
                if any(x.get('kind') == 'DeclRefExpr' and (x.get('referencedDecl') or {}).get('id') == var.get('id') for x, _ in A.walk(w[1])):
                    return False
                writes.append((s, block))
                continue
        reads.append(ref)
    if not writes:
        return False
    if any(x.get('kind') in ('GotoStmt', 'LabelStmt') for x, _ in A.walk(fn.body)):
        return False
    for ref in reads:
        where = A.begin(ref)
        covered = False
        for s, block in writes:
            if A.begin(s) < where and statement_in(fn, ref, block) is not None:
                covered = True
                break
        if not covered:
            return False
    return True


def pass_entry_values(ctx, fn):
    """`T x = ee::callerRegister(...)` where the code always sets x before it reads it: no read of the register."""
    for var, stmt in local_vars(fn):
        init = initializer(var)
        if init is None or A.in_macro(stmt) or not ENTRY_REGISTER.search(fn.source(init)):
            continue
        t = A.desugar(var.get('type', {}))
        if re.sub(r'^const\s+', '', t) in SCALAR and always_set_first(fn, var, stmt):
            ctx.rw.edits.add(fn.path, A.begin(init), A.end(init), '0.0f' if 'loat' in t else '0')
            if ctx.rw.edits.commit():
                ctx.report.did('dead', fn)


def empty_helpers(fns, shared):
    """{(file, name)}: the functions and methods with nothing left in them, that no table reaches."""
    found = set()
    for fn in fns:
        if A.children(fn.body) or fn.path.endswith('.h') or fn.decl.get('kind') not in ('FunctionDecl', 'CXXMethodDecl'):
            continue
        if (fn.path, fn.name) in shared.regs or not fn.name or fn.name.startswith('operator'):
            continue
        if not fn.decl.get('type', {}).get('qualType', '').startswith('void'):
            continue
        found.add((fn.path, fn.name))
    return found


def pass_empty_calls(ctx, fn):
    """The calls of a helper with nothing left in it; the helper, once nothing refers to it."""
    for n, p in A.walk_code(fn.body):
        if n.get('kind') in ('CallExpr', 'CXXMemberCallExpr') and (fn.path, callee_name(n)) in ctx.shared.empty:
            if all(is_pure(fn, a) for a in A.children(n)[1:]) and statement_of(fn, n) is not None:
                if ctx.rw.remove(fn, n) and ctx.rw.edits.commit():
                    ctx.report.did('dead', fn)
                ctx.rw.edits.rollback()
    if (fn.path, fn.name) in ctx.shared.empty and not fn.decl.get('isUsed') and not fn.decl.get('isReferenced'):
        start, stop = A.begin(fn.decl), A.end(fn.decl)
        if not A.in_macro(fn.decl):
            ctx.rw.remove_lines(fn, start, stop)
            if ctx.rw.edits.commit():
                ctx.report.did('dead', fn)


def pass_empty_lambdas(ctx, fn):
    """A local lambda with nothing left in it, and its calls; a lambda parameter nobody uses, with its arguments."""
    refs = refs_of(fn)
    for var, stmt in local_vars(fn):
        lam = lambda_of(var)
        if lam is None or A.in_macro(stmt):
            continue
        body = [c for c in A.children(lam) if c.get('kind') == 'CompoundStmt'][-1]
        uses = refs.get(var.get('id'), [])
        calls = []
        ok = True
        for ref, parent in uses:
            call = ref_call(fn, ref)
            if call is None:
                ok = False
                break
            calls.append(call)
        if not ok:
            continue
        if not A.children(body):
            if all(all(is_pure(fn, a) for a in A.children(c)[1:]) and statement_of(fn, c) is not None for c in calls):
                for c in calls:
                    ctx.rw.remove(fn, c)
                ctx.rw.remove_lines(fn, A.begin(stmt), ctx.rw.past_semicolon(fn.text, A.end(stmt)))
                if ctx.rw.edits.commit():
                    ctx.report.did('dead', fn)
            continue
        drop_lambda_parameters(ctx, fn, lam, calls)


def ref_call(fn, ref):
    """The call whose function a reference to a lambda variable is (`name(args)`): operator() on it."""
    n, p = ref, fn.up(ref)
    while p is not None and p.get('kind') == 'ImplicitCastExpr':
        n, p = p, fn.up(p)
    if p is not None and p.get('kind') == 'CXXOperatorCallExpr' and callee_name(p) == 'operator()' and A.children(p)[1] is n:
        return p
    return None


def drop_lambda_parameters(ctx, fn, lam, calls):
    """Only the parameters at the end of the list one by one, or any when all its arguments are pure."""
    method = None
    for n, _ in A.walk(lam):
        if n.get('kind') == 'CXXMethodDecl' and n.get('name') == 'operator()':
            method = n
            break
    if method is None:
        return
    params = [c for c in A.children(method) if c.get('kind') == 'ParmVarDecl']
    if not params or any('auto' in p.get('type', {}).get('qualType', '') for p in params):
        return
    body = [c for c in A.children(lam) if c.get('kind') == 'CompoundStmt'][-1]
    used = set()
    for n, _ in A.walk(body):
        if n.get('kind') == 'DeclRefExpr':
            used.add((n.get('referencedDecl') or {}).get('id'))
    for i, p in enumerate(params):
        if p.get('id') in used or p.get('isReferenced') and p.get('name') and re.search(r'\b%s\b' % re.escape(p['name']), fn.source(body)):
            continue
        ok = all(len(A.children(c)) - 2 == len(params) and is_pure(fn, A.children(c)[2 + i]) for c in calls)
        if not ok or A.in_macro(p):
            continue
        # The parameter with its comma.
        start, stop = A.begin(p), A.end(p)
        if i + 1 < len(params):
            stop = A.begin(params[i + 1])
        elif i > 0:
            start = A.end(params[i - 1])
        ctx.rw.edits.add(fn.path, start, stop, '')
        for c in calls:
            args = A.children(c)[2:]
            a_start, a_stop = A.begin(args[i]), A.end(args[i])
            if i + 1 < len(args):
                a_stop = A.begin(args[i + 1])
            elif i > 0:
                a_start = A.end(args[i - 1])
            ctx.rw.edits.add(fn.path, a_start, a_stop, '')
        if ctx.rw.edits.commit():
            ctx.report.did('dead', fn)
        return  # (one a round: the next round sees the new list)


def join_declarations(ctx, fn):
    """`T x;` then, with nothing using x in between, `x = value;` in the same block: `const T x = value;`."""
    refs = refs_of(fn)
    gotos = any(x.get('kind') in ('GotoStmt', 'LabelStmt') for x, _ in A.walk(fn.body))
    if gotos:
        return
    for var, stmt in local_vars(fn):
        if A.in_macro(stmt):
            continue
        declared = fn.source(stmt).rstrip(';').rstrip()
        name = var.get('name')
        m = re.fullmatch(r'([\w:<>\s,*]+?)\s+%s' % re.escape(name or '?'), declared)
        if not m or 'static' in m.group(1) or 'extern' in m.group(1) or 'Local<' in m.group(1):
            continue
        uses = sorted(refs.get(var.get('id'), []), key=lambda r: A.begin(r[0]))
        if not uses:
            continue
        first, parent = uses[0]
        w = written_only(fn, first, parent)
        if w is None or w[0].get('kind') not in ('BinaryOperator', 'CXXOperatorCallExpr') or \
                (w[0].get('kind') == 'BinaryOperator' and w[0].get('opcode') != '=') or \
                (w[0].get('kind') == 'CXXOperatorCallExpr' and callee_name(w[0]) != 'operator='):
            continue
        s = statement_of(fn, w[0])
        block = fn.up(stmt)
        if s is None or fn.up(s) is not block or w[1] is None:
            continue
        if any(x.get('kind') == 'DeclRefExpr' and (x.get('referencedDecl') or {}).get('id') == var.get('id') for x, _ in A.walk(w[1])):
            continue
        # A reference declared between them, or a Local claimed in between, is not moved past.
        again = any(written_only(fn, r, q) is not None for r, q in uses[1:])
        kind = m.group(1).strip()
        constant = '' if again or kind.startswith('const') or kind.endswith('*') else 'const '
        ctx.rw.edits.add(fn.path, A.begin(w[0]), A.end(w[0]), '%s%s %s = %s' % (constant, kind, name, fn.source(w[1])))
        ctx.rw.remove_lines(fn, A.begin(stmt), ctx.rw.past_semicolon(fn.text, A.end(stmt)))
        if ctx.rw.edits.commit():
            ctx.report.did('tidy', fn)


def pass_tidy(ctx, fn):
    """What the deletions leave: braces round the one statement left, a condition with nothing to do."""
    if ctx.touched is not None and (fn.rel, fn.name) not in ctx.touched:
        return
    join_declarations(ctx, fn)
    text = fn.text
    for n, p in A.walk(fn.body):
        if n.get('kind') != 'CompoundStmt' or p is None or p.get('kind') not in ('IfStmt', 'ForStmt', 'WhileStmt', 'CXXForRangeStmt'):
            continue
        if A.in_macro(n):
            continue
        inside = A.children(n)
        start, stop = A.begin(n), A.end(n)
        if not inside:
            # `if (c) { }` with nothing left in it.
            if p.get('kind') == 'IfStmt' and ctx.rw.remove(fn, n):
                ctx.rw.edits.commit()
                ctx.report.did('tidy', fn)
            ctx.rw.edits.rollback()
            continue
        if len(inside) != 1 or inside[0].get('kind') in ('DeclStmt', 'IfStmt', 'NullStmt', 'CaseStmt', 'LabelStmt', 'ForStmt', 'WhileStmt', 'DoStmt',
                                                         'SwitchStmt', 'CXXForRangeStmt'):
            continue
        one = inside[0]
        open_line = text.line_start(start)
        close_line = text.line_start(stop - 1)
        if text.raw[open_line:start].strip() or text.raw[close_line:stop - 1].strip():
            continue
        if text.raw[stop:text.line_end(stop)].strip():
            continue
        if text.raw[start + 1:text.line_end(start)].strip():
            continue
        between = text.raw[start + 1:stop - 1]
        if '//' in between or '/*' in between:
            continue
        ctx.rw.edits.add(fn.path, open_line, text.line_end(start) + 1, '')
        ctx.rw.edits.add(fn.path, close_line, text.line_end(stop) + 1, '')
        if ctx.rw.edits.commit():
            ctx.report.did('tidy', fn)
    # `return f();` left of a lambda that only returns a constant.
    refs = refs_of(fn)
    for var, stmt in local_vars(fn):
        lam = lambda_of(var)
        if lam is None or A.in_macro(stmt):
            continue
        body = [c for c in A.children(lam) if c.get('kind') == 'CompoundStmt'][-1]
        inside = A.children(body)
        if len(inside) != 1 or inside[0].get('kind') != 'ReturnStmt' or not A.children(inside[0]):
            continue
        value = A.strip_implicit(A.children(inside[0])[0])
        if value.get('kind') not in ('IntegerLiteral', 'CXXBoolLiteralExpr', 'FloatingLiteral'):
            continue
        calls = [ref_call(fn, ref) for ref, _ in refs.get(var.get('id'), [])]
        if not calls or any(c is None or len(A.children(c)) != 2 for c in calls):
            continue
        for c in calls:
            ctx.rw.edits.add(fn.path, A.begin(c), A.end(c), fn.source(value))
        ctx.rw.remove_lines(fn, A.begin(stmt), ctx.rw.past_semicolon(text, A.end(stmt)))
        if ctx.rw.edits.commit():
            ctx.report.did('tidy', fn)



# ---- what the passes share: registrations, the frame table, signatures, constants ----------------

REGISTRATION = re.compile(r'^\s*READABLE_(FUNCTION(?:_WAITS|_VU0)?)(_IN)?\(([^)]*)\)', re.M)
GAME_NUMBER = {'ai': 0, 'dm2': 1, 'bmx': 2}
FRAME_ROW = re.compile(r'\{(\d+)u, 0x([0-9a-fA-F]+)u, 0x([0-9a-fA-F]+)u, \d+u, \d+u, \d+u, \d+u\}')
LOCALS_TSV = os.path.join(ztree.TOOLS, 'readable_locals.tsv')
SIZES = {'unsigned int': (4, 4), 'int': (4, 4), 'uint32_t': (4, 4), 'int32_t': (4, 4), 'float': (4, 4), 'ee::Float': (4, 4), 'Float': (4, 4),
         'uint16_t': (2, 2), 'int16_t': (2, 2), 'short': (2, 2), 'unsigned short': (2, 2), 'uint8_t': (1, 1), 'int8_t': (1, 1), 'char': (1, 1),
         'unsigned char': (1, 1), 'uint64_t': (8, 8), 'int64_t': (8, 8), 'Vec4': (16, 16), 'zge::Vec4': (16, 16), 'Vec3': (12, 4), 'zge::Vec3': (12, 4),
         'Matrix': (64, 16), 'zge::Matrix': (64, 16), 'Quaternion': (16, 16), 'zge::Quaternion': (16, 16)}


def symbols(tree):
    """{name: address} of the functions and data in tools/readable_symbols.tsv (none before zbind lands)."""
    path = ztree.zlayout.path(tree.tree, 'tools', 'readable', 'readable_symbols.tsv')
    table = {}
    if os.path.exists(path):
        for line in open(path, errors='replace'):
            f = line.rstrip('\n').split('\t')
            if len(f) >= 3 and f[0] in ('function', 'data') and not (len(f) > 4 and f[4]):
                table[f[1]] = int(f[2], 16)
    return table


def registrations(tree, table):
    """{(file, short name): [(game, address, kind, status)]}, and {address: (file, name)} of AI's."""
    by_name, by_address = collections.defaultdict(list), {}
    for path in tree.files:
        code = re.sub(r'//[^\n]*', '', tree.texts[path].raw)
        for m in REGISTRATION.finditer(code):
            args = [a.strip() for a in m.group(3).split(',')]
            game = 'ai'
            if m.group(2):
                game, args = args[0], args[1:]
            if len(args) == 2:
                args = ['sym::' + args[0].split('::')[-1]] + args
            if len(args) < 3:
                continue
            spelled = args[0]
            if spelled.startswith('sym::'):
                address = table.get(spelled[5:])
            elif re.fullmatch(r'0[xX][0-9a-fA-F]+u?', spelled):
                address = int(spelled.rstrip('uU'), 16)
            else:
                address = None
            if address is None:
                continue
            short = args[1].split('::')[-1]
            by_name[(path, short)].append((game, address, m.group(1), args[2]))
            if game == 'ai':
                by_address[address] = (path, short)
    return by_name, by_address


STACK_ROW = re.compile(r'\{(\d+)u, 0x([0-9a-fA-F]+)u, (\d+)u, (\d+)u\}')


def stack_arguments(build):
    """{address: bytes} of the functions that read arguments off their caller's stack."""
    rows = {}
    path = os.path.join(build, 'runtime', 'fncheck_build', 'fncheck_stack_args.cpp')
    text = open(path, errors='replace').read()
    for m in STACK_ROW.finditer(text[:text.find('}};') if '}};' in text else len(text)]):
        if int(m.group(1)) == 0:
            rows[int(m.group(2), 16)] = int(m.group(4))
    return rows


def frame_rows(build):
    rows = {}
    path = os.path.join(build, 'runtime', 'fncheck_build', 'readable_frames.cpp')
    for m in FRAME_ROW.finditer(open(path, errors='replace').read()):
        rows[(int(m.group(1)), int(m.group(2), 16))] = int(m.group(3), 16)
    return rows


def constant_of(node):
    """The value of an integer constant expression, as far as the AST says it plainly."""
    n = A.strip_implicit(node)
    kind = n.get('kind')
    if kind == 'IntegerLiteral':
        return int(n['value'], 0)
    if kind == 'ConstantExpr' and 'value' in n:
        return int(n['value'], 0)
    if 'value' in node and node.get('kind') == 'ConstantExpr':
        return int(node['value'], 0)
    if kind == 'BinaryOperator':
        a, b = [constant_of(c) for c in A.children(n)]
        if a is None or b is None:
            return None
        if n.get('opcode') == '<<':
            return a << b if 0 <= b < 64 else None
        return {'+': a + b, '-': a - b, '*': a * b}.get(n.get('opcode'))
    return None


def size_of(tree_sizes, t):
    t = t.replace('const ', '').strip()
    return SIZES.get(t) or tree_sizes.get(t.split('::')[-1])


def struct_sizes(tree):
    """{type: (size, alignment)} from the static_asserts of the tree (sizeof(T) == N)."""
    sizes = {}
    for path in tree.files:
        raw = tree.texts[path].raw
        for m in re.finditer(r'static_assert\(sizeof\((\w+)\)\s*==\s*(0x[0-9A-Fa-f]+|\d+)', raw):
            n = int(m.group(2), 0)
            sizes[m.group(1)] = (n, 16 if re.search(r'alignas\(16\)\s+%s\b' % m.group(1), raw) else 4)
    return sizes


def skip_up(fn, node, kinds=('ImplicitCastExpr', 'ParenExpr', 'ExprWithCleanups', 'MaterializeTemporaryExpr')):
    """The first ancestor that is not an implicit step, and the child it was reached from."""
    n, p = node, fn.up(node)
    while p is not None and p.get('kind') in kinds:
        n, p = p, fn.up(p)
    return n, p


INT32_NAMES = {'uint32_t', 'int32_t', 'int', 'unsigned int'}
TYPE_NAMES = {'char': 'text', 'Vec4': 'vector', 'Vec3': 'vector', 'Matrix': 'matrix', 'Quaternion': 'rotation', 'Float': None, 'uint8_t': None}


def name_from_type(t):
    """A local's name when only its type says what it is: a struct's name."""
    if t is None:
        return 'buffer'
    short = t.split('::')[-1].replace('const ', '').strip()
    if short in TYPE_NAMES:
        return TYPE_NAMES[short]
    if re.fullmatch(r'[A-Z]\w*', short):
        return short[0].lower() + short[1:]
    return None


class Local:
    """One place of a frame: the uses at that offset."""

    def __init__(self, offset):
        self.offset = offset
        self.types = []      # what `at<T>` and `Ptr<T>` call it
        self.names = []      # (priority, name)
        self.uses = []       # (form, call node, extra)
        self.escapes = False
        self.cast = False
        self.subscript = False


FRAME_WRAPPERS = ('ImplicitCastExpr', 'ParenExpr', 'ExprWithCleanups', 'MaterializeTemporaryExpr')
FRAME_TYPE = 'ee::StackFrame'
NON_ESCAPING_MEMBERS = {'load', 'store', 'dot', 'normalize', 'normalize4', 'difference', 'sum', 'scale', 'absolute', 'cross', 'squaredLength',
                        'transform', 'rotate'}


def frame_var(fn):
    found = []
    for n, p in A.walk(fn.body):
        if n.get('kind') == 'VarDecl' and A.desugar(n.get('type', {})) == FRAME_TYPE:
            found.append((n, p))
    # (a lambda's body is walked twice)
    unique = {A.begin(n): (n, p) for n, p in found}
    return list(unique.values())


class FrameRewrite:
    """A function's StackFrame as ee::Local objects and plain values, or the reason it stays."""

    def __init__(self, ctx, fn, var, stmt):
        self.ctx, self.fn, self.var, self.stmt = ctx, fn, var, stmt
        self.locals = {}
        self.vars = []       # (VarDecl, DeclStmt, form, Local)
        self.problem = None
        self.size = 0

    def local(self, offset):
        return self.locals.setdefault(offset, Local(offset))

    def fail(self, reason):
        self.problem = self.problem or reason
        return None

    def collect(self):
        fn = self.fn
        seen = set()
        for n, p in A.walk_code(fn.decl):
            if n.get('kind') != 'DeclRefExpr' or (n.get('referencedDecl') or {}).get('id') != self.var.get('id'):
                continue
            where = A.begin(n)
            if where in seen:
                continue
            seen.add(where)
            member = p
            while member is not None and member.get('kind') in ('ImplicitCastExpr',):
                member = fn.up(member)
            if member is None or member.get('kind') != 'MemberExpr' or member.get('name') not in ('at', 'address'):
                if member is not None and member.get('kind') == 'MemberExpr' and member.get('name') == 'saveEntry':
                    continue
                return self.fail('the frame is handed on')
            call = fn.up(member)
            if call is None or call.get('kind') != 'CXXMemberCallExpr':
                return self.fail('the frame is handed on')
            self.use(member.get('name'), call)
            if self.problem:
                return None
        return True

    def use(self, what, call):
        fn = self.fn
        args = A.children(call)[1:]
        offset = 0
        if args and args[0].get('kind') != 'CXXDefaultArgExpr':
            offset = constant_of(args[0])
            if offset is None:
                # A named constant of the function or the file.
                offset = self.ctx.constants.get(fn.source(args[0]).strip())
        if offset is None:
            return self.fail('a place in the frame that is not a constant')
        if offset >= self.size:
            return self.fail('a stack argument: above its own frame, it has to become a parameter')
        loc = self.local(offset)
        n, p = skip_up(fn, call)
        source = fn.source(call)
        t = None
        if what == 'at':
            m = re.match(r'[\w.]+\s*\.\s*at<(.*)>\s*\(', source, re.S)
            if not m:
                return self.fail('frame.at without a type')
            t = self.spelled(m.group(1).strip(), call)
            loc.types.append(t)
            if p is not None and p.get('kind') == 'UnaryOperator' and p.get('opcode') == '*':
                d, q = skip_up(fn, p)
                declared = q.get('type', {}).get('qualType', '') if q is not None and q.get('kind') == 'VarDecl' else ''
                if declared.endswith('&') or re.search(r'\(&\)\[\d+\]$', declared):
                    loc.names.append((0, q.get('name')))
                    self.vars.append((q, self.decl_statement(q), 'reference', loc))
                    return
                loc.uses.append(('value', p, t))
                self.name_from_value(loc, p, q)
                return
            if p is not None and p.get('kind') == 'VarDecl':
                loc.names.append((0, p.get('name')))
                self.vars.append((p, self.decl_statement(p), 'pointer', loc))
                return
            if p is not None and p.get('kind') == 'MemberExpr' and p.get('isArrow'):
                loc.uses.append(('arrow', call, p))
                return
            if p is not None and p.get('kind') == 'ArraySubscriptExpr':
                loc.uses.append(('subscript', call, None))
                loc.subscript = True
                return
            loc.uses.append(('pointer', call, None))
            if self.call_is_harmless(call):
                return
            loc.escapes = True
            return
        # frame.address(n)
        if p is not None and p.get('kind') == 'VarDecl':
            loc.names.append((0, p.get('name')))
            self.vars.append((p, self.decl_statement(p), 'integer', loc))
            loc.escapes = True
            return
        if p is not None and p.get('kind') == 'CXXConstructExpr' and 'ee::Ptr<' in A.desugar(p.get('type', {})):
            typed = re.sub(r'^const\s+', '', A.desugar(p.get('type', {})))
            wrapper = fn.up(p)
            cast = wrapper if wrapper is not None and wrapper.get('kind') == 'CXXFunctionalCastExpr' else None
            spelled = fn.source(cast) if cast else None
            m = re.match(r'ee::Ptr<(.*?)>\s*\(', spelled or '', re.S)
            inner = self.spelled(m.group(1).strip(), None, typed[len('ee::Ptr<'):-1]) if m else self.spelled(typed[len('ee::Ptr<'):-1], None)
            if inner not in ('void',):
                loc.types.append(inner)
            q = fn.up(cast) if cast else wrapper
            while q is not None and q.get('kind') in FRAME_WRAPPERS:
                q = fn.up(q)
            if cast is None and q is not None and q.get('kind') == 'VarDecl':
                loc.names.append((0, q.get('name')))
                self.vars.append((q, self.decl_statement(q), 'ptr', loc))
                loc.escapes = True
                return
        loc.escapes = True
        loc.uses.append(('address', call, None))
        name, typed = self.name_from_call(call)
        if name:
            loc.names.append((1, name))
        if typed:
            loc.types.append(typed)

    def spelled(self, t, call, full=None):
        """A local's type as it can be written where the frame was: `Vec4` is only known below a `using` that may come
        later, so a class goes by its name as clang has it; a function's own namespaces and an anonymous one fall away."""
        fn = self.fn
        if (call is not None or full) and re.fullmatch(r'(const )?[\w:]+', t) and not re.fullmatch(r'(const )?(ee::\w+|u?int\d+_t|float|char|.*Float\w*)', t):
            if call is not None:
                full = re.sub(r'^const\s+', '', A.desugar(call.get('type', {}))).strip()
                full = full[:-1].strip() if full.endswith('*') else ''
            full = re.sub(r'^const\s+', '', full)
            if full and '(' not in full.replace('(anonymous namespace)', ''):
                t = full
        known = self.ctx.shared.full_names.get(re.sub(r'^const\s+', '', t))
        return clean_type(known or t, fn.namespaces)

    def name_from_value(self, loc, node, parent):
        """`const T x = *place;` or `*place = x;`: the place is x's slot."""
        fn = self.fn
        name = None
        if parent is not None and parent.get('kind') == 'VarDecl':
            name = parent.get('name')
        elif parent is not None and parent.get('kind') == 'BinaryOperator' and parent.get('opcode') == '=':
            inner = A.children(parent)
            if A.strip_implicit(inner[0]) is node or inner[0] is node:
                name = first_identifier(fn.source(inner[1]))
        if name and name not in KEYWORDS:
            loc.names.append((2, name + 'Slot'))

    def call_is_harmless(self, call):
        """A pointer handed to memcpy or a VU0 macro: it goes no further."""
        n, p = skip_up(self.fn, call)
        if p is None:
            return False
        if p.get('kind') == 'CallExpr' and callee_name(p) in ('memcpy', 'memset', '__builtin___memcpy_chk', '__builtin___memset_chk'):
            return True
        if p.get('kind') == 'CXXMemberCallExpr' and callee_name(p) in NON_ESCAPING_MEMBERS and 'Machine' in member_base_type(p):
            return True
        return False

    def decl_statement(self, var):
        p = self.fn.up(var)
        if p is None or p.get('kind') != 'DeclStmt' or (self.fn.up(p) or {}).get('kind') not in ('CompoundStmt', 'CaseStmt', 'DefaultStmt'):
            self.fail('a frame variable declared outside a block')
        return p

    def check_statements(self):
        """A declaration goes whole, so every variable of it must be the frame's."""
        ours = {id(v) for v, _, _, _ in self.vars}
        for var, stmt, _, _ in self.vars:
            if stmt is not None and any(id(c) not in ours for c in A.children(stmt)):
                return self.fail('a frame variable declared with others')
        return True

    def name_from_call(self, call):
        """What a callee calls the parameter this address goes to."""
        fn = self.fn
        n, p = skip_up(fn, call)
        while p is not None and p.get('kind') in ('CXXConstructExpr', 'CXXFunctionalCastExpr', 'CStyleCastExpr', 'CXXStaticCastExpr'):
            n, p = skip_up(fn, p)
        if p is None or p.get('kind') != 'CallExpr':
            return None, None
        site = guest_site(self.ctx, fn, p)
        if site is None:
            return None, None
        site.address = target_address(self.ctx, fn, site)
        for i, a in enumerate(site.args):
            if a is n or A.begin(a) == A.begin(n):
                names = parameter_names(self.ctx, site)
                name = names[i] if names and i < len(names) and names[i] else None
                typed = None
                known = self.ctx.shared.declared_types.get(self.ctx.by_address.get(site.address))
                if known and i < len(known):
                    pointer = r'(?:const )?ee::Ptr<(?:const )?(.+)>|(?:const )?(.+?)\s*\*'
                    m = re.fullmatch(pointer, known[i][0])
                    if m and (m.group(1) or m.group(2)) not in ('void', 'char', 'uint8_t'):
                        full = re.fullmatch(pointer, known[i][1])
                        typed = self.spelled((m.group(1) or m.group(2)).strip(), None, (full.group(1) or full.group(2)).strip() if full else None)
                return name, typed
        return None, None

    def choose(self):
        if not self.check_statements():
            return None
        taken = set(re.findall(r'\b[A-Za-z_]\w*\b', self.fn.source(self.fn.decl)))
        for offset, loc in sorted(self.locals.items()):
            types = []
            for t in loc.types:
                t = t.replace('const ', '').strip()
                if t not in types:
                    types.append(t)
            short = {t.split('::')[-1] for t in types}
            if len(short) > 1:
                if short <= INT32_NAMES or short <= {'uint16_t', 'int16_t', 'short'} or short <= {'uint8_t', 'int8_t', 'char'}:
                    loc.cast = True   # one integer of one size, read with either sign
                else:
                    return self.fail('one place read as two types (%s)' % ', '.join(types))
            loc.type = types[0] if types else None
            if not loc.names:
                name = name_from_type(loc.type)
                if name is None:
                    return self.fail('a local nothing names (offset 0x%x)' % offset)
                loc.names.append((3, name))
            priority, name = sorted(loc.names, key=lambda x: x[0])[0]
            if priority:  # from a parameter of a callee or the like: it must be free in this function
                base = name
                i = 2
                while name in taken:
                    name = '%s%d' % (base, i)
                    i += 1
                taken.add(name)
            loc.name = name
            loc.array = None
            after = [o for o in self.locals if o > offset]
            if loc.type is None:
                # Only its address is used: as many bytes as there are up to the next place.
                loc.type = 'uint8_t'
                loc.array = (min(after) if after else self.size) - offset
            elif loc.subscript:
                # Indexed: an array up to the next place.
                known = size_of(self.ctx.sizes, loc.type) or (loc.type.startswith('ee::Ptr<') and (4, 4)) or None
                if known and '[' not in loc.type:
                    loc.array = ((min(after) if after else self.size) - offset) // known[0]
        self.choose_plain()
        return True

    def choose_plain(self):
        """A place whose address goes nowhere, that is whole before anything reads it, and that nothing overlaps, is a
        plain value; anything else stays on the guest stack, where it always was."""
        spans = []
        for offset, loc in self.locals.items():
            known = size_of(self.ctx.sizes, loc.type)
            spans.append((offset, offset + known[0] if known else None))
        # What a callee reads off this frame as its stack arguments is the callee's, address taken or not.
        outgoing = 0
        for n, _ in A.walk_code(self.fn.body):
            if n.get('kind') == 'CallExpr':
                site = guest_site(self.ctx, self.fn, n)
                if site is not None:
                    outgoing = max(outgoing, self.ctx.shared.stack_arguments.get(target_address(self.ctx, self.fn, site), 0))
        # A pointer handed on reaches past its own type: a message's second word, a record the signature calls a word
        # (setSoundStreamVolume's volume, chooseSaveFile's slot name). Whatever lies above an address that goes on stays there.
        handed = [o for o, l in self.locals.items() if l.escapes or l.array]
        lowest = min(handed) if handed else None
        for offset, loc in self.locals.items():
            loc.plain = False
            known = size_of(self.ctx.sizes, loc.type)
            if loc.escapes or known is None or loc.type.endswith('char') or loc.array or offset < outgoing:
                continue
            if lowest is not None and offset > lowest:
                continue
            stop = offset + known[0]
            if any(o != offset and (e is None or (o < stop and offset < e)) for o, e in spans):
                continue
            events = self.events_of(loc)
            if events is not None and whole_first(self.fn, events):
                loc.plain = True

    def events_of(self, loc):
        """[(node, kind)] of a place: 'whole' writes all of it, 'read' anything else; None if its address goes on."""
        fn = self.fn
        events = []

        def value_event(node):
            n, p = skip_up(fn, node)
            kind = p.get('kind') if p else None
            inner = A.children(p) if p else []
            if kind == 'BinaryOperator' and p.get('opcode') == '=' and inner[0] is n:
                return p, 'whole'
            if kind == 'CXXOperatorCallExpr' and callee_name(p) == 'operator=' and len(inner) > 1 and A.strip_implicit(inner[1]) is A.strip_implicit(node):
                return p, 'whole'
            return node, 'read'

        def pointer_event(node):
            """A pointer handed on: to memcpy or a VU0 macro only."""
            n, p = skip_up(fn, node)
            if p is None:
                return None
            if p.get('kind') == 'CallExpr' and callee_name(p) in ('memcpy', 'memset', '__builtin___memcpy_chk', '__builtin___memset_chk'):
                args = A.children(p)[1:]
                bytes_ = constant_of(args[2]) if len(args) > 2 else None
                known = size_of(self.ctx.sizes, loc.type)
                if args and args[0] is n and bytes_ is not None and known and bytes_ == known[0]:
                    return p, 'whole'
                return p, 'read'
            if p.get('kind') == 'CXXMemberCallExpr' and 'Machine' in member_base_type(p) and callee_name(p) in NON_ESCAPING_MEMBERS:
                args = A.children(p)[1:]
                if args and args[-1] is n and callee_name(p) in ('sum', 'difference', 'scale', 'normalize', 'cross', 'transform', 'rotate', 'absolute') \
                        and len(args) >= 2:
                    return p, 'whole'
                return p, 'read'
            return None

        for form, node, extra in loc.uses:
            if form == 'value':
                events.append(value_event(node))
            elif form == 'arrow':
                events.append((node, 'read'))
            elif form == 'pointer':
                e = pointer_event(node)
                if e is None:
                    return None
                events.append(e)
            else:
                return None
        for var, stmt, form, l in self.vars:
            if l is not loc:
                continue
            for ref, parent in var_refs(fn, var):
                n, p = skip_up(fn, ref)
                kind = p.get('kind') if p else None
                if form == 'reference':
                    if kind == 'UnaryOperator' and p.get('opcode') == '&':
                        e = pointer_event(p)
                        if e is None:
                            return None
                        events.append(e)
                    elif kind in ('CallExpr', 'CXXMemberCallExpr') or (kind == 'CXXConstructExpr' and 'Local' in A.desugar(p.get('type', {}))):
                        return None
                    elif kind == 'CXXOperatorCallExpr' and callee_name(p) not in ('operator=', 'operator+', 'operator-', 'operator*', 'operator/',
                                                                               'operator<', 'operator>', 'operator<=', 'operator>=', 'operator==',
                                                                               'operator!=', 'operator+=', 'operator-=', 'operator*=', 'operator/='):
                        return None
                    else:
                        events.append(value_event(ref))
                elif form == 'pointer':
                    if kind == 'UnaryOperator' and p.get('opcode') == '*':
                        events.append(value_event(p))
                    elif kind == 'MemberExpr' and p.get('isArrow'):
                        events.append((ref, 'read'))
                    else:
                        e = pointer_event(ref)
                        if e is None:
                            return None
                        events.append(e)
                else:
                    return None
        return events


def pass_frames(ctx, fn):
    frames = frame_var(fn)
    if not frames or fn.path in ctx.shared.blocked_files:
        return
    if len(frames) > 1:
        ctx.report.keep('frames', fn, fn.body, 'two StackFrames')
        return
    var, stmt = frames[0]
    regs = ctx.regs.get((fn.path, fn.name))
    if not regs:
        ctx.report.keep('frames', fn, var, 'a helper: its frame is its caller\'s business')
        return
    if (fn.path, fn.name) in ctx.shared.plain_called:
        ctx.report.keep('frames', fn, var, 'called as plain C++ (%s): a Local would land in the caller\'s frame' % ctx.shared.plain_called[(fn.path, fn.name)])
        return
    size = constant_of(A.children(var)[0]) if False else None
    for n, _ in A.walk(var):
        if n.get('kind') == 'IntegerLiteral':
            size = int(n['value'], 0)
            break
    if size is None:
        ctx.report.keep('frames', fn, var, 'a frame whose size is not a literal')
        return
    for game, address, _, _ in regs:
        if ctx.frames.get((GAME_NUMBER.get(game, 0), address)) != size:
            ctx.report.keep('frames', fn, var, 'the frame is not the table\'s (0x%x)' % size)
            return
    work = FrameRewrite(ctx, fn, var, stmt)
    work.size = size
    if not work.collect() or work.problem or not work.choose() or work.problem:
        ctx.report.keep('frames', fn, var, work.problem or 'frame not understood')
        return
    if not rewrite_frame(ctx, fn, work, regs, size):
        ctx.report.keep('frames', fn, var, work.problem or 'frame not understood')
        ctx.rw.edits.rollback()
        return
    if ctx.rw.edits.commit():
        ctx.report.did('frames', fn)
        ctx.shared.rows[(fn.path, fn.name)] = work.rows


def is_write(fn, node):
    n, p = skip_up(fn, node)
    return p is not None and (p.get('kind') == 'CompoundAssignOperator' or
                              (p.get('kind') == 'BinaryOperator' and p.get('opcode') == '=' and A.children(p)[0] is n) or
                              (p.get('kind') == 'UnaryOperator' and p.get('opcode') in ('++', '--', '&')))


def whole_first(fn, events):
    """Every read comes after a whole write that always runs before it."""
    if any(x.get('kind') in ('GotoStmt', 'LabelStmt') for x, _ in A.walk(fn.body)):
        return False
    writes = []
    reads = []
    for node, kind in events:
        if fn.enclosing(node, ('LambdaExpr',)) is not None:
            return False
        if kind == 'whole':
            s = node
            if statement_of(fn, node) is None and node.get('kind') not in ('CallExpr', 'CXXMemberCallExpr', 'CXXOperatorCallExpr'):
                reads.append(node)
                continue
            s = statement_of(fn, node)
            block = fn.up(s) if s is not None else None
            if block is not None and block.get('kind') == 'CompoundStmt':
                writes.append((s, block))
        else:
            reads.append(node)
    if not writes:
        return not reads and False
    for node in reads:
        where = A.begin(node)
        if not any(A.begin(s) < where and statement_in(fn, node, block) is not None for s, block in writes):
            return False
    return True


def var_refs(fn, var):
    out = {}
    for n, p in A.walk_code(fn.decl):
        if n.get('kind') == 'DeclRefExpr' and (n.get('referencedDecl') or {}).get('id') == var.get('id'):
            out.setdefault(A.begin(n), (n, p))
    return list(out.values())


def rewrite_var_uses(fn, edits, var, form, name, typed, problem, plain=False):
    """Every use of a variable the frame gave a name to, now the Local `name`, or the plain value `name`. typed: its type."""
    for ref, parent in var_refs(fn, var):
        n, p = skip_up(fn, ref)
        start, stop = A.begin(ref), A.end(ref)
        written = written_only(fn, ref, parent)
        kind = p.get('kind') if p else None
        if plain:
            if form == 'reference':
                if name != var.get('name'):
                    edits.add(fn.path, start, stop, name)
            elif kind == 'UnaryOperator' and p.get('opcode') == '*':
                edits.add(fn.path, A.begin(p), stop, name)
            elif kind == 'MemberExpr' and p.get('isArrow'):
                edits.add(fn.path, start, stop, name)
                arrow = fn.text.raw.find('->', stop)
                edits.add(fn.path, arrow, arrow + 2, '.')
            else:
                edits.add(fn.path, start, stop, '&' + name)
            continue
        if form == 'reference' and '[' in (typed or ''):
            # A Local array indexes and hands itself on as its first element does.
            if kind == 'ArraySubscriptExpr':
                edits.add(fn.path, start, stop, name)
            else:
                edits.add(fn.path, start, stop, '&%s[0]' % name)
        elif form == 'reference':
            if kind == 'MemberExpr' and not p.get('isArrow'):
                edits.add(fn.path, start, stop, name)
                dot = fn.text.raw.find('.', stop)
                edits.add(fn.path, dot, dot + 1, '->')
            elif kind == 'UnaryOperator' and p.get('opcode') == '&':
                edits.add(fn.path, A.begin(p), stop, '&*' + name)
            elif kind == 'CXXMemberCallExpr' or (kind == 'MemberExpr' and p.get('isArrow')) or \
                    (kind == 'CXXOperatorCallExpr' and callee_name(p) in ('operator[]', 'operator->', 'operator()')):
                edits.add(fn.path, start, stop, '(*%s)' % name)
            else:
                edits.add(fn.path, start, stop, '*' + name)
        elif form == 'pointer':
            if written is not None:
                problem.append('a frame pointer that is moved')
                return False
            if kind == 'UnaryOperator' and p.get('opcode') == '*':
                edits.add(fn.path, start, stop, name)
            elif kind == 'MemberExpr' and p.get('isArrow'):
                edits.add(fn.path, start, stop, name)
            elif kind == 'ArraySubscriptExpr':
                edits.add(fn.path, start, stop, '(&*%s)' % name)
            else:
                edits.add(fn.path, start, stop, '&*' + name)
        elif form == 'integer':
            if written is not None:
                problem.append('a frame address that is moved')
                return False
            edits.add(fn.path, start, stop, name + '.address()')
        elif form == 'ptr':
            if written is not None:
                problem.append('a frame pointer that is moved')
                return False
            if kind == 'MemberExpr' and p.get('name') == 'address':
                edits.add(fn.path, start, A.end(p), name + '.address()')
            elif kind == 'CXXOperatorCallExpr' and callee_name(p) in ('operator->', 'operator*'):
                edits.add(fn.path, start, stop, name)
            else:
                edits.add(fn.path, start, stop, 'ee::Ptr<%s>(%s.address())' % (typed, name))
    return True


def ensure_include(ctx, path, header='ee/plain.h'):
    """The file includes `header`: after its first project include of the ee folder, else its last one."""
    text = ctx.tree.texts[path]
    if '#include "%s"' % header in text.raw:
        return
    includes = list(re.finditer(r'^#include "([^"]+)"[^\n]*\n', text.raw, re.M))
    if not includes:
        ctx.rw.edits.add(path, 0, 0, '#include "%s"\n' % header)
        return
    ee = [m for m in includes if m.group(1).startswith('ee/')]
    at = (ee[-1] if ee else includes[-1]).end()
    ctx.rw.edits.add(path, at, at, '#include "%s"\n' % header)


def rewrite_frame(ctx, fn, work, regs, size):
    edits = ctx.rw.edits
    ensure_include(ctx, fn.path)
    text = fn.text
    problems = []
    for var, stmt, form, loc in work.vars:
        if stmt is None:
            return False
        if not rewrite_var_uses(fn, edits, var, form, loc.name, loc.type, problems, loc.plain):
            work.problem = problems[0]
            return False
        ctx.rw.remove_lines(fn, A.begin(stmt), ctx.rw.past_semicolon(text, A.end(stmt)))
    for offset, loc in work.locals.items():
        for form, node, extra in loc.uses:
            if form == 'value':
                spelled = ('' if loc.plain else '*') + loc.name
                if loc.cast and extra != loc.type and not is_write(fn, node):
                    spelled = 'static_cast<%s>(%s)' % (extra, spelled)
                edits.add(fn.path, A.begin(node), A.end(node), spelled)
            elif form == 'arrow':
                edits.add(fn.path, A.begin(node), A.end(node), loc.name)
                if loc.plain:
                    arrow = text.raw.find('->', A.end(node))
                    edits.add(fn.path, arrow, arrow + 2, '.')
            elif form == 'subscript':
                edits.add(fn.path, A.begin(node), A.end(node), loc.name if loc.array else '(&*%s)' % loc.name)
            elif form == 'pointer':
                edits.add(fn.path, A.begin(node), A.end(node), ('&' if loc.plain else '&*') + loc.name)
            else:
                edits.add(fn.path, A.begin(node), A.end(node), loc.name + '.address()')
    pad = indent_of(text, work.stmt)
    declared = []
    places = []
    for offset, loc in sorted(work.locals.items()):
        if loc.plain:
            declared.append('%s%s %s;' % (pad, loc.type, loc.name))
        else:
            declared.append('%see::Local<%s%s> %s;' % (pad, loc.type, '[0x%X]' % loc.array if loc.array else '', loc.name))
            places.append((loc.name, offset, None if loc.array or '[' in loc.type else size_of(ctx.sizes, loc.type)))
    # After the `using` lines that follow it, which its types may need.
    block = fn.up(work.stmt)
    siblings = A.children(block) if block else []
    index = next((i for i, c in enumerate(siblings) if c is work.stmt), None)
    after = work.stmt
    while index is not None and index + 1 < len(siblings):
        next_stmt = siblings[index + 1]
        if next_stmt.get('kind') == 'DeclStmt' and A.children(next_stmt)[0].get('kind') in ('UsingDecl', 'UsingDirectiveDecl', 'TypeAliasDecl', 'NamespaceAliasDecl',
                                                                                             'TypedefDecl', 'CXXRecordDecl'):
            after, index = next_stmt, index + 1
        else:
            break
    # (with what its line says of the frame)
    edits.add(fn.path, text.line_start(A.begin(work.stmt)), text.line_end(ctx.rw.past_semicolon(text, A.end(work.stmt))) + 1, '')
    # After a type of the function's own that a Local is of.
    for c in siblings[(index or 0) + 1:]:
        d = A.children(c)[0] if c.get('kind') == 'DeclStmt' and A.children(c) else {}
        if d.get('kind') == 'CXXRecordDecl' and any(re.search(r'\b%s\b' % re.escape(d.get('name') or '?'), x) for x in declared):
            after = c
    insert = ctx.rw.past_semicolon(text, A.end(after))
    insert = text.line_end(insert) + 1
    edits.add(fn.path, insert, insert, '\n'.join(declared) + ('\n' if declared else ''))
    work.rows = []
    next_free = 0
    in_order = True
    for name, offset, known in places:
        if known is None:
            in_order = False
            break
        at = (next_free + known[1] - 1) & ~(known[1] - 1)
        if at != offset:
            in_order = False
            break
        next_free = at + known[0]
    if not in_order:
        for game, address, _, _ in regs:
            for ordinal, (name, offset, _) in enumerate(places):
                work.rows.append((GAME_NUMBER.get(game, 0), address, fn.name, ordinal, name, offset))
    return True


def write_locals(rows):
    """Adds rows to readable_locals.tsv. The rows there stay as they are and where they are (the file is hand-edited
    and merged, so a re-sort is a diff of every line): a row of the same function and ordinal is replaced in place, a
    new one goes after the last row of a smaller key, which in a sorted file is its sorted place."""
    lines = [line.rstrip('\n') for line in open(LOCALS_TSV, encoding='utf-8')]

    def key(line):
        if line.startswith('#') or not line.strip():
            return None
        f = line.split('\t')
        return (int(f[0]), int(f[1], 16), int(f[3]))
    where = {key(line): i for i, line in enumerate(lines) if key(line) is not None}
    for game, address, name, ordinal, local, offset in rows:
        k = (game, address, ordinal)
        row = '%d\t0x%08X\t%s\t%d\t%s\t0x%02X' % (game, address, name, ordinal, local, offset)
        if k in where:
            lines[where[k]] = row
            continue
        at = max([i for i, line in enumerate(lines) if key(line) is not None and key(line) < k], default=None)
        if at is None:
            at = max([i for i, line in enumerate(lines) if line.startswith('#')], default=-1)
        lines.insert(at + 1, row)
        where = {key(line): i for i, line in enumerate(lines) if key(line) is not None}
    with open(LOCALS_TSV, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')


# ---- calls ----------------------------------------------------------------------------------------


class Site:
    """A call of a game function."""

    def __init__(self, node, form, target, name, args, result):
        self.node, self.form, self.target, self.name, self.args, self.result = node, form, target, name, args, result


GUEST_FORMS = re.compile(r'(ee::(?:call|tailCall)|game::(?:call|tailCall|callAt|tailCallAt))\b(?:<(.*?)>)?\s*\(', re.S)


def guest_site(ctx, fn, call):
    """A CallExpr as a call of a game function, or None."""
    begin = A.begin(call)
    if begin is None or A.end(call) is None or call.get('kind') != 'CallExpr':
        return None
    head = fn.text.raw[begin:begin + 8]
    if not head.startswith(('ee::call', 'ee::tail', 'ee::game', 'game::ca', 'game::ta')):
        return None
    source = fn.text.raw[begin:A.end(call)]
    if source.startswith(('ee::calleeSaves', 'ee::callerRegister', 'ee::callerFloat', 'ee::callKeepingVu0')):
        return None
    m = re.match(r'(ee::(?:call|tailCall|game)|game::(?:call|tailCall|callAt|tailCallAt))', source)
    if not m:
        return None
    form = m.group(1)
    i = m.end()
    template = None
    if source[i:i + 1] == '<':
        depth, j = 0, i
        while j < len(source):
            depth += source[j] == '<'
            depth -= source[j] == '>'
            if depth == 0:
                break
            j += 1
        template = source[i + 1:j]
        i = j + 1
    if source[i:i + 1] != '(':
        return None
    inner = A.children(call)[1:]
    if form.startswith('ee::'):
        if not inner:
            return None
        target = inner[0]
        return Site(call, form, target, None, inner[1:], template)
    name = (template or '').split(',')[0].strip().lstrip('&').strip()
    return Site(call, form, None, name, inner, None)


def target_address(ctx, fn, site):
    """The address a call goes to, when the code says it."""
    if site.target is None:
        return ctx.named_address(fn, site.name)
    t = A.strip_implicit(site.target)
    v = constant_of(site.target)
    if v is not None:
        return v
    if t.get('kind') == 'DeclRefExpr':
        name = (t.get('referencedDecl') or {}).get('name')
        if name in ctx.constants:
            return ctx.constants[name]
        return SYMBOL_TABLE.get(name) if fn.source(site.target).startswith('sym::') else None
    return None


def parameter_names(ctx, site):
    """The callee's parameter names, as far as they are known: the readable function's own, else the names table."""
    address = ctx.site_address(site)
    if address is None:
        return None
    known = ctx.by_address.get(address)
    return (ctx.shared.declared.get(known) if known else None) or ctx.params.get(address)


INT_TYPES = {'unsigned int', 'int', 'short', 'unsigned short', 'char', 'signed char', 'unsigned char', 'bool', 'uint32_t', 'int32_t', 'uint16_t',
             'int16_t', 'uint8_t', 'int8_t'}
WIDE_TYPES = {'unsigned long', 'unsigned long long', 'long', 'long long', 'size_t', 'uint64_t', 'int64_t'}
KEYWORDS = {'default', 'new', 'delete', 'this', 'register', 'class', 'struct', 'union', 'enum', 'template', 'typename', 'operator', 'auto', 'int',
            'char', 'float', 'double', 'long', 'short', 'bool', 'void', 'const', 'static', 'do', 'for', 'while', 'if', 'else', 'switch', 'case',
            'return', 'goto', 'break', 'continue', 'true', 'false', 'namespace', 'using', 'public', 'private', 'friend', 'virtual', 'signed',
            'unsigned', 'typedef', 'volatile', 'extern', 'inline', 'nullptr', 'sizeof', 'and', 'or', 'not', 'xor'}


class Argument:
    def __init__(self, text, kind, spelled, wrapper_body):
        self.node = None              # the site's expression
        self.text = text              # what the site passes
        self.kind = kind              # 'int', 'float', 'pointer', 'wide', 'other'
        self.spelled = spelled        # the parameter's type
        self.body = wrapper_body      # how the wrapper hands it on: '%s' with the parameter's name


def clean_type(t, namespaces):
    t = t.replace('(anonymous namespace)::', '').replace('struct ', '').replace('class ', '').replace('enum ', '')
    t = re.sub(r'^const\s+', '', t).strip()
    for long, short in (('unsigned long long', 'uint64_t'), ('unsigned int', 'uint32_t'), ('unsigned short', 'uint16_t'), ('unsigned char', 'uint8_t'),
                        ('signed char', 'int8_t'), ('long long', 'int64_t')):
        t = re.sub(r'\b%s\b' % long, short, t)
    t = re.sub(r'(?<![\w:])short\b', 'int16_t', re.sub(r'(?<![\w:])int\b', 'int32_t', t)) if '<' in t else t
    for ns in namespaces:
        t = t.replace(ns + '::', '')
    return t


def argument_of(ctx, fn, node, site_is_guest):
    a = argument_kind(ctx, fn, node, site_is_guest)
    a.node = node
    return a


def argument_kind(ctx, fn, node, site_is_guest):
    """What a site's argument is: its text, and the type the wrapper's parameter takes."""
    x = A.strip_implicit(node)
    t = A.desugar(node.get('type', {}))
    t0 = re.sub(r'^const\s+', '', t).strip()
    text = fn.source(node)
    namespaces = fn.namespaces
    # `ptr.address` and `local.address()`: the object itself.
    if x.get('kind') == 'MemberExpr' and x.get('name') == 'address' and A.children(x):
        base = A.children(x)[0]
        bt = re.sub(r'^const\s+', '', A.desugar(base.get('type', {})))
        if bt.startswith('ee::Ptr<') and not re.search(r'[+\-*/]', fn.source(base)):
            return Argument(fn.source(base), 'pointer', clean_type(bt, namespaces), '%s' if site_is_guest else '%s.address')
    if x.get('kind') == 'CXXMemberCallExpr' and callee_name(x) == 'address':
        bt = member_base_type(x)
        m = re.match(r'(?:const )?ee::Local<(.*)>$', bt)
        if m:
            base = fn.source(A.children(A.strip_implicit(A.children(x)[0]))[0])
            elem = m.group(1)
            e = re.match(r'(.*?)\s*\[\d+\]$', elem)
            elem = e.group(1) if e else elem
            return Argument(base, 'pointer', 'ee::Ptr<%s>' % clean_type(elem, namespaces), '%s' if site_is_guest else '%s.address')
    if t0 in INT_TYPES:
        return Argument(text, 'int', 'uint32_t', '%s')
    if t0 in ('float', 'ee::Float'):
        return Argument(text, 'float', 'ee::Float', '%s')
    if t0 in WIDE_TYPES:
        if constant_of(node) is not None or x.get('kind') == 'UnaryExprOrTypeTraitExpr' or re.match(r'sizeof\b', text):
            return Argument(text, 'int', 'uint32_t', '%s')
        return Argument(text, 'wide', clean_type(t0, namespaces), '%s')
    if t0.endswith('*'):
        local = re.fullmatch(r'&\*(\w+)', text)
        return Argument(local.group(1) if local else text, 'pointer', 'ee::Ptr<%s>' % clean_type(t0[:-1].strip(), namespaces), '%s')
    if t0.startswith('ee::Ptr<'):
        return Argument(text, 'pointer', clean_type(t0, namespaces), '%s')
    return Argument(text, 'other', clean_type(t0, namespaces), '%s')


class Wrapper:
    def __init__(self, name, tail):
        self.name, self.tail = name, tail
        self.signatures = collections.OrderedDict()   # parameter types -> Arguments of its first site
        self.results = collections.Counter()
        self.address = None
        self.target_text = None      # the first argument of ee::game, or the template of game::call
        self.form = None
        self.first = None
        self.scope = None
        self.not_small = set()       # (argument count, position) where an int site is not a small constant


def first_identifier(text):
    """A name for a parameter from what the first site hands it."""
    t = re.sub(r'\bee::(Ptr|Float|at)<[^>]*>', ' ', text)
    t = re.sub(r'static_cast<[^>]*>', ' ', t)
    names = re.findall(r'[A-Za-z_]\w*', t)
    names = [n for n in names if n not in KEYWORDS and n not in ('ee', 'zge', 'get', 'address', 'sizeof', 'offsetof', 'at', 'sym', 'std') and not n[0].isupper()]
    if not names:
        return None
    n = names[-1]
    n = re.sub(r'^(g_|k(?=[A-Z]))', '', n)
    return (n[0].lower() + n[1:]) if n else None


def wrapper_name(ctx, fn, site, address):
    if address is not None:
        known = ctx.by_address.get(address)
        if known:
            return public_name(known[1])
        name = ctx.shared.function_names.get(address)
        if name:
            return name
    if site.name:
        return public_name(site.name.split('::')[-1])
    t = A.strip_implicit(site.target)
    d = (t.get('referencedDecl') or {}).get('name', '')
    if d.startswith('k') and len(d) > 1:
        return d[1].lower() + d[2:]
    return None


class Row:
    """A call site on its way to its new form."""

    def __init__(self, fn, site, name, tail, args, result, address):
        self.fn, self.site, self.name, self.tail, self.args, self.result, self.address = fn, site, name, tail, args, result, address
        self.body = None     # the marked body a GAME_CALL name reaches, when the caller's namespace sees it
        self.guest, self.chain, self.shadowed = True, (), False
        self.body_chain = ()


WRAPPER_BODY = re.compile(r'\{\s*(?:return\s+)?(?:ee::(?:game|tailCall)|game::(?:call|tailCall|callAt|tailCallAt))\b[^;]*;\s*\}$')


def is_wrapper(fn, regs=()):
    """One of the one-line typed declarations `calls` writes: one statement, a guest call, and no game function itself."""
    return (fn.path, fn.name) not in regs and len(A.children(fn.body)) == 1 and WRAPPER_BODY.fullmatch(fn.source(fn.body)) is not None


LEFT_READ = re.compile(r'\bee::leftByCall')


def local_names(fn):
    names = getattr(fn, 'locals_cache', None)
    if names is None:
        names = {c.get('name') for c, _ in A.walk(fn.decl) if c.get('kind') in ('VarDecl', 'ParmVarDecl')}
        fn.locals_cache = names
    return names


def collect_calls(ctx, fn):
    if fn.path.endswith('.h'):
        for n, _ in A.walk(fn.body):
            if n.get('kind') == 'CallExpr' and guest_site(ctx, fn, n):
                ctx.report.keep('calls', fn, n, 'a call in a header')
        return
    if any(d.get('kind') in ('FunctionTemplateDecl', 'ClassTemplateDecl') for d in fn.chain) or is_wrapper(fn, ctx.regs):
        return
    seen = set()
    chain = namespace_chain(fn.decl)
    for n, p in A.walk(fn.body):
        if n.get('kind') != 'CallExpr' or A.begin(n) in seen or A.in_macro(n):
            continue
        site = guest_site(ctx, fn, n)
        if site is None:
            continue
        seen.add(A.begin(n))
        address = target_address(ctx, fn, site)
        site.address = address
        if site.form.startswith('ee::') and address is None:
            ctx.report.keep('calls', fn, n, 'an indirect call: no typed declaration can name it')
            continue
        name = wrapper_name(ctx, fn, site, address)
        if not name or (fn.path, name) in ctx.shared.blocked or (fn.unit, name) in ctx.shared.blocked:
            ctx.report.keep('calls', fn, n, 'a callee with no name' if not name else 'its wrapper did not compile')
            continue
        tail = 'tail' in site.form.lower()
        guest = site.form.startswith('ee::')
        args = [argument_of(ctx, fn, a, guest) for a in site.args]
        result = site.result if guest else None
        if not guest:
            result = A.desugar(n.get('type', {}))
        result = result.strip() if result else 'void'
        row = Row(fn, site, name, tail, args, result, address)
        row.guest = guest
        row.chain = chain
        known = ctx.by_address.get(address) if address is not None else None
        if known and known[1].endswith('_'):
            rows = body_declarations(ctx, fn.unit, known)
            row.body = known[1] if rows else None
            row.body_chain = rows[0].chain if rows else ()
        if row.body and guest and not tail and LEFT_READ.search(fn.source(fn.body)):
            # What a table call leaves in the registers is not what a direct one leaves: the site stays.
            ctx.report.keep('calls', fn, n, 'what the callee leaves in the registers is read after a call: it stays a table call')
            continue
        row.shadowed = name in local_names(fn)
        if row.body and not tail and same_scope(row.body_chain, chain):
            # A nearer `name` hides the GAME_CALL: a method's class, or a deeper namespace's.
            nearer = [d for d in ctx.shared.declarations[fn.unit].get(name, []) if same_scope(d.chain, chain) and len(d.chain) > len(row.body_chain)]
            if nearer or fn.decl.get('kind') != 'FunctionDecl':
                row.shadowed = True
        ctx.sites.setdefault(fn.path, []).append(row)


def template_end(source):
    depth, i = 0, source.index('<')
    while i < len(source):
        depth += source[i] == '<'
        depth -= source[i] == '>'
        if depth == 0:
            return i
        i += 1
    return i


def spell_result(result, fn):
    return clean_type(result, fn.namespaces)


def public_name(body):
    """The name a game function is called by: a body marked with a trailing underscore goes by the name without it."""
    return body[:-1] if body.endswith('_') else body


def nodes_with_files(docs):
    """Every node of a unit's dump with the file its location is in (the dumper names a file only when it changes)."""
    current = None
    for doc in docs:
        for node, parent in A.walk(doc):
            found = None
            loc = node.get('loc') or {}
            for where in (loc, loc.get('spellingLoc') or {}, loc.get('expansionLoc') or {}):
                if 'file' in where:
                    current = where['file']
            found = current
            r = node.get('range') or {}
            for side in ('begin', 'end'):
                e = r.get(side) or {}
                for where in (e, e.get('spellingLoc') or {}, e.get('expansionLoc') or {}):
                    if 'file' in where:
                        current = where['file']
            yield node, parent, found if loc else current


class Declared:
    """A declaration of a function at namespace level, in one unit."""

    def __init__(self, path, decl):
        self.path, self.decl = path, decl
        self.mangled = decl.get('mangledName', '')
        self.chain = namespace_chain(decl)
        self.defines = any(c.get('kind') == 'CompoundStmt' for c in A.children(decl))


def unit_declarations(tree, docs, elsewhere=()):
    """{short name: [Declared]} of the functions a unit declares, headers included, the other layers' too (elsewhere)."""
    out = collections.defaultdict(list)
    for node, parent, path in nodes_with_files(docs):
        if node.get('kind') == 'FunctionDecl' and node.get('mangledName') and (parent is None or parent.get('kind') == 'NamespaceDecl'):
            if path in tree.texts:
                out[node.get('name')].append(Declared(path, node))
    for node in elsewhere:
        out[node.get('name')].append(Declared(node['path_of'], node))
    return out


def body_declarations(ctx, unit, known):
    """The declarations a unit has of the marked body `known` (file, name): the same function, not only the same name."""
    shared = ctx.shared
    if not shared.definition_of:
        for table in shared.declarations.values():
            for rows in table.values():
                for d in rows:
                    if d.defines:
                        shared.definition_of[(d.path, d.decl.get('name'))] = d.mangled
    mangled = shared.definition_of.get(known)
    return [d for d in shared.declarations[unit].get(known[1], []) if d.mangled == mangled]


def same_scope(chain, caller):
    return tuple(caller[:len(chain)]) == tuple(chain)


def pass_routes(ctx, fn):
    """Which bodies carry a name a caller would have to use: a readable function a typed call's unit declares, in its own
    namespace (a bare name) or another (a qualified one: a wrapper there would meet the body through argument-dependent
    lookup). The rest of the pass is finish_routes."""
    if any(d.get('kind') in ('FunctionTemplateDecl', 'ClassTemplateDecl') for d in fn.chain):
        return
    for n, _ in A.walk(fn.body):
        if n.get('kind') != 'CallExpr' or A.in_macro(n):
            continue
        site = guest_site(ctx, fn, n)
        if site is None:
            continue
        site.address = target_address(ctx, fn, site)
        known = ctx.by_address.get(site.address) if site.address is not None else None
        if not known or known[1].endswith('_'):
            continue
        if ctx.shared.only and known[1] not in ctx.shared.only:
            continue    # (--only: another function's body is not this run's to mark)
        if len(ctx.regs.get(known, ())) != 1:
            ctx.report.keep('routes', fn, n, 'a body registered at more than one address: a plain name could not say which')
            continue
        for d in ctx.shared.declarations[fn.unit].get(known[1], []):
            ctx.sites.setdefault(known, set()).add(d.mangled)


def bodies_to_mark(ctx):
    """{mangled: short} of the functions whose bodies take the marker: one definition, registered, not overloaded."""
    shared = ctx.shared
    definitions = {}
    for table in shared.declarations.values():
        for rows in table.values():
            for d in rows:
                if d.defines:
                    definitions[d.mangled] = d
    chains = collections.defaultdict(set)   # (a file's anonymous helper of the name is no overload of it: non-template, it wins in that file)
    for table in shared.declarations.values():
        for short, rows in table.items():
            for d in rows:
                if '_GLOBAL__N_1' not in d.mangled:
                    chains[(short, d.chain)].add(d.mangled)
    out = {}
    candidates = list(shared.regs)
    for known, mangled in ctx.sites.items():
        path, short = known
        for m in mangled:
            d = definitions.get(m)
            if d is None or d.path != path:
                continue  # (the same name, another function)
            if sum(k[1] == short for k in candidates) > 1:
                shared.overloaded.add(known)  # (two registered functions of this name: a text search could not tell them)
                continue
            if chains[(short, d.chain)] - {m}:
                shared.overloaded.add(known)
                continue
            if not any(short + '_' in table for table in shared.declarations.values()):
                out[m] = short
    return out


TEMPLATE_ARGUMENT = r'(<\s*&\s*(?:\w+::)*)%s\b(?=\s*[,>])'


def finish_routes(ctx):
    """Marks the bodies: every declaration, every registration and every reference of the C++ name take the underscore,
    so what compiled before compiles to the same thing; `calls` then hands out the plain name."""
    tree, shared = ctx.tree, ctx.shared
    marked = bodies_to_mark(ctx)
    shared.marked = marked
    shared.marked_chains = {d.mangled: d.chain for table in shared.declarations.values() for rows in table.values() for d in rows}
    if not marked:
        return
    shorts = set(marked.values())
    seen = set()
    edit = ctx.rw.edits

    chains = collections.defaultdict(set)
    for m, short in marked.items():
        chains[short].add(shared.marked_chains.get(m, ()))

    def qualifier_fits(path, at, short):
        """Whether what a name is qualified with here is the tail of a marked function's namespaces (`std::strcmp` is not)."""
        q = re.search(r'((?:\w+::)+)$', tree.texts[path].raw[max(0, at - 120):at])
        if not q:
            return True
        written = tuple(q.group(1).split('::')[:-1])
        return any(chain[-len(written):] == written for chain in chains[short])

    def rename(path, at, short):
        if (path, at) not in seen and tree.texts[path].raw[at:at + len(short)] == short and qualifier_fits(path, at, short):
            seen.add((path, at))
            edit.add(path, at, at + len(short), short + '_')

    for unit, docs in shared.asts.items():
        ids = {}
        for node, _, path in nodes_with_files(docs):
            kind = node.get('kind')
            if kind == 'FunctionDecl' and node.get('mangledName') and node.get('name') in shorts:
                ids[node.get('id')] = node.get('mangledName')
                loc = node.get('loc') or {}
                loc = loc.get('spellingLoc') or loc
                if node.get('mangledName') in marked and path in tree.texts and 'offset' in loc:
                    rename(path, loc['offset'], node.get('name'))
        for node, _, path in nodes_with_files(docs):
            if node.get('kind') != 'DeclRefExpr' or path not in tree.texts:
                continue
            d = node.get('referencedDecl') or {}
            short = d.get('name')
            if short not in shorts or d.get('kind') != 'FunctionDecl' or A.in_macro(node):
                continue
            m = ids.get(d.get('id'))
            if (m in marked if m else True):
                last = node['range']['end']
                if 'offset' in last:
                    rename(path, last['offset'], short)
    edit.commit()
    for path, text in tree.texts.items():
        for short in {short for short in shorts if short in text.raw}:
            for r in re.finditer(r'READABLE_\w+\(\s*(?:\w+\s*,\s*)?(?:0[xX][0-9a-fA-F]+|sym::[\w:]+)\s*,\s*(%s)\s*,' % short, text.raw):
                if (path, short) in shared.regs:
                    rename(path, r.start(1), short)
            # (fn, status), the name its own address: `sym::fn, fn_, status`, the symbol keeping the plain name.
            for r in re.finditer(r'READABLE_\w+\(\s*(%s)\s*,\s*\w+\s*\)' % short, text.raw):
                if (path, short) in shared.regs and (path, r.start(1)) not in seen:
                    seen.add((path, r.start(1)))
                    edit.add(path, r.start(1), r.end(1), 'sym::%s, %s_' % (short, short))
            for r in re.finditer(TEMPLATE_ARGUMENT % short, text.raw):
                rename(path, r.end(1), short)
            # (`static_cast<F (*)(T)>(&name)`, picking among a name's overloads)
            for r in re.finditer(r'static_cast<[^;]*?\(\*\)\([^;]*?\)>\s*\(\s*&\s*(?:\w+::)*(%s)\s*\)' % short, text.raw):
                rename(path, r.start(1), short)
            for r in re.finditer(r'\busing\s+(?:\w+::)+(%s)\s*;' % short, text.raw):
                rename(path, r.start(1), short)
        edit.commit()
    # What the clang pass does not see: the other game layers and the tests, by the qualified name.
    pattern = re.compile(r'\b(?:%s)\b(?!_)' % '|'.join(sorted({'::'.join(ctx.shared.marked_chains.get(m, ()) + (short,)) for m, short in marked.items()}, key=len, reverse=True)))
    walks = [ztree.zlayout.readable_walk(tree.tree, d) for d in ('dm2', 'ee')] + [os.walk(ztree.zlayout.path(tree.tree, 'tests', 'src'))]
    for walked in walks:
        for folder, _, names in walked:
            for name in names:
                if name.endswith(('.cpp', '.h')):
                    p = os.path.join(folder, name)
                    t = A.Text(p)
                    new = pattern.sub(lambda m: m.group(0) + '_', t.raw)
                    if new != t.raw:
                        t.write(new)


def site_call(row, fn_name):
    """The text of the call: the name, qualified where it is another namespace's, or where a local of the same name hides it."""
    if fn_name.startswith('tail::'):
        return fn_name
    if row.body and not row.tail:
        if not same_scope(row.body_chain, row.chain):
            shared = 0
            while shared < len(row.body_chain) - 1 and shared < len(row.chain) and row.body_chain[shared] == row.chain[shared]:
                shared += 1
            return '%s::%s' % ('::'.join(row.body_chain[shared:]), fn_name)
        if row.shadowed and row.body_chain:
            return '%s::%s' % ('::'.join(row.body_chain), fn_name)
        return fn_name
    if row.shadowed and row.chain:
        return '%s::%s' % ('::'.join(row.chain), fn_name)
    return fn_name


def with_result(call, result, wanted):
    """The call as what the site took its result as. (Never a functional cast of a spaced type: as a statement,
    `unsigned long long(name());` declares a function and calls nothing.)"""
    if result == 'void' or result == wanted or wanted == 'void' or same_type(result, wanted):
        return call
    integer = r'(u?int\d+_t|unsigned int|int)'
    wanted = re.sub(r'^const\s+', '', wanted)
    if wanted.startswith('ee::Ptr<'):
        if re.fullmatch(integer, result):
            return '%s.address' % call if result in ('uint32_t', 'unsigned int') else 'static_cast<%s>(%s.address)' % (result, call)
        return '%s(%s.address)' % (result, call) if result.startswith('ee::Ptr<') else 'static_cast<%s>(%s)' % (result, call)
    if wanted.endswith('*'):
        if re.fullmatch(integer, result):
            return 'ee::addressOf(%s)' % call
        return '%s(ee::addressOf(%s))' % (result, call) if result.startswith('ee::Ptr<') else 'static_cast<%s>(%s)' % (result, call)
    return 'static_cast<%s>(%s)' % (result, call)


def place_router(ctx, row, placed):
    """GAME_CALL(name) after the declaration of the body a caller sees: its header's, else the file's own first one (or the
    registration under the definition). A caller above any declaration gets a forward declaration above it."""
    shared, fn = ctx.shared, row.fn
    short = row.name
    if ('unit', fn.unit, short) in placed:
        return
    placed.add(('unit', fn.unit, short))
    rows = body_declarations(ctx, fn.unit, ctx.by_address[row.address])
    home = shared.homes.get(rows[0].mangled)
    at_home = sorted((d for d in rows if d.path == home), key=lambda d: A.begin(d.decl))
    own = sorted((d for d in rows if d.path == fn.path), key=lambda d: A.begin(d.decl))
    line = 'GAME_CALL(%s);' % short
    variadic = re.search(r'GAME_CALL_VARIADIC\(%s\);' % short, ctx.tree.texts[home].raw) if home else None
    if variadic:
        line = variadic.group(0)
    if at_home:
        d = at_home[0]
    elif own:
        d = own[0]
    else:
        # Another header than its home: a unit that does not see the GAME_CALL has its own line.
        if ('file', fn.path, short) not in placed and line not in fn.text.raw:
            placed.add(('file', fn.path, short))
            at = position(ctx, fn)
            if '_GLOBAL__N_1' not in rows[0].mangled:
                at = anonymous_block(fn.text, at)
            shared.inserts[(fn.path, at)].append('namespace %s { %s }\n\n' % ('::'.join(rows[0].chain), line))
        return
    text = ctx.tree.texts[d.path]
    if ('file', d.path, short) in placed or line in text.raw:
        return
    placed.add(('file', d.path, short))
    start = text.line_start(A.begin(d.decl))
    pad = re.match(r'[ \t]*', text.raw[start:]).group(0)
    inserts = ctx.shared.inserts
    if d.path.endswith('.h'):
        ctx.shared.needs_readable.add(d.path)
    if d.path == fn.path and A.begin(row.site.node) < A.begin(d.decl):
        at = position(ctx, fn)
        if '_GLOBAL__N_1' not in d.mangled:
            at = anonymous_block(text, at)  # (inside one it would declare another function)
        body = next((c for c in A.children(d.decl) if c.get('kind') == 'CompoundStmt'), None)
        head = text.raw[A.begin(d.decl):A.begin(body) if body else A.end(d.decl)].rstrip()
        inserts[(d.path, at)].append('%s%s;\n%s%s\n\n' % (pad, head, pad, line))
        return
    if d.defines:
        m = re.compile(r'^[ \t]*READABLE_\w+\([^;]*?\b%s\b[^;]*\);[^\n]*$' % re.escape(row.body), re.M).search(text.raw, A.end(d.decl))
        at = m.end() if m else text.line_end(A.end(d.decl))
        inserts[(d.path, at)].append('\n%s%s' % (pad, line))
    elif text.raw[start:A.begin(d.decl)].lstrip().startswith('namespace'):
        # `namespace x { int32_t name_(); }`: inside its braces.
        at = text.raw.index(';', A.end(d.decl)) + 1
        inserts[(d.path, at)].append(' ' + line)
    else:
        at = text.line_end(text.raw.index(';', A.end(d.decl)))
        inserts[(d.path, at)].append('\n%s%s' % (pad, line))


def router_homes(ctx):
    """{mangled: header}: a marked body's GAME_CALL goes after one declaration only, or a unit with two would have two:
    the header most units see."""
    count = collections.Counter()
    for table in ctx.shared.declarations.values():
        seen = set()
        for short, rows in table.items():
            if short.endswith('_'):
                seen |= {(d.mangled, d.path) for d in rows if d.path.endswith('.h')}
        count.update(seen)
    # (a header that has the line already is its home, whatever a partial run's units see)
    placed = {}
    for table in ctx.shared.declarations.values():
        for short, rows in table.items():
            if short.endswith('_'):
                for d in rows:
                    if d.path.endswith('.h') and re.search(r'GAME_CALL\w*\(%s\);' % short[:-1], ctx.tree.texts[d.path].raw):
                        placed[d.mangled] = d.path
    homes = dict(placed)
    for (mangled, path), n in sorted(count.items()):
        if mangled in placed:
            continue
        if mangled not in homes or n > count[(mangled, homes[mangled])]:
            homes[mangled] = path
    return homes


def finish_calls(ctx):
    placed = set()
    ctx.shared.inserts = collections.defaultdict(list)
    ctx.shared.needs_readable = set()
    ctx.shared.homes = router_homes(ctx)
    for path, sites in ctx.sites.items():
        if path in ctx.shared.blocked_files:
            for r in sites:
                ctx.report.keep('calls', r.fn, r.site.node, 'a compile error in this file that no name explains')
            continue
        # A call inside another's arguments waits for the next round: the outer one's text carries it.
        reach, outer = -1, []
        for r in sorted(sites, key=lambda r: A.begin(r.site.node)):
            if A.begin(r.site.node) >= reach:
                outer.append(r)
                reach = A.end(r.site.node)
        sites = outer
        ctx.shared.converted[path] = {r.name for r in sites}
        routers = {r.name for r in sites if r.body and not r.tail}
        wrappers = collections.OrderedDict()
        converted = []
        host_form = {}
        for row in sites:
            if not row.guest and not row.body:
                source = row.fn.source(row.site.node)
                host_form.setdefault((tuple(row.fn.namespaces), row.name, row.tail),
                                     (row.site.form, source[source.index('<') + 1:template_end(source)]))
        for row in sites:
            fn, site, name = row.fn, row.site, row.name
            if row.body and not row.tail:
                place_router(ctx, row, placed)
                call = site_call(row, name) + '(%s)' % ', '.join(a.text for a in row.args)
                wanted = ctx.shared.returns.get((ctx.by_address[row.address][0], row.body), 'void')
                used = statement_of(fn, site.node) is None
                ctx.rw.edits.add(path, A.begin(site.node), A.end(site.node), with_result(call, row.result, wanted) if used else call)
                ctx.report.did('calls', fn)
                continue
            key = (tuple(fn.namespaces), name, row.tail)
            clash = [d for d in ctx.shared.declarations[fn.unit].get(name, []) if same_scope(d.chain, row.chain)]
            if clash and not row.tail:
                ctx.report.keep('calls', fn, site.node, 'another function of this name is in scope')
                continue
            w = wrappers.get(key)
            source = fn.source(site.node)
            spelled = fn.source(site.target) if row.guest else source[source.index('<') + 1:template_end(source)]
            if key in host_form and row.guest:
                row.guest = False
                row.args = [argument_of(ctx, fn, a, False) for a in site.args]
                spelled = host_form[key][1]
            if w is None:
                w = wrappers[key] = Wrapper(name, row.tail)
                w.address, w.first, w.scope, w.fn = row.address, fn, key[0], fn
                w.form = host_form[key][0] if key in host_form else site.form
                w.target_text = spelled
            elif w.address != row.address or (not row.guest and w.target_text != spelled):
                ctx.report.keep('calls', fn, site.node, 'one name, two callees in a file')
                continue
            converted.append(row)
            k = tuple(a.spelled for a in row.args)
            if k not in w.signatures:
                w.signatures[k] = (row.args, fn)
            for i, a in enumerate(row.args):
                v = constant_of(a.node) if a.kind == 'int' else None
                if a.kind == 'int' and not (v is not None and 0 <= v < 0x80000000):
                    w.not_small.add((len(row.args), i))
            w.results[row.result] += 1
        for w in wrappers.values():
            merge_signatures(w)
        by_name = collections.Counter(k[1:2] for k in wrappers)
        groups = collections.defaultdict(list)
        for key, w in wrappers.items():
            w.result = next((r for r, _ in w.results.most_common() if r != 'void'), 'void')
            # (a tail wrapper of a name that also calls plainly would take those calls over, and quietly)
            w.nested = w.tail and (by_name[(w.name,)] > 1 or w.name in routers or bool(ctx.shared.declarations.get(w.fn.unit, {}).get(w.name + '_')) or
                                   re.search(r'^\s*[\w:<>*& ]+\b%s\(.*\{ .*(ee::game|ee::tailCall|game::\w+)' % re.escape(w.name), ctx.tree.texts[path].raw, re.M) is not None)
            groups[position(ctx, w.first)].append(w)
        for row in converted:
            w = wrappers[(tuple(row.fn.namespaces), row.name, row.tail)]
            call = site_call(row, ('tail::' if w.nested else '') + row.name) + '(%s)' % ', '.join(a.text for a in row.args)
            used = statement_of(row.fn, row.site.node) is None
            ctx.rw.edits.add(path, A.begin(row.site.node), A.end(row.site.node), with_result(call, row.result, w.result) if used else call)
            ctx.report.did('calls', row.fn)
        text = ctx.tree.texts[path]
        for at, group in groups.items():
            ctx.shared.inserts[(path, at)].append(wrappers_block(ctx, text, at, group))
        ensure_include(ctx, path)
        waiting = len(ctx.rw.edits.pending)
        if not ctx.rw.edits.commit() and waiting:
            print('  edits of %s overlap: seen again next round' % ctx.tree.rel(path), flush=True)
    # A name's GAME_CALL and a file's wrappers, on their own: what is inserted at one place goes in as one text.
    for (path, at), texts in sorted(ctx.shared.inserts.items()):
        ctx.rw.edits.add(path, at, at, ''.join(texts))
        if not ctx.rw.edits.commit():
            print('  an insertion in %s overlaps at %d' % (ctx.tree.rel(path), at), flush=True)
    for path in sorted(ctx.shared.needs_readable):
        ensure_include(ctx, path, 'ee/readable.h')
        ctx.rw.edits.commit()


FLOAT_TYPES = {'ee::Float', 'float'}
WIDE_NAMES = {'uint64_t', 'int64_t'}


def merge_signatures(w):
    """One wrapper for the sites of one shape: where they hand over different things (a Ptr, a Local, a number), the parameter
    is what all of those are in the register, ee::Address. An overload for each would be ambiguous: a Local converts to both."""
    if not w.form.startswith('ee::'):
        return
    shapes = collections.OrderedDict()
    for key, value in w.signatures.items():
        shapes.setdefault((len(key), tuple(t in FLOAT_TYPES for t in key)), []).append((key, value))
    fresh = collections.OrderedDict()
    for shape, group in shapes.items():
        if len(group) == 1:
            fresh[group[0][0]] = group[0][1]
            continue
        columns = list(zip(*(key for key, _ in group)))
        merged = []
        for i, c in enumerate(columns):
            wide = set(c) & WIDE_NAMES
            if len(set(c)) == 1:
                merged.append(c[0])
            elif wide and len(wide) == 1 and set(c) <= wide | {'uint32_t'} and (len(columns), i) not in w.not_small:
                merged.append(wide.pop())  # (a small constant reads the same in a whole register)
            elif not wide and 'ee::Qword' not in c and not any(a.kind == 'other' for _, (args, _) in group for a in args):
                merged.append('ee::Address')
            else:
                merged = None
                break
        if merged is None:
            for key, value in group:
                fresh[key] = value
        else:
            fresh[tuple(merged)] = group[0][1]
    w.signatures = fresh


def anonymous_block(text, at):
    """The start of the outermost `namespace { }` around an offset, with its comment; else the offset."""
    code = re.sub(r'//[^\n]*|/\*.*?\*/|"(?:[^"\\\n]|\\.)*"|\'(?:[^\'\\\n]|\\.)*\'', lambda m: ' ' * len(m.group(0)), text.raw, flags=re.S)
    for m in re.finditer(r'^[ \t]*namespace\s*\{', code, re.M):
        if m.start() >= at:
            break
        depth, i = 0, m.end() - 1
        while i < len(code):
            depth += code[i] == '{'
            depth -= code[i] == '}'
            if depth == 0:
                break
            i += 1
        if m.start() < at < i:
            start = text.line_start(m.start())
            while start > 0 and text.raw[text.line_start(start - 1):start].strip().startswith('//'):
                start = text.line_start(start - 1)
            return start
    return at


def position(ctx, fn):
    """Where a function's wrappers go: above the outermost thing it is in and its comment."""
    text = fn.text
    outer = next((d for d in fn.chain if d.get('kind') != 'NamespaceDecl'), fn.decl)
    start = text.line_start(A.begin(outer))
    while start > 0:
        prev = text.line_start(start - 1)
        if text.raw[prev:start].strip().startswith('//'):
            start = prev
        else:
            break
    return start


def parameter_list(ctx, w, types, args, used):
    names = names_for(ctx, w)
    out = []
    taken = set(used)
    for i, a in enumerate(args):
        name = names[i] if i < len(names) and names[i] else None
        if name:
            name = name.replace('*', '').replace('&', '').strip()
            if not re.fullmatch(r'[A-Za-z_]\w*', name) or name in KEYWORDS:
                name = None
        name = name or first_identifier(a.text) or ('text' if a.spelled.endswith('char>') else 'value')
        base = name
        k = 2
        while name in taken:
            name = '%s%d' % (base, k)
            k += 1
        taken.add(name)
        out.append(name)
    return out


def names_for(ctx, w):
    known = ctx.by_address.get(w.address) if w.address is not None else None
    if known:
        names = ctx.shared.declared.get(known)
        if names:
            return names
    return ctx.params.get(w.address) or ctx.shared.comment_names.get(w.target_text.split('::')[-1] if w.target_text else '') or []


def qualified_target(ctx, w):
    """`&name` of game::call as it reads from inside `calls`, where a wrapper of the same name hides it."""
    text = w.target_text
    m = re.match(r'\s*&\s*([A-Za-z_]\w*)\s*(,.*)?$', text, re.S)
    if not m:
        return text  # (qualified already, or a cast that picks an overload)
    short = m.group(1)
    chains = ctx.shared.chains.get(short, [])
    mine = '::'.join(n for n in reversed(w.fn.namespaces[:1])) if w.fn.namespaces else ''
    same_file = [c for p, c in chains if p == w.fn.path]
    candidates = same_file or [c for _, c in chains]
    best = None
    for c in sorted(set(candidates), key=len, reverse=True):
        if mine == c or mine.startswith(c + '::'):
            best = c
            break
    if best is None and candidates:
        best = candidates[0]
    if best is None:
        return text
    return '&%s::%s%s' % (best, short, m.group(2) or '')


def wrappers_block(ctx, text, at, group):
    pad = re.match(r'[ \t]*', text.raw[at:]).group(0)
    lines = []
    plain, nested = [], []
    for w in group:
        (nested if w.nested else plain).append(w)
    for group_part, opener in ((plain, None), (nested, 'tail')):
        if not group_part:
            continue
        if opener:
            lines.append('namespace tail')
            lines.append('{')
        for w in group_part:
            for key, (args, fn) in w.signatures.items():
                names = parameter_list(ctx, w, key, args, set(re.findall(r'\w+', w.target_text or '')) | {'ee', 'game', 'zge', w.name})
                params = ', '.join('%s %s' % (t, n) for t, n in zip(key, names))
                handed = ', '.join(a.body % n for a, n in zip(args, names))
                result = spell_result(w.result, w.fn)
                ret = '' if result == 'void' else 'return '
                if w.form.startswith('ee::'):
                    template = '' if result == 'void' else '<%s>' % result
                    call = '%s%s(%s%s)' % ('ee::tailCall' if w.tail else 'ee::game', template, w.target_text, ', ' + handed if handed else '')
                else:
                    call = '%s<%s>(%s)' % (w.form, qualified_target(ctx, w), handed)
                lines.append('%s %s(%s) { %s%s; }' % (result, w.name, params, ret, call))
        if opener:
            lines.append('}')
    body = '\n'.join((pad + '    ' + l if l else l) for l in lines)
    return '%snamespace // the game functions below call: ee/plain.h\n%s{\n%s\n%s}\n\n' % (pad, pad, body, pad)


def never_written(fn, var_id, refs):
    for ref, parent in refs.get(var_id, []):
        if written_only(fn, ref, parent) is not None:
            return False
        n, p = skip_up(fn, ref)
        if p is not None and p.get('kind') == 'UnaryOperator' and p.get('opcode') == '&':
            return False
    return True


def pass_aliases(ctx, fn):
    """`const uint32_t x = pointer.address;`, neither ever moved: x is the pointer, and the call sites and field accesses
    that follow see its type."""
    refs = refs_of(fn)
    for var, stmt in local_vars(fn):
        init = initializer(var)
        if init is None or A.in_macro(stmt) or re.sub(r'^const\s+', '', A.desugar(var.get('type', {}))) != 'unsigned int':
            continue
        x = A.strip_implicit(init)
        if x.get('kind') != 'MemberExpr' or x.get('name') != 'address' or not A.children(x):
            continue
        base = A.strip_implicit(A.children(x)[0])
        if base.get('kind') != 'DeclRefExpr' or not A.desugar(base.get('type', {})).replace('const ', '').startswith('ee::Ptr<'):
            continue
        y = (base.get('referencedDecl') or {}).get('id')
        name = (base.get('referencedDecl') or {}).get('name')
        if not never_written(fn, var.get('id'), refs) or not never_written(fn, y, refs):
            continue
        for ref, _ in refs.get(var.get('id'), []):
            ctx.rw.edits.add(fn.path, A.begin(ref), A.end(ref), name + '.address')
        ctx.rw.remove_lines(fn, A.begin(stmt), ctx.rw.past_semicolon(fn.text, A.end(stmt)))
        if ctx.rw.edits.commit():
            ctx.report.did('aliases', fn)


OFFSETOF = re.compile(r'offsetof\(\s*([\w:]+)\s*,\s*([\w.\[\]]+)\s*\)')
ARRAY = re.compile(r'^(.*?)((?:\[\d+\])+)$')


def other_sign(a, b):
    """Whether two spellings are one integer width with the other signedness."""
    def norm(t):
        t = re.sub(r'\bconst\b', '', t).strip()
        return {'unsigned int': 'uint32_t', 'int': 'int32_t', 'unsigned short': 'uint16_t', 'short': 'int16_t', 'unsigned char': 'uint8_t',
                'signed char': 'int8_t'}.get(t, t)
    a, b = norm(a), norm(b)
    return a != b and a in ('int32_t', 'uint32_t', 'int16_t', 'uint16_t', 'int8_t', 'uint8_t') and a.lstrip('u') == b.lstrip('u')


def same_type(a, b):
    """Whether two spellings are one type, namespaces and const aside."""
    def norm(t):
        t = re.sub(r'\bconst\b', '', t).replace('unsigned int', 'uint32_t').replace('(anonymous namespace)::', '')
        t = re.sub(r'\bint\b(?! )', 'int32_t', t) if t.strip() == 'int' else t
        t = {'unsigned short': 'uint16_t', 'short': 'int16_t', 'unsigned char': 'uint8_t', 'signed char': 'int8_t',
             'unsigned long long': 'uint64_t', 'long long': 'int64_t', 'unsigned long': 'uint64_t'}.get(t.strip(), t)
        return re.sub(r'\b\w+::', '', re.sub(r'\s+', '', t))
    return norm(a) == norm(b)


def field_at(ctx, record, offset, wanted):
    """The member of a known struct at an offset with the type the code reads it as: its path, or (None, why)."""
    entry = ctx.shared.layouts.get(record)
    if entry is None:
        return None, 'no layout of %s' % record
    size, _, fields = entry
    near = []
    for at, path, kind in fields:
        if re.search(r'(^|\.)_[0-9a-fA-F]{2,4}$', path):
            continue
        a = ARRAY.match(kind)
        if a:
            dims = [int(x) for x in re.findall(r'\d+', a.group(2))]
            base = a.group(1).strip()
            known = size_of(ctx.sizes, base)
            if known and len(dims) <= 2:
                count = dims[0] * (dims[1] if len(dims) > 1 else 1)
                if at <= offset < at + count * known[0] and (offset - at) % known[0] == 0:
                    i = (offset - at) // known[0]
                    near.append(('%s[%d]' % (path, i) if len(dims) == 1 else '%s[%d][%d]' % (path, i // dims[1], i % dims[1]), base))
            continue
        if at == offset:
            near.append((path, kind))
    matches = [p for p, k in near if same_type(k, wanted)]
    if len(matches) == 1 or (matches and all(m.startswith(matches[0]) for m in matches)):
        return matches[0], None
    # The other signedness of the same width: the bits are the field's, and the read keeps the sign it was read with.
    other = [p for p, k in near if other_sign(k, wanted)]
    if len(other) == 1:
        return other[0], None
    if not near:
        return None, 'no field of %s at +0x%x' % (record.split('::')[-1], offset)
    return None, '%s at +0x%x is %s, read as %s' % (record.split('::')[-1], offset, near[0][1], wanted)


def pass_fields(ctx, fn):
    """`*ee::at<T>(pointer.address + 0x20)` as `pointer->field`, when a struct says that field is there and is a T."""
    if fn.path in ctx.shared.blocked_files:
        return
    for n, p in A.walk_code(fn.body):
        if n.get('kind') != 'CallExpr' or not is_ee_mark(fn, n, {'at'}) or A.in_macro(n):
            continue
        args = A.children(n)[1:]
        if len(args) != 1:
            continue
        source = fn.source(n)
        m = re.match(r'ee::at<(.*?)>\s*\(', source, re.S)
        if not m:
            continue
        wanted = m.group(1).strip()
        e = A.strip_implicit(args[0])
        offset = 0
        base = e
        written_offset = None
        if e.get('kind') == 'BinaryOperator' and e.get('opcode') == '+':
            left, right = A.children(e)
            offset = constant_of(right)
            right_text = fn.source(right)
            o = OFFSETOF.search(right_text)
            if o and o.group(0) == right_text.strip():
                written_offset = o
            if offset is None and not written_offset:
                offset = ctx.constants.get(right_text.strip())
                if offset is None:
                    continue
            base = A.strip_implicit(left)
        pointer = None
        record = None
        if base.get('kind') == 'MemberExpr' and base.get('name') == 'address' and A.children(base):
            b = A.children(base)[0]
            t = re.sub(r'^const\s+', '', A.desugar(b.get('type', {})))
            if t.startswith('ee::Ptr<'):
                pointer, record = fn.source(b), t[len('ee::Ptr<'):-1]
                record = re.sub(r'^const\s+', '', record)
        path = None
        if written_offset:
            # offsetof names the struct and the field.
            if pointer is None:
                t = A.desugar(base.get('type', {}))
                if t not in ('unsigned int', 'const unsigned int') or not is_pure(fn, base):
                    continue
                pointer = 'ee::Ptr<%s>(%s)' % (written_offset.group(1), fn.source(base))
            path = written_offset.group(2)
            kind = None
            # What the offsetof says is there is what was read, or the access keeps its width by itself (a field's
            # other half, a signed read of an unsigned field in a comparison).
            layout = ctx.shared.layouts.get(record.replace('(anonymous namespace)::', '') if record else None) or next((v for k, v in ctx.shared.layouts.items() if k.split('::')[-1] == written_offset.group(1).split('::')[-1]), None)
            field = next((f for f in layout[2] if f[1] == path), None) if layout else None
            if field is None or not (same_type(field[2], wanted) or other_sign(field[2], wanted)):
                ctx.report.keep('fields', fn, n, '%s is %s, read as %s' % (path, field[2] if field else 'unknown here', wanted))
                continue
        elif pointer is None or offset is None:
            continue
        else:
            path, why = field_at(ctx, record.replace('(anonymous namespace)::', ''), offset, wanted)
            if path is None:
                ctx.report.keep('fields', fn, n, why)
                continue
        q, parent = skip_up(fn, n)
        if parent is not None and parent.get('kind') == 'UnaryOperator' and parent.get('opcode') == '*':
            text = '%s->%s' % (pointer, path)
            record_name = record or written_offset.group(1)
            layout = ctx.shared.layouts.get(record_name.replace('(anonymous namespace)::', '')) or next((v for k, v in ctx.shared.layouts.items() if k.split('::')[-1] == record_name.split('::')[-1]), None)
            field = next((f for f in layout[2] if f[1] == path), None) if layout else None
            written_to = is_write(fn, parent)
            if field is not None and other_sign(field[2], wanted) and not written_to:
                text = 'static_cast<%s>(%s)' % (wanted, text)
            ctx.rw.edits.add(fn.path, A.begin(parent), A.end(parent), text)
        elif parent is not None and parent.get('kind') == 'MemberExpr' and parent.get('isArrow'):
            arrow = fn.text.raw.find('->', A.end(n))
            ctx.rw.edits.add(fn.path, A.begin(n), arrow + 2, '%s->%s.' % (pointer, path))
        else:
            ctx.rw.edits.add(fn.path, A.begin(n), A.end(n), '&%s->%s' % (pointer, path))
        if ctx.rw.edits.commit():
            ctx.report.did('fields', fn)


VEC_OUT = {'sum': '%s + %s', 'difference': '%s - %s'}
VEC4_POINTERS = ('zge::Vec4 *', 'const zge::Vec4 *', 'ee::Ptr<zge::Vec4>', 'const ee::Ptr<zge::Vec4>')


def vector_operand(fn, node):
    """A VU0 macro's operand as the Vec4 it points to: its text, or None when it is not plainly one."""
    x = A.strip_implicit(node)
    # The address of one: `pointer.address`, `local.address()`.
    if x.get('kind') == 'MemberExpr' and x.get('name') == 'address' and A.children(x):
        base = A.strip_implicit(A.children(x)[0])
        if re.fullmatch(r'(const )?ee::Ptr<(const )?zge::Vec4>', A.desugar(base.get('type', {}))) and base.get('kind') == 'DeclRefExpr':
            return '*' + fn.source(base)
    if x.get('kind') == 'CXXMemberCallExpr' and callee_name(x) == 'address' and re.fullmatch(r'(const )?ee::Local<zge::Vec4>', member_base_type(x)):
        return '*' + fn.source(A.children(A.children(x)[0])[0])
    t = A.desugar(x.get('type', {}))
    if t not in VEC4_POINTERS:
        if t.startswith('const ') and t[6:] in VEC4_POINTERS:
            pass
        else:
            return None
    if x.get('kind') == 'UnaryOperator' and x.get('opcode') == '&':
        return fn.source(A.children(x)[0])
    text = fn.source(x)
    if re.fullmatch(r'[\w:]+', text) or re.fullmatch(r'[\w:.\->\[\]]+', text):
        return '*' + text
    return '*(%s)' % text


def scalar_operand(fn, node):
    return fn.source(node)


def pass_vectors(ctx, fn):
    """ee::vu0::Machine's macros on Vec4s, in vec.h's words; the Machine goes when nothing uses it."""
    if fn.path in ctx.shared.blocked_files:
        return
    for n, p in A.walk_code(fn.body):
        if n.get('kind') != 'CXXMemberCallExpr' or 'Machine' not in member_base_type(n) or A.in_macro(n):
            continue
        name = callee_name(n)
        args = A.children(n)[1:]
        statement = statement_of(fn, n) is not None
        text = None
        if name in VEC_OUT and len(args) == 3 and statement:
            a, b, out = [vector_operand(fn, x) for x in args]
            if a and b and out:
                text = '%s = %s' % (out, VEC_OUT[name] % (a, b))
        elif name == 'scale' and len(args) == 3 and statement:
            v, out = vector_operand(fn, args[0]), vector_operand(fn, args[2])
            if v and out:
                text = '%s = %s * %s' % (out, v, scalar_operand(fn, args[1]))
        elif name == 'normalize' and len(args) == 2 and statement:
            v, out = vector_operand(fn, args[0]), vector_operand(fn, args[1])
            if v and out:
                text = '%s = zge::unit(%s)' % (out, v)
        elif name == 'absolute' and len(args) == 2 and statement:
            v, out = vector_operand(fn, args[0]), vector_operand(fn, args[1])
            if v and out:
                text = '%s = zge::abs(%s)' % (out, v)
        elif name == 'cross' and len(args) == 3 and statement:
            a, b, out = [vector_operand(fn, x) for x in args]
            if a and b and out:
                text = '%s = zge::cross(%s, %s)' % (out, a, b)
        elif name == 'dot' and len(args) == 2:
            a, b = [vector_operand(fn, x) for x in args]
            if a and b:
                text = 'zge::dot(%s, %s)' % (a, b)
        elif name == 'squaredLength' and len(args) == 1:
            a = vector_operand(fn, args[0])
            if a:
                text = 'zge::squaredLength(%s)' % a
        if text is None:
            ctx.report.keep('vectors', fn, n, 'VU0 macro %s not on plain Vec4s' % name)
            continue
        ctx.rw.edits.add(fn.path, A.begin(n), A.end(n), text)
        ensure_include(ctx, fn.path, 'zge/math/vec.h')
        if ctx.rw.edits.commit():
            ctx.report.did('vectors', fn)


ARGUMENT_REGISTERS = {'a0': 4, 'a1': 5, 'a2': 6, 'a3': 7, 't0': 8, 't1': 9, 't2': 10, 't3': 11}
ARGUMENT_READ = re.compile(r'ee::callerRegister\(\s*ee::reg::(a[0-3]|t[0-3])\s*\)')


def direct_callers(tree, fns, shared):
    """{(file, name) of a registered function}: reached as C++, by game::call and its kin, a GAME_CALL name or a plain call."""
    names = collections.defaultdict(set)
    for key in shared.regs:
        names[public_name(key[1])].add(key)
    reached = set()
    for path, text in tree.texts.items():
        for m in re.finditer(r'\bgame::(?:call|tailCall|callAt|tailCallAt)<\s*&\s*(?:\w+::)*(\w+)', text.raw):
            reached |= names.get(public_name(m.group(1)), set())
        for m in re.finditer(r'\bGAME_CALL\((\w+)\)', text.raw):
            used = re.compile(r'(?<![\w&])%s\s*\(' % re.escape(m.group(1)))
            if any(len(used.findall(x.raw)) for x in tree.texts.values()):
                reached |= names.get(m.group(1), set())
    for key, caller in plain_callers(fns, shared).items():
        reached.add(key)
    return reached


def pass_handons(ctx, fn):
    """An argument register the function reads as it came and hands on, as the parameter it is: only where nothing can change:
    no C++ caller (they would have to hand it over), and nothing called before the read. A call through the table sets those
    registers and ee::entry reads them, so the value is the one it was. Never dropped: what stays is listed."""
    key = (fn.path, fn.name)
    if key not in ctx.regs or A.in_macro(fn.decl):
        return
    reads = []
    calls = []
    for n, _ in A.walk_code(fn.body):
        if n.get('kind') != 'CallExpr' or A.begin(n) is None:
            continue
        m = ARGUMENT_READ.fullmatch(fn.source(n))
        if m:
            reads.append((n, ARGUMENT_REGISTERS[m.group(1)]))
        elif guest_site(ctx, fn, n) or A.strip_implicit(A.children(n)[0]).get('kind') == 'DeclRefExpr' and callee_name(n) not in PURE_CALLS | PURE_HELPERS:
            calls.append(A.begin(n))
    if not reads:
        return
    params = [c for c in A.children(fn.decl) if c.get('kind') == 'ParmVarDecl']
    taken = 4 + sum(A.desugar(p.get('type', {})) not in FLOAT_TYPES | {'float'} for p in params)
    if any(r < taken for _, r in reads):
        ctx.report.keep('handons', fn, reads[0][0], 'it reads a register its signature names: after a call, what the call left')
        return
    if key in ctx.shared.reached:
        ctx.report.keep('handons', fn, reads[0][0], 'a C++ caller would have to hand it over')
        return
    if calls and min(calls) < max(A.begin(n) for n, _ in reads):
        ctx.report.keep('handons', fn, reads[0][0], 'read after a call: what the call left in it')
        return
    if any(fn.enclosing(n, ('LambdaExpr',)) is not None for n, _ in reads):
        ctx.report.keep('handons', fn, reads[0][0], 'read in a lambda')
        return
    used = {r for _, r in reads}
    names = {}
    added = []
    for r in range(taken, max(used) + 1):
        if r in used:
            names[r] = 'passedOn%s' % ('' if not names else str(len(names) + 1))
            added.append('uint64_t ' + names[r])
        else:
            added.append('uint64_t /*unused*/')
    edits = ctx.rw.edits
    for n, r in reads:
        edits.add(fn.path, A.begin(n), A.end(n), names[r])
    list_end = fn.text.raw.rfind(')', A.begin(fn.decl), A.begin(fn.body))
    edits.add(fn.path, list_end, list_end, (', ' if params else '') + ', '.join(added))
    for d in ctx.shared.declarations[fn.unit].get(fn.name, []):
        if d.defines or d.mangled != fn.decl.get('mangledName'):
            continue
        text = ctx.tree.texts[d.path]
        at = text.raw.rfind(')', A.begin(d.decl), A.end(d.decl))
        edits.add(d.path, at, at, (', ' if params else '') + ', '.join(added))
    if edits.commit():
        ctx.report.did('handons', fn)


STALE = re.compile(r'ee::(?:block|afterCall|loopBack|tableJump|jumpDispatch|chargeLoop|calleeSaves)\b|calleeSaves|saveEntry|\bentry blocks?\b|'
                   r'\bsafe[- ]points?\b|\bbranch(?:es)? back\b|\bEE time\b|\bdelay slot\b|\breturn address(?:es)?\b|callee-saved|ee::StackFrame|'
                   r'\bpublish(?:es|ed)?\b|\bsafepoints\b')


def drop_stale(text):
    """A comment's text without its parentheses and sentences about the scaffolding."""
    out, i, n = [], 0, len(text)
    # Parentheses first.
    while i < n:
        c = text[i]
        if c == '(':
            depth, j = 0, i
            while j < n:
                depth += text[j] == '('
                depth -= text[j] == ')'
                if depth == 0:
                    break
                j += 1
            group = text[i:j + 1]
            if STALE.search(group):
                i = j + 1
                if text[i:i + 1] == '.' and (not out or out[-1].rstrip().endswith(('.', ':'))):
                    i += 1
                continue
            out.append(group)
            i = j + 1
            continue
        out.append(c)
        i += 1
    rest = ''.join(out)
    sentences = re.split(r'(?<=[.:;])\s+(?=[A-Z(`])', rest)
    kept = [x for x in sentences if not STALE.search(x)]
    return re.sub(r'\s+', ' ', ' '.join(kept)).strip()


MIRROR_SAVED = {'s0': 16, 's1': 17, 's2': 18, 's3': 19, 's4': 20, 's5': 21, 's6': 22, 's7': 23, 'fp': 30}


def entry_reads():
    """{function: registers the original really reads} of readable_entry_reads.tsv; `*` is all of them."""
    kept = collections.defaultdict(set)
    for line in open(os.path.join(ztree.TOOLS, 'readable_entry_reads.tsv'), encoding='utf-8'):
        if line.startswith('#') or not line.strip():
            continue
        fields = line.split('\t')
        if len(fields) > 1 and fields[1] in ('ai', 'dm2', 'bmx'):
            del fields[1]    # (a game's own row, readable_entries.py: its registers are kept in every version)
        name, registers = fields[:2]
        for part in registers.split(','):
            m = re.fullmatch(r'f(\d+)-f(\d+)', part)
            if m:
                kept[name].update(32 + i for i in range(int(m.group(1)), int(m.group(2)) + 1))
            elif part == '*':
                kept[name].add('*')
            elif part in MIRROR_SAVED:
                kept[name].add(MIRROR_SAVED[part])
            elif re.fullmatch(r'f\d+', part):
                kept[name].add(32 + int(part[1:]))
    return kept


ENTRY_READS = {}


def mirror_register(text, float_read):
    """The register a read names, in callerRegister's numbering (32 + f for a float); None when it is not a constant."""
    text = re.sub(r'/\*.*?\*/', '', text).strip()
    text = re.sub(r'^(ee::reg|saved|aggressive::saved)::', '', text)
    if re.fullmatch(r'\d+', text):
        return (32 if float_read else 0) + int(text)
    return MIRROR_SAVED.get(text)


def mirror_is_dead(fn, register):
    if not ENTRY_READS:
        ENTRY_READS.update(entry_reads())
    kept = ENTRY_READS.get(fn.name.rstrip('_'), ())
    if '*' in kept or register in kept:
        return False
    return register in MIRROR_SAVED.values() or 52 <= register < 64


def split_cleared_declarations(ctx, fn):
    """`T a = 0, b = f();`: a declarator per statement, so that `dead` sees each one."""
    for n, p in A.walk_code(fn.body):
        if n.get('kind') != 'DeclStmt' or p is None or p.get('kind') != 'CompoundStmt' or A.in_macro(n):
            continue
        decls = A.children(n)
        if len(decls) < 2 or not all(d.get('kind') == 'VarDecl' for d in decls):
            continue
        start, stop = A.begin(n), A.end(n)
        raw = fn.text.raw[start:stop]
        if '//' in raw or '/*' in raw or '\n' in raw and 'lambda' in raw:
            continue
        first = decls[0].get('loc', {}).get('offset')
        if first is None or not start < first < stop:
            continue
        head = fn.text.raw[start:first]
        parts = A.split_top_level(fn.text.raw[first:stop - 1]) if hasattr(A, 'split_top_level') else split_commas(fn.text.raw[first:stop - 1])
        if len(parts) != len(decls) or any(q.lstrip().startswith(('*', '&')) for q in parts):
            continue
        if not any(re.search(r'(=\s*0(\.0f)?|\(0(\.0f)?\))\s*$|ee::leftByCall', q) for q in parts):
            continue
        ind = indent_of(fn.text, n)
        ctx.rw.edits.add(fn.path, start, stop, ('\n' + ind).join(head + q.strip() + ';' for q in parts))
        ctx.rw.edits.commit()
        ctx.report.did('mirrors', fn)


def split_commas(text):
    parts, depth, begin = [], 0, 0
    for i, c in enumerate(text):
        if c in '([{<':
            depth += 1
        elif c in ')]}>':
            depth -= 1
        elif c == ',' and depth == 0:
            parts.append(text[begin:i])
            begin = i + 1
    parts.append(text[begin:])
    return parts


def pass_mirrors(ctx, fn):
    split_cleared_declarations(ctx, fn)
    seen = set()
    for n, p in A.walk_code(fn.body):
        if n.get('kind') not in ('CallExpr', 'CXXMemberCallExpr') or A.begin(n) in seen:
            continue
        begin = A.begin(n)
        if n.get('kind') == 'CallExpr' and is_ee_mark(fn, n, {'callerRegister', 'callerFloat'}):
            float_read = callee_name(n) == 'callerFloat'
            arguments = A.children(n)[1:]
            register = mirror_register(fn.source(arguments[0]), float_read) if len(arguments) == 1 else None
            if register is None:
                ctx.report.keep('mirrors', fn, n, 'a register in a variable')
                continue
            if not mirror_is_dead(fn, register):
                ctx.report.keep('mirrors', fn, n, 'the original reads it, or it is not callee-saved')
                continue
            if A.in_macro(n):
                ctx.report.keep('mirrors', fn, n, 'in a macro')
                continue
            seen.add(begin)
            # A plain cast of the read goes with it.
            whole, up = n, fn.up(n)
            while up is not None and up.get('kind') in ('ImplicitCastExpr', 'CXXStaticCastExpr', 'ParenExpr') and not float_read:
                if up.get('kind') != 'ImplicitCastExpr' or A.begin(up) == A.begin(whole):
                    whole = up
                up = fn.up(up)
            ctx.rw.edits.add(fn.path, A.begin(whole), A.end(whole), '0.0f' if float_read else '0')
            if ctx.rw.edits.commit():
                ctx.report.did('mirrors', fn)
        elif saved_registers_method(n) in ('keepCallers', 'keepCallersFloat'):
            seen.add(begin)
            statement = statement_of(fn, n)
            parent = fn.up(statement) if statement is not None else fn.up(n)
            if parent is not None and parent.get('kind') == 'CXXForRangeStmt' and A.end(parent) is not None:
                ctx.rw.remove_lines(fn, A.begin(parent), ctx.rw.past_semicolon(fn.text, A.end(parent)))
                done = True
            else:
                done = ctx.rw.remove(fn, n)
            if done and ctx.rw.edits.commit():
                ctx.report.did('mirrors', fn)
            else:
                ctx.rw.edits.rollback()
                ctx.report.keep('mirrors', fn, n, 'keepCallers in a context the pass does not know')


def pass_comments(tree, report):
    """Comments that explain what the passes took out. A file that keeps some of it keeps its comments."""
    import textwrap
    changed = 0
    for path in tree.files:
        if tree.rel(path) in LEAVE_ALONE or (FILES and not any(f in path for f in FILES)):
            continue
        text = tree.texts[path]
        lines = text.raw.split('\n')
        out, i = [], 0
        keeps = re.search(r'\bee::(?:block|afterCall|loopBack|tableJump|calleeSaves)\(', re.sub(r'//[^\n]*', '', text.raw)) is not None
        while i < len(lines):
            m = re.match(r'^(\s*)// ?(.*)$', lines[i])
            if not m:
                out.append(lines[i])
                i += 1
                continue
            j = i
            pad = m.group(1)
            paragraph = []
            width = 0
            while j < len(lines):
                k = re.match(r'^(\s*)// ?(.*)$', lines[j])
                if not k or k.group(1) != pad or not k.group(2).strip():
                    break
                paragraph.append(k.group(2).strip())
                width = max(width, len(lines[j]))
                j += 1
            if not paragraph:
                out.append(lines[i])
                i += 1
                continue
            joined = ' '.join(paragraph)
            if STALE.search(joined) and not keeps:
                fresh = drop_stale(joined)
                if fresh != joined:
                    changed += 1
                    if fresh and len(fresh) > 3:
                        wrapped = textwrap.wrap(fresh, width=max(40, width - len(pad) - 3))
                        out += ['%s// %s' % (pad, w) for w in wrapped]
                    i = j
                    # (no blank line left doubled)
                    if not fresh and out and not out[-1].strip() and i < len(lines) and not lines[i].strip():
                        i += 1
                    continue
            out += lines[i:j]
            i = j
        new = '\n'.join(out)
        if new != text.raw:
            text.write(new)
    report.sites['comments'] += changed
    print('comments: %d paragraphs' % changed)


PASSES = collections.OrderedDict([('marks', pass_marks), ('saves', pass_saves), ('dead', pass_dead), ('frames', pass_frames), ('aliases', pass_aliases), ('routes', pass_routes), ('calls', collect_calls), ('fields', pass_fields), ('vectors', pass_vectors), ('handons', pass_handons), ('mirrors', pass_mirrors), ('tidy', pass_tidy)])
FINISH = {'calls': finish_calls, 'routes': finish_routes}


class Context:
    """What every pass of a round can look up."""

    def __init__(self, tree, report, shared):
        self.tree, self.report, self.rw = tree, report, Rewriter(tree)
        self.shared = shared
        self.regs, self.by_address = shared.regs, shared.by_address
        self.frames, self.sizes = shared.frames, shared.sizes
        self.params = shared.params
        self.constants = {}
        self.sites = {}
        self.new_rows = []
        self.touched = None

    def site_address(self, site):
        return getattr(site, 'address', None)

    def named_address(self, fn, name):
        short = name.split('::')[-1]
        rows = self.shared.address_of_name.get(short, ())
        mine = [a for p, a in rows if p == fn.path]
        if mine:
            return mine[0]
        return rows[0][1] if len(rows) == 1 else None  # (two functions of this name: the site says which, not this)


class Shared:
    """The tables a run reads once."""

    def __init__(self, tree):
        self.table = symbols(tree)
        SYMBOL_TABLE.update(self.table)
        self.regs, self.by_address = registrations(tree, self.table)
        self.frames = frame_rows(tree.build)
        self.stack_arguments = stack_arguments(tree.build)
        self.sizes = struct_sizes(tree)
        self.address_of_name = collections.defaultdict(list)
        for (path, short), rows in self.regs.items():
            for g, address, _, _ in rows:
                if g == 'ai':
                    self.address_of_name[short].append((path, address))
        self.params = names_table(tree)
        self.function_names = function_names(tree)
        self.comment_names = constant_comments(tree)
        self.declared = {}      # (file, short name) -> its parameters' names
        self.chains = collections.defaultdict(list)  # short name -> [(file, namespace)]
        self.declared_types = {}
        self.blocked = set()    # (file, wrapper) that did not compile
        self.blocked_functions = set()  # (file, function, step)
        self.left_plain = set()         # (file, function, offset): a plain value that did not compile: a Local
        self.full_names = {}            # a record's short name -> its name with namespaces, when one
        self.blocked_files = set()      # units whose compile error no name explains
        self.plain_called = {}
        self.rows = {}          # (file, function) -> its readable_locals.tsv rows
        self.declared_keys = {k for k in self.regs}
        self.returns = {}       # (file, name) -> the function's return type
        self.overloaded = set()
        self.converted = {}
        self.marked = {}
        self.asts = {}
        self.declarations = collections.defaultdict(dict)
        self.definition_of = {}  # (file, name of a body) -> its mangled name
        self.elsewhere = None    # unit -> what it declares in the layers it does not open
        self.headers_moved = False
        self.homes = {}


def function_names(tree):
    names = {}
    path = ztree.zlayout.path(tree.tree, 'tools', 'readable', 'readable_names.tsv')
    for line in open(path, encoding='utf-8', errors='replace'):
        f = line.rstrip('\n').split('\t')
        if len(f) >= 3 and f[0] == 'function':
            names[int(f[1], 16)] = f[2]
    return names


COMMENT_NAMES = re.compile(r'\b(?:constexpr|const)\s+(?:std::)?uint32_t\s+(k\w+)\s*=[^;\n]*;\s*//\s*\(([^()]*)\)')


def constant_comments(tree):
    """{kName: [parameter names]} from `constexpr uint32_t kX = ...; // (out, normal, v)`."""
    out = {}
    for path in tree.files:
        for m in COMMENT_NAMES.finditer(tree.texts[path].raw):
            names = [a.strip().rstrip('*').strip() for a in m.group(2).split(',')]
            if names and all(re.fullmatch(r'[A-Za-z_]\w*', n) or not n for n in names):
                out[m.group(1)] = names
    return out


def names_table(tree):
    """{address: [parameter names]} from readable_names.tsv; the functions' own declarations go over these."""
    params = collections.defaultdict(dict)
    path = ztree.zlayout.path(tree.tree, 'tools', 'readable', 'readable_names.tsv')
    for line in open(path, encoding='utf-8', errors='replace'):
        f = line.rstrip('\n').split('\t')
        if len(f) >= 3 and f[0] == 'param' and ':' in f[1]:
            address, index = f[1].split(':')
            if index.isdigit() and int(index) > 0 and (len(f) < 4 or f[3] != 'low'):
                params[int(address, 16)][int(index) - 1] = f[2]  # (1 is the first)
    return {a: [names.get(i) for i in range(max(names) + 1)] for a, names in params.items()}


class OutOfBudget(Exception):
    """A run stopped at the end of a round: the tree compiles and the same command goes on from here."""


FILES = None     # --files
DEADLINE = None  # --budget


def load_functions(tree, only, asts):
    seen = {}
    for unit, docs in asts.items():
        for path, decl, chain in A.functions_of(docs, tree.game + '/'):
            if not any(path.startswith(os.path.join(tree.game, layer) + '/') for layer in A.LAYERS):
                continue
            if tree.rel(path) in LEAVE_ALONE or (FILES and not any(f in path for f in FILES)):
                continue
            key = (path, A.begin(decl))
            if key not in seen:
                seen[key] = (path, decl, unit, chain)
    fns = []
    for p, d, unit, chain in seen.values():
        if not only or d.get('name') in only or d.get('name', '').rstrip('_') in only:
            fn = Fn(tree, p, d)
            fn.unit = unit
            fn.chain = chain
            fn.namespaces = namespaces_of(p, tree, chain, d)
            fns.append(fn)
    return fns


def namespace_chain(decl):
    """The named namespaces a declaration is in, outermost first, from its mangled name: the dump's filter hands
    declarations without the namespaces around them. A method's class is left out."""
    m = re.sub(r'^_+Z', '', decl.get('mangledName', ''))
    if not m.startswith('N'):
        return ()
    i, names = 1, []
    while i < len(m) and m[i] in 'KVr':
        i += 1
    while i < len(m) and (m[i].isdigit() or m[i] == 'L'):
        if m[i] == 'L':
            i += 1
            continue
        j = i
        while m[j].isdigit():
            j += 1
        names.append(m[j:j + int(m[i:j])])
        i = j + int(m[i:j])
    names = names[:-1]
    if decl.get('kind') != 'FunctionDecl' and names:
        names = names[:-1]
    return tuple(n for n in names if n != '_GLOBAL__N_1')


def namespaces_of(path, tree, chain, decl):
    """The named namespaces a function is in, as `a::b` prefixes, longest first."""
    names = namespace_chain(decl)
    return ['::'.join(names[:i]) for i in range(len(names), 0, -1)]


def constants_of(fn, unit_constants):
    table = dict(unit_constants.get(fn.unit, {}))
    if 'k' in fn.source(fn.body):
        for n, _ in A.walk(fn.body):
            if n.get('kind') == 'VarDecl' and n.get('constexpr') and n.get('init'):
                v = constant_of(A.children(n)[-1])
                v = symbol_value(A.children(n)[-1]) if v is None else v
                if v is not None:
                    table[n.get('name')] = v
    return table


SYMBOL_TABLE = {}   # readable_symbols.tsv: what `sym::name` is


def symbol_value(node):
    """A `sym::name` as written in a constant's initializer: its address."""
    x = A.strip_implicit(node)
    d = x.get('referencedDecl') or {}
    return SYMBOL_TABLE.get(d.get('name')) if x.get('kind') == 'DeclRefExpr' and d.get('kind') == 'VarDecl' else None


def all_constants(asts):
    """{unit: {name: value}}: the integer constants a unit sees (constexpr or const, namespace level)."""
    out = {}
    for unit, docs in asts.items():
        table = {}

        def visit(decl):
            kind = decl.get('kind')
            if kind == 'VarDecl' and decl.get('init') and (decl.get('constexpr') or 'const' in decl.get('type', {}).get('qualType', '')):
                v = constant_of(A.children(decl)[-1])
                v = symbol_value(A.children(decl)[-1]) if v is None else v
                if v is not None:
                    table[decl.get('name')] = v
            elif kind == 'NamespaceDecl':
                for c in A.children(decl):
                    visit(c)

        for d in docs:
            visit(d)
        out[unit] = table
    return out


ERROR_LINE = re.compile(r'^(/[^:\n]+):(\d+):\d+: error: (.*)$', re.M)


INSTANTIATION = re.compile(r'^(/[^:\n]+):(\d+):\d+: note: in instantiation of function template specialization \'(?:\w+::)*(\w+)<', re.M)
ROUTE_NOTE = re.compile(r"specialization 'game::detail::Route<&[^<>]*?(\w+)_>::call<")


def in_header(tree, failed):
    """Whether a compile error is in a header of the tree, which a site's unit going back would not mend."""
    for errors in failed.values():
        for path, _, _ in ERROR_LINE.findall(errors):
            if path in tree.texts and path.endswith('.h'):
                return True
    return False


def blame(tree, errors, fns, step, shared, unit=None):
    """What a unit's compile errors point at: the function or the wrapper to leave out next time."""
    found = 0
    if step == 'calls':
        # A GAME_CALL a site cannot use: the name, in the unit that uses it.
        for name in ROUTE_NOTE.findall(errors):
            shared.blocked.add((unit, name))
            found += 1
        # A wrapper a callee's signature does not fit: the error is in ee/readable.h, the note names the wrapper's line.
        for path, line in re.findall(r'^(/[^:\n]+):(\d+):\d+: note: in instantiation of', errors, re.M):
            if path in tree.texts:
                lines = tree.texts[path].raw.split('\n')
                here = lines[int(line) - 1] if int(line) <= len(lines) else ''
                m = re.match(r'\s*[\w:<>*& ]+\s(\w+)\(.*\{ .*(ee::game|ee::tailCall|game::\w+)', here)
                if m:
                    shared.blocked.add((path, m.group(1)))
                    found += 1
        # A GAME_CALL that does not fit: the error is in ee/readable.h, the note names the call.
        for path, line, name in INSTANTIATION.findall(errors):
            if path in tree.texts and name in shared.converted.get(path, ()):
                shared.blocked.add((path, name))
                found += 1
    names_by_file = collections.defaultdict(set)
    for fn in fns:
        names_by_file[fn.path].add(fn.name)
    for path, line, message in ERROR_LINE.findall(errors):
        if path not in tree.texts:
            continue
        lines = tree.texts[path].raw.split('\n')
        row = int(line) - 1
        here = lines[row] if row < len(lines) else ''
        if step == 'calls':
            m = re.match(r'\s*[\w:<>*& ]+\s(\w+)\(.*\{ .*(ee::game|ee::tailCall|game::\w+)', here)
            names = [m.group(1)] if m else [n for n in re.findall(r'\b(\w+)\(', here) if n in shared.converted.get(path, ())]
            if names:
                shared.blocked.update((path, n) for n in names)
                found += 1
                continue
        for i in range(row, -1, -1):
            m = re.match(r'^\s*(?:[\w:<>*&]+\s+)+[*&]?(\w+)\(', lines[i])
            if m and m.group(1) in names_by_file[path]:
                shared.blocked_functions.add((path, m.group(1), step))
                found += 1
                break
    return found


def finish_frames(tree, shared, asts):
    """The places of the Locals of the functions that kept their change."""
    rows = []
    fns = {(fn.path, fn.name): fn for fn in load_functions(tree, None, asts)}
    for key, new in shared.rows.items():
        fn = fns.get(key)
        if fn is not None and not frame_var(fn):
            rows += new
    if rows:
        write_locals(rows)


def plain_callers(fns, shared):
    """{(file, name) of a registered function: a function that calls it as plain C++}. Such a call has no frame of its
    own in the frame table's sense: a Local in the callee would land in the caller's frame."""
    short = collections.defaultdict(set)
    for key in shared.regs:
        short[key[1]].add(key)
    out = {}
    for fn in fns:
        for n, _ in A.walk_code(fn.body):
            if n.get('kind') == 'CallExpr' and A.children(n):
                x = A.strip_implicit(A.children(n)[0])
                d = x.get('referencedDecl') or {}
                if x.get('kind') == 'DeclRefExpr' and d.get('kind') == 'FunctionDecl' and d.get('name') in short:
                    for key in short[d.get('name')]:
                        out.setdefault(key, fn.name)
    return out


STATE = None      # what a run stopped by --budget hands the next one
RESUMED = {}


def save_state(report, shared):
    with open(STATE, 'w', encoding='utf-8') as f:
        json.dump({'sites': report.sites, 'functions': {k: sorted(v) for k, v in report.functions.items()},
                   'blocked': sorted(shared.blocked), 'blocked_functions': sorted(shared.blocked_functions),
                   'blocked_files': sorted(shared.blocked_files),
                   'plain': sorted(shared.left_plain)}, f)


def load_state(report):
    if not os.path.exists(STATE):
        return
    state = json.load(open(STATE, encoding='utf-8'))
    report.sites.update(state['sites'])
    for k, v in state['functions'].items():
        report.functions[k].update(tuple(x) for x in v)
    RESUMED['blocked'] = {tuple(x) for x in state['blocked']}
    RESUMED['blocked_functions'] = {tuple(x) for x in state['blocked_functions']}
    RESUMED['blocked_files'] = set(state['blocked_files'])
    RESUMED['plain'] = {tuple(x) for x in state['plain']}


def run_pass(tree, name, report, only, asts, rounds=30):
    """A pass, then what it left to clean up, until neither changes anything. A unit that stops compiling goes back
    to where it was and what broke it is left out."""
    kept = []
    shared = Shared(tree)
    shared.only = only
    shared.blocked |= RESUMED.get('blocked', set())
    shared.blocked_functions |= RESUMED.get('blocked_functions', set())
    shared.blocked_files |= RESUMED.get('blocked_files', set())
    shared.left_plain |= RESUMED.get('plain', set())
    if name in ('fields', 'frames'):
        shared.layouts = tree.layouts()
        seen = {}
        for record, (size, align, _) in shared.layouts.items():
            short = record.split('::')[-1]
            seen[short] = (size, align) if seen.get(short, (size, align)) == (size, align) else None
        shared.sizes.update({k: v for k, v in seen.items() if v})
        full = collections.defaultdict(set)
        for record in shared.layouts:
            full[record.split('::')[-1]].add(record)
        shared.full_names = {short: names.pop() for short, names in full.items() if len(names) == 1}
    steps = [name] + (['dead', 'tidy'] if name not in ('dead', 'tidy') else [])
    idle = 0
    r = 0
    index = 0
    while r < rounds:
        r += 1
        step = steps[index % len(steps)]
        fns = load_functions(tree, only, asts)
        unit_constants = all_constants(asts)
        find_pure_helpers(fns)
        shared.empty = empty_helpers(fns, shared)
        if step == 'frames':
            shared.plain_called = plain_callers(fns, shared)
        if step in ('routes', 'calls', 'handons'):
            shared.asts = asts
            if shared.elsewhere is None or shared.headers_moved:
                shared.elsewhere = tree.declared_elsewhere(list(asts))
                shared.headers_moved = False
            shared.declarations = collections.defaultdict(dict, {u: unit_declarations(tree, docs, shared.elsewhere.get(u, ())) for u, docs in asts.items()})
            shared.definition_of = {}
        if step == 'handons':
            shared.reached = direct_callers(tree, fns, shared)
        ctx = Context(tree, report, shared)
        ctx.touched = None if name == 'tidy' else {key for names in report.functions.values() for key in names}
        mark = len(report.rows)
        shared.chains.clear()
        for fn in fns:
            if fn.namespaces:
                shared.chains[fn.name].append((fn.path, fn.namespaces[0]))
            if (fn.path, fn.name) in shared.regs:
                parameters = [c for c in A.children(fn.decl) if c.get('kind') == 'ParmVarDecl']
                shared.declared[(fn.path, fn.name)] = [c.get('name') for c in parameters]
                shared.declared_types[(fn.path, fn.name)] = [(c.get('type', {}).get('qualType', ''), A.desugar(c.get('type', {}))) for c in parameters]
                shared.returns[(fn.path, fn.name)] = re.match(r'(.*?)\s*\(', fn.decl.get('type', {}).get('qualType', 'void (')).group(1)
        for fn in fns:
            if (fn.path, fn.name, step) in shared.blocked_functions:
                report.keep(step, fn, fn.decl, 'the pass broke the compile here')
                continue
            ctx.constants = constants_of(fn, unit_constants)
            PASSES[step](ctx, fn)
            ctx.rw.edits.rollback()
        if step in FINISH:
            FINISH[step](ctx)
        if step == name:
            kept = report.rows[mark:]
        del report.rows[mark:]
        if not len(ctx.rw.edits):
            idle += 1
            index += 1
            if idle == len(steps):
                break
            continue
        idle = 0
        changed = set(ctx.rw.edits.by_file)
        shared.headers_moved = shared.headers_moved or any(p.endswith('.h') for p in changed)
        before = {p: tree.texts[p].raw for p in changed}
        print('%s round %d (%s): %d edits in %d files' % (name, r, step, len(ctx.rw.edits), len(changed)), flush=True)
        ctx.rw.edits.apply(tree.texts)
        again = tree.units() if any(p.endswith('.h') for p in changed) else sorted(changed)
        fresh, failed = tree.asts(again)
        asts.update(fresh)
        if failed:
            # Back to before this round, in the failed units; the next round leaves out what broke them.
            blamed = 0
            known = len(shared.blocked) + len(shared.blocked_functions) + len(shared.blocked_files)
            for unit, errors in failed.items():
                placed = blame(tree, errors, fns, step, shared, unit)
                blamed += placed
                if not placed and step == 'calls':
                    shared.blocked_files.add(unit)
                    blamed += 1
                with open(STATE + '.errors', 'a', encoding='latin-1') as f:
                    f.write('=== round %d %s\n%s\n' % (r, tree.rel(unit), errors[:6000]))
                if unit in tree.texts:
                    saved = STATE + '.failed'
                    os.makedirs(saved, exist_ok=True)
                    with open(os.path.join(saved, tree.rel(unit).replace('/', '-')), 'w', encoding='latin-1') as f:
                        f.write(tree.texts[unit].raw)
                print('  did not compile: %s: %s' % (tree.rel(unit), ERROR_LINE.search(errors).group(3)[:120] if ERROR_LINE.search(errors) else errors[:120]), flush=True)
                if unit in before:
                    tree.texts[unit].write(before[unit])
            if step in ('calls', 'vectors', 'fields', 'frames') and known == len(shared.blocked) + len(shared.blocked_functions) + len(shared.blocked_files):
                # Nothing new left out, so the same round would fail again.
                shared.blocked_files.update(u for u in failed if u in before)
                print('  no progress: files left out: %s' % sorted(tree.rel(u) for u in failed)[:8], flush=True)
            if DEADLINE and time.monotonic() > DEADLINE and step == 'calls':
                shared.stop_soon = True
            if step == 'routes' or (step == 'calls' and (in_header(tree, failed) or any(u not in before for u in failed))):
                # A name that changed in a header goes with every unit that sees it: all of the round goes back.
                for p in changed:
                    tree.texts[p].write(before[p])
                fresh, failed2 = tree.asts(tree.units())
                asts.update(fresh)
                if failed2:
                    raise SystemExit('still not compiling after putting back: %s' % sorted(failed2)[:5])
                if not blamed or step == 'routes':
                    raise SystemExit('a compile error the tool cannot place')
                continue
            if step == 'calls':
                # A GAME_CALL line is harmless without its sites: only the units that failed went back.
                fresh, failed2 = tree.asts(sorted(failed))
                asts.update(fresh)
                if failed2:
                    raise SystemExit('still not compiling after putting back: %s' % sorted(failed2)[:5])
                if getattr(shared, 'stop_soon', False):
                    report.rows += kept
                    save_state(report, shared)
                    raise OutOfBudget()
                continue
            headers = [p for p in changed if p.endswith('.h')]
            for p in headers:
                tree.texts[p].write(before[p])
            if headers:
                print('  headers put back: %d' % len(headers), flush=True)
            fresh, failed2 = tree.asts(tree.units() if headers else sorted(failed))
            asts.update(fresh)
            if failed2:
                raise SystemExit('still not compiling after putting back: %s' % sorted(failed2)[:5])
            if not blamed:
                raise SystemExit('a compile error the tool cannot place')
            continue  # the same step again
        index += 1
        if DEADLINE and time.monotonic() > DEADLINE:
            report.rows += kept
            if name == 'frames':
                finish_frames(tree, shared, asts)
            save_state(report, shared)
            raise OutOfBudget()
    report.rows += kept
    if name == 'frames':
        finish_frames(tree, shared, asts)
    return asts


def call_map(tree, asts):
    """{function: [callee, ...]} in source order. A callee is a game address however it is written, ee::call, game::call, a
    typed wrapper or a GAME_CALL name; anything else called by name is its name. What `audit` compares across a run: a plain
    name found by the wrong lookup (a nearer declaration, argument-dependent lookup) would show here, and in no replay."""
    report = Report()
    shared = Shared(tree)
    fns = load_functions(tree, None, asts)
    unit_constants = all_constants(asts)
    ctx = Context(tree, report, shared)
    wrappers = {}
    helpers = set()   # the templates of the tree that GAME_CALL did not make (it is the only macro that makes one)
    for docs in asts.values():
        for node, _ in (x for doc in docs for x in A.walk(doc)):
            if node.get('kind') == 'FunctionTemplateDecl' and 'expansionLoc' not in (node.get('loc') or {}):
                helpers.add(node.get('id'))
    for fn in fns:
        if is_wrapper(fn, shared.regs):
            ctx.constants = constants_of(fn, unit_constants)
            for n, _ in A.walk(fn.body):
                site = guest_site(ctx, fn, n) if n.get('kind') == 'CallExpr' else None
                if site is not None:
                    tail = 'tail' in site.form.lower()
                    wrappers.setdefault((fn.path, fn.name, tail), target_address(ctx, fn, site))
                    wrappers[(fn.unit, fn.decl.get('id'))] = (tail, target_address(ctx, fn, site))
    out = {}
    seen = collections.Counter()
    for fn in fns:
        if is_wrapper(fn, shared.regs) or any(d.get('kind') in ('FunctionTemplateDecl', 'ClassTemplateDecl') for d in fn.chain):
            continue
        ctx.constants = constants_of(fn, unit_constants)
        rows = []
        done = set()
        for n, _ in A.walk_code(fn.body):
            if n.get('kind') != 'CallExpr' or A.begin(n) in done:
                continue
            done.add(A.begin(n))
            site = guest_site(ctx, fn, n)
            if site is not None:
                tail = 'tail' in site.form.lower()
                address = target_address(ctx, fn, site)
                rows.append('%s0x%x' % ('tail ' if tail else '', address) if address is not None else 'indirect')
                continue
            x = A.strip_implicit(A.children(n)[0]) if A.children(n) else {}
            d = x.get('referencedDecl') or {}
            if x.get('kind') != 'DeclRefExpr' or d.get('kind') != 'FunctionDecl':
                continue
            name = d.get('name', '')
            found = x.get('foundReferencedDecl') or {}
            written = fn.text.raw[A.begin(n):A.end(n)] if A.begin(n) is not None and A.end(n) is not None else ''
            tail = written.startswith('tail::')
            exact = wrappers.get((fn.unit, d.get('id')))
            if exact:
                rows.append('%s0x%x' % ('tail ' if exact[0] else '', exact[1]) if exact[1] is not None else 'indirect')
            elif found.get('kind') == 'FunctionTemplateDecl' and found.get('id') not in helpers and ctx.named_address(fn, name + '_') is not None:
                rows.append('0x%x' % ctx.named_address(fn, name + '_'))
            elif not name.startswith('__builtin'):
                rows.append(public_name(name))
        key = '%s %s' % (fn.rel, public_name(fn.name))
        seen[key] += 1
        out['%s #%d' % (key, seen[key])] = rows
    return out


def audit_compare(before, after):
    old, new = json.load(open(before)), json.load(open(after))
    wrong = 0
    for key in sorted(set(old) | set(new)):
        a, b = old.get(key), new.get(key)
        if a is None or b is None:
            print('%s: %s' % (key, 'only before' if b is None else 'only after'))
            wrong += a is not None
        elif a != b:
            wrong += 1
            i = next((i for i, (x, y) in enumerate(zip(a, b)) if x != y), min(len(a), len(b)))
            print('%s: call %d: %s, was %s' % (key, i, b[i] if i < len(b) else 'nothing', a[i] if i < len(a) else 'nothing'))
    print('%d functions, %d differ' % (len(old), wrong))
    return 1 if wrong else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('command', choices=('run', 'list', 'report', 'audit', 'audit-compare'))
    ap.add_argument('passes', nargs='?')
    ap.add_argument('--tree', default=ztree.TREE)
    ap.add_argument('--build')
    ap.add_argument('--report', default=os.path.join(SCRATCH, 'report.tsv'))
    ap.add_argument('--only')
    ap.add_argument('--files')
    ap.add_argument('--units', action='store_true', help='compile only the units --files names: for functions no other unit can name yet')
    ap.add_argument('--out')
    ap.add_argument('--against')
    ap.add_argument('--jobs', type=int, default=5)
    ap.add_argument('--budget', type=float)
    a = ap.parse_args()
    if a.command == 'list':
        print('\n'.join(list(PASSES) + ['comments']))
        return 0
    if a.command == 'report':
        rows = collections.Counter()
        for line in open(a.report, encoding='utf-8'):
            f = line.rstrip('\n').split('\t')
            rows[(f[0], f[4])] += 1
        for (name, reason), n in rows.most_common():
            print('%6d %-8s %s' % (n, name, reason))
        return 0
    if a.command == 'audit-compare':
        return audit_compare(a.against, a.out)
    global LOCALS_TSV, FILES, DEADLINE, STATE
    if a.budget:
        DEADLINE = time.monotonic() + a.budget
    FILES = a.files.split(',') if a.files else None
    STATE = a.report + '.state'
    tree = A.Tree(a.tree, a.build or os.path.join(a.tree, 'out', 'pass'), jobs=a.jobs)
    LOCALS_TSV = ztree.zlayout.path(tree.tree, 'tools', 'readable', 'readable_locals.tsv')
    if a.units:
        tree.only_units = FILES
    if a.command == 'audit':
        asts, failed = tree.asts()
        if failed:
            raise SystemExit('the tree does not compile: %s' % sorted(failed)[:5])
        with open(a.out, 'w', encoding='utf-8') as f:
            json.dump(call_map(tree, asts), f, indent=0)
        return 0
    report = Report()
    load_state(report)
    names = list(PASSES) if a.passes == 'all' else a.passes.split(',')
    only = set(a.only.split(',')) if a.only else None
    asts, failed = tree.asts()
    if failed:
        first = sorted(failed)[0]
        raise SystemExit('the tree does not compile: %d units, %s:\n%s' % (len(failed), tree.rel(first), failed[first][:1500]))
    def count_registrations(raw):
        return len(REGISTRATION.findall(re.sub(r'//[^\n]*', '', raw)))

    registered = {p: count_registrations(tree.texts[p].raw) for p in tree.files}
    stopped = False
    try:
        for name in names:
            if name == 'comments':
                pass_comments(tree, report)
            else:
                asts = run_pass(tree, name, report, only, asts)
    except OutOfBudget:
        stopped = True
    if not stopped and os.path.exists(STATE):
        os.remove(STATE)
    for p, n in registered.items():
        if count_registrations(tree.texts[p].raw) != n:
            raise SystemExit('%s: a registration came or went (%d, was %d): a bug in a pass' % (tree.rel(p), count_registrations(tree.texts[p].raw), n))
    os.makedirs(os.path.dirname(a.report), exist_ok=True)
    with open(a.report, 'w', encoding='utf-8') as f:
        for row in report.rows:
            f.write('\t'.join(map(str, row)) + '\n')
    for name in names:
        print('%-8s %6d sites, %5d functions' % (name, report.sites[name], len(report.functions[name])))
    print('kept: %d (%s)' % (len(report.rows), a.report))
    if stopped:
        print('stopped by --budget: run it again to go on')
    return 3 if stopped else 0


if __name__ == '__main__':
    sys.exit(main())
