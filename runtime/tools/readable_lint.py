#!/usr/bin/env python3
"""The user's bar for readable code, per function (CONTRIBUTING.md "Writing readable code", docs/READABLE.md).

  readable_lint.py [--game GAME_DIR] [--json FILE] [--by-file] [--fails N] [--only NAME ...]
                   [--ratchet [FILE]] [--update-ratchet [FILE]] [--stamp FILE] [--rebaseline REASON [--baseline FILE]]

Verified says a function works; this says a person can read and mod it. Neither implies
the other, and the two are always reported apart. A function is `readable` when it has
authored source that passes every rule:

  placed     lives in a named subsystem file: not sweep/, not generated at build time, no file
             named for addresses, batches or tools (blocks_, hooked_)
  addresses  no guest address in its body, directly or through a constant whose value is one, comments
             included; a guest address in a comment anywhere in game code fails the --ratchet too
  registers  no registers, no stack offsets, no raw field offsets, no raw guest pointer reads; no local named for
             a register (heldS0, leftA2, a uint32_t a0) and no register named in its comments, its body's or the
             block above it (a word the file's code declares is that name, not a register: a triangle's v0); an
             ee::LeftByCaller names no register (its place is readable_entry_reads.tsv's, given by the build)
  named      no machine names (FUN_, fn_, kFun, field_0x, param_N, unknown) and no use of a
             padding field; a guess never passes, so a name marked inferred fails too
  scaffold   no proof scaffolding in the logic (marks, callee saves, frames, VU0 register work)
  calls      plain typed calls: no ee::call, game::call or ee::game in the logic

The registration line itself is not the function's body: its address goes when the symbol
table does. ee::Local, ee::Float and ee::Ptr are allowed: they are types, not scaffolding.
A helper a body calls answers for what it does: a function with no registration of its own, in the same file or a
thin wrapper (two statements at most) in a header the file includes, is scanned for raw addresses and offsets, VU0
register work, ee::selfCall and the other scaffolding, and what it holds fails it and every function that reaches
it, as `via NAME: LABEL`. A registered function answers for itself and is not followed. The framework is not charged:
ee/, zge/math/vec.h and zge/math/scale_leftover.h stay legal (scale_leftover.h: the one named form of what the
engine's scale macro carries into VU0 where no vector can be named, zge::scaleLeftover(zge::LeftBy::caller|lastCall)).

Game folders (game/dm2, game/bmx: a game's own code, in its instance only) are counted apart, each against its own
baseline (readable_lint_baseline_<game>.txt here), so AI's numbers and AI's baseline are what they were: a folder's
functions are linted with the engine and the framework beside it for its helpers, keyed by the game's own symbol rows,
and its guest addresses in comments are a count that only goes down (the baseline's `# comment addresses` line).

--ratchet: the build's check: readable only goes up. FILE (default readable_lint_baseline.txt here) lists
every function that passed, by address; exit 1 if one of them fails a rule now or is no longer registered,
naming it and the rule. --update-ratchet writes FILE from today's tree, refusing to drop a function (the
orchestrator's step at a merge: the file is pinned, docs/READABLE.md "The gate"). --stamp FILE: touched
after a passing --ratchet, for the build. --rebaseline REASON: the one step that may lower it, when the rules got
stricter: writes the file from today's tree and keeps the reason and every function dropped in the file's comments, so
the drop is in the diff; a function that fails is named with what it fails. The orchestrator's alone, in its own commit,
then pins.py update. A reason is required.

Verified is not in that file, and this tool says nothing about whether a function works.
"""
import argparse
import collections
import json
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import readable_symbols  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
RULES = ('placed', 'addresses', 'registers', 'named', 'scaffold', 'calls')

