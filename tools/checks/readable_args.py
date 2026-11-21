#!/usr/bin/env python3
"""A register the original hands a callee and the readable call does not: every call site in
hand-written and drafted code against what its callee reads.

A replay cannot see this. zx-fncheck answers a call from the recording, so a callee that lost
an argument still "returns" the recorded result, and a register the caller leaves in place is
right in the replay's context whoever called the function. In the game a game::call reaches its
callee as C++: the context holds what the last guest call left, not the arguments. Lost this way
(all with every replay passing): activateEditTool's a1, updateEditTool's f12, readEditUiButtons'
a0 (an EE hang at tick 4480), levelObjectsFrame's seconds (a garbage physics step rate),
freeLoadedFile's a0 (a level-load hang).

Registers, a function's `needs`: what its code reads before setting it (a0-a3, t0-t3, f12-f19), on
any path, its callees' and tail calls' reads of what it leaves included: fncheck_traced_gen.py's
scan, the one zx-fncheck's args_left uses. An indirect call counts what its recorded callees
need (readable_indirect_calls.tsv: `readable_args.py indirect CORPUS` writes it from the
recordings); a site no recording reached counts nothing and is listed (`unseen`). Reads that a
person has read and found dead are taken out with their reason (readable_args_dead.tsv), for the
tool and the checker alike.

What it reports:

  signature  a readable function reached as C++ (game::call, tailCall, callAt, a plain call)
             whose original needs a register its signature does not name.
  dropped    an ee::call or ee::tailCall that leaves out a register its callee needs and the
             original set before the call. A call through a game function pointer (below) too, and
             there also a value the original sets in the jalr's own block, whatever its recorded
             callees read. A call of a game's own function (its own row: sym::bmx::x, sym::dm2::x, or a plain
             sym::x in a file of games/bmx or games/dm2, in the call, its calls:: stand-in's line or a constant of
             one) counts what that game's function needs, read in its recompiled code (--generated-dm2,
             --generated-bmx; missing, the call is a hit), never AI's function at the same address.
  entry      the same, the register being the caller's own entry value left in place, in a
             function reached as C++: there the context does not hold it.
  unseen     an indirect call no recording reached: the original holds a value it set, or a
             named parameter, in a register the call does not pass.
  entrytable the same as entry in a function only reached through the table (no C++ caller): right in
             the game today, the bug the day a hand-written caller appears. Listed, not counted.
  left       the register is what an earlier callee left; listed, not counted (ee::leftByCall
             passes it on where it is meant).
  caller     ee::callerRegister or ee::callerFloat of an argument register its original needs
             in a function reached as C++.
  stack      a callee that reads stack arguments called from a function with no ee::StackFrame.
  leftdirect ee::leftByCall straight after a game::call: in the game t_left is what the callee's
             last guest call left, not the recording's. Listed, not counted.
  format     a call of a printf-style function (formatString, formatScratch, formatLocalizedText, debugPrint,
             report, sprintf, printf, fiprintf) whose literal format reads another number of words than the
             call passes (see "Formats" below). Counted; a draft's gets `draft` like the rest.
  result     ee::callerRegister(v0) in a function reached as C++ whose direct caller keeps the
             result: the v0 a path of the original leaves as it found it. Listed, not counted.

Calls that do not say ee::call count the same. A call through ee::FunctionPtr or ee::Callback (a field, a local, a
parameter, a global, a table's element, a copy of one through `auto`, a temporary, a function that returns one, by an
alias or not, `.tail` too) is an indirect guest call: the pointer's declared signature says which arguments are floats,
and for such a call `entry` counts wherever the function is reached from, because the type claims what the call takes. A
helper that hands on a pack (`template <typename... A> R call(uint32_t target, A... args)`, member or not) is checked at
each place it is called.

A hit in a Draft row (it does not run in the game) gets `draft` after its kind and is not counted;
the hit counts once the row is promoted. A hit read and found harmless goes in
readable_args_ok.tsv with its reason (kind, function, callee, registers, why). A `dropped` row names its
jal (`a1@120174`), not the function: another call of the same callee that loses an argument is a new hit.

Formats. A variadic callee spills a2-t3 (or what follows its fixed parameters) to a frame and
vsprintf reads that frame as far as the format asks, so the dead-read rows take those registers
out of the argument scan above and nothing else would notice a call that drops a word the format
reads (or passes one it does not read). format_check parses each call's format and compares:
every conversion, a `*` width or precision, takes one word, `%%` none. The EE ABI puts every word
in its own 8-byte slot (_vfprintf_r's va_arg is `ld/lw -8($s4)` after `addiu $s4, $s4, 8` at every
site: an int, a long long, a pointer and a double each one slot), so a 64-bit or double word is one
argument, not two. The registers hold 8 words less the fixed parameters; a call by name of a function with a long
form (an inline overload that stores the rest with ee::storeVariadicStackWords, as formatString and formatScratch have:
zge/text/cstring.h) may pass more, and is judged by the words its format reads alone. A format is found by name or address: a string the ELF holds (--elf, default
../extracted/SLUS_203.27); one that is a game's own row (sym::bmx::x, sym::dm2::x, or a plain sym::x in a file of games/bmx or games/dm2, in the
call or a constant of one)
is read in that game's ELF (--elf-bmx, --elf-dm2). formatLocalizedText expands `$KEY` through the language's string table
first (expandStringKeys: the key ends at a space, NUL, CR, LF or `:`, an unknown or empty one stays
as written); the tables are assets/STRING.* (--text-dir, default the AI workspace's recomp/disc-root/ASSETS) and all
languages must agree. A call whose format is not a literal, or passes words the format does not
read, is a row of readable_args_formats.tsv (function, callee, format as written, words passed,
why); a row no call uses fails too.

usage: readable_args.py [--check] [--tree DIR] [--build DIR] [--generated DIR] [--list KINDS] [--all]
       readable_args.py explain ADDR|NAME [REG]    why a function needs a register
       readable_args.py formats                    only the format check (a few seconds)
       readable_args.py indirect CORPUS            rewrites readable_indirect_calls.tsv

--check: exit status 1 if anything counted is left (the merge gate). --build: the build folder
whose readable_sweep holds the drafts.
"""
import argparse
import bisect
import os
import re
import struct
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ztree  # noqa: E402  (the z-axis tree checked: ztree.py)
import zaxis_paths  # noqa: E402  (tools/paths.env: the workspaces, the data, the work database)
import fnrec_io  # noqa: E402  (a recording plain or compressed)
import readable_symbols
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True
sys.setrecursionlimit(20000)
import fncheck_traced_gen as scan  # noqa: E402

NAMES = ['a0', 'a1', 'a2', 'a3', 't0', 't1', 't2', 't3'] + ['f%d' % (12 + i) for i in range(8)]
GPR_NAMES = ('zero at v0 v1 a0 a1 a2 a3 t0 t1 t2 t3 t4 t5 t6 t7 s0 s1 s2 s3 s4 s5 s6 s7 t8 t9 k0 k1 '
             'gp sp fp ra').split()
COUNTED = ('signature', 'dropped', 'entry', 'unseen', 'caller', 'stack', 'format')


def mask_names(mask):
    return [NAMES[i] for i in range(16) if mask & (1 << i)]


STD_MEMBERS = {'begin', 'end', 'size', 'empty', 'data', 'find', 'insert', 'erase', 'clear', 'count', 'get', 'reset', 'swap'}