GUEST_LOW, GUEST_HIGH = 0x00100000, 0x02000000
HEX = re.compile(r'\b0[xX]([0-9a-fA-F]+)u?\b', re.I)
BAD_FILE = re.compile(r'(^|/)(sweep/|blocks_|hooked_|batch|draft)|_[0-9a-fA-F]{6,8}\.cpp$')
REGISTERS = [
    (r'\bee::reg::', 'ee::reg'), (r'\bcallerRegister(High)?\b', 'callerRegister'), (r'\bcallerFloat\b', 'callerFloat'),
    (r'\bleftByCall(High|Float)?\b', 'leftByCall'), (r'\bee::leave(Float)?\b', 'ee::leave'),
    (r'\bpassRegisters\b', 'passRegisters'), (r'\bframe\.at<', 'frame.at'), (r'\bframe\.address\(', 'frame.address'),
    (r'\bLeftByCaller\s*<[^;<>]*,', 'LeftByCaller register'),
    (r'\bee::at<[^;\n]*\+\s*(0x[0-9a-fA-F]+|\d+)', 'ee::at + offset'), (r'\bee::at<', 'ee::at'),
    (r'\bat<[^;\n>]*>\([^;\n]*\+\s*0x', 'at + offset'), (r'\breinterpret_cast<[^>]*>\([^;\n]*\+\s*0x', 'cast + offset'),
    (r'\+\s*0x[0-9a-fA-F]+u?\b', 'raw offset'), (r'\.cast<', 'Ptr::cast'),
    (r'(\.|->)address\b(?!\()', 'raw address'), (r'\baddressOf\(', 'addressOf'), (r'\breinterpret_cast<', 'reinterpret_cast'),
]
NAMED = [
    (r'\bFUN_[0-9a-fA-F]+\b', 'FUN_'), (r'\bfn_[0-9a-fA-F]+\b', 'fn_'), (r'\bkFun[0-9A-Fa-f]+\b', 'kFun'),
    (r'\bfield_0x', 'field_0x'), (r'\bparam_\d+\b', 'param_N'), (r'\bunknown\w*', 'unknown'),
    (r'\bg_[0-9a-fA-F]{6,8}\b', 'g_ADDRESS'), (r'\bunk_?[0-9A-Fa-f]+\b', 'unk'), (r'(\.|->)_[0-9a-fA-F]{2,4}\b', 'padding field'),
    (r'\bvar_?\d+\b', 'varN'), (r'\blocal_[0-9a-f]+\b', 'local_N'), (r'\binferred\b', 'inferred'),
]
SCAFFOLD = [
    (r'\bee::block\(', 'ee::block'), (r'\bee::loopBack(Whole)?\(', 'ee::loopBack'), (r'\bee::afterCall\(', 'ee::afterCall'),
    (r'\bee::blockWhole\(', 'ee::blockWhole'), (r'\bee::chargeLoop\(', 'ee::chargeLoop'), (r'\bee::chargeCycles\(', 'ee::chargeCycles'),
    (r'\bee::safePoint\(', 'ee::safePoint'), (r'\bee::tableJump\(', 'ee::tableJump'), (r'\bee::jumpDispatch\(', 'ee::jumpDispatch'),
    (r'\bee::calleeSaves\(', 'calleeSaves'), (r'\bsaves\(', 'saves'), (r'\bee::StackFrame\b', 'StackFrame'), (r'\bsaveEntry\(', 'saveEntry'),
    (r'\b(?:ee::|zge::)?selfCall\b', 'selfCall'), (r'\bee::callKeepingVu0\b', 'callKeepingVu0'), (r'\bVu0Registers\b', 'Vu0Registers'),
    (r'\bee::vu0::', 'ee::vu0'), (r'\bleftInVf\d?\b', 'leftInVf'), (r'\bleaveInVf\d?\b', 'leaveInVf'), (r'\bvu0?\.\w+\(', 'vu0 machine'),
    (r'\bee::mmi::', 'ee::mmi'), (r'\bgoto\b', 'goto'), (r'\b(__asm__|__asm|asm)\b\s*[(_]', 'inline asm'),
]
CALLS = [(r'\bee::(tailCall|call|kernelCall|game)\b', 'ee::call'), (r'\bgame::(tailCall|call)(At)?<', 'game::call')]

# What a helper answers for. A typed view alone (ee::at, Ptr::cast, reinterpret_cast) is how a header names a thing.
HELPER_LABELS = {
    'raw address', 'raw offset', 'ee::at + offset', 'at + offset', 'cast + offset', 'addressOf', 'frame.at', 'frame.address',
    'selfCall', 'ee::vu0', 'leftInVf', 'leaveInVf', 'vu0 machine', 'Vu0Registers', 'callKeepingVu0',
    'ee::block', 'ee::loopBack', 'ee::afterCall', 'ee::blockWhole', 'ee::chargeLoop', 'ee::chargeCycles', 'ee::safePoint',
    'calleeSaves', 'saves', 'StackFrame', 'saveEntry', 'ee::tableJump', 'ee::jumpDispatch',
    'ee::reg', 'callerRegister', 'callerFloat', 'leftByCall', 'LeftByCaller register', 'ee::leave', 'passRegisters', 'ee::mmi'}

REGISTRATION = re.compile(r'^\s*READABLE_(FUNCTION(?:_WAITS|_VU0)?|PLANNED)(_IN|_OWN)?\(([^)]*)\)', re.M)
# A function only one engine version has, in a shared file (ee/readable_waits.h): (form, game, `fn` and a status word
# the form had before tools19, read by nothing: its status is the game's table's). Linted with that game's folder and
# keyed by its row there; AI's count does not see it.
ONLY_REGISTRATION = re.compile(r'^\s*READABLE_(FUNCTION(?:_WAITS|_VU0)?)_ONLY\(\s*(\w+)\s*,([^)]*)\)', re.M)
BASELINE = os.path.join(HERE, 'readable_lint_baseline.txt')
_only_statuses = {}


def only_status(root, game, name):
    """An _ONLY function's status: its row of ROOT/GAME/symbols_status.tsv, Draft without one (readable_symbols.py's)."""
    if (root, game) not in _only_statuses:
        rows = {}
        path = os.path.join(root, game, 'symbols_status.tsv')
        if os.path.exists(path):
            for line in open(path, encoding='utf-8'):
                p = line.split()
                if len(p) >= 2 and not line.startswith('#'):
                    rows[p[0]] = p[1]
        _only_statuses[(root, game)] = rows
    return _only_statuses[(root, game)].get(name, 'Draft')
AI_LAYERS = ('zge', 'aggressive', 'sce', 'ee')
GAME_FOLDERS = ('dm2', 'bmx')  # a game's own folder: linted with zge, sce and ee beside it, counted against its own baseline
GAME_BASELINE = os.path.join(HERE, 'readable_lint_baseline_%s.txt')
SYMBOLS = os.path.join(HERE, 'readable_symbols.tsv')
CONSTANT = re.compile(r'\b(?:constexpr|const)\s+[\w:<>]+\s+(k\w+|g_\w+)\s*(?:=|\{)\s*(0[xX][0-9a-fA-F]+)')
GLOBAL = re.compile(r'\bee::Global<[^;]*?,\s*(0[xX][0-9a-fA-F]+)u?>\s+(\w+)')


def strip_comments(text):
    """Blanks comments and strings, keeping every newline, so offsets and lines stay."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        two = text[i:i + 2]
        if two == '//':
            j = text.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i))
            i = j
        elif two == '/*':
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r'[^\n]', ' ', text[i:j]))
            i = j
        elif c == '"' or c == "'":
            j = i + 1
            while j < n and text[j] != c and text[j] != '\n':
                j += 2 if text[j] == '\\' else 1
            out.append(' ' * (j + 1 - i))
            i = j + 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def comments_only(text):
    """Blanks everything but comments, keeping every newline, so offsets and lines stay."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        two = text[i:i + 2]
        if two == '//' or two == '/*':
            j = text.find('\n' if two == '//' else '*/', i + 2)
            j = n if j < 0 else (j if two == '//' else j + 2)
            out.append(text[i:j])
            i = j
        elif c == '"' or c == "'":
            j = i + 1
            while j < n and text[j] != c and text[j] != '\n':
                j += 2 if text[j] == '\\' else 1
            out.append(re.sub(r'[^\n]', ' ', text[i:j + 1]))
            i = j + 1
        else:
            out.append(c if c == '\n' else ' ')
            i += 1
    return ''.join(out)


def body_of(code, name, before):
    """The definition of `name` that ends before offset `before`: the (start, end) of its braces."""
    pattern = re.compile(r'(?<![\w:.>&])' + re.escape(name) + r'\s*\(')
    best = None
    for m in pattern.finditer(code, max(0, before - 400000), before):
        depth, i = 0, m.end() - 1
        while i < before:  # past the parameters
            if code[i] == '(':
                depth += 1
            elif code[i] == ')':
                depth -= 1
                if depth == 0:
                    break
            i += 1
        i += 1
        while i < before and code[i] in ' \t\r\nconstexptruvolai':
            i += 1
        if i < before and code[i] == '{':
            best = i
    if best is None:
        return None
    depth, i = 0, best
    while i < len(code):
        if code[i] == '{':
            depth += 1
        elif code[i] == '}':
            depth -= 1
            if depth == 0:
                return best, i + 1
        i += 1
    return None


def text_word(v):
    """Three capitals and a NUL read as a word (0x00534857, "WHS"): above every game's image, the shape of a tag."""
    return v >> 24 == 0 and all(0x41 <= (v >> shift) & 0xFF <= 0x5A for shift in (0, 8, 16))


def guest_value(text):
    v = int(text, 16)
    return not text_word(v) and (GUEST_LOW <= v < GUEST_HIGH or 0x70000000 <= v < 0x70004000 or 0x10000000 <= v < 0x14000000)


def sources(game, layers=AI_LAYERS):
    for layer in layers:  # (a game folder has its own count: lint_folder)
        for root, _, files in os.walk(os.path.join(game, layer)):
            for f in sorted(files):
                if f.endswith(('.h', '.cpp')):
                    yield os.path.join(root, f)


def comment_addresses(game, layers=AI_LAYERS):
    """(file, line, text) of every guest address in a comment of game code."""
    found = []
    for path in sources(game, layers):
        notes = comments_only(open(path, errors='replace').read())
        for hit in HEX.finditer(notes):
            if guest_value(hit.group(1)):
                found.append((relative(path, game), notes.count('\n', 0, hit.start()) + 1, hit.group(0)))
    return found


REGISTER_WORD = re.compile(r'(?<![\w.$\-/])\$?(v[01]|a[0-3]|t[0-9]|s[0-7]|k[01]|ra|gp|sp|fp|f(?:[0-9]|[12][0-9]|3[01])|vf\d{1,2}|vi\d{1,2})(?![\w\-])')
# held, kept and left are what the tools wrote for a register's value, so a vertex (toV0) stays out: V needs one of those.
REGISTER_LOCAL = re.compile(r'\b(?:(?!k[A-Z])[a-z]\w{2,}?(?:S[0-7]|A[0-3]|T[0-9]|Ra|Gp|Fp|K[01])|(?:held|kept|left|saved|caller|entry)V[01]'
                            r'|(?:s[0-7]|a[0-3]|t[0-9]|ra)[A-Z]\w*)\b')