def blank_comments(raw):
    """raw with comments and string contents as spaces (offsets kept)."""
    out = list(raw)
    i, n = 0, len(raw)
    while i < n:
        c = raw[i]
        if c == '/' and raw[i + 1:i + 2] == '/':
            while i < n and raw[i] != '\n':
                out[i] = ' '
                i += 1
        elif c == '/' and raw[i + 1:i + 2] == '*':
            j = raw.find('*/', i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if raw[k] != '\n':
                    out[k] = ' '
            i = j
        elif c == '"':
            j = i + 1
            while j < n and raw[j] != '"':
                j += 2 if raw[j] == '\\' else 1
            for k in range(i + 1, min(j, n)):
                if raw[k] != '\n':
                    out[k] = ' '
            i = j + 1
        elif c == "'" and not raw[i - 1:i].isalnum():
            j = i + 1
            while j < n and raw[j] != "'":
                j += 2 if raw[j] == '\\' else 1
            for k in range(i + 1, min(j, n)):
                out[k] = ' '
            i = j + 1
        else:
            i += 1
    return ''.join(out)


class Function:
    """A C++ function with a body (a registered readable function or a helper)."""

    def __init__(self, path, name, ret, params, start, end):
        self.path, self.name, self.ret, self.params = path, name, ret, params
        self.start, self.end = start, end  # the body's braces
        self.addresses = []
        self.status = {}      # address -> Draft, Tested, Verified
        self.calls = []       # Site
        self.sees_entry = []  # (reg, line) of callerRegister / callerFloat
        self.plain = set()    # identifiers called as functions
        self.typed = set()    # typed declarations it calls (calls::name, ee/plain.h)
        self.wrapper = False  # itself one of those: its own call is the call site
        self.stack_frame = False

    @property
    def short(self):
        return self.name.split('::')[-1]


class Site:
    def __init__(self, fn, line, kind, how, target, args, ret_address=None):
        self.fn, self.line, self.kind, self.how = fn, line, kind, how
        self.target, self.args = target, args  # target: an address, None (indirect), or a function name
        self.target_text = None  # the target as written
        self.ret_address = ret_address  # addresses before it, last first: its jal's return address among them


CONTAINER = re.compile(r'\b(namespace|struct|class|union|enum)\b')
HEADER = re.compile(r'([~\w:]+)\s*\(((?:[^()]|\((?:[^()]|\([^()]*\))*\))*)\)\s*(?:const\s*)?(?:noexcept\s*)?(?:override\s*)?$')
NOT_FUNCTIONS = {'if', 'for', 'while', 'switch', 'return', 'catch', 'sizeof', 'decltype', 'alignas', 'static_assert'}
CALL = re.compile(r'\b(ee|game)::(call|tailCall|callAt|tailCallAt|callKeepingVu0|game)\b')
WRAPPER_BODY = re.compile(r'\b(ee::(game|tailCall|call)|game::(call|tailCall|callAt|tailCallAt))\b')
PLAIN = re.compile(r'(?<![\w:.>&])((?:\w+::)*[A-Za-z_]\w*)\s*\(')
ENTRY_READ = re.compile(r'\bee::caller(Register|Float)\(\s*((?:ee::reg::)?\w+)')
FIELDS = re.compile(r'\b(?:ee::)?(?:Float|float)\s+((?:\w+\s*(?:\[[^\]]*\])?\s*,\s*)*\w+\s*(?:\[[^\]]*\])?)\s*;')
OTHER_FIELDS = re.compile(r'\b(?:u?int\d+_t|char|bool|\w*Ptr<[^>;]*>|[A-Z]\w+)\s+((?:\w+\s*(?:\[[^\]]*\])?\s*,\s*)*\w+\s*(?:\[[^\]]*\])?)\s*;')
# A game function held as data (ee/function_ptr.h, ee/callback.h): `pool->allocate(bytes)` is the original's jalr.
POINTER_TYPE = re.compile(r'\b(?:ee::)?(?:FunctionPtr|Callback)\s*<')
POINTER_ALIAS = re.compile(r'\busing\s+(\w+)\s*=\s*(?=(?:ee::)?(?:FunctionPtr|Callback)\s*<)')
RECORD = re.compile(r'\b(?:struct|class|union)\s+(\w+)[^;{()]*\{')
FLOAT_GLOBAL = re.compile(r'\b(?:ee::)?\w+<\s*(?:ee::)?(?:Float|float)\s*(?:\[[^\]]*\])?\s*(?:,[^>]*)?>\s+(\w+)')


def split_args(text):
    """The top-level arguments of `a, b<c, d>(e), f`."""
    out, depth, angle, cur = [], 0, 0, []
    i = 0
    while i < len(text):
        c = text[i]
        if c in '([{':
            depth += 1
        elif c in ')]}':
            depth -= 1
        elif c == '<' and depth == 0 and re.search(r'[\w>]$', ''.join(cur[-1:])) and \
                re.match(r'\s*[\w:]+(\s*\*)?\s*(,\s*[\w:]+\s*)*>\s*\(', text[i + 1:]):
            j = text.find('>', i)
            cur.append(text[i:j + 1])
            i = j + 1
            continue
        if c == ',' and depth == 0:
            out.append(''.join(cur).strip())
            cur = []
        else:
            cur.append(c)
        i += 1
    last = ''.join(cur).strip()
    if last:
        out.append(last)
    return out


def matching(text, i):
    """The index past the bracket that closes text[i]."""
    pairs = {'(': ')', '<': '>'}
    opener, closer = text[i], pairs[text[i]]
    depth = 0
    while i < len(text):
        c = text[i]
        if c == opener:
            depth += 1
        elif c == closer:
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return len(text)


def parse_functions(path, raw, code):
    """Every function with a body in the file, at namespace or struct level."""
    out = []
    stack = []  # 'ns', 'type', 'fn', 'other'
    last = 0  # the end of the last ; { or } outside brackets
    paren = 0
    i, n = 0, len(code)
    while i < n:
        c = code[i]
        if c == '(':
            paren += 1
        elif c == ')':
            paren -= 1
        elif c == ';' and paren <= 0:
            last = i + 1
        elif c == '{':
            head = code[last:i]
            head1 = ' '.join(head.split())
            if 'fn' in stack:
                stack.append('other')
            elif re.match(r'^namespace calls\b', head1):
                stack.append('calls')
            elif re.match(r'^namespace( tail)?\s*$', head1):
                stack.append('anon')
            elif re.match(r'^(template\s*<[^>]*>\s*)?(inline\s+)?namespace\b', head1) or head1.startswith('extern'):
                stack.append('ns')
            elif CONTAINER.search(head1) and '(' not in head1:
                stack.append('type')
            else:
                m = HEADER.search(head1)
                if m and m.group(1).split('::')[-1] not in NOT_FUNCTIONS and '=' not in head1.split('(')[0]:
                    close = i
                    depth = 0
                    while close < n:
                        if code[close] == '{':
                            depth += 1
                        elif code[close] == '}':
                            depth -= 1
                            if depth == 0:
                                break
                        close += 1
                    ret = head1[:m.start()].strip()
                    out.append(Function(path, m.group(1), ret, split_args(m.group(2)), i, close))
                    # One of the typed lines readable_pass.py writes in a namespace { }: its call is the call site.
                    out[-1].wrapper = 'calls' in stack or ('anon' in stack and code.count(';', i, close) == 1 and
                                                           WRAPPER_BODY.search(code, i, close) is not None)
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


def is_float_type(t):
    return bool(re.search(r'\b(ee::)?(Float|float)\b', t)) and '*' not in t and 'Ptr' not in t and '&' not in t and \
        POINTER_TYPE.search(t) is None


class Tree:
    """The sources: functions, registrations, constants, call sites."""

    def __init__(self):
        self.files = {}          # path -> (raw, code)
        self.originals = {}      # path -> the source as written (sym:: names; the same lines as raw)
        self.functions = []
        self.by_name = {}        # short name -> [Function]
        self.by_address = {}     # address -> Function
        self.names = {}          # address -> name
        self.constants = {}      # path -> {kName: address}
        self.everywhere = {}     # kName -> {address}
        self.float_fields = set()
        self.other_fields = set()
        self.float_constants = set()
        self.float_values = set()  # ee::Global<ee::Float, ...> g_x, ee::Ptr<ee::Float> p: *g_x
        self.float_helpers = set() # ee.h, vu0.h: ee::toFloat, ee::max
        self.sites = []
        self.reached = {}        # path -> what it includes
        self.pointer_aliases = {}  # alias -> its signature's parameters (the last one declared)
        self.alias_defs = {}       # alias -> {path: parameters}: one name can be declared in several files
        self.pointer_decls = {}    # path -> [(offset, name, parameters)]
        self.pointer_fields = {}   # name -> {struct: parameters}: a member of a struct
        self.ee_methods = set()    # member functions of ee/'s types
        self.untold = set()        # (path, function start, name): a copy of a member several structs have
        self.told_by_receiver = False
        self.pointer_globals = {}  # path -> {name: parameters}: outside any struct and function
        self.pointer_returns = {}  # a function that returns a pointer -> parameters
        self.record_ranges = {}    # path -> [(start, end)] of its structs

    def add(self, path):
        original = open(path, encoding='utf-8', errors='replace').read()
        self.originals[path] = original
        raw = readable_symbols.literal_view(original)
        # (the signatures and calls this game's build compiles: a per-version parameter or argument, ee/readable_waits.h,
        # is its word in the game it is for and no text in another)
        raw = readable_symbols.version_view(raw, getattr(self, 'which', 'ai'), keep_length=True)
        code = blank_comments(raw)
        self.files[path] = (raw, code)
        constants = {}
        # (constexpr uint32_t kA = 0x1u, kB = 0x2u;: readable_args.py read only the first form and
        # took a declaration of several for no constants, so a call of kB went unchecked)
        for m in re.finditer(r'\b(?:constexpr|const)\s+(?:std::)?uint32_t\s+(k\w+\s*=[^;]*);', code):
            for one in m.group(1).split(','):
                v = re.fullmatch(r'\s*(k\w+)\s*=\s*(0x[0-9A-Fa-f]+)u?\s*', one)
                if v:
                    constants[v.group(1)] = int(v.group(2), 16)
                    self.everywhere.setdefault(v.group(1), set()).add(constants[v.group(1)])
        for m in re.finditer(r'\bconstexpr\s+(?:ee::)?(?:Float|float)\s+(k\w+)\b', code):
            self.float_constants.add(m.group(1))
        self.constants[path] = constants
        for m in FIELDS.finditer(code):
            for name in m.group(1).split(','):
                self.float_fields.add(name.split('[')[0].strip())
        for m in OTHER_FIELDS.finditer(code):
            for name in m.group(1).split(','):
                self.other_fields.add(name.split('[')[0].strip())
        for m in FLOAT_GLOBAL.finditer(code):
            self.float_values.add(m.group(1))
        functions = parse_functions(path, raw, code)
        self.functions += functions
        for f in functions:
            self.by_name.setdefault(f.short, []).append(f)
            if f.short.endswith('_'):
                # A marked body: game code calls it by the name without the underscore (GAME_CALL, ee/readable.h).
                self.by_name.setdefault(f.short[:-1], []).append(f)
        return functions

    def order(self, path):
        """A file's place in the order: a readable source by its key (the order before the readable folders were
        apart), a draft by its path."""
        key = ztree.zlayout.readable_key(self.game_root, path) if getattr(self, 'game_root', None) else None
        return (0, key) if key is not None else (1, path)

    def includes(self, path):
        out = []
        for m in re.finditer(r'#include\s+"([^"]+)"', self.files[path][0]):
            base = os.path.dirname(path)
            for root in (base,) + tuple(os.path.join(self.game_root, r) for r in ztree.zlayout.READABLE_ROOTS):
                cand = os.path.normpath(os.path.join(root, m.group(1)))
                if cand in self.files:
                    out.append(cand)
                    break
        return out

    def reachable(self, path):
        """The files a file includes, directly or through another."""
        if path in self.reached:
            return self.reached[path]
        found, todo = self.reached.setdefault(path, set()), [path]
        while todo:
            for header in self.includes(todo.pop()):
                if header not in found:
                    found.add(header)
                    todo.append(header)
        return found

    def resolve(self, path, expr):
        """A call's target expression: an address, or None for one known only at run time."""
        e = re.sub(r'^(?:\w+::)+', '', expr.strip())
        m = re.fullmatch(r'0x([0-9A-Fa-f]+)u?', e)
        if m:
            return int(m.group(1), 16)
        if not re.fullmatch(r'k\w+', e):
            return None
        if e in self.constants[path]:
            return self.constants[path][e]
        for header in self.includes(path):
            if e in self.constants[header]:
                return self.constants[header][e]
        values = self.everywhere.get(e, set())
        return next(iter(values)) if len(values) == 1 else None

    def register(self):
        import readable_digest as rd
        for path, (raw, _) in self.files.items():
            regs = [(int(m.group(2), 16), m.group(3), m.group(4)) for m in rd.REG.finditer(raw)] if self.which == 'ai' else []
            regs += [(int(m.group(2), 16), m.group(3), m.group(4)) for m in rd.REG_IN.finditer(raw) if m.group(1) == self.which]
            for address, name, status in regs:
                short = name.split('::')[-1]
                real = [f for f in self.by_name.get(short, []) if not f.wrapper]
                cands = [f for f in real if f.path == path] or real
                self.names[address] = short
                if cands:
                    cands[0].addresses.append(address)
                    cands[0].status[address] = status
                    self.by_address[address] = cands[0]

    def pointer_signature(self, code, at):
        """The parameters of the pointer type whose `<` opens at code[at], and the index past its `>`."""
        close = matching(code, at)
        text = ' '.join(code[at + 1:close - 1].split())
        i = text.find('(')
        inner = text[i + 1:text.rfind(')')] if i >= 0 else ''
        return [p for p in split_args(inner) if p and p != 'void'], close

    def alias_params(self, path, name):
        """The parameters of the alias `name` as the file sees it: its own declaration, else one in a header it
        includes (several files declare a MessageHandler of their own)."""
        defs = self.alias_defs.get(name, {})
        if path in defs:
            return defs[path]
        for header in sorted(self.reachable(path)):
            if header in defs:
                return defs[header]
        return self.pointer_aliases[name]

    def scan_pointers(self):
        """Every name declared with a function pointer type: its signature, by where it is declared."""
        for path, (_, code) in self.files.items():
            for m in POINTER_ALIAS.finditer(code):
                self.pointer_aliases[m.group(1)] = self.pointer_signature(code, code.index('<', m.end()))[0]
                self.alias_defs.setdefault(m.group(1), {})[path] = self.pointer_aliases[m.group(1)]
        alias = re.compile(r'\b(%s)\b' % '|'.join(sorted(self.pointer_aliases))) if self.pointer_aliases else None
        bodies = {}
        for f in self.functions:
            bodies.setdefault(f.path, []).append((f.start, f.end))
        for path, (_, code) in self.files.items():
            types = []  # (start of the type, index past it, parameters)
            for m in POINTER_TYPE.finditer(code):
                params, close = self.pointer_signature(code, m.end() - 1)
                types.append((m.start(), close, params))
            if alias:
                for m in alias.finditer(code):
                    if code[max(0, m.start() - 6):m.start()].rstrip().endswith('using'):
                        continue
                    close = matching(code, m.end()) if code[m.end():m.end() + 1] == '<' else m.end()
                    types.append((m.start(), close, self.alias_params(path, m.group(1))))
            found = []
            records = []
            for m in RECORD.finditer(code):
                depth, i = 0, m.end() - 1
                while i < len(code):
                    depth += (code[i] == '{') - (code[i] == '}')
                    if depth == 0:
                        break
                    i += 1
                records.append((m.end(), i, m.group(1)))
            self.record_ranges[path] = records
            for start, close, params in sorted(types):
                # ee::Global<TYPE, sym::x> g_x, ee::Ptr<TYPE> table, std::array<TYPE, 4> table
                wrapped = re.compile(r'(?:\s*,\s*[\w:]+)?\s*>').match(code, close)
                decl = re.compile(r'\s*[&*]*\s*(?:const\s+)?([A-Za-z_]\w*)((?:\s*\[[^\]]*\])*)\s*([;=({,])').match(code, wrapped.end() if wrapped else close)
                if decl is None or decl.group(1) in ('operator', 'const'):
                    continue
                name, follow = decl.group(1), decl.group(3)
                inside = any(a <= start < b for a, b in bodies.get(path, ()))
                if follow == ',' and not re.match(r'\s*\w+\s*[;,=]', code[decl.end():decl.end() + 80]):
                    continue  # a parameter: Function.params has it
                names = [name]
                if follow == ',':
                    tail = code[decl.end() - 1:code.find(';', decl.end())]
                    names += re.findall(r',\s*(\w+)', tail)
                for name in names:
                    found.append((decl.start(1), name, params))
                    if inside:
                        continue
                    if follow == '(':
                        self.pointer_returns[name] = params
                    else:
                        owner = [r for a, b, r in records if a <= start < b]
                        if not owner:
                            self.pointer_globals.setdefault(path, {})[name] = params
                            continue
                        self.pointer_fields.setdefault(name, {})[owner[-1]] = params
            self.pointer_decls[path] = found
        for f in self.functions:
            if POINTER_TYPE.search(f.ret) or (alias and alias.search(f.ret)):
                m = POINTER_TYPE.search(f.ret)
                self.pointer_returns[f.short] = self.pointer_signature(f.ret, m.end() - 1)[0] if m else \
                    self.alias_params(f.path, alias.search(f.ret).group(1))

    def visible_pointers(self, f, body):
        """The names f's code can call bare: its parameters, its locals, the file's and its headers' globals."""
        names = dict(self.pointer_globals.get(f.path, {}))
        for header in self.reachable(f.path):
            names.update(self.pointer_globals.get(header, {}))
        names.update({d[1]: d[2] for d in self.pointer_decls.get(f.path, ()) if f.start <= d[0] < f.end})
        for p in f.params:
            m = re.search(r'(\w+)\s*(?:\[[^\]]*\])?$', p)
            if not m:
                continue
            t = POINTER_TYPE.search(p)
            if t:
                names[m.group(1)] = self.pointer_signature(p, t.end() - 1)[0]
            else:
                for word in re.findall(r'\w+', p[:m.start()]):
                    if word in self.pointer_aliases:
                        names[m.group(1)] = self.alias_params(f.path, word)
        # const auto hook = record->hook; for (const auto setup : kSetups): a copy of a pointer
        for m in re.finditer(r'\bauto\s*[&*]*\s*(\w+)\s*(?:=\s*(?!\[)([^;{]*?)\s*;|:\s*([^;{]*?)\s*\))', body):
            tail = re.search(r'(\w+)((?:\[[^\]]*\])*)$', m.group(2) or m.group(3))
            if not tail:
                continue
            known = names.get(tail.group(1))
            records = self.pointer_fields.get(tail.group(1), {})
            if known is None and records:
                known = next(iter(records.values()))
                if len({tuple(r) for r in records.values()}) > 1:  # (the struct is not told: no signature can be trusted)
                    self.untold.add((f.path, f.start, m.group(1)))
            if known is not None:
                names[m.group(1)] = known
        return names

    def pointer_calls(self, f, body, lines):
        """The calls through a function pointer in f: one guest site each, its target known only at run time."""
        declared = {d[0] for d in self.pointer_decls.get(f.path, ())}
        visible = self.visible_pointers(f, body)
        found = []

        def after(i):
            while body[i:i + 1] in (' ', '\n', '\t'):
                i += 1
            return i

        def add(at, open_at, params, how, exact=True):
            close = matching(body, open_at)
            args = split_args(body[open_at + 1:close - 1])
            if not exact and len(params) != len(args):  # (a member of that name in another struct: the one the call fits)
                params = next((p for p in self.pointer_fields.get(body[at:].split('(')[0].strip(), {}).values() if len(p) == len(args)), params)
            found.append((at, how, args, params, body[at:at + 24].split('(')[0].strip(), exact))

        for m in re.finditer(r'[A-Za-z_]\w*', body):
            name = m.group(0)
            at = f.start + m.start()
            if at in declared or (m.start() and (body[m.start() - 1].isalnum() or body[m.start() - 1] in '_')):
                continue
            head = body[:m.start()].rstrip()
            member = head.endswith('->') or head.endswith('.')
            params = visible.get(name) if not member else self.member_pointer(f, body, head, name)
            exact = (f.path, f.start, name) not in self.untold if not member else \
                self.told_by_receiver or len({tuple(r) for r in self.pointer_fields.get(name, {}).values()}) == 1
            if params is None and not member and name in self.pointer_fields and self.in_record(f, name):
                params = next(iter(self.pointer_fields[name].values()))
            if params is None:
                continue
            i = after(m.end())
            while body[i:i + 1] == '[':
                depth = 0
                j = i
                while j < len(body):
                    depth += (body[j] == '[') - (body[j] == ']')
                    j += 1
                    if depth == 0:
                        break
                i = after(j)
            if body[i:i + 1] == ')':  # (*g_hook)(x), (record->hook)(x); not `if (*g_hook) (*g_hook)(x)`
                i = after(i + 1)
                if body[i:i + 1] == '(' and not re.search(r'\b(?:if|while|for|switch)\s*\(\s*!?\s*\*?\s*(?:[\w.]+(?:->)?)*$', head):
                    add(m.start(), i, params, 'pointer', exact)
            elif body[i:i + 1] == '(':
                add(m.start(), i, params, 'pointer', exact)
            elif re.compile(r'\.\s*tail\s*\(').match(body, i):
                add(m.start(), body.index('(', i), params, 'tailCall', exact)
        # ee::FunctionPtr<int32_t()>(address)(), Alias{address}(), hookOf(kind)(x)
        for m in POINTER_TYPE.finditer(body):
            params, close = self.pointer_signature(body, m.end() - 1)
            self.temporary(body, close, m.start(), params, add)
        for name in self.pointer_aliases:
            params = self.alias_params(f.path, name)
            for m in re.finditer(r'(?<![\w:])%s\b' % re.escape(name), body):
                self.temporary(body, m.end(), m.start(), params, add)
        for name, params in self.pointer_returns.items():
            for m in re.finditer(r'(?<![\w])%s\s*\(' % re.escape(name), body):
                j = after(matching(body, m.end() - 1))
                if body[j:j + 1] == '(':
                    add(m.start(), j, params, 'pointer')
        for start, how, args, params, name, exact in sorted(found, key=lambda x: x[0]):
            published = [int(h, 16) for h in re.findall(r'\b(0x0*[12][0-9A-Fa-f]{5})u?\b', body[max(0, start - 300):start])][::-1]
            site = Site(f, lines.at(f.start + start), 'guest', how, None, args, published)
            site.signature, site.pointer, site.told = params, name, exact
            site.offset = f.start + start
            f.calls.append(site)
            self.sites.append(site)

    def member_pointer(self, f, body, head, name):
        """The signature of `receiver->name` when name is a pointer member of the receiver's type. A receiver whose type
        the text does not tell is taken for one, unless a member function of that name exists too: then the call's
        arity decides (a call through the pointer compiles only with the signature's)."""
        self.told_by_receiver = False
        records = self.pointer_fields.get(name)
        if not records:
            return None
        receiver = re.search(r'(\w+)\s*(?:\[[^\]]*\]\s*)?(?:\)\s*)?(?:->|\.)$', head)
        if receiver:
            own = '%s|%s' % (body, ' , '.join(f.params))
            kind = re.search(r'\b([A-Z]\w*)\s*(?:>\s*)?[&*]*\s*(?:const\s+)?%s\b(?!\s*\()' % re.escape(receiver.group(1)), own)
            if kind and kind.group(1) in records:
                self.told_by_receiver = True
                return records[kind.group(1)]
            if kind:
                return None
        for g in self.by_name.get(name, []):
            if any(a <= g.start < b for a, b, _ in self.record_ranges.get(g.path, ())):
                return None
        if name in self.ee_methods or name in STD_MEMBERS:
            return None
        return next(iter(records.values()))

    @staticmethod
    def temporary(body, i, start, params, add):
        """A pointer type's temporary called at once: `Type(address)(args)`, `Type{address}(args)`."""
        if body[i:i + 1] == '(':
            j = matching(body, i)
        elif body[i:i + 1] == '{':
            depth, j = 0, i
            while j < len(body):
                depth += (body[j] == '{') - (body[j] == '}')
                j += 1
                if depth == 0:
                    break
        else:
            return
        if body[j:j + 1] == '(':
            add(start, j, params, 'pointer')

    def in_record(self, f, name):
        """Whether f is a member of a struct in whose text the pointer `name` is declared."""
        return any(a < f.start and f.start < b and any(a <= d[0] < b and d[1] == name for d in self.pointer_decls.get(f.path, ()))
                   for a, b, _ in self.record_ranges.get(f.path, ()))

    def extract(self):
        self.scan_pointers()
        for f in self.functions:
            raw, code = self.files[f.path]
            body = code[f.start:f.end]
            f.stack_frame = 'StackFrame' in body or 'ee::Local<' in body
            lines = LineIndex(code)
            for m in ENTRY_READ.finditer(body):
                reg = m.group(2).split('::')[-1]
                f.sees_entry.append((m.group(1), reg, lines.at(f.start + m.start())))
            for m in PLAIN.finditer(body):
                short = m.group(1).split('::')[-1]
                wrapper = m.group(1).startswith(('calls::', 'tail::')) or any(g.wrapper and g.path == f.path for g in self.by_name.get(short, []))
                (f.typed if wrapper else f.plain).add(short)
            for m in CALL.finditer(body):
                i = m.end()
                tmpl = None
                if body[i:i + 1] == '<':
                    j = matching(body, i)
                    tmpl = body[i + 1:j - 1]
                    i = j
                if body[i:i + 1] != '(':
                    continue
                j = matching(body, i)
                args = split_args(body[i + 1:j - 1])
                line = lines.at(f.start + m.start())
                # The published return address (ee::calleeSaves' {31, ...}, or a helper's argument).
                published = [int(h, 16) for h in re.findall(r'\b(0x0*[12][0-9A-Fa-f]{5})u?\b', body[max(0, m.start() - 300):m.start()])][::-1]
                ns, what = m.group(1), m.group(2)
                if ns == 'game':
                    t = (tmpl or '').split(',')
                    name = t[0].strip().lstrip('&').strip()
                    address = self.resolve(f.path, t[1]) if len(t) > 1 else None
                    site = Site(f, line, 'direct', what, address if address is not None else name, args, published)
                else:
                    if what == 'callKeepingVu0':
                        args = args[1:]
                    if not args:
                        continue
                    site = Site(f, line, 'guest', what, self.resolve(f.path, args[0]), args[1:], published)
                    site.target_text = args[0].strip()
                site.offset = f.start + m.start()
                f.calls.append(site)
                self.sites.append(site)
            self.pointer_calls(f, body, lines)

    def wrapped(self):
        """A typed line's call of the game is the caller's call: one guest site at each place it is called, the line's own
        taken out (its registers are the line's, what the original has around the jal is the caller's). A helper that hands
        on a pack (`template <typename... A> R call(uint32_t target, A... args)`) is one too, whatever its size: only its
        callers say what the pack holds, and a member's (`vf6.call<int32_t>(kFind, a)`) is called through its object."""
        found = []
        packed = {}
        for g in self.functions:
            own = [s for s in g.calls if s.kind == 'guest' and s.args and s.args[-1].endswith('...')]
            if own:
                g.pack_sites = own
                packed.setdefault(g.short, []).append(g)
        member = re.compile(r'(?:\.|->)\s*(%s)\s*(?:<[^()]*>)?\s*\(' % '|'.join(map(re.escape, sorted(packed)))) if packed else None
        for f in self.functions:
            if f.wrapper:
                continue
            code = self.files[f.path][1]
            body = code[f.start:f.end]
            lines = LineIndex(code)
            calls = [(m, m.group(1), True) for m in PLAIN.finditer(body)]
            if member:
                calls += [(m, m.group(1), False) for m in member.finditer(body)]
            for m, called, plain_call in sorted(calls, key=lambda c: c[0].start()):
                short = called.split('::')[-1]
                named = [g for g in self.by_name.get(short, []) if (g.wrapper or getattr(g, 'pack_sites', None)) and g is not f]
                if not plain_call:
                    named = [g for g in named if getattr(g, 'pack_sites', None)]
                lines_of = [g for g in named if g.path == f.path and g.calls] or \
                    [g for g in named if g.path in self.reachable(f.path) and g.calls]
                if not lines_of:
                    continue
                args = split_args(body[m.end():matching(body, m.end() - 1) - 1])
                tail = called.startswith('tail::')
                fits = [g for g in lines_of if len(g.params) == len(args) and (g.calls[0].how == 'tailCall') == tail] or \
                    [g for g in lines_of if len(g.params) == len(args)] or lines_of
                line_of = getattr(fits[0], 'pack_sites', fits[0].calls)
                own = line_of[0]
                if own.kind != 'guest':
                    continue
                passed = own.args
                if passed and passed[-1].endswith('...'):  # a pack: what this call hands it
                    fixed = next((k for k, p in enumerate(fits[0].params) if '...' in p), len(args))
                    passed = passed[:-1] + args[fixed:]
                target = own.target
                if target is None:  # a parameter of the helper: this call's argument says
                    held = [k for k, p in enumerate(fits[0].params) if re.search(r'\b%s$' % re.escape(own.target_text or ' '), p)]
                    if held and held[0] < len(args):
                        target = self.resolve(f.path, args[held[0]])
                site = Site(f, lines.at(f.start + m.start()), 'guest', own.how, target, passed, [])
                site.offset = f.start + m.start()
                site.line_of = own
                site.written = args  # (site.args are the typed line's parameters)
                f.calls.append(site)
                found.append(site)
                for other in line_of[1:]:  # (the branches of an `if constexpr`: one call)
                    other.line_of = own
                    found.append(other)
        lines = {id(s.line_of) for s in found} | {id(s) for s in found}
        self.sites = [s for s in self.sites if id(s) not in lines] + [s for s in found if s.fn.calls and getattr(s, 'written', None) is not None]
        self.sites.sort(key=lambda s: (self.order(s.fn.path), getattr(s, 'offset', 0)))

    def is_float(self, expr, f, depth=0):
        """Whether an argument goes in a float register (a guess from the source; a wrong
        one shows as a false hit in triage)."""
        e = expr.strip()
        if depth > 6:
            return False
        while e.startswith('(') and matching(e, 0) == len(e):
            e = e[1:-1].strip()
        level = 0
        for k, c in enumerate(e):  # a ?: is what its branches are
            level += c in '([{'
            level -= c in ')]}'
            if c == '?' and level == 0:
                return self.is_float(e[k + 1:].split(':')[0], f, depth + 1)
        if re.search(r'\.bits\(\)|\bfloatBits\b|\bbit_cast<\s*(?:std::)?uint', e):
            return False
        if re.match(r'^(?:ee::)?Float\b|^float\s*\(|^static_cast<\s*(?:ee::)?(?:Float|float)\s*>', e):
            return True
        if re.match(r'^static_cast<|^reinterpret_cast<', e):
            return False
        if re.fullmatch(r'-?[\d.]+(?:[eE][-+]?\d+)?[fF]', e):
            return True
        if re.match(r'^-?\*\s*(?:\w+\.|ee::)?(?:at|Ptr)<\s*(?:const\s+)?(?:ee::)?(?:Float|float)\s*>', e):
            return True
        if re.match(r'^-?\*?\s*g_\w+', e) and re.match(r'^-?\*?\s*(g_\w+)', e).group(1) in self.float_values:
            return True
        if re.match(r'^ee::(?:call|tailCall)<\s*(?:ee::)?(?:Float|float)\s*>', e):
            return True
        m = re.match(r'^game::(?:call|callAt)<&\s*([\w:]+)', e)
        if m:
            fs = self.by_name.get(m.group(1).split('::')[-1], [])
            return bool(fs) and is_float_type(fs[0].ret)
        if re.search(r'[!=<>]=|&&|\|\||\s[<>]\s', e):
            return False
        depth0, parts, cur = 0, [], ''
        for k, c in enumerate(e):
            if c in '([{':
                depth0 += 1
            elif c in ')]}':
                depth0 -= 1
            if depth0 == 0 and c in '+*/?' and k:
                parts.append(cur)
                cur = ''
            elif depth0 == 0 and c == '-' and k and e[k + 1:k + 2] != '>' and cur.strip():
                parts.append(cur)
                cur = ''
            else:
                cur += c
        parts.append(cur)
        if len(parts) > 1:
            return any(self.is_float(p, f, depth + 1) for p in parts if p.strip())
        e = e.lstrip('*-! ')
        m = re.fullmatch(r'(k\w+)', e.split('::')[-1])
        if m and m.group(1) in self.float_constants:
            return True
        m = re.match(r'^([\w:]+)\s*\(', e)
        if m:
            short = m.group(1).split('::')[-1]
            code = self.files[f.path][1][f.start:f.end]
            lam = re.search(r'\bauto\s+%s\s*=\s*\[[^\]]*\]\s*\(([^)]*)\)\s*(?:->\s*([\w:]+))?' % re.escape(short), code)
            if lam:
                if lam.group(2):
                    return is_float_type(lam.group(2))
                ret = re.search(r'\breturn\s+([^;]+);', code[lam.end():])
                return bool(ret) and self.is_float(ret.group(1), f, depth + 1)
            if m.group(1).startswith('ee::') and short in self.float_helpers:
                return True
        m = re.match(r'^\w+\.(\w+)\s*\(', e)
        if m and m.group(1) in self.float_helpers:  # vu.dot(...)
            return True
        m = re.match(r'^([\w:]+)\s*\(', e)
        if m:
            short = m.group(1).split('::')[-1]
            fs = self.by_name.get(short, [])
            return bool(fs) and is_float_type(fs[0].ret)
        if re.search(r'(->|\.)\s*(\w+)(\[[^\]]*\])?$', e):
            field = re.search(r'(->|\.)\s*(\w+)(\[[^\]]*\])?$', e).group(2)
            return field in self.float_fields and field not in self.other_fields
        if re.fullmatch(r'\w+(\[[^\]]*\])?', e):
            name = e.split('[')[0]
            code = self.files[f.path][1][f.start:f.end]
            for p in f.params:
                if re.search(r'\b%s\s*$' % re.escape(name), p):
                    return is_float_type(p)
            if re.search(r'\b(?:ee::)?(?:Float|float)\s+(?:\w+\s*,\s*)*%s\b' % re.escape(name), code):
                return True
            m = re.search(r'\bauto\s+%s\s*=\s*([^;]+);' % re.escape(name), code)
            if m:
                return self.is_float(m.group(1), f, depth + 1)
        return False

    def passed_mask(self, site, needs=None, own_callee=True):
        """The registers a guest call sets (ints from a0, floats from f12), None for a pack. The
        expressions say which are floats; a callee with a readable signature settles the ones
        they do not (an argument of the wrong class fails the replay's comparison). own_callee False: the
        target is another game's function, whose readable signature this run has not got (needs say)."""
        floats = open_args = 0
        signature = getattr(site, 'signature', None)
        if signature is not None and getattr(site, 'told', False) and len(signature) == len(site.args):  # (the pointer's type says which are floats)
            floats = sum(1 for p in signature if is_float_type(p))
            ints = len(signature) - floats
            return ((1 << ints) - 1) | (((1 << floats) - 1) << 8)
        for a in site.args:
            if a.endswith('...'):
                return None
            if self.is_float(a, getattr(site, 'line_of', site).fn):
                floats += 1
            elif not re.match(r'^(?:-?\d|0x|k[A-Z]|&|static_cast|reinterpret_cast|ee::Ptr|\w+\.address\b)', a) and \
                    '.address' not in a:
                open_args += 1
        callee = self.by_address.get(site.target) if isinstance(site.target, int) and own_callee else None
        wanted = sum(1 for p in callee.params if p and p != 'void' and is_float_type(p)) if callee else \
            bin((needs or {}).get(site.target, 0) >> 8).count('1')
        floats = max(floats, min(wanted, floats + open_args))
        ints = len(site.args) - floats
        return ((1 << ints) - 1) | (((1 << floats) - 1) << 8)


class LineIndex:
    def __init__(self, code):
        self.starts = [m.end() for m in re.finditer(r'\n', code)]

    def at(self, offset):
        return bisect.bisect_right(self.starts, offset) + 1


def signature_mask(f):
    ints = floats = 0
    for p in f.params:
        if p == 'void' or not p:
            continue
        if is_float_type(p):
            floats += 1
        else:
            ints += 1
    return ((1 << ints) - 1) | (((1 << floats) - 1) << 8)


def load_flows(generated):
    flows, texts = {}, {}
    stack = {}
    for name in sorted(os.listdir(generated)):
        m = scan.FUNC_FILE.search(name)
        if not m:
            continue
        try:
            text = open(os.path.join(generated, name), encoding='utf-8', errors='replace').read()
        except OSError:
            continue
        address = int(m.group(1), 16)
        r = scan.stack_args(text)
        if r and name.startswith('entry_') and not re.search(r'addiu\s+\$sp, \$sp, -', text):
            r = None
        if r:
            stack[address] = r
        if not name.startswith('entry_'):
            flow = scan.function_flow(text)
            if flow:
                flows[address] = flow
    return flows, stack


def jalr_sites(flows):
    """{function: [addresses of its jalr]}, and of its jumps through a register."""
    calls, tails = {}, {}
    for f, (_, blocks) in flows.items():
        for steps, succs in blocks.values():
            for s in steps:
                if s[0] == 'call' and s[1] is None:
                    calls.setdefault(f, []).append(s[2])
            for s in succs:
                if isinstance(s, tuple) and s[0] == 'tail' and s[1] is None:
                    tails.setdefault(f, []).append(s[2])
    return calls, tails


def reads_register(x, reg):
    return x is not None and any(n.k == 'reg' and n.v == reg for n in x.walk())


def read_since(ops, op, end, reg):
    """Whether an op between op and ops[end] reads reg."""
    i = next(k for k, o in enumerate(ops) if o is op)
    return any(reads_register(o.x, reg) or reads_register(o.addr, reg) for o in ops[i + 1:end])


class Original:
    """What the original holds in a register at a call: the drafter's register states."""

    def __init__(self, generated):
        import readable_draft as rd
        self.rd = rd
        workspace = os.path.dirname(os.path.dirname(os.path.abspath(generated)))
        self.ws = rd.Workspace(workspace, generated, None)
        self.cal = rd.Callees(self.ws)
        self.cache = {}
        self.variadics = {}
        self.first_argument = {}  # (function, jal) -> a0 when every path makes the same number

    def sites(self, address):
        """[(target or None, {reg: set of origins})] of the calls of a function."""
        if address in self.cache:
            return self.cache[address]
        rd, out = self.rd, []
        r = self.ws.readable.get(address)
        saved = dict(r) if r else None
        if r:
            r['signature'] = None
        self.cal.memo.pop(address, None)
        try:
            L = rd.Lifter(self.ws, address, 1)
            rd.function_params(L, self.cal, 1)
            by_id = {o.id: o for k in L.block_keys() for o in L.blocks[k].ops}
            placed = {o.id: (k, i) for k in L.block_keys() for i, o in enumerate(L.blocks[k].ops)}
            memo = {}

            def number(x, at, depth):
                if x.k == 'c':
                    return x.v
                if x.k == 'bin' and x.v == '+':
                    left, right = number(x.a[0], at, depth), number(x.a[1], at, depth)
                    return None if left is None or right is None else (left + right) & 0xFFFFFFFF
                return held(x.v, at, depth + 1) if x.k == 'reg' and depth < 12 else None

            def held(reg, at, depth):
                if (reg, at) in memo:
                    return memo[(reg, at)]
                memo[(reg, at)] = None
                values = set()
                for v in L.state_before(*at).get(reg, ()):
                    o = by_id.get(v[0])
                    values.add(number(o.x, placed[o.id], depth) if o is not None and o.k == 'set' else None)
                memo[(reg, at)] = values.pop() if len(values) == 1 else None
                return memo[(reg, at)]

            for k in L.block_keys():
                b = L.blocks[k]
                calls = [(o.info, i) for i, o in enumerate(b.ops) if o.k == 'call']
                if b.term[0] == 'tail':
                    calls.append((b.term[1], len(b.ops)))
                for info, i in calls:
                    st = L.state_before(k, i)
                    if isinstance(info.target, int) and self.variadic(info.target):
                        self.first_argument[(address, info.pc)] = held('a0', (k, i), 0)
                    here = {o.id for o in b.ops[:i]}
                    kinds = {}
                    for reg in NAMES:
                        found = set()
                        for v in st.get(reg, {('entry', reg)}):
                            if v[0] == 'entry':
                                found.add('entry' if v[1] == reg else 'set')
                            else:
                                o = by_id.get(v[0])
                                if o is not None and o.k == 'call':
                                    found.add('left')
                                else:
                                    found.add('set')
                                    if v[0] in here:
                                        found.add('near')  # set in the call's own block
                                        if not read_since(b.ops, by_id[v[0]], i, reg) and not reads_register(info.target_x, reg):
                                            found.add('fresh')  # and left for the call: not the block's temporary
                        kinds[reg] = found
                    out.append((info.target, info.pc, kinds))
        except Exception:  # (a function the drafter cannot lift)
            out = None
        finally:
            if r:
                r.update(saved)
            self.cal.memo.pop(address, None)
        self.cache[address] = out
        return out

    def fresh(self, address, reg, pc):
        """Whether the original sets reg in the block of the indirect call at pc and nothing reads it before the call."""
        return any(p == pc and 'fresh' in kinds[reg] for _, p, kinds in self.sites(address) or [])

    def pcs(self, address, target):
        return {pc for t, pc, _ in self.sites(address) or [] if t == target}

    def variadic(self, address):
        if address not in self.variadics:
            try:
                self.variadics[address] = bool(self.cal.reads(address)[2])
            except Exception:
                self.variadics[address] = False
        return self.variadics[address]

    def origin(self, address, target, reg, near=False, pcs=None):
        """'entry': the function's own entry value on every path; 'set': its code set it on
        some path; 'left': an earlier callee's; '?': not known."""
        sites = self.sites(address)
        if sites is None:
            return '?'
        found = set()
        for t, pc, kinds in sites:
            if t == target and (pcs is None or pc in pcs):
                found |= kinds[reg]
        if not found:
            return '?'
        if near and 'near' in found:
            return 'near'
        for kind in ('set', 'entry', 'left'):  # (the worst on any path)
            if kind in found:
                return kind
        return '?'


def sources(tree, build, which='ai'):
    game = tree     # (its readable code: ztree.walk, keys)
    paths = []
    for root, dirs, files in ztree.walk(game):
        rel = ztree.key_of(root, game)
        if rel == 'ee' or rel.startswith('dm2') != (which == 'dm2'):
            continue
        for name in files:
            if name.endswith(('.cpp', '.h')):
                paths.append(os.path.join(root, name))
    drafts = []
    if build:
        for root, dirs, files in os.walk(os.path.join(build, 'runtime', 'readable_sweep')):
            drafts += [os.path.join(root, n) for n in files if n.endswith('.cpp')]
    return game, sorted(paths, key=lambda p: ztree.key_of(p, game)), sorted(drafts)   # (in key order, as before)


def build_tree(a):
    game, paths, drafts = sources(a.tree, a.build, a.game)
    if a.game == 'dm2':  # (the engine's headers its code includes; their calls are AI's)
        drafts = []
    t = Tree()
    t.which = a.game
    t.game_root = game
    for root, _, names in ztree.walk(game, 'ee'):
        for name in names:
            t.ee_methods |= set(re.findall(r'^\s+(?:[\w:<>,&*]+\s+)+(\w+)\s*\([^;]*\)\s*(?:const\s*)?(?:noexcept\s*)?[{;]',
                                           open(os.path.join(root, name), encoding='utf-8', errors='replace').read(), re.M))
    for name in ('ee.h', 'vu0.h', 'abi.h'):
        for m in re.finditer(r'\bFloat\s+(\w+)\s*\(', open(ztree.readable(game, 'ee', name), encoding='utf-8').read()):
            t.float_helpers.add(m.group(1))
    for p in paths + drafts:
        t.add(p)
    t.register()
    t.extract()
    t.wrapped()
    return t, len(drafts)


# ---- Formats -----------------------------------------------------------------------------------

# The printf-style callees: name -> (format parameter, first variadic parameter, localized). The
# registers are a0..t3 in order, so a call can hand 8 - first words.
PRINTF_STYLE = {
    'formatString': (1, 2, False),         # (buffer, format, ...)
    'formatScratch': (0, 1, False),        # (format, ...)
    'formatLocalizedText': (0, 1, True),   # (text with $KEYs, ...)
    'debugPrint': (2, 3, False),           # (x, y, format, ...)
    'report': (0, 1, False),               # (format, ...)
    'sprintf': (1, 2, False),
    'printf': (0, 1, False),
    'fiprintf': (1, 2, False),
}
FORMATS = os.path.join(HERE, 'readable_args_formats.tsv')
LAST_KEYS = []  # (where, row key) of the calls the last format_check listed: what `formats --rows` prints
CONVERSION = re.compile(r'%([-+ #0]*)(\*|\d+)?(?:\.(\*|\d*))?([hlLqjzt]*)([A-Za-z%]?)')
KNOWN_CONVERSIONS = set('diouxXcspfeEgGn')


# A printf-style function's long form: an inline overload that takes words past its registers and stores them where the
# original's caller does, on its own stack (ee::storeVariadicStackWords, ee/plain.h; zge/text/cstring.h's formatString and
# formatScratch, tools11 99ef254a). A call of one by name is judged by the words its format reads, whatever their count.
LONG_FORM = re.compile(r'\bstoreVariadicStackWords\s*\(')


def long_form_callees(t):
    """The printf-style names whose long form the tree defines: a function of that name whose body stores the words
    after the registers' with ee::storeVariadicStackWords."""
    found = set()
    for f in t.functions:
        if f.short in PRINTF_STYLE and LONG_FORM.search(t.files[f.path][1][f.start:f.end]):
            found.add(f.short)
    return found


def register_problem(callee, passed, how, long_forms):
    """Why a call passes more words than it can, or None: the registers hold 8 - first, unless the call is by name and
    the callee has a long form (the rest go on the caller's stack, as the original's call puts them)."""
    first = PRINTF_STYLE[callee][1]
    if passed <= 8 - first or (how == 'a call' and callee in long_forms):
        return None
    return 'passes %d words, the registers hold %d' % (passed, 8 - first)


def format_words(text):
    """The words vsprintf reads for a format, or (None, why). Every conversion and `*` is one slot."""
    words = 0
    for m in CONVERSION.finditer(text):
        flags, width, precision, length, conv = m.groups()
        if conv == '%':
            if flags or width or precision is not None or length:
                return None, 'a modified %% in %r' % m.group(0)
            continue
        if conv not in KNOWN_CONVERSIONS:
            return None, 'the conversion %r is not one vsprintf takes' % m.group(0)
        if length and (set(length) - set('hlq') or len(length) > 2):
            return None, 'the length %r in %r is not one the original reads' % (length, m.group(0))
        words += 1 + (width == '*') + (precision == '*')
    return words, None


def name_hash(name):
    """zge::nameHash: the ELF hash over the upper-cased letters."""
    h = 0
    for c in name:
        h = ((h << 4) + ord(c.upper() if c.isascii() else c)) & 0xFFFFFFFF
        high = h & 0xF0000000
        if high:
            h ^= high >> 24
        h &= ~high & 0xFFFFFFFF
    return h


def string_tables(directory):
    """language -> {key: text}: assets/STRING.* as stringTableText reads them (size, count, [key, offset], texts)."""
    out = {}
    if not os.path.isdir(directory):
        return out
    for name in sorted(os.listdir(directory)):
        if re.fullmatch(r'STRING\.[A-Z]{3}', name.upper()):
            b = open(os.path.join(directory, name), 'rb').read()
            count = struct.unpack_from('<I', b, 4)[0]
            table = {}
            for i in range(count):
                key, offset = struct.unpack_from('<II', b, 8 + 8 * i)
                if key not in table:  # (first match wins)
                    table[key] = b[offset:b.index(b'\0', offset)].decode('latin-1') if offset else ''
            out[name.upper().split('.')[1]] = table
    return out


def expand_string_keys(text, table):
    """expandStringKeys: `$NAME` replaced by the table's text; an unknown or empty one stays as written."""
    def ends(c):
        return c in ' \0\r\n:'
    out, i, n = [], 0, len(text)
    while i < n:
        if text[i] != '$':
            out.append(text[i])
            i += 1
            continue
        i += 1
        name = ''
        if i < n and not ends(text[i]):
            while True:
                if i >= n or text[i] in '"\\':
                    break
                name += text[i]
                i += 1
                if i >= n or ends(text[i]):
                    break
        found = table.get(name_hash(name), '')
        out.append(found if found else '$' + name)
    return ''.join(out)


def load_format_rows():
    """readable_args_formats.tsv: function, callee, format as written, words passed, why."""
    rows = {}
    if os.path.exists(FORMATS):
        for line in open(FORMATS, encoding='utf-8'):
            if line.strip() and not line.startswith('#'):
                f = line.rstrip('\n').split('\t')
                rows[tuple(f[:4])] = f[4] if len(f) > 4 else 'read'
    return rows


def row_game(text, folder, own_rows, address):
    """The game whose own row at address text names: sym::bmx::x or sym::dm2::x anywhere, and a plain sym::x in a file
    of that game's folder (folder: dm2, bmx or None; games/bmx's `sym::textFormat` is BMX XXX's row, as
    readable_symbols.literal_view reads it), or None. own_rows: {game: its own table}."""
    for m in re.finditer(r'\bsym::(dm2|bmx)::(\w+)', text):
        entry = own_rows[m.group(1)].syms.get(m.group(2))
        if entry and entry[1] == address:
            return m.group(1)
    if folder in own_rows:
        for m in re.finditer(r'\bsym::(\w+)(?!\w|::)', text):
            entry = own_rows[folder].syms.get(m.group(1))
            if entry and entry[1] == address:
                return folder
    return None


def call_game(t, s, own_rows):
    """The game whose own function a guest call by address reaches, or None (AI's, or a function the game shares with AI
    through its bind table): row_game of the line that names the target, the call's own line or, for a calls:: stand-in,
    the stand-in's (BMX XXX's `calls::levelFogColour(...)` is `ee::game(sym::bmx::levelFogColour, colour)`, 0x199AA0,
    where AI has moveSkater), or of a constant's definition (`kFind = sym::x`)."""
    if not isinstance(s.target, int):
        return None
    for one in [getattr(s, 'line_of', None), s]:
        if one is None or getattr(one, 'offset', None) is None:
            continue
        path = one.fn.path
        folder = readable_symbols.folder_game(t.originals.get(path, ''))
        if not folder and not re.search(r'\bsym::(?:dm2|bmx)::', t.originals.get(path, '')):
            continue
        code = t.files[path][1]
        original = t.originals.get(path, '').split('\n')
        expr = (one.target_text or '').strip()
        at = code.find(expr, one.offset) if expr else -1
        line = code.count('\n', 0, at if at >= 0 else one.offset)
        game = row_game(original[line], folder, own_rows, s.target) if line < len(original) else None
        if game:
            return game
        if re.fullmatch(r'k\w+', expr):
            for p in [path] + sorted(t.reachable(path)):
                m = re.search(r'\b%s\s*=\s*(sym::(?:(?:dm2|bmx)::)?\w+)' % re.escape(expr), t.originals.get(p, ''))
                if m:
                    game = row_game(m.group(1), readable_symbols.folder_game(t.originals.get(p, '')), own_rows, s.target)
                    if game:
                        return game
                    break
    return None


class GameCode:
    """Another game's recompiled code, read when a call reaches one of its own functions: what each function needs (the
    same scan as AI's, with that game's dead reads; its indirect calls count nothing, as fncheck_traced_gen's table)
    and the stack bytes it reads. The folders: --generated-dm2, --generated-bmx."""
    NUMBER = {'dm2': 1, 'bmx': 2}

    def __init__(self, a):
        self.folders = {'dm2': getattr(a, 'generated_dm2', None), 'bmx': getattr(a, 'generated_bmx', None)}
        self.read = {}
        self.names = {}  # game -> {address: its own function's name}

    def get(self, game):
        """(needs, stack), or None when the game's folder is missing."""
        if game not in self.read:
            folder = self.folders.get(game)
            if not folder or not os.path.isdir(folder):
                self.read[game] = None
            else:
                flows, stack = load_flows(folder)
                dead = scan.load_dead(scan.DEAD_READS_OTHER[self.NUMBER[game]])
                self.read[game] = (scan.arg_readers(flows, 0, None, dead), stack)
        return self.read[game]


def format_check(a, t):
    """[(kind, where, text, key)]: calls of printf-style functions whose literal format reads another number of words
    than the call passes; calls with no literal format that no row of readable_args_formats.tsv names."""
    table = readable_symbols.table()
    own = table if a.game == 'ai' else table.own(a.game)
    address_of = {name: own.syms[name][1] for name in PRINTF_STYLE if name in own.syms}
    callee_of = {v: k for k, v in address_of.items()}
    elf = readable_symbols.Elf(a.elf)
    game_elves = {}     # game -> its Elf, read when a format is one of its own rows
    texts = string_tables(a.text_dir)
    rows = load_format_rows()
    used = set()
    hits = []
    judged = [0]
    long_forms = long_form_callees(t)

    def string_at(address, game=None):
        source = elf
        if game:
            if game not in game_elves:
                path = getattr(a, 'elf_' + game)
                game_elves[game] = readable_symbols.Elf(path) if path and os.path.exists(path) else None
            source = game_elves[game]
            if source is None:
                return None
        for size in (256, 64, 16, 1):
            raw = source.data(address, size)
            if raw is not None and b'\0' in raw:
                return raw[:raw.index(b'\0')].decode('latin-1')
        return None

    own_rows = {g: table.own(g) for g in ('dm2', 'bmx')}

    def own_game(path, offset, expr, address):
        """The game whose own row a format is (sym::bmx::x written in the call, or a constant of one: BMX XXX's
        openFile names the file in its error; in a game folder's file its plain sym::x too), or None: its text is in
        that game's ELF, not AI's."""
        def folder(p):
            return readable_symbols.folder_game(t.originals.get(p, ''))
        code = t.files[path][1]
        at = code.find(expr, offset)
        if at >= 0:
            line = code.count('\n', 0, at)
            original = t.originals.get(path, '').split('\n')
            game = row_game(original[line], folder(path), own_rows, address) if line < len(original) else None
            if game:
                return game
        if re.fullmatch(r'k\w+', expr.strip()):
            for p in [path] + sorted(t.reachable(path)):
                m = re.search(r'\b%s\s*=\s*(sym::(?:(?:dm2|bmx)::)?\w+)' % re.escape(expr.strip()), t.originals.get(p, ''))
                if m:
                    return row_game(m.group(1), folder(p), own_rows, address)
        return None

    def enclosing(path, offset):
        best = None
        for f in t.functions:
            if f.path == path and f.start <= offset < f.end and (best is None or f.start >= best.start):
                best = f
        return best

    sums = {}  # path -> {kName: 'kOther + 0x41D0u'}: a constant built from another and an offset
    for path, (raw, code) in t.files.items():
        sums[path] = {m.group(1): m.group(2) for m in re.finditer(
            r'\bconstexpr\s+(?:std::)?uint32_t\s+(k\w+)\s*=\s*(k\w+\s*\+\s*0x[0-9A-Fa-f]+u?)\s*;', code)}

    def literal(path, expr, depth=0):
        """The address a format expression names, or None."""
        e = expr.strip()
        m = re.fullmatch(r'(?:ee::at<[^>]*>)?\(?\s*(0x[0-9A-Fa-f]+)u?\s*\)?', e)
        if m:
            return int(m.group(1), 16)
        if depth < 4 and re.fullmatch(r'k\w+', e):
            for p in [path] + sorted(t.reachable(path)):
                if e in sums.get(p, {}):
                    base, offset = re.fullmatch(r'(k\w+)\s*\+\s*(0x[0-9A-Fa-f]+)u?', sums[p][e]).groups()
                    start = literal(p, base, depth + 1)
                    return None if start is None else (start + int(offset, 16)) & 0xFFFFFFFF
        return t.resolve(path, e)

    def variable_formats(path, fn, expr):
        """The texts a local variable holding the format can have: what each assignment in the function gives it
        (a constant, or the two of `cond ? a : b`), or [] when one of them is not a literal or it is a parameter."""
        if fn is None or not re.fullmatch(r'[A-Za-z_]\w*', expr):
            return []
        body = t.files[fn.path][1][fn.start:fn.end]
        found = []
        for m in re.finditer(r'(?<![\w.>])%s\s*=(?!=)\s*([^;]*);' % re.escape(expr), body):
            value = m.group(1)
            depth, cut = 0, None
            for k, ch in enumerate(value):
                depth += ch in '([{'
                depth -= ch in ')]}'
                if ch == '?' and depth == 0:
                    cut = k
                    break
            parts = [value] if cut is None else [p for p in value[cut + 1:].split(':')]
            for part in parts:
                if part.strip() in ('0', '0u'):
                    continue  # (the variable's start value, before it is set)
                at = literal(fn.path, part.strip())
                text = string_at(at) if at is not None else None
                if text is None:
                    return []
                found.append(text)
        return found

    def judge(path, offset, line, callee, args, how):
        fmt_at, first, localized = PRINTF_STYLE[callee]
        fn = enclosing(path, offset)
        short = fn.short if fn else '?'
        rel = os.path.relpath(path, a.tree)
        where = '%s:%d' % ((rel if not rel.startswith(('..', 'out')) else os.path.join('sweep', os.path.basename(path))), line)
        kind = 'format'
        if fn is not None and fn.addresses and all(fn.status.get(x) == 'Draft' for x in fn.addresses):
            kind = 'formatdraft'
        if any('...' in x for x in args):
            return
        if len(args) <= fmt_at:
            hits.append((kind, where, '%s: %s(%s) passes no format' % (short, callee, ', '.join(args)), None))
            return
        expr = args[fmt_at]
        # A wrapper forwarding its own parameter: its callers are the calls checked.
        if fn is not None and re.fullmatch(r'\w+', expr) and any(re.search(r'\b%s$' % re.escape(expr), p.strip()) for p in fn.params):
            return
        passed = len(args) - first
        judged[0] += 1
        address = literal(path, expr)
        game = own_game(path, offset, expr, address) if address is not None else None
        text = string_at(address, game) if address is not None else None
        if game and text is None:
            problem_elf = 'no string at 0x%08X in %s\'s ELF (%s)' % (address, game, getattr(a, 'elf_' + game))
        candidates = [text] if text is not None else ([] if game else variable_formats(path, fn, expr))
        key = (short, callee, expr if text is None else text.encode('unicode_escape').decode(), str(passed))
        problem = None
        needs_of = []
        if not candidates:
            problem = problem_elf if game else 'the format `%s` is not a literal' % expr
        for text in candidates or []:
            if not localized:
                n, why = format_words(text)
            elif not texts:
                n, why = None, 'no string tables in %s (--text-dir) to read the $ keys of %r' % (a.text_dir, text)
            else:
                counts = {}
                why = None
                for lang, tab in texts.items():
                    w, why1 = format_words(expand_string_keys(text, tab))
                    why = why or (why1 and '%s: %s' % (lang, why1))
                    counts.setdefault(w, []).append(lang)
                # (the shipped languages' tables differ in places, EUS asks two words where the others ask one: a call
                # hands what the widest asks, the narrower ones read less of it)
                n = None if why else max(counts)
            problem = problem or why
            needs_of.append(n)
        need = needs_of[0] if len(needs_of) == 1 else (passed if passed in needs_of else (needs_of[0] if needs_of else None))
        text = candidates[0] if candidates else None
        if problem is None:
            problem = register_problem(callee, passed, how, long_forms)
        if problem is None and passed == need:
            return
        if key in rows:
            used.add(key)
            return
        if problem:
            msg = '%s: %s via %s: %s (passes %d)' % (short, callee, how, problem, passed)
        else:
            msg = '%s: %s %r reads %d words, the call passes %d' % (short, callee, text, need, passed)
        hits.append((kind, where, msg, None))
        LAST_KEYS.append((where, key))

    # Calls by name: formatString(...), zge::formatString(...), tail::debugPrint(...).
    names = '|'.join(PRINTF_STYLE)
    call = re.compile(r'(?<![\w.>])((?:\w+::)*)(%s)\s*\(' % names)
    for path, (raw, code) in t.files.items():
        lines = LineIndex(code)
        for m in call.finditer(code):
            if m.group(1).startswith(('std::', 'ee::')):
                continue
            # (a typed line of this file or a header it includes is a wrapper: its callers are sites, below)
            if not m.group(1) and any(g.wrapper and (g.path == path or g.path in t.reachable(path)) for g in t.by_name.get(m.group(2), [])):
                continue
            before = code[:m.start()].rstrip()
            word = re.search(r'([\w>*&]+)$', before)
            if word and word.group(1) not in ('return', 'else', 'case', 'co_return', 'throw', 'new', 'delete'):
                continue  # a declaration or definition: `void formatString(...)`
            j = matching(code, m.end() - 1)
            args = split_args(code[m.end():j - 1])
            judge(path, m.start(), lines.at(m.start()), m.group(2), args, 'a call')
    # Guest and game:: calls by address: ee::game(kSprintf, ...), game::call<&f, addr>(...).
    for s in t.sites:
        if isinstance(s.target, int) and s.target in callee_of:
            args = list(s.args)
            if hasattr(s, 'line_of'):
                # The wrapper hands its parameters on in order (the call's own words are its parameters').
                own = s.line_of
                params = [re.split(r'[\s*&]+', p.strip())[-1] for p in s.line_of.fn.params]
                if own.args != params[:len(own.args)] and own.args[:len(params)] != params:
                    hits.append(('format', '%s:%d' % (os.path.relpath(s.fn.path, a.tree), s.line),
                                 '%s: %s through a wrapper that does not hand its parameters on in order' % (s.fn.short, callee_of[s.target]), None))
                    continue
                args = list(s.written)
            judge(s.fn.path, s.offset, s.line, callee_of[s.target], args, 'a wrapper' if hasattr(s, 'line_of') else s.how)
    for key in sorted(set(rows) - used):
        hits.append(('format', 'tools/checks/readable_args_formats.tsv', 'no call matches %s' % ' | '.join(key), None))
    return hits, judged[0]


def analyse(a):
    generated = a.generated
    t, drafts = build_tree(a)
    format_hits, formats_checked = format_check(a, t)
    flows, stack = load_flows(generated)
    # (the tables are Aggressive Inline's)
    if a.game != 'ai':
        scan.load_indirect = scan.load_dead = lambda *_: {}
    # An indirect call of hand-written code: its jalr by the published return address, else by
    # order when a function has as many indirect calls as its original has.
    in_order = {}
    for s in t.sites:
        if s.kind == 'guest' and not isinstance(s.target, int):
            in_order.setdefault(id(s.fn), []).append(s)
    indirect = scan.load_indirect()
    dead = scan.load_dead()
    needs = scan.arg_readers(flows, 0, indirect, dead)
    jalrs, tail_jumps = jalr_sites(flows)

    # Functions reached as C++.
    direct = {}
    for s in t.sites:
        if s.kind != 'direct':
            continue
        if isinstance(s.target, int):
            direct.setdefault(s.target, []).append(s)
        else:
            cands = t.by_name.get(s.target.split('::')[-1], [])
            if cands:
                s.target_fn = cands[0]
                for x in cands[0].addresses:
                    direct.setdefault(x, []).append(s)
    registered_names = {f.short: f for f in t.functions if f.addresses and not f.wrapper}
    registered_names.update({n[:-1]: f for n, f in list(registered_names.items()) if n.endswith('_')})
    plain = {}
    for f in t.functions:
        for name in f.plain:
            g = registered_names.get(name)
            if g is not None and g is not f:
                for x in g.addresses:
                    plain.setdefault(x, []).append(f)

    # Helpers: the registered functions a function's code runs for.
    callers_of = {}

    def helpers(f, name, wrapper):
        """What a call of `name` in f runs: the function of that name in f's file, else in a file it includes,
        else the only one anywhere."""
        named = [g for g in t.by_name.get(name, []) if g is not f and bool(g.wrapper) == wrapper]
        same = [g for g in named if g.path == f.path]
        if same:
            return same
        return [g for g in named if g.path in t.reachable(f.path)] or (named if len(named) == 1 else [])

    for f in t.functions:
        for name in f.plain:
            for g in helpers(f, name, False):
                callers_of.setdefault(id(g), []).append(f)
        for name in f.typed:
            for g in helpers(f, name, True):
                callers_of.setdefault(id(g), []).append(f)

    def owners(f, seen=None):
        if f.addresses:
            return list(f.addresses)
        seen = seen or set()
        if id(f) in seen:
            return []
        seen.add(id(f))
        out = []
        for c in callers_of.get(id(f), []):
            out += owners(c, seen)
        return out

    original = Original(generated)
    hits = []
    by_target = {}
    for s in t.sites:
        if s.kind == 'guest' and isinstance(s.target, int):
            by_target.setdefault((id(s.fn), s.target), []).append(s)

    def site_pcs(address, s):
        """The jal of a call in its function: by the published return address; else by its first
        argument where the call and the jal both have a number there (a text's address, read from
        the code), so a call the compiler moved keeps its jal; else by order when the function makes as
        many calls of the callee as the original (None: not told)."""
        pcs = sorted(p for p in original.pcs(address, s.target))
        known = [r - 8 for r in s.ret_address if r - 8 in pcs]
        if known:
            return set(known[:1])
        mine = by_target[(id(s.fn), s.target)]

        def number(call):
            written = getattr(call, 'written', None)
            return t.resolve(call.fn.path, written[0]) if written else None

        keys = {p: original.first_argument.get((address, p)) for p in pcs}
        told = {number(m) for m in mine} & set(keys.values()) - {None}
        own = number(s)
        if own in told:
            same = [p for p in pcs if keys[p] == own]
            mine = [m for m in mine if number(m) == own]
        elif told:  # (a variable there: the calls and the jals no number told apart)
            same = [p for p in pcs if keys[p] not in told]
            mine = [m for m in mine if number(m) not in told]
        else:
            same = pcs
        return {same[mine.index(s)]} if len(mine) == len(same) else set(same) if told else None

    def add(kind, where, text, key):
        function = t.by_address.get(key[0]) if key else None
        draft = function is not None and function.status.get(key[0]) == 'Draft'
        hits.append((kind + 'draft' if draft and kind in COUNTED else kind, where, text, key))

    def where(fn, line):
        rel = os.path.relpath(fn.path, a.tree)
        if rel.startswith('..') or rel.startswith('out'):
            rel = os.path.join('sweep', os.path.basename(fn.path))
        return '%s:%d' % (rel, line)

    # 1: signatures of functions reached as C++.
    for x in sorted(set(direct) | set(plain)):
        f = t.by_address.get(x)
        if f is None or x not in needs:
            continue
        missing = needs[x] & ~signature_mask(f)
        if missing:
            sites = direct.get(x, [])
            who = ', '.join(sorted({s.fn.short for s in sites} | {g.short for g in plain.get(x, [])}))
            add('signature', where(f, 0), '0x%06X %s(%s) needs %s; reached as C++ from %s' % (
                x, f.short, ', '.join(f.params), ' '.join(mask_names(missing)), who[:100]), (x, 0, missing))

    # A call of a game's own function (games/bmx's ee::game(sym::levelFogColour, ...), BMX XXX's 0x199AA0) reads
    # that game's code, never AI's function at the same address (moveSkater).
    own_rows = {g: readable_symbols.table().own(g) for g in ('dm2', 'bmx')}
    game_code = GameCode(a)

    def code_of(s):
        """(game or None, needs, stack, names) for a call's target; needs None: that game's code is missing."""
        game = call_game(t, s, own_rows)
        if game is None or game == a.game:
            return None, needs, stack, t.names
        read = game_code.get(game)
        names = game_code.names.setdefault(game, {address: name for (kind, address), name in own_rows[game].by_address.items()
                                                  if kind == 'function'})
        return (game, None, {}, names) if read is None else (game, read[0], read[1], names)

    # 2: guest calls.
    checked = unseen = 0
    for s in t.sites:
        if s.kind != 'guest':
            continue
        game, site_needs, _, site_names = code_of(s)
        if site_needs is None:
            add('dropped', where(s.fn, s.line), '%s: %s to %s\'s 0x%X %s: no generated code of %s to read it in (--generated-%s)' % (
                s.fn.short, s.how, game, s.target, site_names.get(s.target, ''), game, game), ((owners(s.fn) or [0])[0], s.target, 0, 0))
            continue
        passed = t.passed_mask(s, site_needs, own_callee=game is None)
        if passed is None:
            continue
        addresses = owners(s.fn)
        checked += 1
        typed = getattr(s, 'signature', None) is not None  # a pointer's type: what it says it takes is what the call passes
        if typed and s.told and len(s.args) != len(s.signature):
            add('dropped', where(s.fn, s.line), '%s: %s passes %d, its type takes %d' % (
                s.fn.short, s.pointer, len(s.args), len(s.signature)), (addresses[0] if addresses else 0, 0, 0, 0))
        pcs = {}
        if isinstance(s.target, int):
            targets = [s.target]
        else:
            targets = []
            for x in addresses:
                sites = sorted(jalrs.get(x, [])) + sorted(tail_jumps.get(x, []))
                mine = in_order[id(s.fn)]
                known = [r - 8 for r in s.ret_address if r - 8 in sites]
                if known:
                    sites = known[:1]
                elif len(mine) == len(sites):
                    sites = sites[mine.index(s):][:1]
                pcs[x] = set(sites)
                for j in sites:
                    targets += indirect.get(j, [])
            targets = sorted(set(targets))
            if not targets:
                # No recording reached it: what the original holds for the callee there, a value
                # it set or one of the function's named parameters, against what the call passes.
                unseen += 1
                held = set()
                for x in addresses:
                    f0 = t.by_address.get(x) or s.fn
                    named = signature_mask(f0)
                    for bit in range(16):
                        if passed & (1 << bit):
                            continue
                        o = original.origin(x, None, NAMES[bit], True, pcs.get(x))
                        if o == 'near' or (o == 'entry' and named & (1 << bit) and (typed or x in direct or x in plain)):
                            held.add(NAMES[bit])
                if held:
                    add('unseen', where(s.fn, s.line), '%s: %s(%s): no recorded callee; the original holds %s there' % (
                        s.fn.short, s.how, ', '.join(s.args)[:50], ' '.join(sorted(held))),
                        (addresses[0] if addresses else 0, 0, ','.join(sorted(held))))
                continue
        missing = 0
        for x in targets:
            missing |= site_needs.get(x, 0) & ~passed
        # A variadic callee's format decides what it reads, and the scan leaves its spills out:
        # what the original sets for it just before the call counts.
        if isinstance(s.target, int) and game is None and original.variadic(s.target):
            for x in addresses:
                pcs = site_pcs(x, s)
                for bit in range(16):
                    if not (passed | missing) & (1 << bit) and \
                            original.origin(x, s.target, NAMES[bit], True, pcs) == 'near':
                        missing |= 1 << bit
        # Through a pointer the recorded callees are only those the corpus reached: a value the original sets for the jalr in
        # the jalr's own block is an argument, whoever is at the other end today.
        if typed:
            for x in addresses:
                at = pcs.get(x) or ()
                for bit in range(16):
                    if at and not (passed | missing) & (1 << bit) and \
                            any(original.fresh(x, NAMES[bit], pc) for pc in at):
                        missing |= 1 << bit
        if not missing:
            continue
        for bit in range(16):
            if not missing & (1 << bit):
                continue
            reg = NAMES[bit]
            callees = [x for x in targets if site_needs.get(x, 0) & (1 << bit)] or targets
            names = ', '.join('0x%X %s' % (x, site_names.get(x, '')) for x in callees[:3])
            kinds = {original.origin(x, s.target if isinstance(s.target, int) else None, reg, False,
                                     site_pcs(x, s) if isinstance(s.target, int) else pcs.get(x)) for x in addresses} or {'?'}
            if 'set' in kinds:
                kind = 'dropped'
            elif 'left' in kinds:
                kind = 'left'
            elif 'entry' in kinds:
                kind = 'entry' if typed or any(x in direct or x in plain for x in addresses) else 'entrytable'
            else:
                kind = 'dropped'
            jal = sorted(site_pcs(addresses[0], s) or []) if addresses and isinstance(s.target, int) else []
            add(kind, where(s.fn, s.line), '%s: %s to %s: %s not passed (of %d)' % (
                s.fn.short, s.how, names, reg, len(s.args)),
                (addresses[0] if addresses else 0, callees[0], 1 << bit, jal[0] if len(jal) == 1 else 0))

    # 3: registers read from the context in a function reached as C++: an argument register its
    # original needs (one it hands to a variadic callee whose format takes nothing there, or that
    # no path reads, is not one), a result register whose value a direct caller uses.
    saved = {'s0', 's1', 's2', 's3', 's4', 's5', 's6', 's7', 'gp', 'sp', 'fp', 'ra'}
    for f in t.functions:
        reached = [x for x in f.addresses if x in direct or x in plain]
        if not f.sees_entry or not reached:
            continue
        for kind, reg, line in f.sees_entry:
            if kind == 'Float':
                if not reg.isdigit() or int(reg) >= 20:
                    continue
                reg = 'f' + reg
            else:
                if reg.isdigit():
                    reg = GPR_NAMES[int(reg)]
                if reg in saved or reg not in GPR_NAMES:
                    continue
            kind2 = 'caller'
            if reg in NAMES:
                bit = 1 << NAMES.index(reg)
                if not any(needs.get(x, 0) & bit for x in reached):
                    continue
            else:
                kind2 = 'result'
                used = []
                for x in reached:
                    for s in direct.get(x, []):
                        code = t.files[s.fn.path][1]
                        at = LineIndex(code).starts[s.line - 2] if s.line >= 2 else 0
                        head = code[at:code.find('game::', at)]
                        if '=' in head or 'return' in head or '(' in head:
                            used.append(s.fn.short)
                    used += [g.short for g in plain.get(x, [])]
                if not used:
                    continue
            add(kind2, where(f, line), '%s: caller%s(%s), reached as C++' % (f.short, kind, reg),
                (f.addresses[0], 0, reg))

    # ee::leftByCall straight after a game::call: what the callee's last guest call left, or an
    # earlier one's if it made none, not what the recording says.
    for f in t.functions:
        code = t.files[f.path][1]
        lines = None
        for m in re.finditer(r'\bee::leftByCall\w*\(\s*([\w:]+)', code[f.start:f.end]):
            at = f.start + m.start()
            last = None
            for s in f.calls:
                if s.offset < at:
                    last = s
            if last is not None and last.kind == 'direct':
                lines = lines or LineIndex(code)
                add('leftdirect', where(f, lines.at(at)), '%s: leftByCall(%s) after %s' % (
                    f.short, m.group(1).split('::')[-1], str(last.target).split('::')[-1]),
                    ((f.addresses or [0])[0], 0, m.group(1).split('::')[-1]))

    # A kernel call's arguments are the registers as the caller left them (ee::kernelCall).
    for f in t.functions:
        if 'ee::kernelCall' in t.files[f.path][1][f.start:f.end] and any(x in direct or x in plain for x in f.addresses):
            add('caller', where(f, 0), '%s: ee::kernelCall, reached as C++: no register reaches it' % f.short,
                (f.addresses[0], 0, 'kernel'))

    # 4: stack arguments.
    for s in t.sites:
        if not isinstance(s.target, int):
            continue
        _, _, site_stack, site_names = code_of(s)
        if s.target in site_stack and s.how != 'tailCall' and not s.fn.stack_frame and \
                not any(x in t.by_address and t.by_address[x].stack_frame for x in owners(s.fn)):
            add('stack', where(s.fn, s.line), '%s: %s to 0x%X %s, which reads stack bytes +0x%x..0x%x, with no '
                'ee::StackFrame' % (s.fn.short, s.how, s.target, site_names.get(s.target, ''), site_stack[s.target][0],
                                    site_stack[s.target][1]), ((owners(s.fn) or [0])[0], s.target, 0))
    stats = {'sites': len(t.sites), 'guest': checked, 'unseen': unseen, 'functions': len(t.functions),
             'drafts': drafts, 'direct': len(set(direct) | set(plain)), 'needs': len(needs),
             'indirect sites': len(indirect), 'dead reads': len(dead), 'format calls': formats_checked}
    hits += format_hits
    return hits, stats, needs, flows, t


def report(a):
    hits, stats, _, _, _ = analyse(a)
    allowed = load_allowed()
    shown = set(a.list.split(',')) if a.list else set(COUNTED)
    counts = {}
    left = 0
    for kind, where, text, key in sorted(hits, key=lambda h: (h[0], h[1])):
        tag = allowed.get(token(kind, key)) if key else None
        counts[kind + (' (read)' if tag else '')] = counts.get(kind + (' (read)' if tag else ''), 0) + 1
        if kind in COUNTED and not tag:
            left += 1
        if kind in shown and (a.all or not tag):
            print('%-10s %s: %s%s' % (kind, where, text, ('   [%s]' % tag) if tag else ''))
    print('readable_args: %s; %s; %d left' % (', '.join('%s %s' % (v, k) for k, v in sorted(stats.items())),
                                               ', '.join('%s %d' % kv for kv in sorted(counts.items())), left))
    return 1 if a.check and left else 0


ALLOWED = os.path.join(HERE, 'readable_args_ok.tsv')


def token(kind, key):
    function, callee, reg, *jal = key
    regs = reg if isinstance(reg, str) else ','.join(mask_names(reg))
    return '%s\t%x\t%x\t%s' % (kind, function, callee, regs + ('@%x' % jal[0] if jal and kind == 'dropped' else ''))


def load_allowed():
    """A hit a person has read: kind, function, callee, registers, why (readable_args_ok.tsv)."""
    out = {}
    if os.path.exists(ALLOWED):
        for line in open(ALLOWED, encoding='utf-8'):
            if line.strip() and not line.startswith('#'):
                f = line.rstrip('\n').split('\t')
                out['\t'.join(f[:4])] = f[4] if len(f) > 4 else 'read'
    return out


def formats_only(a):
    if a.build:
        a.build = os.path.abspath(a.build)
    t, drafts = build_tree(a)
    hits, checked = format_check(a, t)
    left = 0
    for kind, where, text, _ in sorted(hits, key=lambda h: h[1]):
        left += kind in COUNTED
        print('%-10s %s: %s' % (kind, where, text))
    if a.list == 'rows':  # (a starting point for readable_args_formats.tsv: write the why in)
        for where, key in sorted(set(LAST_KEYS)):
            print('\t'.join(key) + '\t')
    print('readable_args formats: %d calls checked, %d left' % (checked, left))
    return 1 if a.check and left else 0


def explain(a):
    flows, _ = load_flows(a.generated)
    indirect = scan.load_indirect()
    dead = scan.load_dead()
    needs = scan.arg_readers(flows, 0, indirect, dead)
    what = a.command[1]
    address = int(what, 16) if re.fullmatch(r'(0x)?[0-9A-Fa-f]+', what) else None
    if address is None:
        t, _ = build_tree(a)
        fs = [f for f in t.by_name.get(what, []) if f.addresses]
        address = fs[0].addresses[0]
    print('0x%X needs %s' % (address, ' '.join(mask_names(needs.get(address, 0)))))
    want = a.command[2] if len(a.command) > 2 else None
    seen = set()

    def walk(x, depth):
        if x in seen or depth > 6 or x not in flows:
            return
        seen.add(x)
        bit = 1 << NAMES.index(want) if want else 0xFFFF
        own = scan.entry_reads(flows[x], lambda t, site=None: 0)
        print('  ' * depth + '0x%X: reads itself %s' % (x, ' '.join(mask_names(own & bit))))
        for steps, succs in flows[x][1].values():
            cs = [(s[1], s[2]) for s in steps if s[0] == 'call'] + [(s[1], s[2]) for s in succs if isinstance(s, tuple) and s[0] == 'tail']
            for target, site in cs:
                for y in ([target] if target is not None else indirect.get(site, [])):
                    if needs.get(y, 0) & bit:
                        print('  ' * depth + ' at 0x%X -> 0x%X (%s)' % (site, y, ' '.join(mask_names(needs[y] & bit))))
                        walk(y, depth + 1)
    walk(address, 0)
    return 0


def indirect_calls(a):
    """Rewrites readable_indirect_calls.tsv from a corpus: every (jalr or indirect jump, callee)
    the recordings saw (a call's site is its return address less 8, as the recording keeps ra)."""
    header = struct.Struct('<8sIIII')
    line_size = 24
    seen = set()
    begun = time.time()
    files = calls = 0
    un = struct.unpack
    for root, _, names in os.walk(a.command[1]):
        if os.path.basename(root).startswith('dm2'):
            continue
        for name in names:
            if not name.endswith('.fnrec'):
                continue
            path = os.path.join(root, name)
            size = fnrec_io.plain_size(path)
            with fnrec_io.open_plain(path) as f:   # (plain or compressed: ps2/ee/fnrec_file.h)
                head = f.read(header.size)
                if len(head) < header.size:
                    continue
                magic, version, address, ctx, game = header.unpack(head)
                if not magic.startswith(b'FNREC1') or (version >= 6 and game != 0):
                    continue
                files += 1
                rd, sk = f.read, f.seek

                def lines():
                    sk(un('<I', rd(4))[0] * line_size, 1)
                try:
                    while f.tell() < size:
                        raw = rd(28)
                        if len(raw) < 28:
                            break
                        mark, flags = un('<II', raw[:8])
                        if mark != 0x4C4C4143:
                            break
                        if version >= 3:
                            sk(8, 1)
                        sk(2 * ctx, 1)
                        lines()
                        lines()
                        for _ in range(un('<I', rd(4))[0]):
                            target, _, cf = un('<III', rd(12))
                            sk(64 + (64 if version >= 5 else 0) + 32, 1)
                            after = rd(ctx)
                            ra = un('<I', after[31 * 16:31 * 16 + 4])[0]
                            seen.add((address, (ra - 8) & 0xFFFFFFFF, target, cf & 3))
                            lines()
                            if version >= 7:
                                lines()   # (the callee's reads of the caller's frame: fncheck.h Call::reads)
                            calls += 1
                        if version >= 5 and flags & 2:
                            sk(24 * un('<I', rd(4))[0], 1)
                except struct.error:
                    pass
    flows, _ = load_flows(a.generated)
    jalrs, tails = jalr_sites(flows)
    rows = set()
    for function, site, target, cf in seen:
        if site in jalrs.get(function, ()):
            rows.add((site, target))
        elif cf & 1 and not cf & 2 and tails.get(function):
            # A jump through a register: its address is not in the recording.
            for j in tails[function]:
                rows.add((j, target))
    with open(scan.INDIRECT_CALLS, 'w', encoding='utf-8') as f:
        f.write('# The callees recordings saw at each jalr and indirect jump (address, callee), written by\n'
                '# readable_args.py indirect CORPUS: fncheck_traced_gen.py and readable_args.py take an\n'
                '# indirect call to read what these read.\n')
        for row in sorted(rows):
            f.write('%x\t%x\n' % row)
    print('%d files, %d calls, %d rows, %d sites in %d s' % (files, calls, len(rows), len({r[0] for r in rows}),
                                                            time.time() - begun))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('command', nargs='*')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--tree', default=ztree.TREE)
    ap.add_argument('--build')
    ap.add_argument('--generated', default=os.environ.get('ZX_RUNNER_GENERATED_DIR', ''))
    ap.add_argument('--game', default='ai', choices=('ai', 'dm2'), help='dm2: games/dm2 against --generated (generated-ns)')
    ap.add_argument('--generated-dm2', help='DM2\'s recompiled code, for a call of one of its own functions (default DM2_WORKSPACE/recomp/generated-ns)')
    ap.add_argument('--generated-bmx', help='BMX XXX\'s recompiled code, for a call of one of its own functions (default BMX_RECOMP/generated-ns)')
    ap.add_argument('--elf', help='the game\'s ELF, where the formats are read (default AI_WORKSPACE/extracted/SLUS_203.27)')
    ap.add_argument('--elf-dm2', help='DM2\'s ELF, for a format that is a sym::dm2:: row (default DM2_WORKSPACE/recomp/disc-root/SLUS_201.59)')
    ap.add_argument('--elf-bmx', help='BMX XXX\'s ELF, for a format that is a sym::bmx:: row (default BMX_DATA/disc/SLUS_204.15)')
    ap.add_argument('--text-dir', help='assets/STRING.* for the $ keys (default AI_WORKSPACE/recomp/disc-root/ASSETS)')
    ap.add_argument('--list', help='kinds to print (default the counted ones), comma-separated')
    ap.add_argument('--all', action='store_true', help='hits already read too')
    a = ap.parse_args()
    a.tree = os.path.abspath(a.tree)
    root = zaxis_paths.get('AI_WORKSPACE')
    a.elf = a.elf or os.path.join(root, 'extracted', 'SLUS_203.27')
    a.text_dir = a.text_dir or os.path.join(root, 'recomp', 'disc-root', 'ASSETS')
    a.elf_dm2 = a.elf_dm2 or os.path.join(zaxis_paths.get('DM2_WORKSPACE', required=False) or '', 'recomp', 'disc-root', 'SLUS_201.59')
    a.elf_bmx = a.elf_bmx or os.path.join(zaxis_paths.get('BMX_DATA', required=False) or '', 'disc', 'SLUS_204.15')
    if not a.generated:
        a.generated = os.path.join(root, 'recomp', 'generated')
    a.generated_dm2 = a.generated_dm2 or os.path.join(zaxis_paths.get('DM2_WORKSPACE', required=False) or '', 'recomp', 'generated-ns')
    a.generated_bmx = a.generated_bmx or os.path.join(zaxis_paths.get('BMX_RECOMP', required=False) or '', 'generated-ns')
    if a.command and a.command[0] == 'indirect':
        return indirect_calls(a)
    if a.command and a.command[0] == 'formats':
        return formats_only(a)
    if a.command and a.command[0] == 'explain':
        return explain(a)
    if a.build:
        a.build = os.path.abspath(a.build)
    return report(a)


if __name__ == '__main__':
    sys.exit(main())