REGISTER_NAMED = re.compile(r'\b(?:u?int\d+_t|auto|unsigned\s+long|unsigned\s+int|ee::Address|ee::Qword)\s*&?\s*(s[0-7]|v[01]|a[0-3]|t[0-9]|ra|gp|fp|sp|k[01])\b(?!\()')
LIBRARY_NAMES = {'s1', 's2', 'fp'}  # a C library's own parameter names (strcmp's, a stream's)
DECLARED = re.compile(r'[\w>&*]\s+[&*]*(\w+)\s*(?:[=;,)({\[])')


def register_locals(text, rel):
    """{label: count} of the names in a text that are a register's."""
    hits = collections.Counter(m.group(0) for m in REGISTER_LOCAL.finditer(text))
    for m in REGISTER_NAMED.finditer(text):
        if not (rel.startswith('sce/') and m.group(1) in LIBRARY_NAMES):
            hits[m.group(1)] += 1
    return hits


def register_comments(notes, declared):
    """The register words of a comment text. A word the file's code declares is that name; so are v0 and v1 beside a v2."""
    found = []
    for line in notes.split('\n'):
        vertices = re.search(r'\bv[2-9]\b', line) is not None
        for m in REGISTER_WORD.finditer(line):
            word = m.group(1)
            if word not in declared and not (vertices and word in ('v0', 'v1')):
                found.append(word)
    return found


def doc_comment(raw, at):
    """The comment lines right above offset at's line."""
    end = raw.rfind('\n', 0, at)
    lines = []
    while end > 0:
        start = raw.rfind('\n', 0, end) + 1
        line = raw[start:end].strip()
        if not line.startswith(('//', '/*', '*')):
            break
        lines.append(line)
        end = start - 1
    return '\n'.join(lines)


CONTAINER = re.compile(r'\b(namespace|struct|class|union|enum)\b')
HEADER = re.compile(r'(~?\w+)\s*\(((?:[^()]|\((?:[^()]|\([^()]*\))*\))*)\)\s*(?:const\s*)?(?:noexcept\s*)?(?:override\s*)?$')
NOT_FUNCTIONS = {'if', 'for', 'while', 'switch', 'return', 'catch', 'sizeof', 'decltype', 'alignas', 'static_assert', 'operator'}
FRAMEWORK = re.compile(r'^(ee/|zge/math/vec\.h$|zge/math/scale_leftover\.h$)')


def relative(path, start):
    """A game path with forward slashes, as the rules' patterns write it, on any host."""
    return os.path.relpath(path, start).replace(os.sep, '/')
PLAIN_CALL = re.compile(r'(?<![\w.>&])(?:\w+::)*([A-Za-z_]\w*)\s*(?:<[^()<>]*>\s*)?\(')
MEMBER_CALL = re.compile(r'(?:\.|->)\s*([A-Za-z_]\w*)\s*(?:<[^()<>]*>\s*)?\(')
INCLUDE = re.compile(r'#include\s+"([^"]+)"')
THIN = 2  # statements


def functions_in(code):
    """(name, start, end of the body, is a member) of every function with a body at namespace or struct level."""
    out = []
    stack = []
    last = paren = i = 0
    n = len(code)
    while i < n:
        c = code[i]
        if c == '(':
            paren += 1
        elif c == ')':
            paren -= 1
        elif c == ';' and paren <= 0:
            last = i + 1
        elif c == '{':
            head = ' '.join(re.sub(r'^\s*#.*$', '', code[last:i], flags=re.M).split())
            if 'fn' in stack:
                stack.append('other')
            elif re.match(r'^(inline\s+)?namespace\b', head) or head.startswith('extern'):
                stack.append('ns')
            elif CONTAINER.search(head) and '(' not in head:
                stack.append('type')
            else:
                m = HEADER.search(head)
                if m and m.group(1) not in NOT_FUNCTIONS and '=' not in head.split('(')[0]:
                    close, depth = i, 0
                    while close < n:
                        depth += (code[close] == '{') - (code[close] == '}')
                        if depth == 0:
                            break
                        close += 1
                    out.append((m.group(1), i, close + 1, 'type' in stack))
                    stack.append('fn')
                else:
                    stack.append('other')
            last = i + 1
        elif c == '}':
            if stack:
                stack.pop()
            last = i + 1
        i += 1
    return out


def charged(text, rel):
    """{rule: {label: count}} of what a helper's body holds."""
    hits = collections.defaultdict(collections.Counter)
    for label, n in register_locals(text, rel).items():
        hits['registers']['register-named ' + label] += n
    for rule, table in (('registers', REGISTERS), ('scaffold', SCAFFOLD)):
        for pattern, label in table:
            n = len(re.findall(pattern, text))
            if n and label in HELPER_LABELS:
                hits[rule][label] += n
    return hits


class Helpers:
    """The functions with no registration of their own, and what each charges its callers."""

    def __init__(self, game, texts, registered):
        self.by_name = collections.defaultdict(list)
        self.includes = {}
        self.reached = {}
        self.game = game
        for path, (raw, code) in texts.items():
            rel = relative(path, game)
            base = os.path.dirname(rel)
            self.includes[rel] = [os.path.normpath(os.path.join(root, i)) for i in INCLUDE.findall(raw) for root in (base, '')]
            if FRAMEWORK.search(rel):
                continue
            for name, start, end, member in functions_in(code):
                if name in registered.get(rel, ()) or name.lstrip('~') != name:
                    continue
                body = code[start:end]
                self.by_name[name].append({'name': name, 'file': rel, 'body': body, 'member': member, 'own': charged(body, rel),
                                           'thin': body.count(';') <= THIN, 'lines': body.count('\n') + 1})

    def reachable(self, rel):
        if rel not in self.reached:
            found, todo = set(), [rel]
            while todo:
                for header in self.includes.get(todo.pop(), ()):
                    if header not in found:
                        found.add(header)
                        todo.append(header)
            self.reached[rel] = found
        return self.reached[rel]

    def called(self, rel, body):
        """The helpers a body in file rel calls: its own file's, or a thin one in a header it includes."""
        out = []
        for pattern, member in ((PLAIN_CALL, False), (MEMBER_CALL, True)):
            for name in set(pattern.findall(body)):
                candidates = [h for h in self.by_name.get(name, ()) if h['member'] == member]
                local = [h for h in candidates if h['file'] == rel]
                if not local:
                    seen = self.reachable(rel)
                    local = [h for h in candidates if h['thin'] and h['file'] in seen]
                out += local
        return out

    def reach(self, rel, body):
        """Every helper a body reaches, through other helpers too."""
        found, todo = {}, self.called(rel, body)
        while todo:
            h = todo.pop()
            if id(h) not in found:
                found[id(h)] = h
                todo += self.called(h['file'], h['body'])
        return list(found.values())


def comment_registers(game):
    """How many register words the comments of game code hold, anywhere: the rule's sites."""
    count = collections.Counter()
    for path in sources(game):
        rel = relative(path, game)
        if rel.startswith('ee/'):
            continue
        raw = open(path, errors='replace').read()
        count[rel] += len(register_comments(comments_only(raw), set(DECLARED.findall(strip_comments(raw)))))
    return count


def manifest_rows(game):
    rows = []
    for layer in ('zge', 'aggressive', 'sce'):
        path = os.path.join(game, layer, 'sweep', 'manifest.txt')
        if os.path.exists(path):
            for line in open(path, errors='replace'):
                if line.startswith('#') or not line.strip():
                    continue
                p = line.rstrip('\n').split('\t')
                rows.append({'addr': p[0], 'status': p[1], 'file': p[2], 'name': p[6] if len(p) > 6 else ''})
    return rows


def function_symbols(of_game=''):
    """{name: address} of the symbol table (AI's rows, or a game's own), for a registration by name."""
    table = {}
    if os.path.exists(SYMBOLS):
        for line in open(SYMBOLS, errors='replace'):
            parts = line.rstrip('\n').split('\t')
            if len(parts) >= 3 and parts[0] == 'function' and (parts[4] if len(parts) > 4 else '') == of_game:
                table[parts[1]] = parts[2]
    return table


def key(address):
    """A function's lasting name in the baseline: its address, in one spelling."""
    return '0x%08X' % int(address, 16) if re.fullmatch(r'0[xX][0-9a-fA-F]+u?', address) else address


LAST_HELPERS = []  # the helpers that hold something: charged to themselves and to whoever reaches them


def lint(game, layers=AI_LAYERS, folder=None):
    """The functions registered in LAYERS (AI's), or with FOLDER a game folder's: its own registrations, keyed by its
    game's symbol rows (a name it has none for keeps its symbol: AI's name, bound in its instance), with the layers
    beside it for its helpers."""
    symbols = function_symbols(folder or '')
    constants = {}
    texts = {}
    for path in sources(game, layers):
        raw = open(path, errors='replace').read()
        # (READABLE_EXCEPT(GAME, REGISTRATION): the registration inside is AI's and the other games', linted as written)
        code = readable_symbols.unwrap_except(strip_comments(raw))
        texts[path] = (raw, code)
        for m in CONSTANT.finditer(code):
            if guest_value(m.group(2)):
                constants[m.group(1)] = m.group(2)
        for m in GLOBAL.finditer(code):
            constants[m.group(2)] = m.group(1)
    constant_re = re.compile(r'\b(' + '|'.join(sorted(map(re.escape, constants), key=len, reverse=True)) + r')\b') if constants else None
    declared_names = {}

    def declared(path, code):
        if path not in declared_names:
            declared_names[path] = set(DECLARED.findall(code))
        return declared_names[path]

    registered = collections.defaultdict(set)
    for path, (raw, code) in texts.items():
        for m in REGISTRATION.finditer(code):
            args = [a.strip() for a in m.group(3).split(',')]
            if m.group(2) == '_OWN':  # (fn, status, game, own): own, a version's own body, is linted as a helper of the file
                args = args[:1]
            registered[relative(path, game)] |= {a.split('::')[-1] for a in args}
        for m in ONLY_REGISTRATION.finditer(code):
            registered[relative(path, game)] |= {a.strip() for a in m.group(3).split(',')}
    helpers = Helpers(game, texts, registered)
    LAST_HELPERS[:] = [{'name': h['name'], 'file': h['file'], 'lines': h['lines'], 'fails': {k: dict(v) for k, v in h['own'].items()}}
                       for found in helpers.by_name.values() for h in found if h['own']]
    results = []
    for path, (raw, code) in texts.items():
        rel = relative(path, game)
        if rel.startswith('ee/'):
            continue
        regs = [] if folder and not rel.startswith(folder + '/') else [
            (m.start(), m.group(1), m.group(3), m.group(2)) for m in REGISTRATION.finditer(code)]
        if folder:
            regs += [(m.start(), m.group(1), m.group(3), None) for m in ONLY_REGISTRATION.finditer(code) if m.group(2) == folder]
        for start, kind, arglist, suffix in sorted(regs):
            args = [a.strip() for a in arglist.split(',')]
            if kind == 'PLANNED':
                continue
            if suffix == '_OWN':  # _OWN(fn, status, game, own[, game, own]): AI's registration of fn (ee/readable_waits.h)
                args = ['sym::' + args[0], args[0], args[1]]
            if len(args) == 4 and not args[0].startswith('0'):  # _IN(game, address, fn, status)
                args = args[1:]
            if len(args) == 1 and suffix is None:  # an _ONLY (fn): its status is its game's table's (readable_symbols only_status)
                args = args + [only_status(game, folder, args[0])]
            if len(args) == 2:  # (fn, status): by name
                args = ['sym::' + args[0].split('::')[-1]] + args
            if len(args) < 3:
                continue
            address, name, status = args[0], args[1].split('::')[-1], args[2]
            if address.startswith('sym::'):
                address = symbols.get(address[5:], address)
            found = body_of(code, name, start)
            fails = collections.defaultdict(collections.Counter)
            if BAD_FILE.search(rel):
                fails['placed'][rel] += 1
            if found is None:
                fails['placed']['no body found'] += 1
                text, comment = '', ''
                lines = 0
            else:
                text = code[found[0]:found[1]]
                comment = raw[found[0]:found[1]]
                lines = text.count('\n') + 1
            for hit in HEX.finditer(text):
                if guest_value(hit.group(1)):
                    fails['addresses'][hit.group(0)] += 1
            if found is not None:
                for hit in HEX.finditer(comments_only(raw[found[0]:found[1]])):
                    if guest_value(hit.group(1)):
                        fails['addresses']['comment ' + hit.group(0)] += 1
            if constant_re:
                for hit in constant_re.finditer(text):
                    fails['addresses'][hit.group(1)] += 1
            for rule, table, source in (('registers', REGISTERS, text), ('named', NAMED, text), ('scaffold', SCAFFOLD, text),
                                        ('calls', CALLS, text)):
                for pattern, label in table:
                    n = len(re.findall(pattern, source))
                    if n:
                        fails[rule][label] += n
            if re.search(r'\binferred\b', comment):
                fails['named']['inferred'] += 1
            if found is not None:
                definition = code.rfind(name, 0, found[0])
                for label, n in register_locals(code[definition:found[1]], rel).items():
                    fails['registers']['register-named ' + label] += n
                words = register_comments(comments_only(raw[found[0]:found[1]]) + '\n' + doc_comment(raw, definition), declared(path, code))
                if words:
                    fails['registers']['register in a comment'] += len(words)
            for h in helpers.reach(rel, text):
                for rule, hits in h['own'].items():
                    for label, n in hits.items():
                        fails[rule]['via %s: %s' % (h['name'], label)] += n
            documented = False
            if found is not None:
                start = raw.rfind('\n', 0, raw.rfind(name, 0, found[0]))
                above = raw[:start].rstrip().rsplit('\n', 1)[-1].strip()
                documented = above.startswith(('//', '*/', '/*'))
            results.append({'addr': address, 'name': name, 'file': rel, 'status': status, 'lines': lines, 'documented': documented,
                            'fails': {k: dict(v) for k, v in fails.items()}})
    return results


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default=os.path.join(HERE, '..', 'src', 'game'))
    ap.add_argument('--json')
    ap.add_argument('--by-file', action='store_true')
    ap.add_argument('--fails', type=int, default=0)
    ap.add_argument('--only', nargs='*')
    ap.add_argument('--ratchet', nargs='?', const=BASELINE)
    ap.add_argument('--update-ratchet', nargs='?', const=BASELINE)
    ap.add_argument('--stamp')
    ap.add_argument('--rebaseline', metavar='REASON')
    ap.add_argument('--baseline', default=BASELINE)
    a = ap.parse_args()
    game = os.path.abspath(a.game)
    results = lint(game)
    rows = manifest_rows(game)
    authored = {key(r['addr']) for r in results}
    swept = [r for r in rows if key(r['addr']) not in authored]
    if a.only:
        for r in results:
            if r['name'] in a.only:
                print(r['name'], r['file'], r['status'], r['lines'], 'lines:', 'readable' if not r['fails'] else json.dumps(r['fails']))
        return 0
    passed = [r for r in results if not r['fails']]
    total = len(results) + len(swept)
    print('authored functions %d, swept rows with no source %d: %d in all' % (len(results), len(swept), total))
    print('readable (all rules): %d, %.2f%% of %d' % (len(passed), 100.0 * len(passed) / max(total, 1), total))
    body = sum(r['lines'] for r in results)
    print('  in lines of authored bodies: %d of %d (%.2f%%)' % (sum(r['lines'] for r in passed), body, 100.0 * sum(r['lines'] for r in passed) / max(body, 1)))
    print('  documented (a comment above, a separate pass): %d of %d authored' % (sum(1 for r in results if r['documented']), len(results)))
    by_status = collections.Counter(r['status'] for r in results)
    ok_status = collections.Counter(r['status'] for r in passed)
    for s in ('Verified', 'Tested', 'Draft'):
        print('  %-9s authored %5d, readable %5d' % (s, by_status[s], ok_status[s]))
    for rule in RULES:
        n = sum(1 for r in results if rule in r['fails'])
        print('  fails %-10s %5d of %d authored' % (rule, n, len(results)))
    via = sum(1 for r in results if r['fails'] and all(k.startswith('via ') for hits in r['fails'].values() for k in hits))
    print('  helpers that fail: %d; functions that fail only through one: %d' % (len(LAST_HELPERS), via))
    print('  register words in comments of game code: %d; functions charged with a comment: %d, with a register-named local: %d' % (
        sum(comment_registers(game).values()),
        sum(1 for r in results if 'register in a comment' in r['fails'].get('registers', {})),
        sum(1 for r in results if any(k.startswith('register-named') for k in r['fails'].get('registers', {})))))
    near = collections.Counter(len(r['fails']) for r in results)
    print('  rules failed per function:', ', '.join('%d: %d' % (k, near[k]) for k in sorted(near)))
    label = collections.Counter()
    for r in results:
        for rule, hits in r['fails'].items():
            for k, n in hits.items():
                label[(rule, k if rule in ('registers', 'named', 'scaffold', 'calls') else 'site')] += n
    for (rule, k), n in sorted(label.items(), key=lambda kv: -kv[1])[:24]:
        print('    %-10s %-20s %6d' % (rule, k, n))
    if a.by_file:
        files = collections.defaultdict(lambda: [0, 0, 0])
        for r in results:
            f = files[r['file']]
            f[0] += 1
            f[1] += 0 if r['fails'] else 1
            f[2] += r['lines']
        for name, (n, ok, lines) in sorted(files.items(), key=lambda kv: -kv[1][2]):
            print('%6d %5d %6d %s' % (n, ok, lines, name))
    for r in results[:a.fails]:
        if r['fails']:
            print(r['name'], json.dumps(r['fails']))
    summary = {'functions': total, 'readable': len(passed), 'authored': len(results), 'lines': body,
               'readable_lines': sum(r['lines'] for r in passed),
               'by_rule': {rule: sum(1 for r in results if rule in r['fails']) for rule in RULES},
               'by_status': {s: [by_status[s], ok_status[s]] for s in ('Verified', 'Tested', 'Draft')}}
    if a.json:
        with open(a.json, 'w') as f:
            json.dump({'summary': summary, 'functions': results, 'swept': swept, 'helpers': LAST_HELPERS}, f, indent=0)
    if a.rebaseline is not None:
        return rebaseline(a.baseline, results, summary, a.rebaseline)
    code = 0
    for path in filter(None, (a.ratchet, a.update_ratchet)):
        code = max(code, ratchet(path, results, summary, bool(a.update_ratchet)))
    if a.ratchet or a.update_ratchet:
        stray = comment_addresses(game)
        print('guest addresses in comments: %d' % len(stray))
        for rel, line, text in stray[:20]:
            print('COMMENT: %s:%d: %s' % (rel, line, text))
        code = max(code, 1 if stray else 0)
    for folder in GAME_FOLDERS:
        code = max(code, report_folder(game, folder, a))
    if a.stamp and code == 0:
        with open(a.stamp, 'w') as f:
            f.write('%d of %d\n' % (len(passed), total))
    return code


def report_folder(game, folder, a):
    """A game folder's count, apart from AI's: its readable functions and its guest addresses in comments, each
    against the folder's baseline under --ratchet (written by --update-ratchet)."""
    if not os.path.isdir(os.path.join(game, folder)):
        return 0
    results = lint(game, (folder, 'zge', 'sce', 'ee'), folder)
    passed = [r for r in results if not r['fails']]
    body = sum(r['lines'] for r in results)
    print('game folder %s: authored functions %d, readable (all rules) %d; in lines of authored bodies: %d of %d' % (
        folder, len(results), len(passed), sum(r['lines'] for r in passed), body))
    for rule in RULES:
        n = sum(1 for r in results if rule in r['fails'])
        if n:
            print('  %s fails %-10s %5d of %d authored' % (folder, rule, n, len(results)))
    stray = comment_addresses(game, (folder,))
    if not (a.ratchet or a.update_ratchet):
        print('  %s guest addresses in comments: %d' % (folder, len(stray)))
        return 0
    path = GAME_BASELINE % folder
    before = read_baseline(path) if os.path.exists(path) else {}
    was = None
    if os.path.exists(path):
        for line in open(path):
            m = re.match(r'# comment addresses: (\d+)', line)
            if m:
                was = int(m.group(1))
    summary = {'readable': len(passed), 'functions': len(results), 'readable_lines': sum(r['lines'] for r in passed), 'lines': body}
    code = ratchet(path, results, summary, False)
    print('  %s guest addresses in comments: %d (baseline %s)' % (folder, len(stray), '-' if was is None else was))
    if was is not None and len(stray) > was:
        for rel, line, text in stray[:20]:
            print('COMMENT: %s:%d: %s' % (rel, line, text))
        code = 1
    if a.update_ratchet and code == 0:
        write_baseline(path, {key(r['addr']): r for r in results}, summary, baseline_history(path),
                       '# comment addresses: %d\n' % len(stray))
    return code


def read_baseline(path):
    rows = {}
    for line in open(path):
        if not line.startswith('#') and line.strip():
            address, _, name = line.strip().partition(' ')
            rows[address] = name
    return rows


def ratchet(path, results, summary, update):
    before = read_baseline(path) if os.path.exists(path) else {}
    now = {key(r['addr']): r for r in results}
    lost = []
    for address, name in sorted(before.items()):
        r = now.get(address)
        if r is None:
            lost.append('%s %s: no longer registered' % (address, name))
        elif r['fails']:
            lost.append('%s %s (%s): %s' % (address, r['name'], r['file'], json.dumps(r['fails'])))
    gained = sorted(k for k, r in now.items() if not r['fails'] and k not in before)
    print('ratchet: %d in %s, %d lost, %d new' % (len(before), os.path.basename(path), len(lost), len(gained)))
    for line in lost:
        print('RATCHET: no longer readable: ' + line)
    if lost:
        return 1
    if update:
        write_baseline(path, now, summary, baseline_history(path))
    return 0


def baseline_history(path):
    """The file's lines about earlier rebaselines."""
    if not os.path.exists(path):
        return []
    return [line.rstrip('\n') for line in open(path) if line.startswith('# rebaseline')]


def write_baseline(path, now, summary, history, extra=''):
    with open(path, 'w') as f:
        f.write('# Every function that passes readable_lint.py, by address: the ratchet (--ratchet). Written by --update-ratchet.\n')
        f.write('# %d of %d functions, %d of %d authored body lines.\n' % (
            summary['readable'], summary['functions'], summary['readable_lines'], summary['lines']))
        f.write(extra)
        for line in history:
            f.write(line + '\n')
        for address in sorted(k for k, r in now.items() if not r['fails']):
            f.write('%s %s\n' % (address, now[address]['name']))


def rebaseline(path, results, summary, reason):
    """Lowers the baseline, saying why and what goes (a stricter rule's truthful number)."""
    reason = ' '.join(reason.split())
    if not reason:
        print('rebaseline: a reason is required')
        return 2
    before = read_baseline(path) if os.path.exists(path) else {}
    now = {key(r['addr']): r for r in results}
    lost = sorted(a for a in before if a not in now or now[a]['fails'])
    history = baseline_history(path)
    history.append('# rebaseline %s: %s: %d dropped' % (time.strftime('%Y-%m-%d'), reason, len(lost)))
    for address in lost:
        r = now.get(address)
        why = 'no longer registered' if r is None else ', '.join(sorted(k for hits in r['fails'].values() for k in hits))[:120]
        history.append('# rebaseline dropped %s %s: %s' % (address, before[address], why))
    write_baseline(path, now, summary, history)
    print('rebaseline: %d in %s, %d dropped, %d now: %s' % (len(before), os.path.basename(path), len(lost), summary['readable'], reason))
    return 0


if __name__ == '__main__':
    sys.exit(main())
