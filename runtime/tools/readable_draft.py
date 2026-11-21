#!/usr/bin/env python3
"""Writes a first draft of readable C++ for game functions (docs/READABLE.md,
"Drafts and the sweep").

    readable_draft.py [options] ADDRESS|NAME ... [-o FILE]

The draft is lifted from the function's instructions as recomp/generated has
them (the disassembly in its comments, the jump tables and tail calls of its
code), so its behaviour is the original's by construction:

  - the signature: integer arguments the code (or a callee it passes them to)
    reads before writing (a0-a7), float arguments (f12-f19), the result in v0
    or f0 (Ghidra's return type decides void);
  - every call in the original's order with the arguments its callee reads:
    ee::call<R>(address, ...), game::call<&fn> / game::callAt for readable
    callees (by address when a game override hooks them), ee::tailCall /
    game::tailCall for the original's tail calls;
  - EE time: loops as `for (;; ee::chargeLoop(1))` (one charge per branch
    back, as generated code charges 32 cycles at each backward branch), other
    backward gotos charged the same;
  - the stack frame: ee::StackFrame when a local's address goes to a callee or
    into memory, else plain locals; the register saves left out;
  - memory through ee::at<T>(address) on 32-bit EE addresses (pointers keep
    their EE address, uncached mirrors included), globals as ee::Global;
  - floats as ee::Float in the original's operation order; VU0 macro code as
    the generated code runs it, on the running context (so what it leaves in
    VU0 is the original's);
  - Ghidra's decompilation (tooling/dm2/ai-decomp.c, a data link) as a
    comment, known names of callees and globals from the readable sources;
  - READABLE_FUNCTION(address, name, Draft), _WAITS when a callee can wait
    (a call path to a waiting syscall or a readable _WAITS function), _VU0
    when a caller reads VU0 registers the function writes.

What is left to the author: names, types (structs instead of offsets), the
structure (gotos where the control flow did not fit if/else and loops), and
the house style's single ee::chargeLoop(n) per loop. zx-fncheck proves each
step (`readable_draft_check.py` builds and checks a draft against recordings).

Options:
  --workspace DIR   the bring-up workspace (default: the parent of this z-axis
                    checkout), for recomp/generated and recomp/ghidra-functions.csv
  --generated DIR   recompiled sources (default ZX_RUNNER_GENERATED_DIR or
                    <workspace>/recomp/generated)
  --decomp FILE     Ghidra's C (default tooling/dm2/ai-decomp.c of the main checkout)
  --namespace NS    namespace of the drafts (default draft)
  --name NAME       the function's C++ name (one address only)
  --status S        Draft (default), Tested, Verified
  --no-ghidra       leave Ghidra's decompilation out
  -o FILE           write the drafts there (default: stdout)
  --stats           print what each draft needed (to stderr)
"""
import argparse
import csv
import datetime
import os
import re
import struct
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import zaxis_paths  # noqa: E402  (tooling/paths.env: the workspaces, the data, the work database)
import readable_symbols
from collections import defaultdict

# (after the standard library: under `python3 -I` the folder is not on the path, and
# a bisect.py in a tools folder must not shadow the real one)
if os.path.dirname(os.path.abspath(__file__)) not in sys.path:
    sys.path.append(os.path.dirname(os.path.abspath(__file__)))
try:
    import readable_names   # names beyond the machine's (readable_names.py; readable_names agent)
except ImportError:
    readable_names = None

HERE = os.path.dirname(os.path.abspath(__file__))
ZAXIS_TREE = os.path.dirname(os.path.dirname(HERE))
GAME_SRC = os.path.join(ZAXIS_TREE, "runtime", "src", "game")

GP = 0x0027A8F0  # the game's _gp (gp-0x6DB0 is 0x273B40, zge::g_stringTable)

GPR = ['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3', 't0', 't1', 't2', 't3', 't4', 't5', 't6', 't7',
       's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7', 't8', 't9', 'k0', 'k1', 'gp', 'sp', 'fp', 'ra']
GPR_ALIASES = {'s8': 'fp', 'r0': 'zero'}
INT_ARGS = ['a0', 'a1', 'a2', 'a3', 't0', 't1', 't2', 't3']   # EABI a0-a7
FLOAT_ARGS = ['f%d' % i for i in range(12, 20)]
CALLEE_SAVED = {'s0', 's1', 's2', 's3', 's4', 's5', 's6', 's7', 'fp', 'gp', 'sp', 'ra'} | \
    {'f%d' % i for i in range(20, 32)}
# Callee-saved registers a call publishes (ee::calleeSaves; gp and sp are
# the same everywhere, ra is the call's return address).
SAVED_MODEL = ['s0', 's1', 's2', 's3', 's4', 's5', 's6', 's7', 'fp'] + ['f%d' % i for i in range(20, 32)]
# Caller-saved registers that are not arguments: a function that reads one
# as its caller left it (ee::callerRegister) gets it from its self-calls
# through ee::passRegisters (drafter24: fn_1587F0's innermost call returns
# its caller's v0, the count; the direct C++ call left the context's stale
# v0, a node pointer).
PASS_MODEL = ['at', 'v0', 'v1', 't4', 't5', 't6', 't7', 't8', 't9']
CALLER_SAVED = ([r for r in GPR if r not in CALLEE_SAVED and r != 'zero'] + ['f%d' % i for i in range(20)] +
                ['hi', 'lo', 'hi1', 'lo1', 'fcc', 'facc', 'mq', 'mq^'] + [r + '^' for r in GPR if r not in CALLEE_SAVED and r != 'zero'])
WAITING_SEEDS = {'WaitSema', 'SleepThread', 'WaitEventFlag', 'iSleepThread', 'sceCdSync', 'sceCdSyncS',
                 'sceSifCallRpc', 'sceSifBindRpc', 'sceCdRead', 'sceCdSearchFile', 'sceCdInit', 'sceSifLoadModule',
                 'DelayThread', 'ReceiveMessage', 'sceMcSync', 'sceSifInitRpc',
                 # HLE stubs that wait (the runtime's sceGsSyncV waits for the vsync tick)
                 'sceGsSyncV'}

LINE = re.compile(r'^\s*// 0x([0-9a-f]+): 0x([0-9a-f]+)\s+(\S+)\s*(.*)$')


# The jal address -> (callee, the ee::calleeSaves line the draft writes
# there): --saves-table, for hand-written functions (drafter24).
SAVES_AT = {}


def die(msg):
    sys.exit('readable_draft.py: ' + msg)


# ---------------------------------------------------------------------------
# Facts a draft reads from the readable sources (for --manifest's cache)
# ---------------------------------------------------------------------------
#
# A batch file depends on recomp/generated, the Ghidra map, this tool and its
# manifest rows (the cache's keys), and on a few facts from the readable
# sources: the readable function registered at an address (name, signature,
# header), a callee constant's or a global's known name, whether an override
# hooks an address, whether two readable functions share machine code, a
# manifest's pinned signature or ELF word. While a batch is drafted, each fact
# it looks up is noted (FACTS[0]) with a digest of its value; the next build
# drafts the file again only when one of them has another value (a function's
# body edited changes none of them).

FACTS = [None]   # {(kind, key): digest} of the batch being drafted, else None


def fact_digest(value):
    import hashlib
    return hashlib.sha1(repr(value).encode('utf-8', 'replace')).hexdigest()[:16]


def note_fact(kind, key, value):
    log = FACTS[0]
    if log is not None:
        log[(kind, key)] = fact_digest(value)


class TrackedDict(dict):
    """A dict whose lookups are noted as facts (kind, key); a walk over it is
    a fact on the whole dict."""

    def __init__(self, kind, data):
        super().__init__(data)
        self.kind = kind
        self._whole = None

    def _note(self, key):
        if FACTS[0] is not None:
            note_fact(self.kind, key, dict.get(self, key))

    def _note_whole(self):
        if FACTS[0] is not None:
            if self._whole is None:
                self._whole = fact_digest(sorted(dict.items(self), key=lambda kv: repr(kv[0])))
            FACTS[0][(self.kind, '*')] = self._whole

    def get(self, key, default=None):
        self._note(key)
        return dict.get(self, key, default)

    def __getitem__(self, key):
        self._note(key)
        return dict.__getitem__(self, key)

    def __contains__(self, key):
        self._note(key)
        return dict.__contains__(self, key)

    def items(self):
        self._note_whole()
        return dict.items(self)

    def keys(self):
        self._note_whole()
        return dict.keys(self)

    def values(self):
        self._note_whole()
        return dict.values(self)

    def __iter__(self):
        self._note_whole()
        return dict.__iter__(self)


class TrackedSet(set):
    """A set whose membership tests are noted as facts."""

    def __init__(self, kind, data):
        super().__init__(data)
        self.kind = kind

    def __contains__(self, key):
        found = set.__contains__(self, key)
        note_fact(self.kind, key, found)
        return found


# ---------------------------------------------------------------------------
# The workspace: generated code, Ghidra, the readable sources
# ---------------------------------------------------------------------------

class Workspace:
    def __init__(self, workspace, generated, decomp):
        self.workspace = workspace
        self.generated_dir = generated
        self.decomp_path = decomp
        self.files = {}
        for name in os.listdir(generated):
            m = re.search(r'_0x([0-9a-f]+)\.cpp$', name)
            if m:
                self.files.setdefault(int(m.group(1), 16), os.path.join(generated, name))
        self.ghidra_names = {}
        path = os.environ.get('READABLE_GHIDRA_CSV') or os.path.join(workspace, 'recomp', 'ghidra-functions.csv')
        if os.path.exists(path):
            with open(path) as f:
                for row in csv.DictReader(f):
                    self.ghidra_names[int(row['Start'], 16)] = row['Name']
        self._decomp = None
        self._parsed = {}
        self._lifted = {}
        self.readable = TrackedDict('readable', read_readable(GAME_SRC))
        constants, globals_ = read_known_names(GAME_SRC)
        self.constants, self.globals = TrackedDict('constants', constants), TrackedDict('globals', globals_)
        hooked = set()
        try:
            import readable_coverage
            funcs = readable_coverage.read_functions(workspace, path)
            hooked = readable_coverage.hooked_addresses(funcs)
        except Exception:
            pass
        self.hooked = TrackedSet('hooked', hooked)
        self._waits = None

    def generated(self, address):
        return self.files.get(address)

    def result_readers(self):
        """{function: (GPRs, FPRs)} some caller reads after calling it
        (fncheck_traced_gen.py's scan of recomp/generated, as zx-fncheck's
        ResultReader table has it)."""
        if getattr(self, '_result_readers', None) is None:
            import fncheck_traced_gen
            sites = []
            fncheck_traced_gen.scan(0, self.generated_dir, set(), None, sites)
            self._result_readers = {f: m for (_, f), m in fncheck_traced_gen.result_readers(sites).items()}
        return self._result_readers

    def table(self):
        """{address: generated function name} of the function table
        (register_functions.cpp): what the scheduler resumes at each address
        (the safe-point model, Draft.safe_points)."""
        if getattr(self, '_table', None) is None:
            self._table = {}
            path = os.path.join(self.generated_dir, 'register_functions.cpp')
            try:
                for line in open(path, errors='replace'):
                    m = re.search(r'= (\w+); // 0x([0-9a-f]+)$', line.rstrip())
                    if m:
                        self._table[int(m.group(2), 16)] = m.group(1)
            except OSError:
                pass
        return self._table

    def elf_word(self, address):
        """A word of the game's ELF as loaded (None outside its segments): the
        jump tables the recompiler did not see. With pinned words (a sweep
        manifest's `elf` column) only those, and the ELF is not read."""
        pinned = getattr(self, 'pinned_elf', None)
        if pinned is not None:
            note_fact('elf', address, pinned.get(address))
            return pinned.get(address)
        word = self._elf_word(address)
        log = getattr(self, 'elf_log', None)
        if log is not None and word is not None:
            log[address] = word
            # (by the function being drafted: its manifest row pins the word,
            # drafter23; the file's first row had them all, lost with it)
            by_fn = getattr(self, 'elf_by_fn', None)
            if by_fn is not None and getattr(self, 'drafting', None) is not None:
                by_fn.setdefault(self.drafting, {})[address] = word
        return word

    def _elf_word(self, address):
        if not hasattr(self, '_segments'):
            self._segments = []
            path = os.path.join(self.workspace, 'recomp', 'disc-root', 'SLUS_203.27')
            try:
                data = open(path, 'rb').read()
                phoff, = struct.unpack_from('<I', data, 0x1C)
                phentsize, phnum = struct.unpack_from('<HH', data, 0x2A)
                for k in range(phnum):
                    kind, offset, vaddr, _, filesz = struct.unpack_from('<IIIII', data, phoff + k * phentsize)
                    if kind == 1:
                        self._segments.append((vaddr, data[offset:offset + filesz]))
            except (OSError, struct.error):
                pass
        for vaddr, body in self._segments:
            if vaddr <= address and address + 4 <= vaddr + len(body):
                return struct.unpack_from('<I', body, address - vaddr)[0]
        return None

    def parsed(self, address):
        if address not in self._parsed:
            path = self.generated(address)
            self._parsed[address] = parse_generated(path) if path else None
        return self._parsed[address]

    def name_of(self, address):
        r = self.readable.get(address)
        if r:
            return r['name']
        n = self.ghidra_names.get(address)
        if n and not n.startswith(('FUN_', 'entry', 'thunk_FUN_')):
            return n
        return None

    def decomp(self, address):
        if self._decomp is None:
            self._decomp = {}
            if self.decomp_path and os.path.exists(self.decomp_path):
                text = open(self.decomp_path, errors='replace').read()
                parts = re.split(r'^//// FUNCTION ([0-9a-f]{8}) (\S+)\n', text, flags=re.M)
                for i in range(1, len(parts) - 2, 3):
                    self._decomp[int(parts[i], 16)] = (parts[i + 1], parts[i + 2].strip('\n'))
        note_fact('decomp', address, self._decomp.get(address))
        return self._decomp.get(address)

    def folded(self, address):
        """Whether another readable function has the same machine code (lld-link
        folds identical host functions on Windows: game::callAt names the
        address)."""
        found = self._folded(address)
        note_fact('folded', address, found)
        return found

    def _folded(self, address):
        if not hasattr(self, '_code_groups'):
            groups = defaultdict(list)
            for a, r in dict.items(self.readable):
                if r['kind'] == 'PLANNED':
                    continue
                p = self.parsed(a)
                if p and p.insns:
                    groups[tuple(p.insns[x].raw for x in sorted(p.insns))].append(a)
            self._code_groups = {a: len(v) for v in groups.values() for a in v}
        return self._code_groups.get(address, 1) > 1

    def jal_sites(self):
        """{target: [(caller, pc)]} of every jal in the generated code (read once)."""
        if getattr(self, '_jal_sites', None) is None:
            sites = defaultdict(list)
            jal = re.compile(r'// 0x([0-9a-f]+): 0x([0-9a-f]+)\s+jal\s')
            for caller, path in self.files.items():
                try:
                    text = open(path, errors='replace').read()
                except OSError:
                    continue
                for m in jal.finditer(text):
                    pc = int(m.group(1), 16)
                    raw = int(m.group(2), 16)
                    sites[(pc & 0xF0000000) | ((raw & 0x03FFFFFF) << 2)].append((caller, pc))
            self._jal_sites = sites
        return self._jal_sites

    def waiting_functions(self):
        """Functions with a call path (jal / j) to a function that waits."""
        if self._waits is not None:
            return self._waits
        callers = defaultdict(set)
        jal = re.compile(r'// 0x[0-9a-f]+: 0x([0-9a-f]+)\s+(jal|j)\s')
        seeds = {a for a, n in self.ghidra_names.items() if n in WAITING_SEEDS}
        seeds |= {a for a, r in self.readable.items() if r['kind'] == 'FUNCTION_WAITS'}
        jalr = re.compile(r'// 0x[0-9a-f]+: 0x[0-9a-f]+\s+jalr\s')
        for address, path in self.files.items():
            try:
                text = open(path, errors='replace').read()
            except OSError:
                continue
            # An indirect call may reach a function that waits (messages,
            # script commands, file devices: the call graph misses those);
            # with indirect_waits (the sweep's) such a function counts as one
            # that waits, and so its callers.
            if getattr(self, 'indirect_waits', False) and jalr.search(text):
                seeds.add(address)
            for m in jal.finditer(text):
                raw = int(m.group(1), 16)
                target = ((address & 0xF0000000) | ((raw & 0x03FFFFFF) << 2))
                callers[target].add(address)
        waits, todo = set(seeds), list(seeds)
        while todo:
            a = todo.pop()
            for c in callers.get(a, ()):
                if c not in waits:
                    waits.add(c)
                    todo.append(c)
        self._waits = waits
        return waits


def read_readable(game_src):
    """{address: {name, kind, status, file, signature, header, namespace, ...}}"""
    out = {}
    hosts = defaultdict(list)
    pattern = re.compile(r'READABLE_(FUNCTION_WAITS|FUNCTION_VU0|FUNCTION|PLANNED)\(\s*(0x[0-9A-Fa-f]+)\s*,\s*([^,\)]+)\s*(?:,\s*(\w+))?')
    sources = {}
    for root, dirs, files in os.walk(game_src):
        dirs.sort()  # the same order on every host (names chosen by order)
        files = sorted(files)
        if os.path.basename(root) in ('drafts', 'sweep'):  # (readable_sweep.py's batch files: generated, not sources)
            continue
        for name in files:
            if name.endswith(('.cpp', '.h')):
                path = os.path.join(root, name)
                sources[path] = readable_symbols.literal_view(open(path, errors='replace').read())
    for path, text in sources.items():
        if not path.endswith('.cpp'):
            continue
        for m in pattern.finditer(text):
            kind, address, fn, status = m.groups()
            if address.lower() == 'address' or fn.strip() == 'FN':
                continue
            fn = fn.strip().strip('"')
            a = int(address, 16)
            entry = {'name': fn, 'kind': kind, 'status': (status or 'Planned') if kind != 'PLANNED' else 'Planned',
                     'file': path, 'signature': None, 'header': None, 'namespace': None}
            if kind != 'PLANNED':
                sig = find_signature(text[:m.start()], fn)
                if sig:
                    entry['signature'] = sig
                    entry['namespace'] = namespace_at(text, m.start())
                    hosts[(path, fn)].append(a)
            out[a] = entry
    # A declaration in a header of the readable sources (game::call needs one).
    declaration = {}   # (one pattern a name: compiled per header and entry it was a quarter of the drafter's start-up)
    for path, text in sources.items():
        if not path.endswith('.h'):
            continue
        for a, e in out.items():
            if e['signature'] and e['header'] is None and e['name'] in text:
                pattern_ = declaration.get(e['name'])
                if pattern_ is None:
                    pattern_ = declaration[e['name']] = re.compile(r'^[^\n/]*\b' + re.escape(e['name']) + r'\s*\([^;{]*\)\s*;', re.M)
                m = pattern_.search(text)
                if m and namespace_at(text, m.start()) == e['namespace'] and '{' not in m.group(0):
                    e['header'] = include_path(path, game_src)
    for (path, fn), addrs in hosts.items():
        if len(addrs) > 1:
            for a in addrs:
                out[a]['shared'] = True
    # One declared only in its own .cpp (zgehand3: searchTreeFind, cachedMatrix
    # were called by address, through the dispatcher): a draft declares it
    # itself (`decl`) when its definition links from outside (not static or
    # inline, not in an anonymous namespace) and the headers that define its
    # signature's types are found (`decl_headers`).
    types = header_types(sources, game_src)
    for a, e in out.items():
        if not e['signature'] or e['header'] or not e['namespace'] or '(anonymous)' in e['namespace']:
            continue
        text = sources[e['file']]
        if re.search(r'(^|\n)[ \t]*(static|inline|constexpr)\b[^\n;{]*\b' + re.escape(e['name']) + r'\s*\(', text):
            continue
        ret, params = e['signature']
        need = set()
        ok = True
        for t in [ret] + params:
            for word in re.findall(r'\b[A-Za-z_]\w*\b', re.sub(r'\b(ee|std)::\w+|/\*.*?\*/', ' ', t)):
                if word in DECL_BUILTIN:
                    continue
                h = resolve_type(types, word, e['namespace'])
                if h is None:
                    ok = False
                    break
                if h:
                    need.add(h)
            if not ok:
                break
        if ok:
            e['decl'] = 'namespace %s { %s %s(%s); }' % (e['namespace'], ret, e['name'], ', '.join(params))
            e['decl_headers'] = sorted(need)
    return out


DECL_BUILTIN = {'const', 'char', 'int', 'unsigned', 'signed', 'short', 'long', 'float', 'double', 'bool', 'void',
                'int8_t', 'uint8_t', 'int16_t', 'uint16_t', 'int32_t', 'uint32_t', 'int64_t', 'uint64_t', 'size_t'}


def include_path(path, root):
    """path relative to root with '/' separators, as an #include names it
    (os.path.relpath gives '\\' on Windows: the drafts' text differed by
    platform, remote1)."""
    return os.path.relpath(path, root).replace(os.sep, '/')


def header_types(sources, game_src):
    """{type name: [(namespace, header)]} of the structs, classes, enums and
    aliases the readable sources' headers define."""
    out = defaultdict(list)
    pat = re.compile(r'^[ \t]*(?:struct|class|union|enum(?:\s+class)?)\s+(\w+)\b[^;\n]*(?:\{|\n\s*\{|:)|'
                     r'^[ \t]*using\s+(\w+)\s*=', re.M)
    for path, text in sources.items():
        if not path.endswith('.h'):
            continue
        rel = include_path(path, game_src)
        for m in pat.finditer(text):
            out[m.group(1) or m.group(2)].append((namespace_at(text, m.start()), rel))
    return out


def resolve_type(types, word, ns):
    """The header defining type `word` as seen from namespace ns (that
    namespace or an enclosing one, the innermost; '' for ee/std); None when
    not found or ambiguous."""
    best, depth, count = None, -1, 0
    for tns, header in types.get(word, ()):
        if '(anonymous)' in tns:
            continue
        if tns == ns or ns.startswith(tns + '::') or tns == '':
            d = len(tns.split('::')) if tns else 0
            if d > depth:
                best, depth, count = header, d, 1
            elif d == depth and header != best:
                count += 1
    if best is None or count > 1:
        return None
    return best


_BLANKS = {}


def _code_only(text):
    """text with comments and string / character literals blanked (the same
    length): braces in them do not count."""
    key = id(text), len(text)
    hit = _BLANKS.get(key)
    if hit is not None and hit[0] is text:
        return hit[1]
    blank = re.sub(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'',
                   lambda m: re.sub(r'[^\n]', ' ', m.group(0)), text, flags=re.S)
    _BLANKS[key] = (text, blank)
    return blank


def namespace_at(text, pos):
    """The namespace path (a::b) at pos, anonymous namespaces as '(anonymous)'."""
    stack = []
    depth = 0
    for m in re.finditer(r'namespace\s+([\w:]*)\s*\{|\{|\}', _code_only(text)[:pos]):
        tok = m.group(0)
        if tok.startswith('namespace'):
            depth += 1
            stack.append((depth, m.group(1) or '(anonymous)'))
        elif tok == '{':
            depth += 1
        else:
            if stack and stack[-1][0] == depth:
                stack.pop()
            depth -= 1
    return '::'.join(n for _, n in stack)


def find_signature(text, name):
    """(return type, [param types]) of the last definition of name in text."""
    best = None
    for m in re.finditer(r'(^|\n)\s*((?:static\s+|inline\s+)?[\w:<>,\s\*&]+?)\s*\b' + re.escape(name) +
                         r'\s*\(([^()]*(?:\([^()]*\)[^()]*)*)\)\s*(?:const\s*)?\{', text):
        best = m
    if not best:
        return None
    ret = re.sub(r'\b(static|inline)\b', '', best.group(2)).strip()
    params = []
    plist = best.group(3).strip()
    if plist and plist != 'void':
        depth, cur = 0, ''
        for c in plist:
            if c in '<(':
                depth += 1
            elif c in '>)':
                depth -= 1
            if c == ',' and depth == 0:
                params.append(cur.strip())
                cur = ''
            else:
                cur += c
        params.append(cur.strip())
    types = []
    for p in params:
        p = re.sub(r'=.*$', '', p).strip()
        m = re.match(r'^(.*?)(\b\w+)?$', p)
        t = p
        if m and m.group(2) and (m.group(1).strip() and not m.group(1).strip().endswith('::')):
            t = m.group(1).strip()
        types.append(t)
    return ret, types


def read_known_names(game_src):
    """Callee constants (address -> [(name, comment)]) and globals (address -> [(name, type)])."""
    constants = defaultdict(list)
    globals_ = defaultdict(list)
    cpat = re.compile(r'constexpr\s+uint32_t\s+(k\w+)\s*=\s*0x([0-9A-Fa-f]+)u?\s*;[ \t]*(?://\s*(.*))?')
    gpat = re.compile(r'ee::Global<\s*(.+?)\s*,\s*0x([0-9A-Fa-f]+)u?\s*>\s*(\w+)\s*;')
    for root, dirs, files in os.walk(game_src):
        dirs.sort()  # the same order on every host (names chosen by order)
        files = sorted(files)
        if os.path.basename(root) in ('drafts', 'sweep'):  # (readable_sweep.py's batch files: generated, not sources)
            continue
        # (readable_names agent) dm2/ names DM2's own addresses (physics.h's kRandom = 0x0012A020u
        # is DM2's, not the AI's function there): the drafts are the AI's, so there only DM2's host
        # side's names of AI addresses are read: dm2/**/ai_*.cpp|h whole, and elsewhere the kAi*
        # constants (hosted.cpp's kAiFindLocators = 0x0015D408u). bmx/ the same: its sym:: names
        # read as BMX XXX's own addresses (literal_view: a game folder's rows), so its globals
        # (weapon/hits.cpp's g_largestFloat at BMX's 0x0029FF64, its g_gravity at 0x002730E8,
        # where AI has g_screenGravityStep) are no AI names.
        in_dm2 = include_path(root, game_src).split('/')[0] in ('dm2', 'bmx')
        for name in files:
            if not name.endswith(('.cpp', '.h')):
                continue
            ai_only = in_dm2 and not name.startswith('ai_')
            path = os.path.join(root, name)
            text = readable_symbols.literal_view(open(path, errors='replace').read())
            for m in cpat.finditer(text):
                if ai_only and not re.match(r'^kAi[A-Z]', m.group(1)):
                    continue
                constants[int(m.group(2), 16)].append((m.group(1), (m.group(3) or '').strip()))
            if ai_only:
                continue
            for m in gpat.finditer(text):
                ns = namespace_at(text, m.start())
                globals_[int(m.group(2), 16)].append((m.group(3), m.group(1), ns))
    return constants, globals_


# ---------------------------------------------------------------------------
# Instructions
# ---------------------------------------------------------------------------

class Insn:
    __slots__ = ('addr', 'raw', 'op', 'args', 'code')

    def __init__(self, addr, raw, op, args):
        self.addr, self.raw, self.op, self.args, self.code = addr, raw, op, args, []

    def __repr__(self):
        return '%06x %s %s' % (self.addr, self.op, ', '.join(self.args))


class Parsed:
    pass


def split_operands(text):
    text = text.split('(Delay Slot)')[0].strip()
    if not text:
        return []
    out, depth, cur = [], 0, ''
    for c in text:
        if c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
        if c == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
        else:
            cur += c
    out.append(cur.strip())
    return out


def parse_generated(path):
    lines = open(path, errors='replace').read().split('\n')
    p = Parsed()
    p.path = path
    p.insns = {}
    p.start = p.end = None
    p.direct_tails = set()
    p.jump_tables = {}
    p.name = None
    cur = None
    last_jr = None
    for i, line in enumerate(lines):
        m = re.match(r'^// Function: (\S+)', line)
        if m:
            p.name = m.group(1)
        m = re.match(r'^// Address: 0x([0-9a-f]+) - 0x([0-9a-f]+)', line)
        if m:
            p.start, p.end = int(m.group(1), 16), int(m.group(2), 16)
        m = LINE.match(line)
        if m:
            addr = int(m.group(1), 16)
            if addr in p.insns:
                cur = None
            else:
                op, operands = m.group(3), m.group(4)
                if op == '.word':
                    # An instruction the disassembler only names in a comment:
                    # ".word 0x460000E4  # cvt.w.s $f3, $f0 # ...".
                    mm = re.search(r'#\s*([a-z][\w.]*)\s+([^#]*)#', operands)
                    if mm:
                        op, operands = mm.group(1), mm.group(2)
                cur = Insn(addr, int(m.group(2), 16), op, split_operands(operands))
                p.insns[addr] = cur
            if m.group(3) in ('jr', 'jalr'):
                last_jr = addr
            continue
        s = line.strip()
        if cur is not None and re.match(r'^ctx->pc = 0x[0-9A-Fa-f]+u;$', s) and not cur.code:
            continue
        if cur is not None:
            if (s.startswith('ctx->pc') or s.startswith('//') or s.startswith('label_') or s.startswith('ctx->in_delay_slot')
                    or s == '}' or s == '{' or s.startswith('if (') or s.startswith('const bool branch_taken') or s.startswith('#')):
                cur = None
            elif s:
                cur.code.append(s)
        m = re.match(r'^(\w+_0x([0-9a-f]+))\(rdram, ctx, runtime\); return;$', s)
        if m:
            p.direct_tails.add(int(m.group(2), 16))
        # Generated code since readable round 11: runtime->tailCall(rdram,
        # ctx, target, source, &FUN_x); return;
        m = re.match(r'^runtime->tailCall\(rdram, ctx, 0x([0-9A-Fa-f]+)u,', s)
        if m:
            p.direct_tails.add(int(m.group(1), 16))
        m = re.match(r'^case 0x([0-9A-Fa-f]+)u: goto label_', s)
        if m and last_jr is not None:
            p.jump_tables.setdefault(last_jr, []).append(int(m.group(1), 16))
    if p.start is None and p.insns:
        p.start = min(p.insns)
        p.end = max(p.insns) + 4
    return p


def reg_name(text):
    t = text.strip().lstrip('$')
    t = GPR_ALIASES.get(t, t)
    return t


def imm(text):
    return int(text.strip(), 0)


def mem_operand(text):
    m = re.match(r'^(-?0x[0-9a-fA-F]+|-?\d+)\((\$\w+)\)$', text.strip())
    if not m:
        raise ValueError('memory operand ' + text)
    return int(m.group(1), 0), reg_name(m.group(2))


def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def s32(v):
    v &= 0xFFFFFFFF
    return v - 0x100000000 if v & 0x80000000 else v


def branch_target(insn):
    return (insn.addr + 4 + (s16(insn.raw) << 2)) & 0xFFFFFFFF


def jump_target(insn):
    return ((insn.addr + 4) & 0xF0000000) | ((insn.raw & 0x03FFFFFF) << 2)


# ---------------------------------------------------------------------------
# Expressions
# ---------------------------------------------------------------------------

class X:
    """An expression. k: kind; a: children; v: value; t: C++ type
    (i32 u32 i64 u64 f bool q, i8 u8 i16 u16 for memory); m: for a load, how
    the generated code makes the access ('io': runtime->LoadN, 'mem': READn or
    FAST_READn; None: not from an instruction)."""
    __slots__ = ('k', 'a', 'v', 't', 'm')

    def __init__(self, k, a=(), v=None, t='i32', m=None):
        self.k, self.a, self.v, self.t, self.m = k, list(a), v, t, m

    def __repr__(self):
        return 'X(%s,%r,%r,%s)' % (self.k, self.v, self.a, self.t)

    def walk(self):
        yield self
        for c in self.a:
            yield from c.walk()

    def copy(self):
        return X(self.k, [c.copy() for c in self.a], self.v, self.t, self.m)


def C(v, t='i32'):
    return X('c', v=v, t=t)


def R(name):
    return X('reg', v=name, t='f' if name.startswith('f') and name[1:].isdigit() else 'i32')


def B(op, a, b, t='i32'):
    return X('bin', [a, b], op, t)


def simplify(x):
    """Constant folding and small rewrites (bottom up)."""
    x.a = [simplify(c) for c in x.a]
    k = x.k
    if k == 'sext':
        return x.a[0]
    if k == 'align16' and x.a[0].k == 'c':
        return C(x.a[0].v & ~15, 'u32')
    if k == 'align16' and x.a[0].k == 'sp':
        return X('sp', v=x.a[0].v & ~15, t='u32')
    if k == 'bin' and x.v == '+' and x.a[0].k == 'sp' and x.a[1].k == 'c':
        return X('sp', v=x.a[0].v + x.a[1].v, t='u32')
    if k == 'bin' and x.t in ('32', 'i32', 'u32') and all(c.k == 'c' for c in x.a):
        a, b = x.a[0].v, x.a[1].v
        op = x.v
        try:
            v = {'+': lambda: a + b, '-': lambda: a - b, '&': lambda: a & b, '|': lambda: a | b, '^': lambda: a ^ b,
                 '<<': lambda: a << b, '>>u': lambda: (a & 0xFFFFFFFF) >> b, '>>s': lambda: s32(a) >> b}[op]()
            return C(s32(v), 'u32' if x.t == 'u32' else 'i32')
        except KeyError:
            pass
    if k == 'bin' and x.v in ('+', '|', '^', '-') and x.a[1].k == 'c' and x.a[1].v == 0:
        return x.a[0]
    if k == 'bin' and x.v == '+' and x.a[0].k == 'bin' and x.a[0].v == '+' and x.a[0].a[1].k == 'c' and x.a[1].k == 'c' \
            and x.t == x.a[0].t:
        return simplify(B('+', x.a[0].a[0], C(x.a[0].a[1].v + x.a[1].v), x.t))
    if k == 'cmp' and x.v in ('==', '!=') and x.a[1].k == 'c' and x.a[1].v == 0 and x.a[0].k == 'b2i':
        inner = x.a[0].a[0]
        return inner if x.v == '!=' else negate(inner)
    if k == 'cmp' and x.v in ('==', '!=') and x.a[1].k == 'c' and x.a[1].v == 0 and x.a[0].t == 'bool':
        return x.a[0] if x.v == '!=' else negate(x.a[0])
    if k == 'not' and x.a[0].k == 'not':
        return x.a[0].a[0]
    # The bits of a float loaded from memory: the word itself.
    if k == 'bits' and x.a[0].k == 'ld' and x.a[0].v == 'f':
        return X('ld', [x.a[0].a[0]], 'i32', 'i32', x.a[0].m)
    if k == 'fromBits' and x.a[0].k == 'ld' and x.a[0].v in ('i32', 'u32'):
        return X('ld', [x.a[0].a[0]], 'f', 'f', x.a[0].m)
    if k == 'bits' and x.a[0].k == 'fromBits':
        return x.a[0].a[0]
    if k == 'sp' and False:
        return x
    return x


def is_scratchpad_address(a):
    a &= 0xFFFFFFFF
    p = a & 0x7FFFFFFF if a >= 0x80000000 else a
    return 0x70000000 <= p < 0x70004000


def is_constant_io_address(a, m=None):
    """A constant address readable code reaches through ee::io: an I/O register
    (is_io_address), or the scratchpad where the generated statement of the
    access is runtime->LoadN / StoreN (m 'io': the recompiler knew the address
    at the instruction, lui + ld together; the recorder keeps those accesses in
    its I/O log). Where the address came from a register set earlier the
    generated code is READn / WRITEn (m 'mem'), which the recorder keeps as
    memory, as ee::at does (verdicts: fn_10D6B8's ld through s1; fn_10D388
    makes both kinds). m None (no instruction of its own): as runtime->LoadN."""
    return is_io_address(a) or is_scratchpad_address(a) and m != 'mem'


def is_io_address(a):
    """An address PS2Runtime::isSpecialAddress takes (runtime/ps2_address.h) but
    the scratchpad, which ee::at reaches as memory: the I/O registers, the GS's,
    VU memory, the BIOS, kseg2/3. Readable code reaches them through ee::io."""
    a &= 0xFFFFFFFF
    if a >= 0xC0000000:
        return True
    p = a & 0x1FFFFFFF if a >= 0x80000000 else a
    return (0x1FC00000 <= p < 0x20000000 or 0x10000000 <= p < 0x10010000 or 0x12000000 <= p < 0x12002000 or
            0x11004000 <= p < 0x1100C000)


# A draft whose memory accesses may reach I/O registers through pointers known
# only at run time (Draft.io_dynamic): its loads are ordered like stores.
IO_DYNAMIC = [False]


def is_io_load(n):
    if n.k != 'ld':
        return False
    if IO_DYNAMIC[0] and n.a[0].k != 'sp':
        return True
    a = const_address(simplify(n.a[0].copy()))
    return a is not None and is_constant_io_address(a, n.m)


def reads_memory(x):
    return any(n.k in ('ld', 'ldu', 'call', 'raw', 'vflo', 'vfhi', 'left') for n in x.walk())


def has_effects(x):
    # An I/O register's load is an access the checker compares in order.
    return any(n.k in ('call', 'raw') or is_io_load(n) for n in x.walk())


# ---------------------------------------------------------------------------
# IR: register-level operations and blocks
# ---------------------------------------------------------------------------

class Op:
    """k: set (dst, x) | st (t, addr, val) | call (call info) | vraw (text, regs) | note (text)
    | unsup (insn)."""
    __slots__ = ('k', 'dst', 'x', 'addr', 't', 'info', 'uses', 'defs', 'pc', 'id', 'note', 'web', 'm', 'saved', 'fp_sp', 'passed')

    def __init__(self, k, pc, dst=None, x=None, addr=None, t=None, info=None, note=None, m=None):
        self.k, self.pc, self.dst, self.x, self.addr, self.t, self.info, self.note = k, pc, dst, x, addr, t, info, note
        self.uses, self.defs, self.id, self.web, self.m = [], [], None, None, m
        self.fp_sp = None
        self.passed = ()


class CallInfo:
    def __init__(self, target, kind, pc, target_x=None):
        self.target, self.kind, self.pc, self.target_x = target, kind, pc, target_x
        self.int_args, self.float_args = [], []
        self.qword_args = []  # int args passed as whole 128-bit registers (ee::Qword)
        self.result = None   # 'v0' | 'f0' | None
        self.ret_t = None


class Block:
    def __init__(self, start):
        self.start = start
        self.ops = []
        self.term = None       # ('fall', b) ('goto', b) ('cond', x, taken, fall, likely) ('ret',) ('tail', CallInfo)
        self.succ = []         # [(block start, charged)]
        self.preds = set()
        self.synthetic = False
        self.stmts = None
        self.end = start


class Unsupported(Exception):
    pass


# COP0 registers the recompiler reads and writes (cop0_translator.cpp).
COP0_REGISTERS = (0, 1, 2, 3, 4, 5, 6, 8, 9, 10, 11, 12, 13, 14, 15, 16, 23, 24, 25, 28, 29, 30)
# Parallel instructions on whole registers, as ee/mmi.h has them: (form, _).
MMI_OPS = {op: ('binary', None) for op in ('paddh', 'psubb', 'psubw', 'pand', 'por', 'pnor', 'pxor', 'pcgth', 'pmaxh', 'pminh',
                                           'pcpyld', 'pcpyud', 'pextlb', 'pextub', 'pextlw', 'pextuw', 'ppacb', 'qfsrv')}
MMI_OPS.update({op: ('shift', None) for op in ('psrlh', 'psrah', 'psllh')})
MMI_OPS['pcpyh'] = ('unary', None)


class Lifter:
    """One function lifted: CFG, dataflow, statements."""

    def __init__(self, ws, address, depth=0):
        self.ws = ws
        self.address = address
        self.depth = depth
        self.p = ws.parsed(address)
        if self.p is None or not self.p.insns or self.p.start is None:
            raise Unsupported('no generated code for 0x%x' % address)
        self.notes = []
        self.unsupported = []
        self.uses_io = False
        self.uses_mmi = False
        self.syscalls = False
        self.vu0 = False
        self.uses_runtime = False
        self.frame_size = 0
        self.frame_mode = False
        self.calls = []
        self.lower()

    # ---- lowering ---------------------------------------------------------

    def find_unchecked_tables(self, insns):
        """A JR through a word of a table with no bounds check before it (the
        compiler knew the index's range; the recompiler then dispatches the
        target at run time): `lui t, hi; addiu b, t, lo; sll i, x, 2; addu a,
        i, b; lw r, 0(a); jr r`, the table's words up to the first that is not
        an address inside the function. Found in FUN_0018D678 (the follow
        camera's offsets, a loop over the table at 0x2643A0)."""
        p = self.p
        by_addr = {i.addr: i for i in insns}
        # (The parse is shared: a table found by an earlier lift of the
        # function, a caller's look at its callee, is kept on it.)
        if not hasattr(p, 'dispatched'):
            p.dispatched = set()
        self.dispatched_jumps |= p.dispatched
        for i in insns:
            if i.op != 'jr' or reg_name(i.args[0]) == 'ra' or p.jump_tables.get(i.addr):
                continue
            rs = reg_name(i.args[0])
            # The load of rs, then the table's base among the few instructions before it.
            load = None
            for back in range(1, 4):
                j = by_addr.get(i.addr - 4 * back)
                if j is not None and j.op == 'lw' and reg_name(j.args[0]) == rs:
                    load = j
                    break
            if load is None:
                continue
            hi = {}
            base = None
            for back in range(12, 0, -1):
                j = by_addr.get(load.addr - 4 * back)
                if j is None:
                    continue
                if j.op == 'lui':
                    hi[reg_name(j.args[0])] = (imm(j.args[1]) & 0xFFFF) << 16
                elif j.op == 'addiu' and reg_name(j.args[1]) in hi:
                    lo = imm(j.args[2])
                    if lo >= 0x8000:
                        lo -= 0x10000
                    base = (hi[reg_name(j.args[1])] + lo) & 0xFFFFFFFF
            if base is None:
                continue
            targets = []
            for k in range(256):
                w = self.ws.elf_word(base + 4 * k)
                if w is None or not (p.start <= w < p.end) or w not in by_addr:
                    break
                targets.append(w)
            if targets:
                p.jump_tables[i.addr] = targets
                p.dispatched.add(i.addr)
                self.dispatched_jumps.add(i.addr)
                self.notes.append('the jump at 0x%x goes through the table at 0x%x (%d entries, no bounds check)'
                                  % (i.addr, base, len(targets)))

    def kernel_call_ends(self, pc):
        """Whether the kernel call at pc does not come back: ExitThread, which
        crt0's exit address (0x1000C0) ends on."""
        before = self.insns.get(pc - 4)
        return before is not None and before.op == 'addiu' and reg_name(before.args[0]) == 'v1' and \
            reg_name(before.args[1]) == 'zero' and imm(before.args[2]) == 0x23

    def lower(self):
        p = self.p
        insns = [p.insns[a] for a in sorted(p.insns) if p.start <= a < p.end]
        self.insns = {i.addr: i for i in insns}
        # JRs through a table the recompiler dispatches at run time
        # (find_unchecked_tables): the blocks that end with one.
        self.dispatched_jumps, self.dispatched_switches = set(), set()
        self.find_unchecked_tables(insns)
        addrs = [i.addr for i in insns]
        # Leaders: the start, branch targets inside, the instruction after a
        # branch's delay slot.
        leaders = {p.start}
        for i in insns:
            if self.is_branch(i):
                leaders.add(i.addr + 8)
                for t in self.branch_targets(i):
                    if p.start <= t < p.end:
                        leaders.add(t)
        # And the function table's entry blocks (entry_ files) that start
        # inside a run of straight code: the original's slice ends there, so
        # the draft's block must too, for its ee::block marker (drafter20:
        # fn_107248 0x1072E0, fn_1CFF58 0x1D0000, reached by falling through).
        table = self.ws.table() if self.ws else None
        if table:
            delay_slots = {i.addr + 4 for i in insns if self.is_branch(i)}
            for i in insns:
                if i.addr != p.start and i.addr not in delay_slots and str(table.get(i.addr, '')).startswith('entry_'):
                    leaders.add(i.addr)
        leaders = sorted(a for a in leaders if p.start <= a < p.end and a in self.insns)
        self.blocks = {}
        order = []
        for li, start in enumerate(leaders):
            end = leaders[li + 1] if li + 1 < len(leaders) else p.end
            b = Block(start)
            b.end = end
            self.blocks[start] = b
            order.append(start)
            a = start
            while a < end:
                i = self.insns.get(a)
                if i is None:
                    a += 4
                    continue
                if self.is_branch(i):
                    delay = self.insns.get(a + 4)
                    self.lower_branch(b, i, delay, end)
                    break
                self.lower_insn(b, i)
                a += 4
            if b.term is None:
                b.term = ('fall', end) if end in self.insns else ('ret',)
        self.order = order
        self.link()

    def is_branch(self, i):
        return i.op in ('b', 'beq', 'bne', 'beqz', 'bnez', 'blez', 'bgtz', 'bltz', 'bgez', 'beql', 'bnel', 'beqzl',
                        'bnezl', 'blezl', 'bgtzl', 'bltzl', 'bgezl', 'bc1t', 'bc1f', 'bc1tl', 'bc1fl', 'j', 'jal',
                        'jr', 'jalr', 'bal', 'bltzal', 'bgezal')

    def branch_targets(self, i):
        if i.op in ('j', 'jal'):
            return [jump_target(i)]
        if i.op in ('jr', 'jalr'):
            return self.p.jump_tables.get(i.addr, [])
        return [branch_target(i)]

    def lower_branch(self, b, i, delay, end):
        op = i.op
        likely = op.endswith('l') and op not in ('bal',) and op != 'jal'
        if op in ('bc1tl', 'bc1fl'):
            likely = True
        delay_ops = Block(i.addr + 4)
        if delay is not None and delay.op != 'nop':
            self.lower_insn(delay_ops, delay)
        fall = i.addr + 8
        if op == 'jal' or op == 'jalr' or op == 'bal':
            b.ops.extend(delay_ops.ops)
            if op == 'jal':
                info = CallInfo(jump_target(i), 'call', i.addr)
            elif op == 'bal':
                raise Unsupported('bal at 0x%x' % i.addr)
            else:
                rs = reg_name(i.args[-1])
                info = CallInfo(None, 'icall', i.addr, R(rs))
                info.link = reg_name(i.args[0]) if len(i.args) > 1 else 'ra'
            op_ = Op('call', i.addr, info=info)
            b.ops.append(op_)
            self.calls.append(op_)
            b.term = ('fall', fall) if fall in self.insns else ('ret',)
            return
        if op == 'jr':
            rs = reg_name(i.args[0])
            b.ops.extend(delay_ops.ops)
            if rs == 'ra':
                b.term = ('ret',)
                return
            targets = self.p.jump_tables.get(i.addr)
            if targets:
                b.term = ('switch', R(rs), sorted(set(targets)))
                if i.addr in self.dispatched_jumps:
                    self.dispatched_switches.add(b.start)
                return
            info = CallInfo(None, 'itail', i.addr, R(rs))
            b.term = ('tail', info)
            return
        if op == 'j':
            t = jump_target(i)
            b.ops.extend(delay_ops.ops)
            if self.p.start <= t < self.p.end:
                b.term = ('goto', t)
            else:
                kind = 'tail' if t in self.p.direct_tails else 'jtail'
                b.term = ('tail', CallInfo(t, kind, i.addr))
            return
        # Conditional (or b).
        t = branch_target(i)
        a = [reg_name(x) for x in i.args[:-1]]

        def rx(n):
            return C(0) if n == 'zero' else R(n)
        if op in ('b',) or (op in ('beq', 'beql') and a[0] == 'zero' and a[1] == 'zero'):
            cond = None
        elif op in ('beq', 'beql'):
            cond = X('cmp', [rx(a[0]), rx(a[1])], '==')
        elif op in ('bne', 'bnel'):
            cond = X('cmp', [rx(a[0]), rx(a[1])], '!=')
        elif op in ('beqz', 'beqzl'):
            cond = X('cmp', [rx(a[0]), C(0)], '==')
        elif op in ('bnez', 'bnezl'):
            cond = X('cmp', [rx(a[0]), C(0)], '!=')
        elif op in ('blez', 'blezl'):
            cond = X('cmp', [rx(a[0]), C(0)], '<=s')
        elif op in ('bgtz', 'bgtzl'):
            cond = X('cmp', [rx(a[0]), C(0)], '>s')
        elif op in ('bltz', 'bltzl'):
            cond = X('cmp', [rx(a[0]), C(0)], '<s')
        elif op in ('bgez', 'bgezl'):
            cond = X('cmp', [rx(a[0]), C(0)], '>=s')
        elif op in ('bc1t', 'bc1tl'):
            cond = X('reg', v='fcc', t='bool')
        elif op in ('bc1f', 'bc1fl'):
            cond = X('not', [X('reg', v='fcc', t='bool')], t='bool')
        else:
            raise Unsupported('branch %s at 0x%x' % (op, i.addr))
        if cond is None:
            b.ops.extend(delay_ops.ops)
            b.term = ('goto', t)
            return
        if likely:
            b.term = ('cond', cond, t, fall, delay_ops.ops)
            return
        # The condition is read before the delay slot runs.
        written = set()
        for o in delay_ops.ops:
            if o.k == 'set':
                written.add(o.dst)
        read = {n.v for n in cond.walk() if n.k == 'reg'}
        if written & read:
            tmp = 'cond_%x' % i.addr
            b.ops.append(Op('set', i.addr, dst=tmp, x=cond))
            cond = X('reg', v=tmp, t='bool')
        b.ops.extend(delay_ops.ops)
        b.term = ('cond', cond, t, fall, None)

    def unsupported_op(self, b, i, why=''):
        self.unsupported.append(i)
        b.ops.append(Op('unsup', i.addr, note='%s %s%s' % (i.op, ', '.join(i.args), (': ' + why) if why else '')))

    def lower_insn(self, b, i):
        if i.addr in getattr(self, 'skip', set()):
            return
        try:
            self._lower_insn(b, i)
        except (ValueError, IndexError) as e:
            self.unsupported_op(b, i, str(e))

    def _lower_insn(self, b, i):
        op, a = i.op, i.args
        pc = i.addr

        def rx(n):
            n = reg_name(n)
            return C(0) if n == 'zero' else R(n)

        def setr(dst, x):
            dst = reg_name(dst)
            if dst == 'zero':
                return
            b.ops.append(Op('set', pc, dst=dst, x=x))

        # How the generated code makes this instruction's access (X.m, Op.m).
        mc = 'io' if any('runtime->Load' in c or 'runtime->Store' in c for c in i.code) else 'mem'

        def store(t, val, operand):
            off, base = mem_operand(operand)
            b.ops.append(Op('st', pc, t=t, addr=B('+', rx(base), C(off), 'u32') if off else rx(base), x=val, m=mc))

        def addr_of(operand):
            off, base = mem_operand(operand)
            return B('+', rx(base), C(off), 'u32') if off else rx(base)

        if op == 'c1':
            # COP1 operations the disassembler does not name: SQRT.S (funct 4),
            # RSQRT.S (0x16).
            funct = i.raw & 0x3F
            fd, fs, ft = (i.raw >> 6) & 31, (i.raw >> 11) & 31, (i.raw >> 16) & 31
            if funct == 4:
                b.ops.append(Op('set', pc, dst='f%d' % fd, x=X('ffn', [R('f%d' % ft)], 'ee::sqrt', 'f')))
                return
            if funct == 0x16:
                b.ops.append(Op('set', pc, dst='f%d' % fd, x=X('ffn', [R('f%d' % fs), R('f%d' % ft)], 'ee::rsqrt', 'f')))
                return
            raise ValueError('COP1 funct 0x%x' % funct)
        if op in ('nop', 'sync', 'sync.p', 'sync.l', 'vnop', 'vwaitq', 'ssnop', 'cache', 'pref'):
            return
        if (i.raw >> 21) == 0x200:
            # MFC0 (MFPS/MFPC too: register 25, the performance counters), as
            # the recompiled instruction reads it (ee/io.h). rd from the word
            # itself (the disassembler names 25 "Reserved25").
            rt, rd = (i.raw >> 16) & 31, (i.raw >> 11) & 31
            if rd not in COP0_REGISTERS:
                raise ValueError('COP0 register %d' % rd)
            self.uses_io = True
            if rt:
                named = {9: 'cop0Count()', 12: 'cop0Status()', 25: 'cop0Perf()'}.get(rd, 'cop0Register(%d)' % rd)
                b.ops.append(Op('set', pc, dst=GPR[rt], x=X('raw', [], 'static_cast<int32_t>(ee::io::%s)' % named, 'i32')))
            return
        if (i.raw >> 21) == 0x204:
            # MTC0 (MTPS/MTPC too), with the masks the recompiled instruction has.
            rt, rd = (i.raw >> 16) & 31, (i.raw >> 11) & 31
            if rd not in COP0_REGISTERS:
                raise ValueError('COP0 register %d' % rd)
            self.uses_io = True
            regs = [GPR[rt]] if rt else []
            b.ops.append(Op('vraw', pc, x=X('raw', v='ee::io::setCop0Register(%d, %s);' % (rd, '{U32:%s}' % GPR[rt] if rt else '0u')),
                            info=regs, note='MTC0 %s' % ', '.join(x.lstrip('$') for x in i.args)))
            return
        if op == 'syscall':
            # A kernel call (the number in v1, the arguments where the caller
            # left them, the result in v0): through ee::kernelCall, which
            # lets a call that waits leave as a guest callee does.
            if pc + 4 not in self.insns and not self.kernel_call_ends(pc):
                raise Unsupported('a kernel call at 0x%x runs on into the next function' % pc)
            self.syscalls = True
            b.ops.append(Op('set', pc, dst='v0', x=X('raw', [R('v1')], 'kernel', 'i32')))
            return
        if op in MMI_OPS:
            self.lower_mmi(b, i)
            return
        if op in ('mtsab', 'mtsah'):
            # The shift amount QFSRV reads (bytes or halfwords, in bits).
            self.uses_mmi = True
            s, imm_ = reg_name(a[0]), imm(a[1]) & 0xFFFF
            value = '{U32:%s}' % s if s != 'zero' else '0u'
            b.ops.append(Op('vraw', pc, x=X('raw', v='ee::mmi::setShift%s(%s ^ 0x%Xu);' % ('Bytes' if op == 'mtsab' else 'Halfwords', value, imm_)),
                            info=[s] if s != 'zero' else [], note=op.upper()))
            return
        if op == 'mtsa':
            self.uses_mmi = True
            s = reg_name(a[0])
            b.ops.append(Op('vraw', pc, x=X('raw', v='ee::mmi::setShift(%s);' % ('{U32:%s}' % s if s != 'zero' else '0u')),
                            info=[s] if s != 'zero' else [], note='MTSA'))
            return
        if op == 'mfsa':
            self.uses_mmi = True
            if reg_name(a[0]) != 'zero':
                b.ops.append(Op('set', pc, dst=reg_name(a[0]), x=X('raw', [], 'static_cast<int32_t>(ee::mmi::shift())', 'i32')))
            return
        if op in ('ei', 'di'):
            # COP0 Status.EIE, as generated code sets it (ee/io.h).
            self.uses_io = True
            b.ops.append(Op('vraw', pc, x=X('raw', v='ee::io::%s();' % (
                'enableInterrupts' if op == 'ei' else 'disableInterrupts')), info=[], note=op.upper()))
            return
        if op in ('addiu', 'addi', 'daddiu', 'daddi'):
            v = imm(a[2])
            src = rx(a[1])
            setr(a[0], C(s32(v)) if src.k == 'c' else (src if v == 0 else B('+', src, C(v), '64' if op.startswith('d') else '32')))
        elif op in ('addu', 'add', 'daddu', 'dadd'):
            x, y = rx(a[1]), rx(a[2])
            w = '64' if op.startswith('d') else '32'
            if y.k == 'c' and y.v == 0:
                setr(a[0], x if w == '64' else X('sext', [x], t='i32'))
            elif x.k == 'c' and x.v == 0:
                setr(a[0], y if w == '64' else X('sext', [y], t='i32'))
            else:
                setr(a[0], B('+', x, y, w))
        elif op in ('subu', 'sub', 'dsubu', 'dsub'):
            x, y = rx(a[1]), rx(a[2])
            w = '64' if op.startswith('d') else '32'
            setr(a[0], X('neg', [y], t='i32') if x.k == 'c' and x.v == 0 else B('-', x, y, w))
        elif op in ('negu', 'neg', 'dnegu'):
            setr(a[0], X('neg', [rx(a[1])], t='i32'))
        elif op in ('and', 'or', 'xor', 'nor'):
            x, y = rx(a[1]), rx(a[2])
            if op == 'or' and y.k == 'c' and y.v == 0:
                setr(a[0], x)
            elif op == 'or' and x.k == 'c' and x.v == 0:
                setr(a[0], y)
            elif op == 'nor':
                if y.k == 'c' and y.v == 0:
                    setr(a[0], X('bnot', [x]))
                elif x.k == 'c' and x.v == 0:
                    setr(a[0], X('bnot', [y]))
                else:
                    setr(a[0], X('bnot', [B('|', x, y)]))
            else:
                setr(a[0], B({'and': '&', 'or': '|', 'xor': '^'}[op], x, y))
        elif op in ('andi', 'ori', 'xori'):
            v = imm(a[2]) & 0xFFFF
            x = rx(a[1])
            if x.k == 'c':
                val = {'andi': x.v & v, 'ori': x.v | v, 'xori': x.v ^ v}[op]
                setr(a[0], C(val))
            else:
                setr(a[0], B({'andi': '&', 'ori': '|', 'xori': '^'}[op], x, C(v, 'u32')))
        elif op == 'lui':
            setr(a[0], C(s32(imm(a[1]) << 16)))
        elif op in ('sll', 'srl', 'sra'):
            sa = imm(a[2])
            x = rx(a[1])
            if op == 'sll' and sa == 0:
                setr(a[0], X('sext', [x], t='i32'))
            else:
                setr(a[0], B({'sll': '<<', 'srl': '>>u', 'sra': '>>s'}[op], x, C(sa)))
        elif op in ('sllv', 'srlv', 'srav'):
            setr(a[0], B({'sllv': '<<', 'srlv': '>>u', 'srav': '>>s'}[op], rx(a[1]), B('&', rx(a[2]), C(31))))
        elif op in ('dsll', 'dsrl', 'dsra', 'dsll32', 'dsrl32', 'dsra32'):
            sa = imm(a[2]) + (32 if op.endswith('32') else 0)
            base = op[:4]
            setr(a[0], B({'dsll': '<<', 'dsrl': '>>u', 'dsra': '>>s'}[base], rx(a[1]), C(sa), '64'))
        elif op in ('dsllv', 'dsrlv', 'dsrav'):
            setr(a[0], B({'dsllv': '<<', 'dsrlv': '>>u', 'dsrav': '>>s'}[op], rx(a[1]), B('&', rx(a[2]), C(63)), '64'))
        elif op in ('slt', 'sltu'):
            setr(a[0], X('b2i', [X('cmp', [rx(a[1]), rx(a[2])], '<s' if op == 'slt' else '<u')]))
        elif op in ('slti', 'sltiu'):
            v = s16(imm(a[2]))
            setr(a[0], X('b2i', [X('cmp', [rx(a[1]), C(v if op == 'slti' else (v & 0xFFFFFFFF), 'i32' if op == 'slti' else 'u32')],
                                   '<s' if op == 'slti' else '<u')]))
        elif op in ('movz', 'movn'):
            d = reg_name(a[0])
            cond = X('cmp', [rx(a[2]), C(0)], '==' if op == 'movz' else '!=')
            setr(a[0], X('sel', [cond, rx(a[1]), R(d) if d != 'zero' else C(0)]))
        elif op in ('mult', 'multu', 'mult1', 'multu1'):
            if len(a) == 2:
                a = ['$zero'] + a
            sfx = '1' if op.endswith('1') else ''
            signed = not op.startswith('multu')
            prod = X('mul64', [rx(a[1]), rx(a[2])], 's' if signed else 'u', 'i64')
            b.ops.append(Op('set', pc, dst='lo' + sfx, x=X('lo32', [prod])))
            b.ops.append(Op('set', pc, dst='hi' + sfx, x=X('hi32', [prod.copy()])))
            if reg_name(a[0]) != 'zero':
                setr(a[0], R('lo' + sfx))
        elif op in ('madd', 'maddu', 'madd1', 'maddu1'):
            if len(a) == 2:
                a = ['$zero'] + a
            sfx = '1' if op.endswith('1') else ''
            signed = not op.startswith('maddu')
            prod = X('mul64', [rx(a[1]), rx(a[2])], 's' if signed else 'u', 'i64')
            # hi:lo and the product as 64 bits; the new hi and lo from it (as
            # the two halves of one value, so hi's reads the old lo).
            b.ops.append(Op('set', pc, dst='mq', x=X('add64', [X('hilo', [R('hi' + sfx), R('lo' + sfx)], t='i64'), prod], t='i64')))
            b.ops.append(Op('set', pc, dst='lo' + sfx, x=X('lo32', [R('mq')])))
            b.ops.append(Op('set', pc, dst='hi' + sfx, x=X('hi32', [R('mq')])))
            if reg_name(a[0]) != 'zero':
                setr(a[0], R('lo' + sfx))
        elif op in ('div', 'divu', 'div1', 'divu1'):
            if len(a) == 3:
                a = a[1:]
            sfx = '1' if op.endswith('1') else ''
            s = 's' if not op.startswith('divu') else 'u'
            b.ops.append(Op('set', pc, dst='lo' + sfx, x=B('/' + s, rx(a[0]), rx(a[1]))))
            b.ops.append(Op('set', pc, dst='hi' + sfx, x=B('%' + s, rx(a[0]), rx(a[1]))))
        elif op in ('mflo', 'mfhi', 'mflo1', 'mfhi1'):
            setr(a[0], R(op[2:]))
        elif op in ('mtlo', 'mthi', 'mtlo1', 'mthi1'):
            b.ops.append(Op('set', pc, dst=op[2:], x=rx(a[0])))
        elif op in ('lb', 'lbu', 'lh', 'lhu', 'lw', 'lwu', 'ld'):
            t = {'lb': 'i8', 'lbu': 'u8', 'lh': 'i16', 'lhu': 'u16', 'lw': 'i32', 'lwu': 'u32', 'ld': 'u64'}[op]
            setr(a[0], X('ld', [addr_of(a[1])], t, t, mc))
        elif op == 'lq':
            ad = X('align16', [addr_of(a[1])], t='u32')
            d = reg_name(a[0])
            setr(a[0], X('ld', [ad], 'u64', 'u64', mc))
            if d != 'zero':
                b.ops.append(Op('set', pc, dst=d + '^', x=X('ld', [B('+', ad.copy(), C(8), 'u32')], 'u64', 'u64', mc)))
        elif op in ('sb', 'sh', 'sw', 'sd'):
            t = {'sb': 'u8', 'sh': 'u16', 'sw': 'i32', 'sd': 'u64'}[op]
            store(t, rx(a[0]), a[1])
        elif op == 'sq':
            ad = X('align16', [addr_of(a[1])], t='u32')
            src = reg_name(a[0])
            b.ops.append(Op('st', pc, t='u64', addr=ad, x=rx(a[0]), m=mc))
            b.ops.append(Op('st', pc, t='u64', addr=B('+', ad.copy(), C(8), 'u32'), x=C(0) if src == 'zero' else R(src + '^'), m=mc))
        elif op in ('lwl', 'lwr', 'ldl', 'ldr', 'swl', 'swr', 'sdl', 'sdr'):
            self.lower_unaligned(b, i)
        elif op == 'lwc1':
            setr(a[0], X('ld', [addr_of(a[1])], 'f', 'f', mc))
        elif op == 'swc1':
            store('f', R(reg_name(a[0])), a[1])
        elif op == 'mtc1':
            setr(a[1], X('fromBits', [rx(a[0])], t='f'))
        elif op == 'mfc1':
            setr(a[0], X('bits', [R(reg_name(a[1]))], t='i32'))
        elif op in ('mov.s',):
            setr(a[0], R(reg_name(a[1])))
        elif op in ('neg.s', 'abs.s', 'sqrt.s'):
            fn = {'neg.s': 'fneg', 'abs.s': 'ee::abs', 'sqrt.s': 'ee::sqrt'}[op]
            src = R(reg_name(a[1]))
            setr(a[0], X('fneg', [src], t='f') if fn == 'fneg' else X('ffn', [src], fn, 'f'))
        elif op in ('add.s', 'sub.s', 'mul.s', 'div.s'):
            setr(a[0], X('fop', [R(reg_name(a[1])), R(reg_name(a[2]))], {'add.s': '+', 'sub.s': '-', 'mul.s': '*', 'div.s': '/'}[op], 'f'))
        elif op in ('min.s', 'max.s', 'rsqrt.s'):
            fn = {'min.s': 'ee::min', 'max.s': 'ee::max', 'rsqrt.s': 'ee::rsqrt'}[op]
            setr(a[0], X('ffn', [R(reg_name(a[1])), R(reg_name(a[2]))], fn, 'f'))
        elif op == 'cvt.s.w':
            setr(a[0], X('itof', [X('bits', [R(reg_name(a[1]))], t='i32')], t='f'))
        elif op == 'cvt.w.s':
            setr(a[0], X('fromBits', [X('ftoi', [R(reg_name(a[1]))], t='i32')], t='f'))
        elif op in ('c.lt.s', 'c.le.s', 'c.eq.s', 'c.olt.s', 'c.ole.s', 'c.f.s'):
            if op == 'c.f.s':
                b.ops.append(Op('set', pc, dst='fcc', x=X('c', v=0, t='bool')))
            else:
                cmpop = {'c.lt.s': '<', 'c.olt.s': '<', 'c.le.s': '<=', 'c.ole.s': '<=', 'c.eq.s': '=='}[op]
                b.ops.append(Op('set', pc, dst='fcc', x=X('fcmp', [R(reg_name(a[0])), R(reg_name(a[1]))], cmpop, 'bool')))
        elif op in ('mula.s', 'adda.s', 'suba.s'):
            fop = {'mula.s': '*', 'adda.s': '+', 'suba.s': '-'}[op]
            b.ops.append(Op('set', pc, dst='facc', x=X('fop', [R(reg_name(a[0])), R(reg_name(a[1]))], fop, 'f')))
        elif op in ('madda.s', 'msuba.s'):
            prod = X('fop', [R(reg_name(a[0])), R(reg_name(a[1]))], '*', 'f')
            b.ops.append(Op('set', pc, dst='facc', x=X('fop', [R('facc'), prod], '+' if op == 'madda.s' else '-', 'f')))
        elif op in ('madd.s', 'msub.s'):
            prod = X('fop', [R(reg_name(a[1])), R(reg_name(a[2]))], '*', 'f')
            setr(a[0], X('fop', [R('facc'), prod], '+' if op == 'madd.s' else '-', 'f'))
        elif op == 'lqc2':
            vf = int(reg_name(a[0])[2:])
            b.ops.append(Op('vld', pc, addr=X('align16', [addr_of(a[1])], t='u32'), t=vf))
            self.vu0 = True
        elif op == 'sqc2':
            vf = int(reg_name(a[0])[2:])
            b.ops.append(Op('vst', pc, addr=X('align16', [addr_of(a[1])], t='u32'), t=vf))
            self.vu0 = True
        elif op in ('qmfc2', 'qmfc2.ni', 'qmfc2.i'):
            vf = int(reg_name(a[1])[2:])
            d = reg_name(a[0])
            setr(a[0], X('vflo', v=vf, t='u64'))
            if d != 'zero':
                b.ops.append(Op('set', pc, dst=d + '^', x=X('vfhi', v=vf, t='u64')))
            self.vu0 = True
        elif op in ('qmtc2', 'qmtc2.ni', 'qmtc2.i'):
            vf = int(reg_name(a[1])[2:])
            s = reg_name(a[0])
            b.ops.append(Op('vset', pc, t=vf, x=rx(a[0]), addr=C(0) if s == 'zero' else R(s + '^')))
            self.vu0 = True
        elif op in ('cfc2', 'cfc2.ni', 'cfc2.i'):
            text = ' '.join(i.code)
            m = re.match(r'^SET_GPR_(?:U32|S32)\(ctx, \d+, (.*)\);$', text)
            if m:
                setr(a[0], X('raw', v=m.group(1), t='u32'))
            else:
                # I and Q: the bits of a float, read through a local.
                m = re.match(r'^\{ (.*) SET_GPR_(?:U32|S32)\(ctx, \d+, (\w+)\); \}$', text)
                if not m:
                    raise ValueError('cfc2 code ' + text)
                setr(a[0], X('raw', v='[&] { %s return %s; }()' % (m.group(1), m.group(2)), t='u32'))
            self.vu0 = True
        elif op.startswith('v') or op in ('ctc2', 'ctc2.ni', 'ctc2.i'):
            text = '\n'.join(i.code)
            if op.startswith('ctc2') and not text:
                # (The recompiled instruction does nothing: a read-only control register.)
                b.ops.append(Op('note', pc, note='%s %s: read-only, ignored' % (op.upper(), ', '.join(a))))
                return
            if not text or 'SET_GPR' in text:
                raise ValueError('VU0 code ' + text)
            regs = []

            def sub(m):
                n = int(m.group(2))
                regs.append(GPR[n])
                return '{%s:%s}' % (m.group(1), GPR[n])
            text = re.sub(r'GPR_(U32|S32|U64|S64)\(ctx,\s*(\d+)\)', sub, text)
            if 'rdram' in text or 'runtime' in text or re.search(r'\b(READ|WRITE)(8|16|32|64|128)\b', text):
                self.uses_runtime = True  # (the generated code's memory macros use rdram and runtime)
            b.ops.append(Op('vraw', pc, x=X('raw', v=text), info=regs, note='%s %s' % (op.upper(), ', '.join(x.lstrip('$') for x in a))))
            self.vu0 = True
        elif op == 'break':
            b.ops.append(Op('note', pc, note='BREAK %s (a trap the original raises; the game never does)' % ', '.join(a)))
        else:
            raise ValueError('not lifted')

    def lower_mmi(self, b, i):
        """A parallel instruction on whole registers (ee/mmi.h, the generated
        code's PS2_* forms). The value goes through the pair mq, mq^, so that
        a destination that is also a source is read whole."""
        self.uses_mmi = True
        a = i.args

        def q(n):
            n = reg_name(n)
            return X('qword', [C(0, 'u64'), C(0, 'u64')], t='q') if n == 'zero' else X('qword', [R(n), R(n + '^')], t='q')
        form, _ = MMI_OPS[i.op]
        d = reg_name(a[0])
        if i.op == 'por' and 'zero' in (reg_name(a[1]), reg_name(a[2])):
            # A move of a whole register (the compiler's), or a clear.
            s = reg_name(a[1]) if reg_name(a[2]) == 'zero' else reg_name(a[2])
            if d != 'zero':
                b.ops.append(Op('set', i.addr, dst=d, x=C(0) if s == 'zero' else R(s)))
                b.ops.append(Op('set', i.addr, dst=d + '^', x=C(0, 'u64') if s == 'zero' else R(s + '^')))
            return
        if form == 'shift':
            x = X('mmi', [q(a[1])], '%s(%%s, %d)' % (i.op, imm(a[2])), 'q')
        elif form == 'unary':
            x = X('mmi', [q(a[1])], i.op + '(%s)', 'q')
        else:
            x = X('mmi', [q(a[1]), q(a[2])], i.op + '(%s, %s)', 'q')
        b.ops.append(Op('set', i.addr, dst='mq', x=X('qlow', [x], t='u64')))
        b.ops.append(Op('set', i.addr, dst='mq^', x=X('qhigh', [x.copy()], t='u64')))
        if d != 'zero':
            b.ops.append(Op('set', i.addr, dst=d, x=R('mq')))
            b.ops.append(Op('set', i.addr, dst=d + '^', x=R('mq^')))

    def lower_unaligned(self, b, i):
        """lwl/lwr (ldl/ldr, swl/swr, sdl/sdr) pairs: an unaligned word."""
        op = i.op
        pair = {'lwl': ('lwr', 3, 'i32'), 'ldl': ('ldr', 7, 'u64'), 'swl': ('swr', 3, 'i32'), 'sdl': ('sdr', 7, 'u64')}
        if i.addr in getattr(self, 'skip', set()):
            return
        if op in pair:
            other_op, span, t = pair[op]
            nxt = None
            rt = reg_name(i.args[0])
            base = mem_operand(i.args[1])[1]
            for k in range(1, 8):
                cand = self.insns.get(i.addr + 4 * k)
                if cand is None:
                    break
                if cand.op == other_op and reg_name(cand.args[0]) == rt:
                    nxt = cand
                    break
                # (Its other half in the delay slot of a call: inlineMenuPopulate's
                # sdl, the call's arguments, jal memset, sdr.)
                if cand.op in ('jal', 'jalr'):
                    delay = self.insns.get(cand.addr + 4)
                    if delay is not None and delay.op == other_op and reg_name(delay.args[0]) == rt:
                        nxt = delay
                    break
                # Nothing in between may touch memory or change the register or the base.
                regs = [reg_name(x) for x in re.findall(r'\$\w+', ', '.join(cand.args))]
                if cand.op.startswith(('l', 's')) and '(' in ', '.join(cand.args) or (regs and regs[0] in (rt, base)) \
                        or cand.op in ('jal', 'jalr'):
                    break
            if nxt is not None:
                off_l, base_l = mem_operand(i.args[1])
                off_r, base_r = mem_operand(nxt.args[1])
                if base_l == base_r and off_l == off_r + span:
                    ad = B('+', R(base_r), C(off_r), 'u32') if off_r else R(base_r)
                    if op.startswith('l'):
                        b.ops.append(Op('set', i.addr, dst=reg_name(i.args[0]), x=X('ldu', [ad], t, t)))
                    else:
                        src = reg_name(i.args[0])
                        b.ops.append(Op('stu', i.addr, t=t, addr=ad, x=C(0) if src == 'zero' else R(src)))
                    self.skip = getattr(self, 'skip', set()) | {nxt.addr}
                    return
        raise ValueError('unpaired ' + op)

    def link(self):
        for start in self.order:
            b = self.blocks[start]
            t = b.term
            if t[0] in ('fall', 'goto'):
                b.succ = [(t[1], t[0] == 'goto' and t[1] <= b.end - 4)] if t[1] in self.blocks else []
            elif t[0] == 'cond':
                taken, fall, likely = t[2], t[3], t[4]
                last = b.end - 8 if b.end - 8 >= b.start else b.start
                br_pc = self.branch_pc(b)
                if likely:
                    s = Block(br_pc + 4)
                    s.synthetic = True
                    s.ops = likely
                    s.term = ('goto', taken)
                    s.succ = [(taken, taken <= br_pc)]
                    key = ('likely', br_pc)
                    self.blocks[key] = s
                    s.start = key
                    b.term = ('cond', t[1], key, fall, None)
                    b.succ = [(key, False), (fall, False)]
                else:
                    b.succ = [(taken, taken <= br_pc), (fall, False)]
            elif t[0] == 'switch':
                b.succ = [(x, x <= self.branch_pc(b)) for x in t[2] if x in self.blocks]
            else:
                b.succ = []
        for k, b in self.blocks.items():
            for s, _ in b.succ:
                if s in self.blocks:
                    self.blocks[s].preds.add(k)
        # Branch pc for goto/fall charges (a j back is charged too).
        for start in self.order:
            b = self.blocks[start]
            if b.term[0] == 'goto':
                pc = self.branch_pc(b)
                b.succ = [(b.term[1], b.term[1] <= pc)]

    def branch_pc(self, b):
        """The address of the branch ending block b."""
        a = b.end - 4
        while a >= b.start:
            i = self.insns.get(a)
            if i and self.is_branch(i):
                return a
            a -= 4
        return b.end - 4

    # ---- the stack ----------------------------------------------------------

    def all_ops(self):
        for k in self.block_keys():
            for o in self.blocks[k].ops:
                yield self.blocks[k], o

    def block_keys(self):
        keys = list(self.order)
        keys += [k for k in self.blocks if k not in self.order]
        return keys

    def resolve_stack(self):
        """sp at each point as entry sp + offset; sp-relative addresses made
        X('sp', off) (off from the entry sp)."""
        # sp (and a frame pointer: fp = sp after the prologue, sp = fp before
        # the epilogue, fp restored in it) at each block's start, along the
        # control flow: the epilogue's restore of fp must not end the frame
        # pointer for blocks reached before it.
        offs = {self.order[0]: (0, None)}
        work = [self.order[0]]
        bad = False
        self.fp_off = None
        while work:
            k = work.pop()
            b = self.blocks[k]
            off, fp = offs[k]
            self.fp_off = fp
            for o in b.ops:
                self._sp_rewrite(o, off)
                if o.k == 'set' and o.dst == 'fp':
                    if o.x is not None and o.x.k == 'sp':
                        self.fp_off = o.x.v
                        o.k = 'note'
                        o.note = 'fp = sp (the frame pointer)'
                    else:
                        self.fp_off = None
                if o.k == 'set' and o.dst == 'sp':
                    x = o.x
                    if x.k == 'sp':
                        off = x.v
                    else:
                        bad = True
            for s, _ in b.succ:
                if s not in offs:
                    offs[s] = (off, self.fp_off)
                    work.append(s)
                elif offs[s][0] != off:
                    bad = True
                elif offs[s][1] != self.fp_off:
                    offs[s] = (off, None if offs[s][1] is None or self.fp_off is None else offs[s][1])
            b.sp_out = off
            if b.term[0] in ('cond', 'switch', 'tail'):
                self._sp_rewrite_term(b, off)
        if bad:
            raise Unsupported('sp changes in a way the drafter does not follow')
        # Each call's frame pointer (fp = entry sp + o.fp_sp there, or None):
        # the set became a note, so fp has no def a call's saves see, but the
        # original's callee saves fp, a stack address (drafter24: 13 Verified
        # drafts gave their callees fp's entry value). From each block's final
        # entry state (a merge of two frame pointers is none).
        for k, (off0, fp0) in offs.items():
            cur = fp0
            for o in self.blocks[k].ops:
                if o.k == 'note' and getattr(o, 'note', '') == 'fp = sp (the frame pointer)' and o.x is not None:
                    cur = o.x.v
                elif o.k == 'set' and o.dst == 'fp':
                    cur = None
                elif o.k == 'call':
                    o.fp_sp = cur
        lows = [o.x.v for _, o in self.all_ops() if o.k == 'set' and o.dst == 'sp']
        self.frame_size = -min(lows) if lows else 0
        # Remove the sp adjustments.
        for k in self.block_keys():
            b = self.blocks[k]
            b.ops = [o for o in b.ops if not (o.k == 'set' and o.dst == 'sp')]

    def _sp_x(self, x, off):
        if x.k == 'reg' and x.v == 'sp':
            return X('sp', v=off, t='u32')
        if x.k == 'reg' and x.v == 'fp' and getattr(self, 'fp_off', None) is not None:
            return X('sp', v=self.fp_off, t='u32')
        if x.k == 'bin' and x.v == '+' and x.a[0].k == 'reg' and x.a[0].v == 'fp' and x.a[1].k == 'c' \
                and getattr(self, 'fp_off', None) is not None:
            return X('sp', v=self.fp_off + x.a[1].v, t='u32')
        if x.k == 'bin' and x.v == '+' and x.a[0].k == 'reg' and x.a[0].v == 'sp' and x.a[1].k == 'c':
            return X('sp', v=off + x.a[1].v, t='u32')
        x.a = [self._sp_x(c, off) for c in x.a]
        if x.k == 'align16' and x.a[0].k == 'sp':
            return X('sp', v=x.a[0].v & ~15, t='u32')  # sp is 16-byte aligned
        if x.k == 'bin' and x.v == '+' and x.a[0].k == 'sp' and x.a[1].k == 'c':
            return X('sp', v=x.a[0].v + x.a[1].v, t='u32')
        return x

    def _sp_rewrite(self, o, off):
        if o.x is not None:
            o.x = self._sp_x(o.x, off)
        if o.addr is not None:
            o.addr = self._sp_x(o.addr, off)
        if o.k == 'call' and o.info.target_x is not None:
            o.info.target_x = self._sp_x(o.info.target_x, off)

    def _sp_rewrite_term(self, b, off):
        t = b.term
        if t[0] == 'cond':
            b.term = ('cond', self._sp_x(t[1], off)) + t[2:]
        elif t[0] == 'switch':
            b.term = ('switch', self._sp_x(t[1], off), t[2])
        elif t[0] == 'tail' and t[1].target_x is not None:
            t[1].target_x = self._sp_x(t[1].target_x, off)

    # ---- dataflow -------------------------------------------------------------

    def collect_uses_defs(self):
        for b, o in self.all_ops():
            o.uses, o.defs = [], []
            if o.k == 'set':
                o.uses = regs_in(o.x)
                o.defs = [o.dst]
            elif o.k in ('st', 'stu'):
                o.uses = regs_in(o.addr) + regs_in(o.x)
            elif o.k in ('vld', 'vst'):
                o.uses = regs_in(o.addr)
            elif o.k == 'vset':
                o.uses = regs_in(o.x) + regs_in(o.addr)
            elif o.k == 'vraw':
                o.uses = list(o.info)
            elif o.k == 'call':
                info = o.info
                o.uses = (regs_in(info.target_x) if info.target_x is not None else []) + info.int_args + \
                    [r + '^' for r in info.qword_args] + info.float_args
                # The callee-saved registers the function has changed by
                # then (Draft.add_saved_uses): the callee's register saves
                # write them (ee::calleeSaves).
                o.uses = o.uses + [r for r in getattr(o, 'saved', ()) if r not in o.uses]
                o.uses = o.uses + [r for r in getattr(o, 'passed', ()) if r not in o.uses]
                o.defs = list(CALLER_SAVED)
        for k in self.block_keys():
            b = self.blocks[k]
            b.term_uses = []
            t = b.term
            if t[0] == 'cond':
                b.term_uses = regs_in(t[1])
            elif t[0] == 'switch':
                b.term_uses = regs_in(t[1])
            elif t[0] == 'ret':
                b.term_uses = list(self.ret_regs)
            elif t[0] == 'tail':
                info = t[1]
                b.term_uses = (regs_in(info.target_x) if info.target_x is not None else []) + info.int_args + \
                    [r + '^' for r in info.qword_args] + info.float_args

    def reaching(self):
        """Reaching definitions: self.rd_use[(op or block term, reg)] = set of def ids.
        Def ids: ('entry', reg) or (op id, reg)."""
        ids = 0
        for b, o in self.all_ops():
            o.id = ids
            ids += 1
        keys = self.block_keys()
        out = {k: {} for k in keys}
        inn = {k: {} for k in keys}
        entry = self.order[0]
        changed = True
        while changed:
            changed = False
            for k in keys:
                b = self.blocks[k]
                state = {}
                if k == entry:
                    state = {'*entry*': True}
                for p in b.preds:
                    for r, ds in out[p].items():
                        if r == '*entry*':
                            continue
                        state.setdefault(r, set()).update(ds)
                # 'entry' defs are implicit: a reg absent in state at entry
                # block reaches as entry; elsewhere absence also means entry
                # only if every path... handled by seeding below.
                inn[k] = state
                cur = {r: set(ds) for r, ds in state.items() if r != '*entry*'}
                for o in b.ops:
                    for r in o.defs:
                        cur[r] = {(o.id, r)}
                if cur != out[k]:
                    out[k] = cur
                    changed = True
        self.inn, self.out = inn, out

    def defs_at(self, state, reg):
        ds = state.get(reg)
        if ds is None:
            return {('entry', reg)}
        return ds

    def reaching_states(self):
        """For each block the state at its start with entry defs made explicit.
        A register some path reaches without a def gets ('entry', reg) too."""
        keys = self.block_keys()
        # may-be-undefined analysis: regs that may reach a block with no def.
        undef_in = {k: set() for k in keys}
        all_regs = set()
        for b, o in self.all_ops():
            all_regs.update(o.uses)
            all_regs.update(o.defs)
        for k in keys:
            all_regs.update(self.blocks[k].term_uses)
        entry = self.order[0]
        undef_out = {k: set() for k in keys}
        changed = True
        while changed:
            changed = False
            for k in keys:
                b = self.blocks[k]
                u = set(all_regs) if k == entry else set()
                for p in b.preds:
                    u |= undef_out[p]
                undef_in[k] = u
                cur = set(u)
                for o in b.ops:
                    cur -= set(o.defs)
                if cur != undef_out[k]:
                    undef_out[k] = cur
                    changed = True
        states = {}
        for k in keys:
            st = {r: set(ds) for r, ds in self.inn[k].items() if r != '*entry*'}
            for r in undef_in[k]:
                st.setdefault(r, set()).add(('entry', r))
            states[k] = st
        return states

    def build_use_map(self):
        states = self.reaching_states()
        self.use_defs = {}
        for k in self.block_keys():
            b = self.blocks[k]
            cur = {r: set(ds) for r, ds in states[k].items()}
            for o in b.ops:
                for r in o.uses:
                    self.use_defs[(o.id, r)] = set(cur.get(r, {('entry', r)}))
                for r in o.defs:
                    cur[r] = {(o.id, r)}
            for r in b.term_uses:
                self.use_defs[(('term', k), r)] = set(cur.get(r, {('entry', r)}))
            b.state_out = cur

    def state_before(self, k, op_index):
        """Register -> defs just before op op_index of block k (for call arguments)."""
        states = self._states_cache
        b = self.blocks[k]
        cur = {r: set(ds) for r, ds in states[k].items()}
        for o in b.ops[:op_index]:
            for r in o.defs:
                cur[r] = {(o.id, r)}
        return cur


def regs_in(x):
    if x is None:
        return []
    return [n.v for n in x.walk() if n.k == 'reg']


# ---------------------------------------------------------------------------
# Callee arguments
# ---------------------------------------------------------------------------

class Callees:
    """What a callee reads: integer and float argument registers, variadic."""

    def __init__(self, ws):
        self.ws = ws
        self.memo = {}
        self.active = set()

    def reads(self, address, depth=0):
        if address in self.memo:
            return self.memo[address]
        r = self.ws.readable.get(address)
        if r and r['signature'] and address not in RUNTIME_PORTS:
            ints = sum(1 for t in r['signature'][1] if not is_float_type(t))
            floats = sum(1 for t in r['signature'][1] if is_float_type(t))
            res = (list(range(ints)), list(range(floats)), False)
            self.memo[address] = res
            return res
        stub = stub_reads(self.ws, address)
        if stub is not None:
            self.memo[address] = stub
            return stub
        if makes_syscall(self.ws, address):
            # A kernel call's wrapper (addiu v1, N; syscall): the kernel reads
            # its arguments from the registers, which no instruction of the
            # wrapper names, so its callers pass what they set, as for the
            # host stubs (drafter25: GetSystemCallTableEntry and
            # InitTLBFunctions called SetSyscall and GetEntryAddress with no
            # arguments: 194 words of the kernel's state differed).
            res = ([], [], True)
            self.memo[address] = res
            return res
        if address in self.active or depth > 4:
            return straight_line_reads(self.ws, address)
        self.active.add(address)
        try:
            lifter = Lifter(self.ws, address, depth + 1)
            res = function_params(lifter, self, depth + 1)
        except Exception:
            res = straight_line_reads(self.ws, address)
        finally:
            self.active.discard(address)
        self.memo[address] = res
        return res


# Argument counts of the C library functions generated code hands to host
# stubs (no instructions to read them from); others count as variadic.
STUB_ARGS = {'memset': 3, 'memcpy': 3, 'memmove': 3, 'memcmp': 3, 'memchr': 3, 'strcpy': 2, 'strncpy': 3, 'strcat': 2,
             'strncat': 3, 'strcmp': 2, 'strncmp': 3, 'strlen': 1, 'strchr': 2, 'strrchr': 2, 'strstr': 2, 'strcasecmp': 2,
             'strncasecmp': 3, 'atoi': 1, 'atol': 1, 'atof': 1, 'strtol': 3, 'strtoul': 3, 'toupper': 1, 'tolower': 1,
             'abs': 1, 'rand': 0, 'srand': 1, 'malloc': 1, 'free': 1, 'calloc': 2, 'realloc': 2, 'qsort': 4,
             'sqrtf': 0, 'sinf': 0, 'cosf': 0, 'fabsf': 0, 'atan2f': 0}
STUB_FLOAT_ARGS = {'sqrtf': 1, 'sinf': 1, 'cosf': 1, 'fabsf': 1, 'atan2f': 2}


def stub_reads(ws, address):
    """(ints, floats, variadic) of a function generated code hands to a host stub, or None."""
    path = ws.generated(address)
    if not path:
        return None
    text = open(path, errors='replace').read()
    m = re.search(r'ps2_(?:stubs|syscalls)::(\w+)\(rdram, ctx, runtime\)', text)
    if not m or '// 0x' in text:
        return None
    name = m.group(1)
    if name in STUB_ARGS:
        return (list(range(STUB_ARGS[name])), list(range(STUB_FLOAT_ARGS.get(name, 0))), False)
    return ([], [], True)


def makes_syscall(ws, address):
    """Whether the function's own code makes a raw kernel call (SYSCALL)."""
    p = ws.parsed(address)
    return p is not None and any(i.op == 'syscall' for i in p.insns.values())


def is_float_type(t):
    t = t.replace('const', '').strip()
    return t in ('ee::Float', 'Float', 'float')


def straight_line_reads(ws, address):
    """readable_args.py's rule: argument registers read before written up to
    the first call or return; variadic when t0-t3 are spilled at entry."""
    p = ws.parsed(address)
    if p is None:
        return ([], [], False)
    written, read, fread, spilled = set(), set(), set(), set()
    end_after = None
    for a in sorted(p.insns):
        i = p.insns[a]
        regs = [reg_name(x) for x in re.findall(r'\$\w+', ', '.join(i.args))]
        op = i.op
        if op.startswith('s') and op not in ('sll', 'srl', 'sra', 'sllv', 'srlv', 'srav', 'slt', 'sltu', 'slti', 'sltiu', 'subu', 'sub') \
                or op.startswith('b') or op in ('jr', 'jal', 'jalr', 'j', 'mtc1', 'swc1', 'mtlo', 'mthi', 'ctc1'):
            sources, dest = regs, None
            if op == 'mtc1':
                sources, dest = regs[:1], regs[1]
        elif regs:
            dest, sources = regs[0], regs[1:]
        else:
            sources, dest = [], None
        if op in ('sw', 'sd', 'sq') and len(regs) == 2 and regs[1] == 'sp' and regs[0] in ('t0', 't1', 't2', 't3') \
                and regs[0] not in written:
            spilled.add(regs[0])
        for r in sources:
            if r in INT_ARGS and r not in written:
                read.add(INT_ARGS.index(r))
            if r in FLOAT_ARGS and r not in written:
                fread.add(FLOAT_ARGS.index(r))
        if dest:
            written.add(dest)
        if end_after is not None:
            break
        if op in ('jal', 'jalr', 'jr', 'j'):
            end_after = a
    variadic = spilled == {'t0', 't1', 't2', 't3'}
    return (sorted(read), sorted(fread), variadic)


def function_params(lifter, callees, depth=0):
    """(int arg indexes, float arg indexes, variadic) the function reads."""
    lifter.ret_regs = []
    lifter.resolve_stack()
    decide_call_args(lifter, callees, depth)
    ints, floats = set(), set()
    for (u, r), ds in lifter.use_defs.items():
        if ('entry', r) in ds:
            # (Not a read: a register the function only hands on to a call of
            # itself, which decide_args_round gives every argument register
            # for its own parameter count to trim. Counted as reads, a
            # self-calling function took all eight and eight, and its callers
            # passed their leftovers: buildMaterialRegisters 0x103A40 and
            # objectShown 0x131718, 582 of the drafts' 2,485 ee::leftByCall
            # reads; drafter24, names4's count.)
            o = lifter.op_by_id(u) if not isinstance(u, tuple) else None
            if o is not None and o.k == 'call' and o.info.target == lifter.address and \
                    o.info.kind not in ('tail', 'itail', 'jtail'):
                continue
            if r in INT_ARGS:
                ints.add(INT_ARGS.index(r))
            elif r in FLOAT_ARGS:
                floats.add(FLOAT_ARGS.index(r))
    variadic = False
    spills = set()
    for b, o in lifter.all_ops():
        if o.k == 'st' and o.x.k == 'reg' and o.x.v in ('t0', 't1', 't2', 't3') and o.addr.k == 'sp':
            if ('entry', o.x.v) in lifter.use_defs.get((o.id, o.x.v), set()):
                spills.add(o.x.v)
    if spills == {'t0', 't1', 't2', 't3'}:
        variadic = True
    # An argument register handed on unchanged from the entry to a call
    # whose callee is not known (an indirect call, a variadic callee): the
    # function passes its caller's arguments on (the heap's free wrapper
    # FUN_001380B0 calls the allocator's hook with its a0), so its callers
    # pass what they set, as for a variadic function (the sweep's
    # passthrough_args class: FUN_0015FC90 called it without the pointer).
    # Also when it reads some of them itself: FUN_0017FD00 reads a0 and
    # hands a0 and a1 on to its jalr, and FUN_001DE208 sets a1 to its frame
    # (an out pointer) before calling it; with only a0 passed, the callee
    # wrote through the caller's stale a1 (career A/B with every Tested
    # function on: what looked like the timing class). Its callers pass
    # what they set (an argument they leave as their own caller's is still
    # in the context: ee::call sets only the registers it passes).
    if not variadic and passes_entry_args_on(lifter, callees, depth):
        variadic = True
    return (sorted(ints), sorted(floats), variadic)


def passes_entry_args_on(lifter, callees, depth=0):
    """Whether an indirect call, or a call of a variadic callee, gets an
    argument register as the function's caller left it."""
    for k in lifter.block_keys():
        b = lifter.blocks[k]
        sites = [(o.info, i) for i, o in enumerate(b.ops) if o.k == 'call']
        if b.term[0] == 'tail':
            sites.append((b.term[1], len(b.ops)))
        for info, i in sites:
            if info.target is not None and info.target == lifter.address:
                continue
            if info.target is not None:
                if depth >= 6 or not callees.reads(info.target, depth)[2]:
                    continue
            state = lifter.state_before(k, i)
            for r in INT_ARGS + FLOAT_ARGS:
                if ('entry', r) in state.get(r, {('entry', r)}):
                    return True
    return False


def decide_call_args(lifter, callees, depth=0):
    """Each call's argument registers: those its callee reads, up to the last
    one this function has a value for."""
    lifter.collect_uses_defs()
    lifter.reaching()
    lifter._states_cache = lifter.reaching_states()
    sites = []
    for k in lifter.block_keys():
        b = lifter.blocks[k]
        for i, o in enumerate(b.ops):
            if o.k == 'call':
                sites.append((o.info, lifter.state_before(k, i), o.id))
        if b.term[0] == 'tail':
            st = lifter.state_before(k, len(b.ops))
            sites.append((b.term[1], st, None))
    # The registers read after a call as the call left them (ee::leftByCall).
    lifter.build_use_map()
    read_after = set(d for ds in lifter.use_defs.values() for d in ds if d[0] != 'entry')
    decide_args_round(lifter, callees, depth, sites, read_after)
    # A call that now passes a register on reads it: the call before it that
    # left it passes it too (rounds until none is added).
    for _ in range(4):
        lifter.collect_uses_defs()
        lifter.reaching()
        lifter.build_use_map()
        more = set(d for ds in lifter.use_defs.values() for d in ds if d[0] != 'entry') - read_after
        if not more:
            break
        read_after |= more
        decide_args_round(lifter, callees, depth, sites, read_after)


def decide_args_round(lifter, callees, depth, sites, read_after):
    for info, state, op_id in sites:
        if info.target is not None and info.target == lifter.address:
            # A call of itself: every argument register it has a value for
            # (its own parameter count trims them).
            ints, floats, variadic = list(range(8)), list(range(8)), False
        elif info.target is not None and depth < 6:
            ints, floats, variadic = callees.reads(info.target, depth)
        else:
            ints, floats, variadic = [], [], True

        def defined(reg):
            # Set by this function, the caller's, or left by an earlier callee
            # (the draft names that value: ee::leftByCall): the original passes
            # whatever the register holds.
            return True
        if variadic:
            ints = [i for i, r in enumerate(INT_ARGS) if written_since_call(lifter, state, r)]
            floats = [i for i, r in enumerate(FLOAT_ARGS) if written_since_call(lifter, state, r)]
        n_int = 0
        for i in ints:
            if defined(INT_ARGS[i]):
                n_int = max(n_int, i + 1)
        n_float = 0
        for i in floats:
            if defined(FLOAT_ARGS[i]):
                n_float = max(n_float, i + 1)
        # An argument register read after the call as the call left it,
        # though the callee does not read it (fn_128A98 sets a3 before
        # 0x135768, which ignores it, and memcpy; 0x103230 reads it after
        # both): passed all the same, so the register the call leaves is the
        # original's value (ee::leftByCall reads what the callee returned
        # with; not passed, the call left the readable caller's own). A later
        # call that passes it on makes the earlier ones pass it too (the
        # rounds in decide_call_args).
        # Only a register this function's own code set on every path to the
        # call (not the caller's value, which would become a parameter, nor
        # a callee's leftover on some path: those gave wrong values, 8
        # functions of the corpus); a chain through calls that leave it is
        # left to zx-fncheck, which now sees it (fn_128A98).
        if op_id is not None:
            def set_here(r):
                return written_since_call(lifter, state, r)
            for i, r in enumerate(INT_ARGS):
                if i >= n_int and (op_id, r) in read_after and set_here(r):
                    n_int = i + 1
            for i, r in enumerate(FLOAT_ARGS):
                if i >= n_float and (op_id, r) in read_after and set_here(r):
                    n_float = i + 1
        info.int_args = INT_ARGS[:n_int]
        info.float_args = FLOAT_ARGS[:n_float]
        # A register whose upper half this function set (LQ, QMFC2) goes as
        # a whole quadword (ee::Qword; recordings of version 5 keep it), and
        # so does one a readable callee takes as a quadword, its upper half
        # whatever the original leaves there (cameraOwnFrame's t0 at
        # updateAnimation: a callee's or the entry's, never 0).
        whole = callee_qword_args(lifter.ws, info.target)
        info.qword_args = [r for n, r in enumerate(info.int_args) if n in whole or written_since_call(lifter, state, r + '^')]


# The registers a call leaves that a draft can name (ee::leftByCall): the
# caller-saved GPRs (whole, and their upper halves) and FPRs.
LEFT_REGS = [r for r in GPR if r not in CALLEE_SAVED and r not in ('zero', 'k0', 'k1')] + \
    [r + '^' for r in GPR if r not in CALLEE_SAVED and r not in ('zero', 'k0', 'k1')] + ['f%d' % i for i in range(20)]


def lifter_def_is_clobber(lifter, d):
    op = lifter.op_by_id(d[0])
    return op is not None and op.k == 'call'


def callee_qword_args(ws, target):
    """The integer arguments a readable callee's signature takes as ee::Qword."""
    readable = ws.readable.get(target) if target is not None else None
    signature = readable.get('signature') if readable else None
    if not signature:
        return ()
    ints = [t for t in signature[1] if not is_float_type(t)]
    return {n for n, t in enumerate(ints) if t.strip() in ('ee::Qword', 'Qword')}


def written_since_call(lifter, state, reg):
    ds = state.get(reg, {('entry', reg)})
    if any(d[0] == 'entry' for d in ds):
        return False
    return all(not lifter_def_is_clobber(lifter, d) for d in ds)


def _op_by_id(self, i):
    if not hasattr(self, '_ops_by_id') or self._ops_by_id_n != len(self._ops_list()):
        self._ops_by_id = {o.id: o for _, o in self.all_ops()}
        self._ops_by_id_n = len(self._ops_by_id)
    return self._ops_by_id.get(i)


def _ops_list(self):
    return [o for _, o in self.all_ops()]


Lifter.op_by_id = _op_by_id
Lifter._ops_list = _ops_list


# ---------------------------------------------------------------------------
# Statements
# ---------------------------------------------------------------------------

def term_attrs(b):
    """The expressions of a block's terminator: its condition, the result and
    the registers it leaves at a return (leave_<reg>), a tail call."""
    return ('cond', 'retx', 'tailx') + tuple(getattr(b, 'leave_attrs', ()))


class S:
    """A statement. k: assign (web, x) | expr (x) | store (t, addr, x) | raw (text) | note |
    if (x, then, else) | loop (body) | goto | label | ret (x) | brk | cont | charge | switch."""
    __slots__ = ('k', 'web', 'x', 'addr', 't', 'body', 'orelse', 'text', 'target', 'cases', 'seq', 'scope', 'm',
                 'leaves', 'jr')

    def __init__(self, k, **kw):
        self.k = k
        self.web = kw.get('web')
        self.x = kw.get('x')
        self.addr = kw.get('addr')
        self.t = kw.get('t')
        self.body = kw.get('body')
        self.orelse = kw.get('orelse')
        self.text = kw.get('text')
        self.target = kw.get('target')
        self.cases = kw.get('cases')
        self.m = kw.get('m')   # a store's access kind (Op.m)
        self.leaves = kw.get('leaves')   # ret: [(register, x)] it leaves besides its result (ee/leave.h)
        self.jr = kw.get('jr')   # switch: the block (start) that ends with the jump table's JR
        self.seq = None
        self.scope = None

    def charged_back(self):
        return False

    def exprs(self):
        out = []
        for e in (self.x, self.addr):
            if e is not None:
                out.append(e)
        return out


class Web:
    def __init__(self, wid, reg):
        self.id = wid
        self.reg = reg
        self.defs = set()
        self.uses = 0
        self.name = None
        self.t = 'i32'
        self.param = None     # param index when the web holds an argument at entry
        self.entry = False    # holds the register's value at entry
        self.const = None
        self.alias = None
        self.pointer = False


class Draft:
    """Builds the C++ draft of one function."""

    def __init__(self, ws, address, callees, name=None, status='Draft', ghidra=True, io_dynamic=False):
        self.ws = ws
        self.address = address
        # Memory through pointers may be I/O registers (a DMA channel's): each
        # such access decided at run time (ee::io::loadAny / storeAny).
        self.io_dynamic = io_dynamic
        self.uses_io = False
        self.callees = callees
        self.status = status
        self.ghidra = ghidra
        self.lifter = Lifter(ws, address)
        reg = ws.readable.get(address)
        self.name = name or (reg['name'] if reg and reg['kind'] != 'PLANNED' and reg['name'] else None) or \
            (reg['name'] if reg and re.match(r'^[A-Za-z_]\w*$', reg['name'] or '') else None) or \
            ws.name_of(address) or ('fn_%06X' % address)
        if not re.match(r'^[A-Za-z_]\w*$', self.name):
            self.name = 'fn_%06X' % address
        self.constants = {}   # address -> (name, comment)
        self.globals = {}     # address -> (name, type, comment)
        self.includes = set()
        self.decls = set()
        self.todo = []
        self.stats = defaultdict(int)

    # ---- signature ----------------------------------------------------------

    def web_constant(self, w):
        """The value every definition of web w gives it, when they are all one
        constant (not a parameter); else None."""
        if not hasattr(self, '_web_constants'):
            self._web_constants = {}
        if id(w) in self._web_constants:
            return self._web_constants[id(w)]
        value = None
        ok = bool(w.defs)
        for d in w.defs:
            o = self.ops_by_id.get(d[0]) if d[0] != 'entry' else None
            if o is None or o.k != 'set' or o.x is None:
                ok = False
                break
            x = simplify(o.x.copy())
            if x.k != 'c' or (value is not None and x.v != value):
                ok = False
                break
            value = x.v
        self._web_constants[id(w)] = value if ok else None
        return self._web_constants[id(w)]

    def constant_webs_in(self, x):
        """x with the variables of constant webs (web_constant) as constants."""
        x = x.copy()

        def sub(n):
            if n.k == 'var':
                v = self.web_constant(n.v)
                if v is not None:
                    return C(v, n.t if n.t in CTYPE else 'i32')
                return n
            n.a = [sub(c) for c in n.a] if isinstance(n.a, list) else n.a
            return n
        return sub(x)

    def reg_at_return(self, reg):
        """What reaches `reg` at the function's returns: a set of 'own' (an
        assignment of the function's), 'call' (what a callee left), 'entry'
        (the caller's) and 'tail' (a tail call's)."""
        L = self.lifter
        L.ret_regs = [reg]
        L.collect_uses_defs()
        L.reaching()
        L._states_cache = L.reaching_states()
        kinds = set()
        for k in L.block_keys():
            b = L.blocks[k]
            if b.term[0] == 'tail':
                kinds.add('tail')
                continue
            if b.term[0] != 'ret':
                continue
            for d in L.state_before(k, len(b.ops)).get(reg, {('entry', reg)}):
                if d[0] == 'entry':
                    kinds.add('entry')
                elif lifter_def_is_clobber(L, d):
                    kinds.add('call')
                else:
                    kinds.add('own')
        L.ret_regs = []
        return kinds

    def returns_float_unlike_ghidra(self, ret):
        """Whether a function Ghidra does not declare float returns a float in
        f0 all the same: f0 at a return is set by the function itself and a
        caller reads it after the call (FUN_001DB2B8's lwc1 f0, 0x14(s0), which
        FUN_001DAD50 compares; fabsf's mtc1 at 0x202648). Not when v0 is the
        function's own result too and a caller reads that (one register only)."""
        L = self.lifter
        # Also a callee's f0 handed on (FUN_00136DA0: tanf of an angle, Ghidra
        # says void; FUN_00101680 and camera.cpp divide by the f0 it returns,
        # and with only v0 returned they got the caller's f0 back).
        f0_kinds = self.reg_at_return('f0')
        if 'own' in f0_kinds:
            if not any(o.k == 'set' and o.dst == 'f0' for _, o in L.all_ops()):
                return False
        elif not f0_kinds & {'call', 'tail'}:
            return False
        if not callers_read_after_call(self.ws, self.address, 'f0'):
            return False
        v0_kinds = {'own'} if 'own' in f0_kinds else {'own', 'call'}
        if ret == 'v0' and self.reg_at_return('v0') & v0_kinds and \
                callers_read_after_call(self.ws, self.address, 'v0'):
            self.todo.append('f0 and v0 at its return are both its own and callers read both: only v0 returned')
            return False
        return True

    def find_leaves(self, ret):
        """The registers besides the result that some caller reads after the
        call (fncheck_traced_gen's result readers over recomp/generated: v1,
        a0-a3, t0-t9, at, f1-f19) and that the function sets on a path to a
        return (its own assignment or a callee's): the draft leaves them as the
        original does (ee::leave before each return)."""
        gprs, fprs = self.ws.result_readers().get(self.address, (0, 0))
        regs = [GPR[r] for r in range(32) if gprs >> r & 1 and GPR[r] not in ('v0', ret)]
        regs += ['f%d' % r for r in range(1, 20) if fprs >> r & 1 and 'f%d' % r != ret]
        out = []
        for reg in regs:
            if self.reg_at_return(reg) & {'own', 'call'}:
                out.append(reg)
        if out:
            self.todo.append('callers read %s after the call: left as the original leaves %s (ee::leave)' %
                             (', '.join(out), 'it' if len(out) == 1 else 'them'))
        return out

    def v0_set_at_return(self):
        """Whether v0 at a return is set by the function (a call or an
        assignment) on some path, rather than the caller's."""
        L = self.lifter
        L.ret_regs = ['v0']
        L.collect_uses_defs()
        L.reaching()
        L._states_cache = L.reaching_states()
        for k in L.block_keys():
            b = L.blocks[k]
            if b.term[0] == 'tail':
                L.ret_regs = []
                return True   # the tail callee's v0 is the function's
            if b.term[0] != 'ret':
                continue
            ds = L.state_before(k, len(b.ops)).get('v0', {('entry', 'v0')})
            if any(d[0] != 'entry' for d in ds):
                L.ret_regs = []
                return True
        L.ret_regs = []
        return False

    def ghidra_signature(self):
        d = self.ws.decomp(self.address)
        if not d:
            return None
        text = d[1]
        m = re.search(r'^(.*?)\b(\w+)\s*\(([^)]*)\)\s*\n\s*\{', text, re.S | re.M)
        if not m:
            return None
        ret = m.group(1).strip().split('\n')[-1].strip()
        params = []
        plist = m.group(3).strip()
        if plist and plist != 'void':
            for p in plist.split(','):
                p = p.strip()
                mm = re.match(r'^(.*?)(\w+)$', p)
                if mm:
                    params.append((mm.group(1).strip(), mm.group(2)))
        return ret, params

    def build(self):
        L = self.lifter
        L.ret_regs = []
        g = self.ghidra_signature()
        self.ghidra_sig = g
        # Pass 1: arguments, to find the parameters.
        L.resolve_stack()
        ints, floats, self.qword_params = function_params_from(L, self.callees)
        gi = [p for p in (g[1] if g else []) if not ('float' in p[0] and '*' not in p[0])]
        gf = [p for p in (g[1] if g else []) if 'float' in p[0] and '*' not in p[0]]
        n_int = max([i + 1 for i in ints] + [len(gi)] + [0])
        n_float = max([i + 1 for i in floats] + [len(gf)] + [0])
        n_int = min(n_int, 8)
        n_float = min(n_float, 8)
        self.int_params = INT_ARGS[:n_int]
        self.float_params = FLOAT_ARGS[:n_float]
        self.used_params = set(INT_ARGS[i] for i in ints) | set(FLOAT_ARGS[i] for i in floats)
        self.param_names = {}
        for i, r in enumerate(self.int_params):
            self.param_names[r] = gi[i][1] if i < len(gi) else 'arg%d' % i
        for i, r in enumerate(self.float_params):
            self.param_names[r] = gf[i][1] if i < len(gf) else 'farg%d' % i
        # The result.
        ret = None
        v0_set = any(o.k == 'set' and o.dst == 'v0' or o.k == 'call' for _, o in L.all_ops())
        f0_set = any(o.k == 'set' and o.dst == 'f0' or o.k == 'call' for _, o in L.all_ops())
        gret = g[0] if g else None
        if gret is not None:
            if gret == 'void':
                ret = None
                # Ghidra says void, but the code leaves a value in v0 at a
                # return (a callee's result handed on: fn_138080's jalr, the
                # heap's realloc): the callers read it, so the draft returns it
                # (zx-fncheck does not compare the v0 of a void function).
                if self.v0_set_at_return():
                    ret = 'v0'
                    self.todo.append('Ghidra declares it void, but v0 at its return is set (a callee\'s result): '
                                     'returned')
            elif 'float' in gret and '*' not in gret:
                ret = 'f0'
            else:
                ret = 'v0'
        else:
            ret = 'v0' if v0_set else None
        # Ghidra types some float results as integers (undefined4): the f0 the
        # function sets is what its callers read (the sweep's int_f0 class;
        # zx-fncheck compares only v0 of an integer function, so the check
        # passes and the caller fails, as FUN_001DAD50 called a null callback
        # with FUN_001DB2B8's f0 stale).
        if ret != 'f0' and self.returns_float_unlike_ghidra(ret):
            self.todo.append('Ghidra declares it %s, but it sets f0 at its return and callers read it: returned '
                             'as a float' % (gret or 'without a type'))
            ret = 'f0'
        if ret == 'v0':
            why = v0_unread(self.ws, self.address)
            if why:
                self.todo.append('returns void: ' + why)
                ret = None
        tails = [L.blocks[k].term[1] for k in L.block_keys() if L.blocks[k].term[0] == 'tail']
        self.ret = ret
        self.leaves = self.find_leaves(ret)
        L.ret_regs = ([ret] if ret else []) + self.leaves
        # Pass 2: the final dataflow.
        decide_call_args(L, self.callees)
        self.find_stack_slots()
        if not self.lifter.frame_mode and self.slot_regs:
            L.collect_uses_defs()
            L.reaching()
            L.build_use_map()
        self.add_saved_uses()
        self.make_webs()
        self.make_statements()
        self.widen()
        self.fold()
        self.structure()
        # A 64-bit result: the v0 at a return is a 64-bit value (rider3's spawn
        # functions return a whole FaceRef, which resetPhysics stores with sd):
        # returned as uint64_t, not cut to its low word (zx-fncheck then
        # compares all of v0, as for any 64-bit result).
        self.ret_wide = ret == 'v0' and any(
            L.blocks[k].term[0] == 'ret' and getattr(L.blocks[k], 'retx', None) is not None and
            self._wide(L.blocks[k].retx) for k in L.block_keys())
        return self

    def add_saved_uses(self):
        """At each call, the callee-saved registers whose value there is not
        the entry value (a def of the function's reaches it): the original
        has them in the registers its callee saves, the draft in its locals,
        so the call publishes them (ee::calleeSaves; drafter21). They become
        uses of the call, so their values stay in variables."""
        L = self.lifter
        # The analysis renumbers the ops and redoes their uses and defs: the
        # state the webs are made from is kept and put back when no call
        # publishes anything, else redone whole (a stale use map with new
        # op ids mixes two numberings: fn_1175A0 lost its constants).
        ops = [o for _, o in L.all_ops()]
        kept = ([(o.id, o.uses, o.defs) for o in ops], [(k, L.blocks[k].term_uses) for k in L.block_keys()],
                getattr(L, 'inn', None), getattr(L, 'out', None), getattr(L, '_states_cache', None))
        L.collect_uses_defs()
        L.reaching()
        L._states_cache = L.reaching_states()
        any_saved = False
        # The PASS_MODEL registers the function reads as its caller left them.
        entry_reads = set()
        for k in L.block_keys():
            b = L.blocks[k]
            for i, o in enumerate(b.ops):
                wanted = [r for r in o.uses if r in PASS_MODEL]
                if wanted:
                    st = L.state_before(k, i)
                    entry_reads |= set(r for r in wanted if ('entry', r) in L.defs_at(st, r))
            wanted = [r for r in b.term_uses if r in PASS_MODEL]
            if wanted:
                st = L.state_before(k, len(b.ops))
                entry_reads |= set(r for r in wanted if ('entry', r) in L.defs_at(st, r))
        for k in L.block_keys():
            b = L.blocks[k]
            for i, o in enumerate(b.ops):
                if o.k != 'call':
                    continue
                if entry_reads and o.info.target == L.address and o.info.kind not in ('tail', 'itail', 'jtail'):
                    o.passed = sorted(entry_reads)
                    any_saved = True
                st = L.state_before(k, i)
                o.saved = [r for r in SAVED_MODEL if L.defs_at(st, r) != {('entry', r)}]
                if getattr(o, 'fp_sp', None) is not None and 'fp' not in o.saved:
                    o.saved.append('fp')   # the frame pointer (resolve_stack)
                any_saved = any_saved or bool(o.saved)
        if any_saved:
            L.collect_uses_defs()
            L.reaching()
            L.build_use_map()
            return
        for o, (i, u, d) in zip(ops, kept[0]):
            o.id, o.uses, o.defs = i, u, d
        for k, tu in kept[1]:
            L.blocks[k].term_uses = tu
        L.inn, L.out, L._states_cache = kept[2], kept[3], kept[4]

    # ---- the stack frame ---------------------------------------------------------

    def find_stack_slots(self):
        """Drop register saves and restores; locals become registers
        ('stk_<off>') unless one's address escapes (then ee::StackFrame)."""
        L = self.lifter
        size = L.frame_size
        saves = {}
        # (frame offset, register, bytes): the saves the draft writes back
        # with StackFrame::saveEntry (the dead stack the original leaves).
        self.entry_saves = []
        escapes = False
        accesses = defaultdict(set)
        for b, o in L.all_ops():
            if o.k in ('st',) and o.addr.k == 'sp':
                src = o.x
                reg = src.v if src.k == 'reg' else None
                if reg and (reg in CALLEE_SAVED or reg.rstrip('^') in CALLEE_SAVED) and \
                        ('entry', reg) in L.use_defs.get((o.id, reg), set()) and \
                        len(L.use_defs.get((o.id, reg), set())) == 1:
                    saves[o.addr.v] = reg
                    if o.t in ('i32', 'u32', 'i64', 'u64', 'f'):
                        self.entry_saves.append((o.addr.v + size, reg.rstrip('^'), type_size(o.t)))
                    o.k = 'dead'
                    continue
            for x in (o.x, o.addr):
                if x is None:
                    continue
                for n in x.walk():
                    pass
        # Restores: loads of a save slot into its register.
        for b, o in L.all_ops():
            if o.k == 'set' and o.x is not None and o.x.k == 'ld' and o.x.a[0].k == 'sp' and \
                    saves.get(o.x.a[0].v) == o.dst:
                o.k = 'dead'
        for b, o in L.all_ops():
            if o.k == 'dead':
                continue
            exprs = []
            if o.k in ('st', 'stu'):
                if o.addr.k == 'sp':
                    accesses[o.addr.v].add(o.t)
                else:
                    exprs.append(o.addr)
                exprs.append(o.x)
            elif o.k == 'set':
                exprs.append(o.x)
            elif o.k in ('vld', 'vst', 'vset'):
                exprs += [o.addr]
                if o.x is not None:
                    exprs.append(o.x)
                if o.k in ('vld', 'vst') and o.addr.k == 'sp':
                    escapes = True
            elif o.k == 'call':
                if o.info.target_x is not None:
                    exprs.append(o.info.target_x)
            for x in exprs:
                for n, parent in walk_parents(x):
                    if n.k == 'sp':
                        if parent is not None and parent.k in ('ld',) and parent.a[0] is n:
                            accesses[n.v].add(parent.v)
                        else:
                            escapes = True
        for k in L.block_keys():
            b = L.blocks[k]
            t = b.term
            if t[0] in ('cond', 'switch'):
                if any(n.k == 'sp' for n in t[1].walk()):
                    escapes = True
        for b, o in L.all_ops():
            for x in (o.x, o.addr):
                if x is not None and any(n.k == 'sp' and n.v >= 0 for n in x.walk()):
                    self.todo.append('reads the caller\'s stack (arguments past a7?) at 0x%x' % o.pc)
        L.ops_removed = True
        for k in L.block_keys():
            b = L.blocks[k]
            b.ops = [o for o in b.ops if o.k != 'dead']
        self.slot_regs = {}
        # Overlapping slots or a slot of two sizes: the frame.
        spans = []
        for off, ts in accesses.items():
            sizes = {type_size(t) for t in ts}
            if len(sizes) > 1:
                escapes = True
            spans.append((off, max(sizes)))
        spans.sort()
        for (o1, s1), (o2, s2) in zip(spans, spans[1:]):
            if o1 + s1 > o2:
                escapes = True
        L.frame_mode = escapes and size > 0
        if L.frame_mode:
            self.stats['frame'] += 1
            return
        # Locals as registers.
        for off, ts in accesses.items():
            name = 'stk_%x' % (off + size)
            self.slot_regs[off] = (name, sorted(ts)[0])
        for b, o in L.all_ops():
            if o.k == 'st' and o.addr.k == 'sp' and o.addr.v in self.slot_regs:
                name, t = self.slot_regs[o.addr.v]
                o.k = 'set'
                o.dst = name
                o.x = X('slotval', [o.x], t, slot_type(t))
                o.addr = None
            for attr in ('x',):
                x = getattr(o, attr)
                if x is not None:
                    setattr(o, attr, self._slot_loads(x))
        for k in L.block_keys():
            b = L.blocks[k]
            t = b.term
            if t[0] == 'cond':
                b.term = ('cond', self._slot_loads(t[1])) + t[2:]

    def _slot_loads(self, x):
        if x.k == 'ld' and x.a[0].k == 'sp' and x.a[0].v in self.slot_regs:
            name, t = self.slot_regs[x.a[0].v]
            r = X('reg', v=name, t=slot_type(t))
            if x.v != t:
                return X('cast', [r], x.v, x.t)
            return r
        x.a = [self._slot_loads(c) for c in x.a]
        return x

    # ---- webs -----------------------------------------------------------------

    def make_webs(self):
        L = self.lifter
        parent = {}

        def find(d):
            while parent.get(d, d) != d:
                parent[d] = parent.get(parent[d], parent[d])
                d = parent[d]
            return d

        def union(a, b):
            ra, rb = find(a), find(b)
            if ra != rb:
                parent[ra] = rb
        for key, ds in L.use_defs.items():
            ds = list(ds)
            for d in ds:
                parent.setdefault(d, d)
            for d in ds[1:]:
                union(ds[0], d)
        webs = {}
        self.def_web = {}
        for key, ds in L.use_defs.items():
            root = find(next(iter(ds)))
            w = webs.get(root)
            if w is None:
                w = webs[root] = Web(len(webs), key[1])
            w.defs |= ds
            w.uses += 1
        for root, w in webs.items():
            for d in w.defs:
                self.def_web[d] = w
        self.webs = list(webs.values())
        ops = {o.id: o for _, o in L.all_ops()}
        self.ops_by_id = ops
        for w in self.webs:
            if any(d[0] == 'entry' for d in w.defs):
                w.entry = True
                r = w.reg
                if r in self.int_params or r in self.float_params or (r.endswith('^') and r[:-1] in self.qword_params):
                    w.param = r
            real = [d for d in w.defs if d[0] != 'entry']
            w.t = self.web_type(w, [ops[d[0]] for d in real if d[0] in ops])
            if w.param and (w.param in self.qword_params or w.param.endswith('^')):
                w.t = 'u64'

    def web_type(self, w, def_ops):
        r = w.reg
        if r.startswith('f') and r[1:].isdigit() or r == 'facc':
            return 'f'
        if r == 'fcc' or r.startswith('cond_'):
            return 'bool'
        if r.endswith('^'):
            return 'u64'
        if r.startswith('stk_'):
            for o in def_ops:
                if o.x is not None:
                    return o.x.t
            return 'i32'
        for o in def_ops:
            if o.k == 'set' and o.x is not None and o.x.t in ('u64', 'i64'):
                if not (o.x.k == 'ld' and o.x.v == 'u64' and False):
                    return 'u64'
            if o.k == 'set' and o.x is not None and o.x.k == 'bin' and o.x.t == '64':
                return 'u64'
            # A callee's v0 that is a 64-bit value (0x119460's ld of its clock:
            # 0x1194D8 hands all of it to __udivdi3; cut to 32 bits the time
            # went wrong once the clock passed 2**31).
            if o.k == 'call' and r == 'v0' and o.info.target is not None and \
                    returns_wide(self.ws, o.info.target):
                return 'u64'
        return 'i32'

    def web_of_use(self, key, reg):
        ds = self.lifter.use_defs.get((key, reg))
        if not ds:
            return None
        return self.def_web.get(next(iter(ds)))

    def web_of_def(self, op_id, reg):
        return self.def_web.get((op_id, reg))

    # ---- statements -------------------------------------------------------------

    def subst(self, x, key):
        """Register leaves to webs (variables) at a use."""
        if x is None:
            return None
        if x.k == 'reg':
            w = self.web_of_use(key, x.v)
            if w is None:
                return X('undef', v=x.v, t=x.t)
            if w.reg == 'gp' and w.entry and len(w.defs) == 1:
                return C(GP, 'u32')
            if w.entry and not w.param and len(w.defs) == 1:
                return X('undef', v=x.v, t=w.t)
            return X('var', v=w, t=w.t)
        x = X(x.k, [self.subst(c, key) for c in x.a], x.v, x.t, x.m)
        return simplify(x)

    def make_statements(self):
        L = self.lifter
        for k in L.block_keys():
            b = L.blocks[k]
            stmts = []
            for o in b.ops:
                key = o.id
                if o.k == 'set':
                    w = self.web_of_def(o.id, o.dst)
                    x = self.subst(o.x, key)
                    if w is None or w.uses == 0:
                        if has_effects(x):
                            stmts.append(S('expr', x=x))
                        continue
                    stmts.append(S('assign', web=w, x=x))
                elif o.k in ('st', 'stu'):
                    stmts.append(S('store' if o.k == 'st' else 'storeu', t=o.t, addr=self.subst(o.addr, key), x=self.subst(o.x, key), m=o.m))
                elif o.k == 'call':
                    stmts.extend(self.call_statements(o.info, key, o.id))
                elif o.k == 'vld':
                    stmts.append(S('vld', t=o.t, addr=self.subst(o.addr, key)))
                elif o.k == 'vst':
                    stmts.append(S('vst', t=o.t, addr=self.subst(o.addr, key)))
                elif o.k == 'vset':
                    stmts.append(S('vset', t=o.t, x=self.subst(o.x, key), addr=self.subst(o.addr, key)))
                elif o.k == 'vraw':
                    text = o.x.v
                    subs = {}
                    for r in o.info:
                        subs[r] = self.subst(R(r), key)
                    stmts.append(S('vraw', text=text, x=X('raw', subs.values(), v=subs), t=o.note))
                elif o.k == 'note':
                    stmts.append(S('note', text=o.note))
                elif o.k == 'unsup':
                    stmts.append(S('unsup', text=o.note))
                    self.todo.append('not lifted: ' + o.note + ' at 0x%x' % o.pc)
            b.stmts = stmts
            t = b.term
            tk = ('term', k)
            if t[0] == 'cond':
                b.cond = self.subst(t[1], tk)
            elif t[0] == 'switch':
                b.cond = self.subst(t[1], tk)
            elif t[0] == 'ret':
                b.retx = self.subst(R(self.ret), tk) if self.ret else None
                b.leave_attrs = tuple('leave_' + r for r in self.leaves)
                for r in self.leaves:
                    setattr(b, 'leave_' + r, self.subst(R(r), tk))
            elif t[0] == 'tail':
                b.tailx = self.call_expr(t[1], tk)

    def call_expr(self, info, key):
        args = []
        if info.target is not None and info.target == self.address:
            # A call of itself takes its own parameters.
            info.int_args = [r for r in info.int_args if r in self.int_params]
            info.float_args = [r for r in info.float_args if r in self.float_params]
            info.qword_args = [r for r in info.qword_args if r in info.int_args]
        for r in info.int_args:
            lo = self.subst(R(r), key)
            if r in info.qword_args:
                lo = X('qword', [lo, self.subst(R(r + '^'), key)], t='q')
            args.append(lo)
        args += [self.subst(R(r), key) for r in info.float_args]
        target = self.subst(info.target_x, key) if info.target_x is not None else None
        return X('call', [target] + args if target is not None else args, info, 'i32')

    def saves_statement(self, info, key, op_id):
        """ee::calleeSaves before a call: the published registers (add_saved_uses)
        and ra, the call's return address (the jal's + 8): a readable callee
        called directly or a self-call writes it in its register saves, and a
        guest callee gets it in ra and returns through it, so its own saves
        and the context after the call hold the original's (drafter25: a
        guest call had the runtime's resume slot there, 0x10000C in three
        stack words of paths6's movie and card functions)."""
        o = self.ops_by_id.get(op_id)
        regs = getattr(o, 'saved', None) or []
        vals, idx = [], []
        for r in regs:
            if r == 'fp' and getattr(o, 'fp_sp', None) is not None:
                v = X('sp', v=o.fp_sp, t='u32')   # fp = sp: the frame's address
            else:
                v = self.subst(R(r), key)
            if v.k == 'undef':
                continue
            vals.append(v)
            idx.append(32 + int(r[1:]) if r.startswith('f') and r[1:].isdigit() else GPR.index(GPR_ALIASES.get(r, r)))
        # (jal and jalr link ra; a jalr to another register links that one)
        links_ra = info.kind == 'call' or (info.kind == 'icall' and getattr(info, 'link', 'ra') == 'ra')
        if info.pc and links_ra:
            vals.append(C(info.pc + 8, 'u32'))
            idx.append(31)
        if not vals:
            return []
        self.stats['callee_saves'] += 1
        return [S('expr', x=X('saves', vals, idx, 'void', m=(info.pc, info.target)))]

    def pass_statement(self, info, key, op_id):
        """ee::passRegisters before a self-call: the caller-saved registers the
        function reads as its caller left them (add_saved_uses), with their
        values here, for the inner call's ee::callerRegister."""
        o = self.ops_by_id.get(op_id)
        vals, idx = [], []
        for r in getattr(o, 'passed', None) or ():
            v = self.subst(R(r), key)
            if v.k == 'undef':
                continue        # (still the caller's: the context has it)
            vals.append(v)
            idx.append(GPR.index(r))
        return [S('expr', x=X('pass', vals, idx, 'void'))] if vals else []

    def call_statements(self, info, key, op_id):
        pre = self.saves_statement(info, key, op_id) + self.pass_statement(info, key, op_id)
        return pre + self.call_statements_only(info, key, op_id)

    def call_statements_only(self, info, key, op_id):
        x = self.call_expr(info, key)
        wv = self.web_of_def(op_id, 'v0')
        wf = self.web_of_def(op_id, 'f0')
        if wv is not None and wv.uses:
            info.result = 'v0'
            info.ret_t = wv.t
            out = [S('assign', web=wv, x=x)]
        elif wf is not None and wf.uses:
            info.result = 'f0'
            info.ret_t = 'f'
            out = [S('assign', web=wf, x=x)]
        else:
            out = [S('expr', x=x)]
        # Caller-saved registers the code reads after the call without
        # setting them: the values the callee left (ee::leftByCall).
        for r in LEFT_REGS:
            if r == info.result:
                continue
            w = self.web_of_def(op_id, r)
            if w is None or not w.uses or w is wv and info.result == 'v0' or w is wf and info.result == 'f0':
                continue
            out.append(S('assign', web=w, x=X('left', v=r, t=w.t)))
            self.stats['left'] += 1
        return out

    def widen(self):
        """A value computed from a 64-bit one by a 64-bit operation (and, or,
        xor, nor, daddu ...) is 64-bit: its variable too."""
        L = self.lifter

        def wide(x):
            if x.k == 'var':
                return x.v.t in ('u64', 'i64')
            if x.k in ('ld',):
                return x.v in ('u64', 'i64')
            if x.k == 'bin':
                if x.t == '64':
                    return True
                if x.v in ('&', '|', '^'):   # 64-bit on the EE; ADDU / SUBU are 32-bit
                    return any(wide(c) for c in x.a)
                return False
            if x.k in ('bnot', 'sel', 'neg'):
                return any(wide(c) for c in (x.a if x.k != 'sel' else x.a[1:]))
            if x.k in ('vflo', 'vfhi', 'qword', 'qlow', 'qhigh', 'add64'):
                return True
            return False
        changed = True
        while changed:
            changed = False
            for k in L.block_keys():
                for s in L.blocks[k].stmts:
                    if s.k == 'assign' and s.web.t in ('i32', 'u32') and not s.web.param and wide(s.x):
                        s.web.t = 'u64'
                        changed = True
        self._wide = wide
        # A parameter a 64-bit operation reads is a 64-bit argument.
        self.wide_params = set()

        def visit(x):
            if x.k == 'bin' and x.t == '64' or x.k == 'qword':
                for c in x.a:
                    if c.k == 'var' and c.v.param and not c.v.param.endswith('^') and c.v.param in self.int_params \
                            and c.v.param not in self.qword_params:
                        self.wide_params.add(c.v.param)
            for c in x.a:
                visit(c)
        # Through copies too: a parameter moved to another register (daddu
        # s0, a1, zero) and read there by a 64-bit operation, or the copy a
        # 64-bit variable (drafter25: _d2b 0x22B150's double in a1, moved to
        # s0 and split with dsra32 there; typed 32-bit, its upper half was
        # lost, the exponent read 0 and s0 published 0xFFFF..FFFC for the
        # original's 0x7FEF..FFFC).
        wide_used = set()   # id() of the webs a 64-bit operation reads
        copies = []         # (destination web, source web)

        def visit_used(x):
            if x.k == 'bin' and x.t == '64' or x.k == 'qword':
                for c in x.a:
                    if c.k == 'var':
                        wide_used.add(id(c.v))
            for c in x.a:
                visit_used(c)
        for k in L.block_keys():
            b = L.blocks[k]
            for st in b.stmts:
                for e in st.exprs():
                    visit(e)
                    visit_used(e)
                if st.k == 'store' and st.t == 'u64' and st.x.k == 'var':
                    wide_used.add(id(st.x.v))
                    if st.x.v.param in self.int_params:
                        self.wide_params.add(st.x.v.param)
                if st.k == 'assign' and st.x.k == 'var' and st.x.v is not st.web:
                    copies.append((st.web, st.x.v))
            for attr in term_attrs(b):
                e = getattr(b, attr, None)
                if e is not None:
                    visit(e)
                    visit_used(e)
        changed = True
        while changed:
            changed = False
            for dst, src in copies:
                if (id(dst) in wide_used or dst.t in ('u64', 'i64')) and id(src) not in wide_used:
                    wide_used.add(id(src))
                    changed = True
        for w in self.webs:
            if id(w) in wide_used and w.param and not w.param.endswith('^') and w.param in self.int_params \
                    and w.param not in self.qword_params:
                self.wide_params.add(w.param)
        # A value a callee left, read whole (cameraOwnFrame's t0 at updateAnimation:
        # a quadword's low half is 64 bits, its words y and x).
        for k in L.block_keys():
            for st in L.blocks[k].stmts:
                if st.k == 'assign' and st.x.k == 'left' and id(st.web) in wide_used and st.web.t in ('i32', 'u32'):
                    st.web.t = st.x.t = 'u64'
        # An indirect call's v0 a 64-bit operation reads is a 64-bit result, the
        # callee being unknown (quar1: __svfscanf's strtol / strtoul through its
        # function pointer return a long, stored with sd on %ld; typed 32-bit
        # the upper word went and 0x1CBE991A14 was stored as 0xFFFFFFFFBE991A14).
        for w in self.webs:
            if id(w) in wide_used and w.reg == 'v0' and w.t in ('i32', 'u32'):
                calls = [self.ops_by_id.get(d[0]) for d in w.defs if d[0] != 'entry']
                if calls and all(o is not None and o.k == 'call' and o.info.kind == 'icall' for o in calls):
                    w.t = 'u64'
                    for o in calls:
                        o.info.ret_t = 'u64'
        # A parameter Ghidra types as a double or a long long (its library
        # signature, not undefined8, which any 64-bit move gives) is 64-bit
        # too: the
        # soft-float routines take their doubles in a0/a1 and hand them on
        # unchanged (drafter25: __fixunsdfdi, __fixdfdi and _ratio passed
        # dpcmp/dpdiv the low word of a double).
        g = getattr(self, 'ghidra_sig', None)
        if g:
            gi = [t for t, _ in g[1] if not ('float' in t and '*' not in t)]
            for i, t in enumerate(gi):
                if i < len(self.int_params) and '*' not in t and \
                        re.sub(r'\bconst\b', '', t).strip() in ('double', 'longlong', 'ulonglong'):
                    r = self.int_params[i]
                    if r not in self.qword_params:
                        self.wide_params.add(r)
        widened = False
        for w in self.webs:
            if w.param in self.wide_params and w.t != 'u64':
                w.t = 'u64'
                widened = True
        # (and what is computed from them)
        changed = widened
        while changed:
            changed = False
            for k in L.block_keys():
                for s in L.blocks[k].stmts:
                    if s.k == 'assign' and s.web.t in ('i32', 'u32') and not s.web.param and wide(s.x):
                        s.web.t = 'u64'
                        changed = True

    # ---- folding ------------------------------------------------------------------

    def fold(self):
        L = self.lifter
        # Constants and copies of single-definition webs everywhere.
        changed = True
        while changed:
            changed = False
            single = {}
            for k in L.block_keys():
                for s in L.blocks[k].stmts:
                    if s.k == 'assign' and len(s.web.defs) == 1 and not s.web.entry:
                        single[s.web] = s
            for w, s in single.items():
                x = s.x
                if x.k == 'slotval' and x.a[0].k == 'var' and x.a[0].v.t == w.t:
                    x = x.a[0]
                if x.k in ('c', 'sp', 'fc') or (x.k == 'var' and len(x.v.defs) == 1 and (x.v.param or x.v in single)) or \
                        (x.k == 'fromBits' and x.a[0].k == 'c'):
                    if self.replace_web(w, x):
                        changed = True
                    s.k = 'nop'
            for k in L.block_keys():
                L.blocks[k].stmts = [s for s in L.blocks[k].stmts if s.k != 'nop']
        # Single uses within a block.
        for k in L.block_keys():
            b = L.blocks[k]
            self.fold_block(b)
        # Dead assignments.
        changed = True
        while changed:
            changed = False
            counts = self.count_uses()
            for k in L.block_keys():
                b = L.blocks[k]
                keep = []
                for s in b.stmts:
                    if s.k == 'assign' and counts.get(s.web, 0) == 0 and not s.web.param:
                        changed = True
                        if has_effects(s.x):
                            keep.append(S('expr', x=s.x))
                        continue
                    keep.append(s)
                b.stmts = keep

    def all_exprs_of_block(self, b):
        for s in b.stmts:
            for e in s.exprs():
                yield e
        for attr in term_attrs(b):
            e = getattr(b, attr, None)
            if e is not None:
                yield e

    def count_uses(self):
        counts = defaultdict(int)
        L = self.lifter
        for k in L.block_keys():
            b = L.blocks[k]
            for e in self.all_exprs_of_block(b):
                for n in e.walk():
                    if n.k == 'var':
                        counts[n.v] += 1
                    if n.k == 'raw' and isinstance(n.v, dict):
                        for v in n.v.values():
                            for m in v.walk():
                                if m.k == 'var':
                                    counts[m.v] += 1
        return counts

    def replace_web(self, w, x):
        L = self.lifter
        done = False
        for k in L.block_keys():
            b = L.blocks[k]
            for s in b.stmts:
                for attr in ('x', 'addr'):
                    e = getattr(s, attr)
                    if e is not None:
                        ne, ch = replace_var(e, w, x)
                        if ch:
                            setattr(s, attr, simplify(ne))
                            done = True
            for attr in term_attrs(b):
                e = getattr(b, attr, None)
                if e is not None:
                    ne, ch = replace_var(e, w, x)
                    if ch:
                        setattr(b, attr, simplify(ne))
                        done = True
        return done

    def fold_block(self, b):
        counts = self.count_uses()
        changed = True
        while changed:
            changed = False
            stmts = b.stmts
            for i, s in enumerate(stmts):
                if s.k != 'assign' or len(s.web.defs) != 1 or s.web.entry or counts.get(s.web, 0) != 1:
                    continue
                # The use: a later statement or the terminator.
                j = None
                for jj in range(i + 1, len(stmts)):
                    if any(n.k == 'var' and n.v is s.web for e in stmts[jj].exprs() for n in all_nodes(e)):
                        j = jj
                        break
                in_term = False
                if j is None:
                    for attr in term_attrs(b):
                        e = getattr(b, attr, None)
                        if e is not None and b.term[0] in ('cond', 'switch', 'ret', 'tail') and \
                                any(n.k == 'var' and n.v is s.web for n in all_nodes(e)):
                            in_term = attr
                    if not in_term:
                        continue
                    between = stmts[i + 1:]
                else:
                    between = stmts[i + 1:j]
                x = s.x
                if not self.can_move(x, between):
                    continue
                if j is not None:
                    target = stmts[j]
                    if not self.can_place(x, target.exprs(), s.web, target):
                        continue
                    for attr in ('x', 'addr'):
                        e = getattr(target, attr)
                        if e is not None:
                            ne, ch = replace_var(e, s.web, x)
                            if ch:
                                setattr(target, attr, ne)
                else:
                    e = getattr(b, in_term)
                    if not self.can_place(x, [e], s.web, None):
                        continue
                    ne, _ = replace_var(e, s.web, x)
                    setattr(b, in_term, ne)
                del stmts[i]
                counts = self.count_uses()
                changed = True
                break

    def can_move(self, x, between):
        """x may be computed after the statements `between` instead of before."""
        inputs = {n.v for n in x.walk() if n.k == 'var'}
        mem = reads_memory(x)
        eff = has_effects(x) or any(n.k == 'raw' or n.k == 'vflo' or n.k == 'vfhi' for n in x.walk())
        for s in between:
            if s.k == 'assign' and s.web in inputs:
                return False
            if s.k in ('note', 'unsup'):
                continue
            s_eff = s.k in ('store', 'storeu', 'vld', 'vst', 'vset', 'vraw') or any(has_effects(e) for e in s.exprs())
            s_mem = s_eff or any(reads_memory(e) for e in s.exprs())
            if eff and (s_eff or s_mem):
                return False
            if mem and s_eff:
                return False
        return True

    def can_place(self, x, exprs, web, stmt):
        """Putting x in place of web's use in exprs keeps the order of effects."""
        eff = has_effects(x)
        mem = reads_memory(x)
        if not (eff or mem):
            return True
        for e in exprs:
            others = siblings_effects(e, web)
            if eff and (others['mem'] or others['eff']):
                return False
            if mem and others['eff']:
                return False
        if stmt is not None and stmt.k in ('store', 'storeu') and eff:
            # *addr = x: the address is evaluated after the value (C++17),
            # the value's call before the store as in the original.
            pass
        return True

    # ---- structure -------------------------------------------------------------------

    def structure(self):
        L = self.lifter
        self.order = [k for k in L.order]
        self.index = {k: i for i, k in enumerate(self.order)}
        self.goto_targets = set()
        self.ret_blocks = set()
        for k in self.order:
            b = L.blocks[k]
            if not b.stmts and b.term[0] == 'ret':
                self.ret_blocks.add(k)
        self.emitted = set()
        self.body = self.emit_range(0, len(self.order), {'follow': None, 'loop_head': None, 'loop_follow': None})
        eliminate_gotos(self.body)
        self.goto_targets = set(goto_targets_in(self.body))
        # Every branch back is charged where it is taken (ee::loopBack): a
        # checkpoint due in the middle of a loop is honoured at the pass the
        # original honours it (no counted loops, ee::chargeLoop(passes)).
        self.safe_points()

    def safe_points(self):
        """What the function table maps inside this function (ee/abi.h, safe
        points): the entry blocks' starts (sp_leaders; each slice runs to the
        next), the addresses resumed in the whole function (sp_whole), and
        the return addresses of calls that start an entry block
        (sp_after_call). Without entry blocks the draft keeps
        ee::chargeLoop(1) (the whole function's charges are all there is)."""
        L = self.lifter
        p = L.p
        table = self.ws.table()
        path = self.ws.generated(self.address)
        own = os.path.basename(path)[:-4] if path else None
        self.sp_leaders, self.sp_whole, self.sp_after_call, self.sp_cross_whole = [], set(), set(), set()
        self.sp_model = False
        if not own or not table:
            return
        for a in range(p.start + 4, p.end, 4):
            n = table.get(a)
            if n is None:
                continue
            if n == own:
                self.sp_whole.add(a)
            elif n.startswith('entry_'):
                self.sp_leaders.append(a)
            else:
                # Another function's start inside this one: not modelled.
                self.stats['safepoint_nested'] += 1
                self.sp_leaders = []
                return
        if not self.sp_leaders:
            return
        self.sp_model = True
        self.sp_leader_set = set(self.sp_leaders)
        for k in L.order:
            b = L.blocks[k]
            if b.term[0] == 'fall' and b.ops and b.ops[-1].k == 'call' and b.term[1] in self.sp_leader_set:
                self.sp_after_call.add(b.term[1])
        # ee::block(L, true) only where every way into L is the call's own
        # fall-through: a branch to L from elsewhere is not the call's return,
        # and after an earlier call that unwound it would put the frame into
        # entry-block mode where the original goes on as the whole function
        # (zgehand3: collisionQuery 0x1262D4, 0x1267DC, 0x13B810, 0x1624F8).
        # Elsewhere the call's statement is followed by ee::afterCall(L) and
        # L gets a plain ee::block(L).
        self.sp_after_call_label, self.aftercall_stmt = set(), {}
        for k in L.order:
            b = L.blocks[k]
            if not (b.term[0] == 'fall' and b.ops and b.ops[-1].k == 'call' and b.term[1] in self.sp_leader_set):
                continue
            target = b.term[1]
            if len(L.blocks[target].preds) <= 1:
                self.sp_after_call_label.add(target)
                continue
            last = b.stmts[-1] if b.stmts else None
            info = b.ops[-1].info

            def has_call(st, only=None):
                return any(n.k == 'call' and (only is None or n.v is only) for e in st.exprs() for n in all_nodes(e))
            # The call's statement, or the last of the call-free ones after it
            # (`x = ee::leftByCall(...)`: registers the call leaves; gamehand5:
            # moveMovers 0x1CA2F8, 0x1CA35C, 0x1CAA4C).
            at = next((i for i in range(len(b.stmts) - 1, -1, -1) if has_call(b.stmts[i], info)), None)
            if at is not None and not any(has_call(st) for st in b.stmts[at + 1:]):
                self.aftercall_stmt[id(last)] = target
            else:
                self.todo.append('the call before 0x%X falls into an entry block other paths reach too, and its '
                                 'statement moved: no ee::afterCall (a call that unwinds there goes on as the whole '
                                 'function)' % target)
        # Block starts reached from another slice: an entry block's start, or
        # an address the table resumes in the whole function (a dispatch of
        # the function); any other would be a pc the table has no entry for.
        for k in L.order:
            b = L.blocks[k]
            for pk in b.preds:
                src = pk[1] if isinstance(pk, tuple) else pk
                if self.sp_slice(src) == self.sp_slice(k) or k in self.sp_leader_set:
                    continue
                if k in self.sp_whole:
                    self.sp_cross_whole.add(k)
                else:
                    self.stats['safepoint_unmapped'] += 1

    def sp_slice(self, address):
        """The entry block (its start) an address of the function lies in; 0
        before the first."""
        import bisect
        i = bisect.bisect_right(self.sp_leaders, address) - 1
        return self.sp_leaders[i] if i >= 0 else 0

    def preds(self, k):
        return self.lifter.blocks[k].preds

    def real(self, k):
        """A synthetic (likely delay slot) block's own target."""
        return k

    def emit_range(self, lo, hi, ctx, skip_loop_at=None):
        out = []
        i = lo
        L = self.lifter
        while i < hi:
            k = self.order[i]
            b = L.blocks[k]
            # A loop headed here?
            if skip_loop_at != i:
                latches = [p for p in b.preds if p in self.index and self.index[p] >= i]
                if latches:
                    e = max(self.index[p] for p in latches)
                    if e < hi:
                        follow = self.order[e + 1] if e + 1 < len(self.order) else None
                        inner = {'follow': k, 'loop_head': k, 'loop_follow': follow, 'outer': ctx}
                        body = self.emit_range(i, e + 1, inner, skip_loop_at=i)
                        out.append(S('label', target=k, text='loop'))
                        out.append(S('loop', body=body, target=k))
                        i = e + 1
                        # After the loop: falls to the follow.
                        continue
            if skip_loop_at != i:
                # (A loop's head has its label before the loop: one label.)
                out.append(S('label', target=k))
            out.extend(b.stmts)
            nxt = self.order[i + 1] if i + 1 < hi else (ctx['follow'] if ctx['follow'] is not None else None)
            at_end = i + 1 >= hi
            t = b.term
            if t[0] == 'cond':
                taken, fall = t[2], t[3]
                edge_code = None
                if isinstance(taken, tuple):   # a likely branch: its delay slot on the taken edge
                    sb = L.blocks[taken]
                    edge_code = list(sb.stmts)
                    taken = sb.term[1]
                ti = self.index.get(taken)
                # if-then / if-else over a forward region.
                if ti is not None and ti > i + 1 and ti <= hi and fall == self.order[i + 1] if i + 1 < len(self.order) else False:
                    region_ok = all(all(p in self.order[i:ti] or p == k for p in self.preds(self.order[x]))
                                    for x in range(i + 1, ti))
                    if region_ok and (ti < hi or ti == hi):
                        follow_k = self.order[ti] if ti < len(self.order) else None
                        last = L.blocks[self.order[ti - 1]]
                        # if / else: the then part ends in a jump over the else part.
                        if last.term[0] == 'goto' and last.term[1] in self.index and ti < hi:
                            ui = self.index[last.term[1]]
                            if ui > ti and ui <= hi and self.preds(self.order[ti]) <= {k} and \
                                    all(all(p in self.order[ti:ui] for p in self.preds(self.order[x])) for x in range(ti + 1, ui)) and \
                                    not self.loop_head_in(ti, ui):
                                follow_u = self.order[ui] if ui < len(self.order) else ctx['follow']
                                c2 = dict(ctx)
                                c2['follow'] = follow_u
                                then = self.emit_range(i + 1, ti, c2)
                                els = (edge_code or []) + self.emit_range(ti, ui, c2)
                                out.append(S('if', x=negate(b.cond), body=then, orelse=els))
                                i = ui
                                if ui == hi:
                                    out.extend(self.transfer(follow_u, False, ctx, True))
                                continue
                        c2 = dict(ctx)
                        c2['follow'] = follow_k if ti < hi else ctx['follow']
                        then = self.emit_range(i + 1, ti, c2)
                        if ti == hi:
                            # The then part runs to this range's end: the taken
                            # edge's transfer is the else part, not after the if.
                            els = (edge_code or []) + self.transfer(taken, False, ctx, True)
                            out.append(S('if', x=negate(b.cond), body=then, orelse=els or None))
                        else:
                            out.append(S('if', x=negate(b.cond), body=then, orelse=edge_code or None))
                        i = ti
                        continue
                tk = self.edge(b, t[2])
                fk = self.edge(b, fall)
                fall_code = self.transfer(fall, fk, ctx, at_end, nxt)
                # When the fall-through edge needs no code, the taken edge
                # may fall out of the if to the same place.
                taken_nxt = nxt if not fall_code and fall == nxt else None
                out.append(S('if', x=b.cond, body=self.transfer(t[2], tk, ctx, False, taken_nxt)))
                out.extend(fall_code)
            elif t[0] in ('fall', 'goto'):
                out.extend(self.transfer(t[1], self.edge(b, t[1]), ctx, at_end, nxt))
            elif t[0] == 'ret':
                out.append(S('ret', x=b.retx, leaves=self.leaves_of(b)))
            elif t[0] == 'tail':
                out.append(S('tail', x=b.tailx))
            elif t[0] == 'switch':
                cases = []
                for target in t[2]:
                    cases.append((target, self.transfer(target, self.edge(b, target), ctx, False)))
                out.append(S('switch', x=b.cond, cases=cases, m='dispatch' if k in L.dispatched_switches else None,
                             jr=k))
            i += 1
        return out

    def loop_head_in(self, lo, hi):
        for x in range(lo, hi):
            k = self.order[x]
            if any(p in self.index and self.index[p] >= x for p in self.preds(k)):
                return True
        return False

    def edge(self, b, target):
        for s, charged in b.succ:
            if s == target:
                return charged
        return False

    @staticmethod
    def leaves_of(b):
        """[(register, x)] a return block leaves besides the result."""
        return [(a[len('leave_'):], getattr(b, a)) for a in getattr(b, 'leave_attrs', ())]

    def transfer(self, target, charged, ctx, at_end, nxt=None):
        """Statements that go from here to block target."""
        L = self.lifter
        if target is None:
            return []
        if isinstance(target, tuple):   # a likely branch's delay slot
            s = L.blocks[target]
            return list(s.stmts) + self.transfer(s.term[1], s.succ[0][1], ctx, False, nxt)
        if target in self.ret_blocks:
            # A branch back to a return block is still a branch back: generated
            # code charges its 32 cycles before the return.
            return ([S('charge', target=target)] if charged else []) + [S('mark', target=target),
                                                                         S('ret', x=L.blocks[target].retx,
                                                                           leaves=self.leaves_of(L.blocks[target]))]
        if ctx.get('loop_head') == target:
            if at_end and ctx.get('follow') == target:
                return []   # the end of the loop's body
            return [S('cont')]
        if ctx.get('loop_follow') == target and ctx.get('loop_head') is not None:
            return ([S('charge', target=target)] if charged else []) + [S('brk')]
        if at_end and (target == nxt or target == ctx.get('follow')):
            return []
        if not at_end and nxt is not None and target == nxt:
            return []
        out = []
        if charged:
            out.append(S('charge', target=target))
        self.goto_targets.add(target)
        out.append(S('goto', target=target))
        return out


def count_loops(ss):
    """A loop with no calls, no inner loop and no goto: counted, charged once at
    each way out (ee::chargeLoop(passes)), as the house style does. Loops with
    calls keep a charge per pass: the callees run at the original's EE time."""
    def walk(ss):
        for s in ss:
            yield s
            for sub in (s.body, s.orelse):
                if sub:
                    yield from walk(sub)
            if s.cases:
                for _, b in s.cases:
                    yield from walk(b)
    targets = set(goto_targets_in(ss))
    for s in walk(ss):
        if s.k != 'loop':
            continue
        inner = list(walk(s.body))
        # Labels are emitted for every block; one a goto targets would let a
        # jump into the loop skip `passes`' initialisation.
        if any(x.k in ('loop', 'goto', 'switch', 'vraw', 'charge') for x in inner):
            continue
        if any(x.k == 'label' and x.target in targets for x in inner):
            continue
        if any(has_effects(e) for x in inner for e in x.exprs()):
            continue
        s.t = 'counted'


def goto_targets_in(ss):
    for s in ss:
        if s.k == 'goto':
            yield s.target
        for sub in (s.body, s.orelse):
            if sub:
                yield from goto_targets_in(sub)
        if s.cases:
            for _, b in s.cases:
                yield from goto_targets_in(b)


def labels_in(ss):
    for s in ss:
        if s.k == 'label':
            yield s.target
        for sub in (s.body, s.orelse):
            if sub:
                yield from labels_in(sub)
        if s.cases:
            for _, b in s.cases:
                yield from labels_in(b)


def eliminate_gotos(root):
    """Forward gotos to a label later in the same statement list, turned into
    if / else: `if (c) goto L; S; L:` -> `if (!c) { S }`; `if (c) { A; goto L; }
    S; L:` -> `if (c) { A } else { S }` (also with the goto ending the else
    part); `goto L; L:` dropped. Only when no goto from elsewhere enters S."""
    def count_gotos(ss, out):
        for s in ss:
            if s.k == 'goto':
                out[s.target] = out.get(s.target, 0) + 1
            for sub in (s.body, s.orelse):
                if sub:
                    count_gotos(sub, out)
            if s.cases:
                for _, b in s.cases:
                    count_gotos(b, out)
        return out

    def ends_with_goto(ss, target):
        real = [x for x in ss if not (x.k == 'label')]
        return bool(real) and ss[-1].k == 'goto' and ss[-1].target == target

    def lists(ss):
        yield ss
        for s in ss:
            for sub in (s.body, s.orelse):
                if sub:
                    yield from lists(sub)
            if s.cases:
                for _, b in s.cases:
                    yield from lists(b)

    changed = True
    while changed:
        changed = False
        total = count_gotos(root, {})
        for ss in list(lists(root)):
            positions = {s.target: j for j, s in enumerate(ss) if s.k == 'label'}
            for i, s in enumerate(ss):
                if s.k == 'goto' and positions.get(s.target) == i + 1:
                    del ss[i]
                    changed = True
                    break
                if s.k != 'if':
                    continue
                cands = []
                if s.body and ends_with_goto(s.body, s.body[-1].target if s.body[-1].k == 'goto' else None):
                    cands.append(('body', s.body[-1].target))
                if s.orelse and s.orelse[-1].k == 'goto':
                    cands.append(('orelse', s.orelse[-1].target))
                done = False
                for side, target in cands:
                    j = positions.get(target)
                    if j is None or j <= i:
                        continue
                    if s.body and s.body[-1].k == 'goto' and side == 'body' and s.body[-1].charged_back():
                        continue
                    middle = ss[i + 1:j]
                    inner = count_gotos(middle, {})
                    own = 1
                    # Labels inside the middle part must be reached from it only.
                    if any(total.get(l, 0) > inner.get(l, 0) for l in labels_in(middle)):
                        continue
                    if side == 'body':
                        new_body = s.body[:-1]
                        if not new_body and not s.orelse:
                            new = S('if', x=negate(s.x), body=middle)
                        else:
                            new = S('if', x=s.x, body=new_body, orelse=(s.orelse or []) + middle)
                    else:
                        new = S('if', x=s.x, body=(s.body or []) + middle, orelse=s.orelse[:-1] or None)
                    ss[i:j] = [new]
                    changed = done = True
                    break
                if done:
                    break
            if changed:
                break


def all_nodes(x):
    for n in x.walk():
        yield n
        if n.k == 'raw' and isinstance(n.v, dict):
            for v in n.v.values():
                yield from v.walk()


def walk_parents(x, parent=None):
    yield x, parent
    for c in x.a:
        yield from walk_parents(c, x)


def replace_var(x, w, by):
    if x.k == 'var' and x.v is w:
        return by.copy(), True
    changed = False
    new = []
    for c in x.a:
        nc, ch = replace_var(c, w, by)
        new.append(nc)
        changed |= ch
    if x.k == 'raw' and isinstance(x.v, dict):
        nv = {}
        for r, e in x.v.items():
            ne, ch = replace_var(e, w, by)
            nv[r] = ne
            changed |= ch
        if changed:
            return X('raw', list(nv.values()), nv, x.t), True
    if changed:
        return X(x.k, new, x.v, x.t, x.m), True
    return x, False


def siblings_effects(e, web):
    """Memory reads and effects in e outside the path to web's use (the
    path's own nodes run after their operands)."""
    res = {'mem': False, 'eff': False}

    def contains(x):
        return any(n.k == 'var' and n.v is web for n in all_nodes(x))

    def visit(x):
        if not contains(x):
            if reads_memory(x):
                res['mem'] = True
            if has_effects(x):
                res['eff'] = True
            return
        for c in x.a:
            visit(c)
    visit(e)
    return res


def negate(x):
    if x.k == 'not':
        return x.a[0]
    if x.k == 'cmp':
        inv = {'==': '!=', '!=': '==', '<s': '>=s', '>=s': '<s', '>s': '<=s', '<=s': '>s',
               '<u': '>=u', '>=u': '<u', '>u': '<=u', '<=u': '>u'}
        if x.v in inv:
            return X('cmp', x.a, inv[x.v], 'bool')
    if x.k == 'fcmp':
        return X('not', [x], t='bool')
    return X('not', [x], t='bool')


def type_size(t):
    return {'i8': 1, 'u8': 1, 'i16': 2, 'u16': 2, 'i32': 4, 'u32': 4, 'f': 4, 'i64': 8, 'u64': 8}[t]


def slot_type(t):
    return {'i8': 'i32', 'u8': 'i32', 'i16': 'i32', 'u16': 'i32', 'i32': 'i32', 'u32': 'u32', 'f': 'f',
            'i64': 'u64', 'u64': 'u64'}[t]


def function_params_from(L, callees):
    """(int args, float args, args read as whole 128-bit registers)."""
    decide_call_args(L, callees)
    ints, floats, qwords = set(), set(), set()
    # A call of itself passes its own arguments on: not a read of them.
    self_calls = {o.id for _, o in L.all_ops() if o.k == 'call' and o.info.target == L.address}
    for (u, r), ds in L.use_defs.items():
        if u in self_calls:
            continue
        if ('entry', r) in ds:
            if r in INT_ARGS:
                ints.add(INT_ARGS.index(r))
            elif r in FLOAT_ARGS:
                floats.add(FLOAT_ARGS.index(r))
            elif r.endswith('^') and r[:-1] in INT_ARGS:
                ints.add(INT_ARGS.index(r[:-1]))
                qwords.add(r[:-1])
    return sorted(ints), sorted(floats), qwords


# ---------------------------------------------------------------------------
# Rendering
# ---------------------------------------------------------------------------

CTYPE = {'i32': 'int32_t', 'u32': 'uint32_t', 'i64': 'int64_t', 'u64': 'uint64_t', 'f': 'ee::Float', 'bool': 'bool',
         'i8': 'int8_t', 'u8': 'uint8_t', 'i16': 'int16_t', 'u16': 'uint16_t'}
PREC = {'||': 3, '&&': 4, '|': 5, '^': 6, '&': 7, '==': 8, '!=': 8, '<': 9, '<=': 9, '>': 9, '>=': 9,
        '<<': 10, '>>': 10, '+': 11, '-': 11, '*': 12, '/': 12, '%': 12}


def fmt_hex(v, digits=0):
    v &= 0xFFFFFFFF
    return ('0x%0' + str(digits) + 'X') % v if digits else '0x%X' % v


def float_literal(bits):
    f = struct.unpack('<f', struct.pack('<I', bits & 0xFFFFFFFF))[0]
    if f != f or f in (float('inf'), float('-inf')) or (bits & 0x7F800000) == 0 and bits & 0x7FFFFF:
        return None
    for prec in range(1, 10):
        s = '%.*g' % (prec, f)
        if struct.unpack('<I', struct.pack('<f', float(s)))[0] == bits & 0xFFFFFFFF:
            if 'e' not in s and '.' not in s:
                s += '.0'
            return s + 'f'
    return None


class Renderer:
    def __init__(self, draft):
        self.d = draft
        self.ws = draft.ws
        self.counting = []

    # ---- expressions --------------------------------------------------------

    def name(self, w):
        return w.name

    def r(self, x, want=None):
        """C++ text of x; want: the type the context needs ('i32', 'u32', 'f', ...)."""
        text, prec = self.rx(x)
        return text

    def paren(self, x, min_prec):
        text, prec = self.rx(x)
        if prec < min_prec:
            return '(' + text + ')'
        return text

    def as_u32(self, x):
        if x.k == 'c':
            return self.const(x.v, 'u32'), 99
        t = self.type_of(x)
        if t in ('u32',):
            return self.rx(x)
        return 'static_cast<uint32_t>(%s)' % self.r(x), 99

    def sign_extended(self, x):
        """Whether x is a 32-bit ADDU, SUBU, NEGU, SLL or DIVU that C++ computes
        unsigned (an unsigned operand, or a 64-bit one cut to its low word by
        rbin): the EE sign-extends the result into the 64-bit register, C++
        would zero-extend it (quar1: _vfprintf_r's n = dprec - size, -2 in a
        64-bit variable as 0xFFFFFFFE, a padding count that never ran out)."""
        if x.k == 'neg':
            return self.unsigned_text(x)
        if x.k != 'bin' or x.t == '64':
            return False
        if x.v in ('&', '|', '^'):
            # (of two such results, or one and a constant: sign-extended too;
            # masked by a constant with bit 31 clear it is positive either way)
            if x.v == '&' and any(c.k == 'c' and 0 <= c.v < 0x80000000 for c in x.a):
                return False
            return self.unsigned_text(x) and all(
                c.k == 'c' or (c.k == 'bin' and c.t != '64' and c.v in ('+', '-', '<<', '>>u', '>>s')) for c in x.a)
        if x.v not in ('+', '-', '<<', '/u', '%u'):
            return False
        return self.unsigned_text(x) or any(self.type_of(c) in ('u64', 'i64') for c in (x.a[:1] if x.v == '<<' else x.a))

    def as_64(self, x, sign):
        """x as the 64-bit register it is on the EE (sign 's': int64_t, 'u':
        uint64_t): a 32-bit constant or value sign-extended (as the EE keeps
        them), an unsigned 32-bit one (LWU, ANDI, LBU, LHU) zero-extended."""
        ct = 'int64_t' if sign == 's' else 'uint64_t'
        t = self.type_of(x)
        if x.k == 'c':
            v = x.v
            if t not in ('u64', 'i64'):
                v &= 0xFFFFFFFF
                v = v - (1 << 32) if v & 0x80000000 else v
            if sign == 'u':
                v &= 0xFFFFFFFFFFFFFFFF
                return ('%dull' % v) if v < 0x10000 else '0x%Xull' % v, 99
            if -0x10000 < v < 0x10000:
                return '%dll' % v, 99
            return ('0x%Xll' % v) if v >= 0 else '%dll' % v, 99
        if t == ('i64' if sign == 's' else 'u64'):
            return self.rx(x)
        if t in ('u64', 'i64'):
            return 'static_cast<%s>(%s)' % (ct, self.r(x)), 99
        if t in ('u32', 'u8', 'u16') and not self.sign_extended(x):
            return 'static_cast<%s>(static_cast<uint32_t>(%s))' % (ct, self.r(x)), 99
        return 'static_cast<%s>(static_cast<int32_t>(%s))' % (ct, self.r(x)), 99

    def unsigned_text(self, x):
        """Whether x's C++ text is unsigned, which type_of does not always say:
        const and rbin write a large constant, and a constant of &, | or ^, with
        a `u` (an SRA of `cursor - 0x27A5A0u` written as a bare >> is logical:
        closeScreenPrimitivePackets; so is a signed compare of it)."""
        if x.k == 'c':
            return x.t == 'u32' or x.v > 0xFFFF
        if x.k == 'bin' and x.t != '64' and x.v in ('+', '-', '&', '|', '^'):
            a, b = x.a
            if b.k != 'c':
                literal = self.unsigned_text(b)
            elif x.v in ('&', '|', '^'):
                literal = b.v > 9 or self.unsigned_text(b)
            elif x.v == '+' and b.v < 0:
                literal = -b.v > 0xFFFF   # (written as a subtraction)
            else:
                literal = self.unsigned_text(b)
            return literal or self.unsigned_text(a)
        if x.k == 'bin' and x.t != '64' and x.v == '<<':
            return self.unsigned_text(x.a[0])
        if x.k in ('neg', 'bnot'):
            return self.unsigned_text(x.a[0])
        return self.type_of(x) == 'u32'

    def as_i32(self, x):
        if x.k == 'c':
            return self.const(x.v, 'i32'), 99
        t = self.type_of(x)
        if t in ('i32', 'i8', 'i16', 'u8', 'u16', 'bool') and not self.unsigned_text(x):
            return self.rx(x)
        return 'static_cast<int32_t>(%s)' % self.r(x), 99

    def type_of(self, x):
        if x.k == 'var':
            return x.v.t
        if x.k == 'c':
            return x.t
        if x.k in ('cmp', 'fcmp', 'not'):
            return 'bool'
        if x.k == 'ld':
            return x.v if x.v in ('i32', 'u32', 'u64', 'f') else ('i32' if x.v in ('i8', 'i16') else 'u32')
        if x.k in ('fop', 'fneg', 'ffn', 'itof', 'fromBits'):
            return 'f'
        if x.k == 'bin' and x.t == '64':
            return 'u64'
        if x.k == 'bin' and x.v in ('&', '|', '^') and any(self.type_of(c) in ('u64', 'i64') for c in x.a):
            return 'u64'
        if x.k == 'bnot' and self.type_of(x.a[0]) in ('u64', 'i64'):
            return 'u64'
        if x.k == 'bin' and x.v in ('>>u', '/u', '%u'):
            return 'u32'
        if x.k == 'bin' and x.v in ('>>s', '/s', '%s'):
            return 'i32'
        if x.k == 'bin' and x.v in ('+', '-', '&', '|', '^', '<<'):
            # C++'s usual conversions: unsigned if an operand is (a shift: its left).
            ops = x.a[:1] if x.v == '<<' else x.a
            return 'u32' if any(self.type_of(c) == 'u32' for c in ops) else 'i32'
        if x.k == 'neg' or x.k == 'bnot':
            return 'u32' if self.type_of(x.a[0]) == 'u32' else 'i32'
        if x.k == 'align16':
            return 'u32'
        if x.k == 'sp':
            return 'u32'
        if x.k == 'call':
            return x.v.ret_t or 'i32'
        if x.k in ('vflo', 'vfhi', 'qlow', 'qhigh'):
            return 'u64'
        if x.k in ('add64', 'hilo'):
            return 'i64'
        if x.k == 'left':
            return x.t if x.t in CTYPE else 'i32'
        return x.t if x.t in CTYPE else 'i32'

    def const(self, v, t):
        if t == 'bool':
            return 'true' if v else 'false'
        if t in ('u32', 'u64') or v > 0xFFFF or v < -0xFFFF:
            if t == 'u64':
                return fmt_hex(v) + 'ull' if v >= 0 else '%dll' % v
            if v < 0 and t != 'u32' and v >= -0x10000:
                return str(v)
            if t == 'i32' and v < 0:
                return 'static_cast<int32_t>(%su)' % fmt_hex(v, 8)
            return fmt_hex(v, 8 if (v & 0xFFFFFFFF) > 0xFFFF else 0) + 'u'
        if v >= 10 and v & 0xF == 0 and v >= 0x10:
            return '0x%X' % v
        return str(v)

    def address(self, x):
        """An address expression: frame offsets, globals, ee::at."""
        return self.r(x)

    def io_access(self, t, addr, m=None):
        """For an access that reaches an I/O register (ee::io): (kind, address
        text), kind 'io' for a constant I/O address, 'any' for one the draft
        cannot place (Draft.io_dynamic: ee::io::loadAny / storeAny decide at
        run time); None for memory."""
        if t == 'f':
            return None
        addr = simplify(addr.copy())
        if addr.k == 'sp':
            return None
        a = const_address(addr)
        if a is not None:
            return ('io', '0x%08Xu' % (a & 0xFFFFFFFF)) if is_constant_io_address(a, m) else None
        # A base in a variable that every path sets to the same constant
        # (vif1Interrupt's 0x10000000 kept across its loop).
        a = const_address(simplify(self.d.constant_webs_in(addr)))
        if a is not None:
            return ('io', '0x%08Xu' % (a & 0xFFFFFFFF)) if is_io_address(a) else None
        if getattr(self.d, 'io_dynamic', False):
            return ('any', self.addr_text(addr))
        return None

    def deref(self, t, addr, m=None):
        """*ee::at<T>(addr) text for a memory operand of type t (m: X.m)."""
        ct = 'ee::Float' if t == 'f' else CTYPE[t]
        io = self.io_access(t, addr, m)
        if io:
            self.d.uses_io = True
            return 'ee::io::%s<%s>(%s)' % ('load' if io[0] == 'io' else 'loadAny', ct, io[1])
        addr = simplify(addr.copy())
        if addr.k == 'sp':
            off = addr.v + self.d.lifter.frame_size
            return '*frame.at<%s>(0x%X)' % (ct, off)
        a = const_address(addr)
        if a is not None:
            # (readable_names agent) a float of .lit4, the source's literal: as the literal
            lit = readable_names and t == 'f' and readable_names.literal_float(self.ws, a)
            if lit:
                return lit
            g = self.d.global_name(a, t)
            if g:
                return '*' + g
            return '*ee::at<%s>(0x%08Xu)' % (ct, a)
        return '*ee::at<%s>(%s)' % (ct, self.addr_text(addr))

    def addr_text(self, addr):
        t = self.type_of(addr)
        text = self.r(addr)
        if t in ('u32',) or addr.k == 'bin' and addr.t == 'u32':
            return text
        return text

    def rx(self, x):
        x = simplify(x.copy())
        k = x.k
        if k == 'var':
            return x.v.name, 99
        if k == 'pass':
            return 'ee::passRegisters({%s})' % ', '.join('{%d, %s}' % (r, self.r(v)) for r, v in zip(x.v, x.a)), 99
        if k == 'saves':
            text = 'ee::calleeSaves({%s})' % ', '.join('{%d, %s}' % (r, self.r(v)) for r, v in zip(x.v, x.a))
            if x.m:
                SAVES_AT[x.m[0]] = (x.m[1], text)   # (--saves-table)
            return text, 99
        if k == 'c':
            return self.const(x.v, x.t), 99
        if k == 'undef':
            self.d.todo.append('reads %s, which the function never set (the caller\'s value: ee::callerRegister)' % x.v)
            r = x.v
            if r in GPR and r != 'zero':
                text = 'ee::callerRegister(%d /* %s */)' % (GPR.index(r), r)
                return (text if x.t == 'u64' else 'static_cast<%s>(%s)' % (CTYPE.get(x.t, 'int32_t'), text)), 99
            if re.match(r'^f\d+$', r):
                return 'ee::Float(ee::callerFloat(%s))' % r[1:], 99
            return '0 /* TODO: the caller\'s %s */' % x.v, 99
        if k == 'sp':
            off = x.v + self.d.lifter.frame_size
            self.d.needs_frame = True
            return 'frame.address(0x%X)' % off, 99
        if k == 'ld':
            return self.deref(x.v, x.a[0], x.m), 14
        if k == 'ldu':
            ct = CTYPE[x.v]
            return 'loadUnaligned<%s>(%s)' % (ct, self.r(x.a[0])), 99
        if k == 'slotval':
            return self.rx(x.a[0])
        if k == 'cast':
            ct = CTYPE.get(x.v, 'int32_t')
            return 'static_cast<%s>(%s)' % (ct, self.r(x.a[0])), 99
        if k == 'sext':
            return self.rx(x.a[0])
        if k == 'align16':
            # LQ / SQ / LQC2 / SQC2 ignore the address's low four bits.
            return '(%s & ~0xFu)' % self.r(x.a[0]), 99
        if k == 'bin':
            return self.rbin(x)
        if k == 'neg':
            return '-' + self.paren(x.a[0], 14), 14
        if k == 'bnot':
            return '~' + self.paren(x.a[0], 14), 14
        if k == 'b2i':
            return 'static_cast<int32_t>(%s)' % self.r(x.a[0]), 99
        if k == 'cmp':
            return self.rcmp(x)
        if k == 'not':
            inner = x.a[0]
            if inner.k == 'fcmp':
                return '!(%s)' % self.r(inner), 14
            return '!' + self.paren(inner, 14), 14
        if k == 'sel':
            return '%s ? %s : %s' % (self.paren(x.a[0], 3), self.paren(x.a[1], 3), self.paren(x.a[2], 3)), 2
        if k == 'mul64':
            cast = 'int64_t' if x.v == 's' else 'uint64_t'
            c32 = 'int32_t' if x.v == 's' else 'uint32_t'
            return 'static_cast<%s>(static_cast<%s>(%s)) * static_cast<%s>(%s)' % (
                cast, c32, self.r(x.a[0]), c32, self.r(x.a[1])), 12
        if k == 'lo32':
            inner = x.a[0]
            if inner.k == 'mul64' and any(c.k == 'c' and 0 <= c.v <= 0xFFFF for c in inner.a):
                v = next(c for c in inner.a if c.k == 'c')
                other = next(c for c in inner.a if c is not v)
                return '%s * %s' % (self.paren(other, 13), ('0x%X' % v.v) if v.v > 9 else str(v.v)), 12
            if inner.k == 'mul64':
                c32 = 'int32_t' if inner.v == 's' else 'uint32_t'
                a0 = self.paren(inner.a[0], 13) if self.type_of(inner.a[0]) == ('i32' if inner.v == 's' else 'u32') else \
                    'static_cast<%s>(%s)' % (c32, self.r(inner.a[0]))
                a1 = self.paren(inner.a[1], 13) if self.type_of(inner.a[1]) == ('i32' if inner.v == 's' else 'u32') else \
                    'static_cast<%s>(%s)' % (c32, self.r(inner.a[1]))
                if inner.v == 's':
                    return 'static_cast<int32_t>(static_cast<uint32_t>(%s) * static_cast<uint32_t>(%s))' % (a0, a1), 99
                return '%s * %s' % (a0, a1), 12
            return 'static_cast<int32_t>(%s)' % self.r(inner), 99
        if k == 'hi32':
            return 'static_cast<int32_t>((%s) >> 32)' % self.r(x.a[0]), 99
        if k == 'fop':
            p = {'+': 11, '-': 11, '*': 12, '/': 12}[x.v]
            left = self.fparen(x.a[0], p)
            right = self.fparen(x.a[1], p + 1)
            return '%s %s %s' % (left, x.v, right), p
        if k == 'fneg':
            return '-' + self.fparen(x.a[0], 14), 14
        if k == 'ffn':
            return '%s(%s)' % (x.v, ', '.join(self.ftext(c) for c in x.a)), 99
        if k == 'fcmp':
            p = 8 if x.v == '==' else 9
            return '%s %s %s' % (self.fparen(x.a[0], p + 1), x.v, self.fparen(x.a[1], p + 1)), p
        if k == 'itof':
            inner = x.a[0]
            if inner.k == 'bits' and inner.a[0].k == 'fromBits':
                return 'ee::toFloat(%s)' % self.as_i32(inner.a[0].a[0])[0], 99
            return 'ee::toFloat(%s)' % self.as_i32(inner)[0], 99
        if k == 'ftoi':
            return 'ee::toInt(%s)' % self.ftext(x.a[0]), 99
        if k == 'fromBits':
            inner = x.a[0]
            if inner.k == 'c':
                lit = float_literal(inner.v)
                if lit:
                    return 'ee::Float(%s)' % lit, 99
                return 'ee::Float::fromBits(%su)' % fmt_hex(inner.v, 8), 99
            if inner.k == 'bits':
                return self.rx(inner.a[0])
            if inner.k == 'ftoi':
                return 'ee::Float::fromBits(static_cast<uint32_t>(%s))' % self.r(inner), 99
            if inner.k == 'cast' and inner.a[0].k == 'vflo':
                return 'ee::Float(_mm_cvtss_f32(ctx->vu0_vf[%d]))' % inner.a[0].v, 99
            if inner.k == 'vflo':
                return 'ee::Float(_mm_cvtss_f32(ctx->vu0_vf[%d]))' % inner.v, 99
            return 'ee::Float::fromBits(static_cast<uint32_t>(%s))' % self.r(inner), 99
        if k == 'bits':
            inner = x.a[0]
            if inner.k == 'fromBits':
                return self.rx(inner.a[0])
            if inner.k == 'fromBits' and inner.a[0].k == 'ftoi':
                return self.rx(inner.a[0])
            return 'static_cast<int32_t>(%s.bits())' % self.fparen(inner, 15), 99
        if k == 'call':
            return self.rcall(x), 99
        if k == 'qword':
            lo, hi = x.a
            lt = self.r(lo)
            if self.type_of(lo) not in ('u64', 'i64'):
                lt = 'static_cast<uint64_t>(static_cast<int64_t>(%s))' % lt
            elif not (lo.k == 'var' and self.type_of(lo) == 'u64'):
                lt = 'static_cast<uint64_t>(%s)' % lt
            ht = self.r(hi)
            if not (hi.k == 'var' and self.type_of(hi) == 'u64' or hi.k == 'c'):
                ht = 'static_cast<uint64_t>(%s)' % ht
            return 'ee::Qword{%s, %s}' % (lt, ht), 99
        if k == 'left':
            r = x.v
            if r.startswith('f') and r[1:].isdigit():
                return 'ee::Float(ee::leftByCallFloat(%s))' % r[1:], 99
            if r.endswith('^'):
                text = 'ee::leftByCallHigh(ee::reg::%s)' % r[:-1]
            else:
                text = 'ee::leftByCall(ee::reg::%s)' % r
            if x.t == 'u64':
                return text, 99
            return 'static_cast<%s>(%s)' % (CTYPE.get(x.t, 'int32_t'), text), 99
        if k in ('qlow', 'qhigh'):
            return '%s.%s' % (self.r(x.a[0]), 'low' if k == 'qlow' else 'high'), 99
        if k == 'mmi':
            return 'ee::mmi::' + x.v % tuple(self.r(c) for c in x.a), 99
        if k == 'hilo':
            return 'ee::mmi::hiLo(%s, %s)' % (self.r(x.a[0]), self.r(x.a[1])), 99
        if k == 'add64':
            return '(%s + %s)' % (self.r(x.a[0]), self.r(x.a[1])), 99
        if k == 'vflo':
            return '_mm_cvtsi128_si64(_mm_castps_si128(ctx->vu0_vf[%d]))' % x.v, 99
        if k == 'vfhi':
            return '_mm_cvtsi128_si64(_mm_unpackhi_epi64(_mm_castps_si128(ctx->vu0_vf[%d]), _mm_castps_si128(ctx->vu0_vf[%d])))' % (x.v, x.v), 99
        if k == 'raw' and x.v == 'kernel':
            return 'ee::kernelCall(%s)' % self.r(x.a[0]), 99
        if k == 'raw':
            return '(%s)' % x.v if isinstance(x.v, str) else '0', 99
        if k == 'reg':
            return x.v, 99
        return '/* ? %s */ 0' % k, 99

    def ftext(self, x):
        return self.r(x)

    def fparen(self, x, p):
        text, prec = self.rx(x)
        return '(' + text + ')' if prec < p else text

    def rbin(self, x):
        op = x.v
        a, b = x.a
        if x.t == '64' or op in ('&', '|', '^') and any(self.type_of(c) in ('u64', 'i64') for c in (a, b)):
            sym = {'+': '+', '-': '-', '&': '&', '|': '|', '^': '^', '<<': '<<', '>>u': '>>', '>>s': '>>'}[op]
            left = self.r(a)
            if op == '>>s':
                if self.sign_extended(a):
                    left = 'static_cast<int32_t>(%s)' % left
                left = 'static_cast<int64_t>(%s)' % left
            elif self.type_of(a) not in ('u64', 'i64'):
                if self.sign_extended(a):
                    left = 'static_cast<int32_t>(%s)' % left
                left = 'static_cast<uint64_t>(static_cast<int64_t>(%s))' % left
            else:
                left = self.paren(a, PREC[sym] if sym in PREC else 10)
            right = self.paren(b, PREC.get(sym, 10) + 1) if b.k != 'c' else (str(b.v) if op in ('<<', '>>u', '>>s') else self.const(b.v, 'u64'))
            if self.sign_extended(b):
                right = 'static_cast<int32_t>(%s)' % self.r(b)
            return '%s %s %s' % (left, sym, right), PREC.get(sym, 10)
        if op in ('/s', '%s', '/u', '%u'):
            sym = op[0]
            if op.endswith('u'):
                l, _ = self.as_u32(a)
                r_, _ = self.as_u32(b)
            else:
                l, _ = self.as_i32(a)
                r_, _ = self.as_i32(b)
            l = l if l.isidentifier() or l.startswith('static_cast') else '(%s)' % l
            r_ = r_ if r_.isidentifier() or r_.startswith('static_cast') else '(%s)' % r_
            return '%s %s %s' % (l, sym, r_), 12
        if op == '>>u':
            l, _ = self.as_u32(a)
            return '%s >> %s' % (l if re.match(r'^[\w.]+$', l) or l.startswith('static_cast') else '(%s)' % l, self.paren(b, 11)), 10
        if op == '>>s':
            l, _ = self.as_i32(a)
            return '%s >> %s' % (l if re.match(r'^[\w.]+$', l) or l.startswith('static_cast') else '(%s)' % l, self.paren(b, 11)), 10
        sym = op
        p = PREC[sym]
        if op in ('+', '-', '*', '<<') and any(self.type_of(c) in ('u64', 'i64') for c in (a, b)):
            # A 32-bit operation (ADDU, SUBU, SLL) on a 64-bit value: its low word.
            a = X('cast', [a], 'u32', 'u32') if self.type_of(a) in ('u64', 'i64') else a
            b = X('cast', [b], 'u32', 'u32') if self.type_of(b) in ('u64', 'i64') else b
        left = self.paren(a, p)
        if b.k == 'c' and op in ('+',) and b.v < 0:
            v = -b.v
            return '%s - %s' % (left, ('0x%X' % v) if 9 < v <= 0xFFFF else self.const(v, 'i32' if v <= 0xFFFF else 'u32')), p
        if b.k == 'c' and op == '+' and 9 < b.v <= 0xFFFF:
            return '%s + 0x%X' % (left, b.v), p
        if b.k == 'c':
            right = self.const(b.v, 'u32' if op in ('&', '|', '^') and b.v > 9 else b.t)
            if op in ('<<',):
                right = str(b.v)
        else:
            right = self.paren(b, p + 1)
        return '%s %s %s' % (left, sym, right), p

    def rcmp(self, x):
        op = x.v
        a, b = x.a
        sym = op.rstrip('su')
        p = 8 if sym in ('==', '!=') else 9
        if op[-1:] in ('s', 'u') and any(self.type_of(c) in ('u64', 'i64') for c in (a, b)):
            # SLT, SLTU, SLTI, SLTIU and the sign branches compare the whole
            # 64-bit registers: with a 64-bit operand the comparison is 64-bit
            # (litodp's normalising loop compared the low words of a 64-bit
            # value with 0x0FFFFFFFFFFFFFFF's and never ended: a corpus
            # replay grew to 113 GB, round drafter23).
            l, lp = self.as_64(a, op[-1])
            r_, rp = self.as_64(b, op[-1])
            if lp <= p:
                l = '(' + l + ')'
            if rp <= p:
                r_ = '(' + r_ + ')'
            return '%s %s %s' % (l, sym, r_), p
        if op.endswith('s'):
            l, lp = self.as_i32(a)
            r_, rp = self.as_i32(b) if b.k != 'c' else (str(b.v) if -0x10000 < b.v < 0x10000 else self.const(b.v, 'i32'), 99)
        elif op.endswith('u'):
            l, lp = self.as_u32(a)
            r_, rp = self.as_u32(b)
        else:
            l, lp = self.rx(a)
            if b.k == 'c':
                ta = self.type_of(a)
                r_, rp = self.const(b.v, 'u32' if ta == 'u32' else ('u64' if ta == 'u64' else 'i32')), 99
                if b.v == 0:
                    r_ = '0'
            else:
                r_, rp = self.rx(b)
        if lp <= p:
            l = '(' + l + ')'
        if rp <= p:
            r_ = '(' + r_ + ')'
        return '%s %s %s' % (l, sym, r_), p

    def self_call(self, x):
        """Whether x is a call of the function itself (not its tail jump)."""
        return x is not None and x.k == 'call' and x.v.target is not None and x.v.target == self.d.address and \
            x.v.kind not in ('tail', 'itail', 'jtail')

    def rcall(self, x):
        info = x.v
        d = self.d
        args = x.a
        target = None
        if info.target_x is not None:
            target = args[0]
            args = args[1:]
        n_int = len(info.int_args)
        int_args, float_args = args[:n_int], args[n_int:]
        tail = info.kind in ('tail', 'itail')
        if info.kind == 'jtail':
            d.todo.append('the jump to 0x%x is dispatched (not a direct C++ call): ee::call charges its 8 cycles' % info.target)
        ret_t = info.ret_t
        if tail:
            ret_t = 'f' if d.ret == 'f0' else (('u64' if getattr(d, 'ret_wide', False) else 'i32') if d.ret == 'v0' else None)
        rtype = CTYPE.get(ret_t, 'int32_t') if ret_t else None
        if info.target is not None and info.target == d.address and not tail:
            # A call of itself: generated code runs it inline (a jump to the
            # start with ra at the return point, no dispatch), and recordings
            # follow it inline: a direct C++ call, no EE cycles for it.
            n = len(d.int_params)
            ia = [self.r(a) for a in int_args[:n]] + ['0'] * max(0, n - len(int_args))
            fa = [self.ftext(a) for a in float_args[:len(d.float_params)]] + \
                ['ee::Float(0.0f)'] * max(0, len(d.float_params) - len(float_args))
            text = '%s(%s)' % (d.name, ', '.join(ia + fa))
            if rtype and d.ret == 'f0' and ret_t != 'f':
                text = 'static_cast<int32_t>(%s.bits())' % text
            return text
        if info.target is not None:
            readable = self.ws.readable.get(info.target)
            use_game = readable and readable['signature'] and (readable['header'] or readable.get('decl')) and \
                info.target not in BY_ADDRESS and \
                readable['namespace'] and '(anonymous)' not in readable['namespace'] and \
                info.target not in self.ws.hooked and \
                not (rtype and readable['signature'][0].strip() == 'void') and \
                not (rtype and is_float_type(readable['signature'][0]) != is_float_type(rtype))
            # (A readable callee declared void whose v0 the original reads
            # anyway, or one returning a float whose v0 is read, or the other
            # way: called by address, which returns the register read.)
            if use_game:
                return self.game_call(info, readable, int_args, float_args, rtype, tail)
            fname = d.callee_constant(info.target)
        else:
            fname = self.r(target)
        parts = [fname] + [self.r(a) for a in int_args] + [self.ftext(a) for a in float_args]
        call = 'ee::tailCall' if tail else 'ee::call'
        tmpl = '<%s>' % rtype if rtype else ''
        return '%s%s(%s)' % (call, tmpl, ', '.join(parts))

    def game_call(self, info, readable, int_args, float_args, rtype, tail):
        d = self.d
        if readable['header']:
            d.includes.add(readable['header'])
        else:
            # Declared only in its own .cpp: the batch file declares it.
            d.decls.add(readable['decl'])
            d.includes.update(readable.get('decl_headers') or ())
        ns = readable['namespace']
        fn = '&%s::%s' % (ns, readable['name'])
        ret, ptypes = readable['signature']
        ints = [t for t in ptypes if not is_float_type(t)]
        conv = []
        ii = fi = 0
        for t in ptypes:
            if is_float_type(t):
                conv.append(self.ftext(float_args[fi]) if fi < len(float_args) else 'ee::Float(0.0f) /* TODO */')
                fi += 1
            else:
                v = int_args[ii] if ii < len(int_args) else None
                ii += 1
                if v is not None and v.k == 'qword' and 'Qword' not in t:
                    # (The register's upper half was set, by an lq of the same
                    # register; the signature takes the low one.)
                    v = v.a[0]
                conv.append(self.convert_arg(t, v, ns))
        if readable.get('shared') or self.ws.folded(info.target):
            call = 'game::%s<%s, 0x%08Xu>' % ('tailCallAt' if tail else 'callAt', fn, info.target)
        else:
            call = 'game::%s<%s>' % ('tailCall' if tail else 'call', fn)
        text = '%s(%s)' % (call, ', '.join(conv))
        if rtype:
            text = self.convert_result(ret, text, rtype, ns)
        return text

    def convert_arg(self, t, v, ns):
        if v is None:
            return '{} /* TODO: not set */'
        text = self.r(v)
        tt = t.strip()
        if tt.endswith('*'):
            # As an ee::Ptr: the callee gets the host pointer (the same
            # memory), but a call answered from a recording or dispatched to
            # the original passes the whole address, not the RAM offset
            # ee::at's pointer gives back (paths5: fn_15AF10 passed
            # 0x038001F0 for the original's 0x3F8001F0; drafter23).
            base = qualify(tt[:-1].strip(), ns)
            if base.replace('const', '').strip() == 'void':
                return 'ee::at<%s>(%s)' % (base, text) # (no ee::Ptr<void>)
            if v.k == 'c' or self.type_of(v) != 'u32':
                text = 'static_cast<uint32_t>(%s)' % text
            return 'ee::Ptr<%s>(%s)' % (base, text)
        m = re.match(r'^(const\s+)?ee::Ptr<(.*)>$', tt)
        if m:
            if v.k == 'c' or self.type_of(v) != 'u32':
                text = 'static_cast<uint32_t>(%s)' % text
            return 'ee::Ptr<%s>(%s)' % (qualify(m.group(2).strip(), ns), text)
        if tt in ('ee::Qword', 'Qword'):
            if v.k == 'qword':
                return text
            if v.k == 'c' and v.v == 0:
                return 'ee::Qword{}'  # (zero, whole: a constant has no upper half to keep)
            # (Not reached: callee_qword_args makes every Qword argument whole.)
            return 'ee::Qword{static_cast<uint64_t>(static_cast<int64_t>(%s)), 0} /* TODO: the upper half */' % text
        if tt == 'bool':
            return '%s != 0' % self.paren(v, 9)
        if re.match(r'^(const\s+)?(u?int(8|16|32|64)_t|int|unsigned|uint32_t)$', tt):
            return text
        return 'static_cast<%s>(%s)' % (qualify(tt, ns), text)

    def convert_result(self, ret, text, rtype, ns):
        rt = ret.strip()
        if rt.endswith('*'):
            return 'ee::addressOf(%s)' % text
        if rt.startswith('ee::Ptr<') or rt.startswith('const ee::Ptr<'):
            return '%s.address' % text
        if is_float_type(rt):
            return text
        if rt in ('int32_t', 'uint32_t', 'int', 'unsigned', 'bool', 'uint64_t', 'int64_t', 'uint8_t', 'int8_t', 'uint16_t', 'int16_t'):
            return text
        return 'static_cast<%s>(%s)' % (rtype, text)

    # ---- statements ----------------------------------------------------------------

    def stmts(self, ss, indent, out):
        dead = False
        for s in ss:
            if s.k == 'label' and s.target in self.d.goto_targets:
                dead = False
            if dead and s.k != 'label' and not self.has_target_label(s):
                continue
            if dead and self.has_target_label(s):
                dead = False
            self.stmt(s, indent, out)
            after = getattr(self.d, 'aftercall_stmt', {}).get(id(s))
            if after is not None:
                # The call's return address starts an entry block other paths
                # reach too: its fall-through only (ee/dispatch.h).
                self.d.includes.add('ee/dispatch.h')
                out.append('    ' * indent + 'ee::afterCall(0x%06X);' % after)
            if s.k in ('ret', 'tail', 'goto', 'cont', 'brk'):
                dead = True

    def stmt(self, s, ind, out):
        pad = '    ' * ind
        k = s.k
        d = self.d
        if k in ('brk', 'ret', 'tail') and self.counting and self.counting[-1]:
            # Out of a counted loop: its branches back, charged at once.
            out.append(pad + 'ee::chargeLoop(passes);')
            self.counting.append(False)
            self.stmt(s, ind, out)
            self.counting.pop()
            return
        for w in d.decl_before.get(id(s), []):
            out.append(pad + '%s %s%s;' % (CTYPE[w.t], w.name, initial_value(w)))
        if k == 'label':
            if s.target in d.goto_targets:
                out.append(('    ' * max(ind - 1, 0)) + '  %s:' % d.label(s.target))
            if s.text != 'loop':
                self.marker(s.target, pad, out)
            return
        if k == 'mark':
            self.marker(s.target, pad, out)
            return
        if k in ('assign', 'expr') and self.self_call(s.x):
            # A call of itself (drafter19): generated code jumps to its own
            # start, and the inner call's return (a jr to the address after
            # the jal) leaves the C++ function, so the original returns
            # through the scheduler from there (8 cycles and a check, every
            # caller up the chain too): ee::selfCall models it (ee/abi.h).
            text = self.r(s.x)
            if k == 'assign':
                w = s.web
                if d.decl_inline.get(id(s)):
                    out.append(pad + '%s %s{};' % (CTYPE[w.t], w.name))
                text = '%s = %s' % (w.name, text)
            back = s.x.v.pc + 8 if s.x.v.pc else None
            if back is not None and self.d.sp_model and back in self.d.sp_leader_set:
                # Its return address starts an entry block: the original goes
                # on there slice by slice (drafter20).
                out.append(pad + 'ee::selfCall(0x%06X, [&] { %s; }); // returns to an entry block' % (back, text))
            else:
                out.append(pad + 'ee::selfCall([&] { %s; }); // returns to 0x%X' % (text, back)
                           if back is not None else pad + 'ee::selfCall([&] { %s; });' % text)
            return
        if k == 'assign':
            w = s.web
            text = self.r(s.x)
            if s.x.k == 'call' and s.x.v.ret_t is None:
                text = text
            if w.t in ('u64', 'i64') and self.sign_extended(s.x):
                text = 'static_cast<int32_t>(%s)' % text
            decl = d.decl_inline.get(id(s))
            if decl:
                out.append(pad + '%s%s %s = %s;' % ('const ' if len(w.defs) == 1 else '', CTYPE[w.t], w.name, text))
            else:
                out.append(pad + '%s = %s;' % (w.name, text))
            return
        if k == 'expr':
            out.append(pad + self.r(s.x) + ';')
            return
        if k in ('store', 'storeu'):
            if k == 'storeu':
                out.append(pad + 'storeUnaligned<%s>(%s, %s);' % (CTYPE[s.t], self.r(s.addr), self.r(s.x)))
                return
            io = self.io_access(s.t, s.addr, s.m)
            lhs = None if io else self.deref(s.t, s.addr, s.m)
            val = s.x
            vt = self.type_of(val)
            vtext = self.r(val)
            if s.t in ('u8', 'i8', 'u16', 'i16') and val.k != 'c':
                vtext = 'static_cast<%s>(%s)' % (CTYPE[s.t], vtext)
            elif s.t == 'u64' and vt not in ('u64', 'i64') and val.k != 'c':
                if self.sign_extended(val):
                    vtext = 'static_cast<int32_t>(%s)' % vtext
                vtext = 'static_cast<uint64_t>(static_cast<int64_t>(%s))' % vtext
            elif s.t == 'i32' and vt in ('u64', 'i64'):
                vtext = 'static_cast<int32_t>(%s)' % vtext
            elif s.t == 'i32' and val.k == 'c':
                vtext = self.const(val.v, 'i32') if -0x10000 < val.v < 0x10000 else self.const(val.v, 'u32')
            if io:
                self.d.uses_io = True
                out.append(pad + 'ee::io::%s<%s>(%s, %s);' % ('store' if io[0] == 'io' else 'storeAny', CTYPE[s.t],
                                                          io[1], vtext))
                return
            out.append(pad + '%s = %s;' % (lhs, vtext))
            return
        if k == 'vld':
            out.append(pad + 'ctx->vu0_vf[%d] = _mm_castsi128_ps(_mm_loadu_si128(ee::at<__m128i>(%s))); // LQC2' % (
                s.t, self.addr_expr(s.addr)))
            return
        if k == 'vst':
            out.append(pad + '_mm_storeu_si128(ee::at<__m128i>(%s), _mm_castps_si128(ctx->vu0_vf[%d])); // SQC2' % (
                self.addr_expr(s.addr), s.t))
            return
        if k == 'vset':
            lo = self.r(s.x)
            hi = self.r(s.addr)
            if s.x.k != 'c' and self.type_of(s.x) not in ('u64', 'i64'):
                lo = 'static_cast<int64_t>(%s)' % lo
            out.append(pad + 'ctx->vu0_vf[%d] = _mm_castsi128_ps(_mm_set_epi64x(static_cast<int64_t>(%s), static_cast<int64_t>(%s))); // QMTC2' % (
                s.t, hi, lo))
            return
        if k == 'vraw':
            text = s.text
            subs = s.x.v

            def sub(m):
                kind, reg = m.group(1), m.group(2)
                e = subs.get(reg)
                if e is None:
                    return '0'
                t = self.r(e)
                return 'static_cast<%s>(%s)' % ({'U32': 'uint32_t', 'S32': 'int32_t', 'U64': 'uint64_t', 'S64': 'int64_t'}[kind], t)
            text = re.sub(r'\{(U32|S32|U64|S64):(\w+)\}', sub, text)
            if s.t:
                out.append(pad + '// ' + s.t)
            for line in text.split('\n'):
                out.append(pad + line.strip())
            return
        if k == 'note':
            out.append(pad + '// ' + s.text)
            return
        if k == 'unsup':
            out.append(pad + '// TODO(draft): not lifted: ' + s.text)
            return
        if k == 'if':
            cond = self.r(s.x)
            body = []
            self.stmts(s.body, ind + 1, body)
            if s.orelse:
                els = []
                self.stmts(s.orelse, ind + 1, els)
                if not body:
                    out.append(pad + 'if (%s)' % self.r(negate(s.x)))
                    out.append(pad + '{')
                    out.extend(els)
                    out.append(pad + '}')
                    return
                last = [x for x in s.body if x.k != 'label' or x.target in d.goto_targets]
                ends = last and last[-1].k in ('ret', 'tail', 'goto', 'cont', 'brk')
                self.if_block(pad, cond, body, out)
                if ends:
                    out.extend(l[4:] if l.startswith(pad + '    ') else l for l in els)
                    return
                out.append(pad + 'else')
                simple = len(els) == 1 and not els[0].strip().startswith(('if', 'for', '//', 'switch')) and \
                    not els[0].strip().endswith(':')
                if simple and len(body) == 1:
                    out.append(els[0])
                    return
                out.append(pad + '{')
                out.extend(els)
                out.append(pad + '}')
                return
            if len(body) == 1 and not body[0].strip().startswith(('if', 'for', '//', 'switch')) and ':' != body[0].strip()[-1:]:
                out.append(pad + 'if (%s)' % cond)
                out.append(body[0])
                return
            if not body:
                return
            out.append(pad + 'if (%s)' % cond)
            out.append(pad + '{')
            out.extend(body)
            out.append(pad + '}')
            return
        if k == 'loop':
            # A loop's exit from inside a jump table's switch cannot be a
            # `break` (it would leave the switch: fn_157A70's table target
            # 0x157E38, the code after its loop, ran case 0x157AE8's pool
            # allocation instead): a goto to a label after the loop.
            exits = getattr(self, 'loop_exits', None)
            if exits is None:
                exits = self.loop_exits = []
            self.loop_ids = getattr(self, 'loop_ids', 0) + 1
            exits.append([self.loop_ids, False])
            if s.t == 'counted':
                out.append(pad + 'for (uint32_t passes = 0;; ++passes) // the original branches back once a pass')
                out.append(pad + '{')
                self.counting.append(True)
                self.stmts(s.body, ind + 1, out)
                self.counting.pop()
                out.append(pad + '}')
            else:
                if d.sp_model:
                    out.append(pad + 'for (;; %s) // the original\'s branch back' % self.back_edge(s.target))
                    out.append(pad + '{')
                    self.marker(s.target, pad + '    ', out)
                else:
                    out.append(pad + 'for (;; ee::chargeLoop(1)) // the original\'s branch back: 32 EE cycles a pass')
                    out.append(pad + '{')
                self.counting.append(False)
                self.stmts(s.body, ind + 1, out)
                self.counting.pop()
                out.append(pad + '}')
            loop_id, used = exits.pop()
            if used:
                out.append(pad + 'loop_%d_exit:;' % loop_id)
            return
        if k == 'goto':
            out.append(pad + 'goto %s;' % d.label(s.target))
            return
        if k == 'charge':
            out.append(pad + '%s; // a branch back' % self.back_edge(s.target))
            return
        if k == 'brk':
            exits = getattr(self, 'loop_exits', None)
            if getattr(self, 'switch_depth', 0) > 0 and exits and exits[-1][0] < getattr(self, 'switch_loop', 0) + 1:
                exits[-1][1] = True
                out.append(pad + 'goto loop_%d_exit; // the loop\'s exit (a break would leave the switch)' % exits[-1][0])
            else:
                out.append(pad + 'break;')
            return
        if k == 'cont':
            out.append(pad + 'continue;')
            return
        if k == 'ret':
            for reg, v in s.leaves or ():
                text = self.r(v)
                # The caller's own value on this path: the context has it still.
                num = str(GPR.index(reg)) if reg in GPR else reg[1:]
                if re.fullmatch(r'(static_cast<\w+>\()?ee::caller(?:Register|Float)\(%s(?: /\* \w+ \*/)?\)\)?' % num,
                                text):
                    continue
                d.includes.add('ee/leave.h')
                if reg.startswith('f'):
                    out.append(pad + 'ee::leaveFloat(%s, %s);' % (reg[1:], text))
                else:
                    out.append(pad + 'ee::leave(ee::reg::%s, %s);' % (reg, text))
            if s.x is None or d.ret is None:
                out.append(pad + 'return;')
            else:
                v = s.x
                text = self.r(v)
                if d.ret == 'v0' and self.type_of(v) in ('u64', 'i64') and not getattr(d, 'ret_wide', False):
                    text = 'static_cast<int32_t>(%s)' % text
                out.append(pad + 'return %s;' % text)
            return
        if k == 'tail':
            text = self.r(s.x)
            if d.ret:
                out.append(pad + 'return %s; // the original\'s tail call' % text)
            else:
                out.append(pad + '%s; // the original\'s tail call' % text)
                out.append(pad + 'return;')
            return
        if k == 'switch':
            out.append(pad + 'switch (%s) // the original\'s jump table' % self.as_u32(s.x)[0])
            out.append(pad + '{')
            # (brk inside: the innermost loop is outside this switch unless a
            # loop opens inside a case; switch_loop is the loop count then.)
            saved_switch = (getattr(self, 'switch_depth', 0), getattr(self, 'switch_loop', 0))
            self.switch_depth = saved_switch[0] + 1
            self.switch_loop = getattr(self, 'loop_ids', 0)
            for target, body in s.cases:
                out.append(pad + 'case 0x%08Xu:' % target)
                inner = []
                if s.m == 'dispatch':
                    # The original's JR through this table is dispatched at run
                    # time (no switch in the generated code): 8 EE cycles and a
                    # check, then the function goes on from the scheduler as the
                    # table maps the target (ee/dispatch.h). 0x1B4438, the
                    # skater box's x table 0x264A30 (verdicts).
                    d.includes.add('ee/dispatch.h')
                    d.stats['jump_dispatch'] += 1
                    entry = d.sp_model and target in d.sp_leader_set
                    inner.append(pad + '    ee::jumpDispatch(0x%06X, %s);' % (target, 'true' if entry else 'false'))
                elif d.sp_model and s.jr is not None and d.sp_slice(s.jr) and \
                        d.sp_slice(target) != d.sp_slice(s.jr):
                    # The recompiler's switch: in place in the whole function,
                    # from the JR's entry block an IndirectJump to a case
                    # outside it (ee/dispatch.h tableJump; drafter23).
                    d.includes.add('ee/dispatch.h')
                    d.stats['table_jump'] += 1
                    entry = target in d.sp_leader_set
                    inner.append(pad + '    ee::tableJump(0x%06X, 0x%06X, %s);' % (
                        target, d.sp_slice(s.jr), 'true' if entry else 'false'))
                n_pre = len(inner)
                self.stmts(body, ind + 1, inner)
                if len(inner) == n_pre:
                    inner.append(pad + '    goto %s;' % d.label(target))
                    d.goto_targets.add(target)
                out.extend(inner)
            self.switch_depth, self.switch_loop = saved_switch
            # Every word of the table is a case, and the compiled code keeps
            # the index in the table (its bounds check, or the compiler knew
            # the range: find_unchecked_tables): nothing reaches this but a
            # bad index, which says so.
            d.includes.add('ee/dispatch.h')
            out.append(pad + 'default:')
            out.append(pad + '    ee::unlistedTarget(0x%06X);' % (s.jr or 0))
            out.append(pad + '    break;')
            out.append(pad + '}')
            return
        out.append(pad + '// ? ' + k)

    def back_edge(self, target):
        """A branch back to target, charged as the original's (ee/abi.h)."""
        d = self.d
        if not d.sp_model or target is None or isinstance(target, tuple):
            return 'ee::chargeLoop(1)'
        if target in d.sp_leader_set:
            return 'ee::loopBack(0x%06X)' % target
        return 'ee::loopBackWhole(0x%06X, 0x%06X)' % (target, d.sp_slice(target))

    def marker(self, target, pad, out):
        """The start of an original entry block (ee::block) or an address the
        table resumes in the whole function, reached from another slice."""
        d = self.d
        if not d.sp_model or isinstance(target, tuple) or target is None:
            return
        if target in d.sp_leader_set:
            out.append(pad + 'ee::block(0x%06X%s);' % (target, ', true' if target in d.sp_after_call_label else ''))
        elif target in d.sp_cross_whole:
            out.append(pad + 'ee::blockWhole(0x%06X, 0x%06X);' % (target, d.sp_slice(target)))

    def has_target_label(self, s):
        if s.k == 'label':
            return s.target in self.d.goto_targets
        for sub in (s.body, s.orelse):
            if sub and any(self.has_target_label(x) for x in sub):
                return True
        if s.cases:
            return any(self.has_target_label(x) for _, body in s.cases for x in body)
        return False

    def if_block(self, pad, cond, body, out):
        if len(body) == 1 and not body[0].strip().startswith(('if', 'for', '//', 'switch')) and not body[0].strip().endswith(':'):
            out.append(pad + 'if (%s)' % cond)
            out.append(body[0])
            return
        out.append(pad + 'if (%s)' % cond)
        out.append(pad + '{')
        out.extend(body)
        out.append(pad + '}')

    def addr_expr(self, x):
        if x.k == 'sp':
            return 'frame.address(0x%X)' % (x.v + self.d.lifter.frame_size)
        a = const_address(x)
        if a is not None:
            return '0x%08Xu' % a
        return self.r(x)


def qualify(t, ns):
    """Qualifies the type names in t that are not std / ee / builtin with ns."""
    builtin = {'const', 'char', 'int', 'unsigned', 'signed', 'short', 'long', 'float', 'double', 'bool', 'void',
               'int8_t', 'uint8_t', 'int16_t', 'uint16_t', 'int32_t', 'uint32_t', 'int64_t', 'uint64_t', 'size_t'}

    def sub(m):
        word = m.group(0)
        start = m.start()
        if word in builtin or (start >= 2 and t[start - 2:start] == '::'):
            return word
        if t[m.end():m.end() + 2] == '::':
            return word if word in ('ee', 'std', 'zge', 'aggressive', 'game', 'sce') else ns + '::' + word
        return ns + '::' + word
    return re.sub(r'\b[A-Za-z_]\w*\b', sub, t)


def const_address(x):
    if x.k == 'c':
        return x.v & 0xFFFFFFFF
    if x.k == 'bin' and x.v == '+' and x.a[0].k == 'c' and x.a[1].k == 'c':
        return (x.a[0].v + x.a[1].v) & 0xFFFFFFFF
    return None


# ---------------------------------------------------------------------------
# Draft output
# ---------------------------------------------------------------------------

def _label(self, k):
    return 'at_%x' % k if not isinstance(k, tuple) else 'at_%x_likely' % k[1]


def _callee_constant(self, address):
    if address in self.constants:
        return self.constants[address][0]
    known = self.ws.constants.get(address)
    name = None
    comment = ''
    sdk = self.ws.name_of(address)
    if known:
        names = defaultdict(int)
        for n, c in known:
            names[n] += 1
        name = max(sorted(names), key=lambda n: names[n])
        comment = next((c for n, c in known if n == name and c), '')
        if readable_names:
            # (readable_names agent) a name the sources give several addresses (kFree):
            # the function's own (kHeapFree)
            name = readable_names.unambiguous_constant(self.ws, address, name)
    elif sdk and (not readable_names or readable_names.function_name(address) in (None, sdk) or
                  self.ws.readable.get(address)):
        name = 'k' + sdk[0].upper() + re.sub(r'\W', '', sdk[1:])
    elif readable_names and readable_names.function_name(address):
        name = readable_names.constant_for(readable_names.function_name(address))
    else:
        name = 'kFun%06X' % address
    taken = {v[0] for v in self.constants.values()}
    base = name
    n = 2
    while name in taken:
        name = '%s%d' % (base, n)
        n += 1
    ghidra = self.ws.ghidra_names.get(address, 'FUN_%08x' % address)
    readable = self.ws.readable.get(address)
    extra = []
    if readable and readable['kind'] != 'PLANNED' and address not in BY_ADDRESS:
        extra.append('readable: %s, %s' % (readable['name'], readable['status']))
        if address in self.ws.hooked:
            extra.append('hooked by a game override: called by address')
    if comment:
        if comment.startswith(ghidra):
            comment = comment[len(ghidra):].strip()
            if comment.startswith('(') and comment.endswith(')'):
                comment = comment[1:-1]
        if comment:
            extra.insert(0, comment)
    extra = [e for i, e in enumerate(extra) if e and e not in extra[:i] and not any(e in o for o in extra if o is not e)]
    self.constants[address] = (name, '%s%s' % (ghidra, (' (' + '; '.join(extra) + ')') if extra else ''))
    return name


def _global_name(self, address, t):
    key = address
    if key in self.globals:
        name, gt, _ = self.globals[key]
        if gt != t:
            return None
        return name
    for a2, (n2, t2, _) in self.globals.items():
        if a2 == address and t2 != t:
            return None
    known = self.ws.globals.get(address)
    comment = 'DAT_%08x' % address
    if known:
        comment += ' (%s)' % ', '.join('%s::%s' % (ns, n) if ns else n for n, _, ns in known[:2])
    name = (readable_names and readable_names.global_name(address, known, t)) or 'g_%06X' % address
    if readable_names and readable_names.global_guessed(address) and name == readable_names.state()['globals'].get(address):
        comment += '; its name is a low-confidence guess (readable_names.tsv)'   # (readable_names agent)
    if address >= 0x02000000 and address < 0x70000000 or address >= 0x70004000:
        return None
    # A name another address of this draft already has (two table rows
    # with one name): this one gets its address, so the draft's text never
    # names two globals alike (the file pass below renames by text; names6:
    # g_worldVertexFlags for 0x27A8DC and 0x27A90C made both the second, and
    # buildLinePacket's store landed at the wrong address; drafter25).
    # The same for a hand-written global's name at another address (a table
    # row that took it): the draft's own would hide that one in the file.
    by_name = getattr(self.ws, '_global_addresses', None)
    if by_name is None:
        by_name = self.ws._global_addresses = defaultdict(set)
        for a2, rows in self.ws.globals.items():
            for n2, _, _ in rows:
                by_name[n2].add(a2)
    if any(n2 == name and a2 != address for a2, (n2, _, _) in self.globals.items()) or \
            any(n2 == name for a2, (n2, _) in self.constants.items()) or \
            by_name.get(name, set()) - {address}:
        name = '%s_%06X' % (name, address)
    self.globals[key] = (name, t, comment)
    return name


Draft.label = _label
Draft.callee_constant = _callee_constant
Draft.global_name = _global_name


def declare(draft, rend):
    """Variable names and where they are declared."""
    d = draft
    used = set()
    counts = defaultdict(int)
    webs = set()
    L = d.lifter
    # Webs still referenced.
    def visit_stmts(ss):
        for s in ss:
            if s.k == 'assign':
                webs.add(s.web)
            for e in s.exprs():
                for n in all_nodes(e):
                    if n.k == 'var':
                        webs.add(n.v)
            if s.body:
                visit_stmts(s.body)
            if s.orelse:
                visit_stmts(s.orelse)
            if s.cases:
                for _, body in s.cases:
                    visit_stmts(body)
    visit_stmts(d.body)
    # Values used as an address are EE pointers: uint32_t.
    def bases(x):
        if x.k == 'var':
            return [x.v]
        if x.k == 'bin' and x.v == '+':
            return bases(x.a[0]) + (bases(x.a[1]) if x.a[0].k == 'c' else [])
        return []

    def mark(ss):
        for s in ss:
            exprs = list(s.exprs())
            if s.k in ('store', 'storeu', 'vld', 'vst') and s.addr is not None:
                for w in bases(s.addr):
                    if w.t == 'i32':
                        w.t = 'u32'
            for e in exprs:
                for n in all_nodes(e):
                    if n.k in ('ld', 'ldu'):
                        for w in bases(n.a[0]):
                            if w.t == 'i32':
                                w.t = 'u32'
            for sub in (s.body, s.orelse):
                if sub:
                    mark(sub)
            if s.cases:
                for _, body in s.cases:
                    mark(body)
    mark(d.body)
    for w in webs:
        if w.param and w.t == 'i32':
            w.t = 'u32'
        if w.param and w.param in getattr(d, 'wide_params', ()):
            w.t = 'u64'
    names = {}
    for w in sorted(webs, key=lambda w: w.id):
        if w.param:
            if w.param.endswith('^'):
                w.name = d.param_names[w.param[:-1]] + '.high'
            elif w.param in d.qword_params:
                w.name = d.param_names[w.param] + '.low'
            else:
                w.name = d.param_names[w.param]
            continue
        base = w.reg.replace('^', '_hi')
        if base.startswith('cond_'):
            base = 'taken'
        if base in ('lo', 'hi', 'lo1', 'hi1'):
            base = {'lo': 'product', 'hi': 'high', 'lo1': 'product1', 'hi1': 'high1'}[base]
        if base == 'facc':
            base = 'acc'
        counts[base] += 1
        w.name = base if counts[base] == 1 else '%s_%d' % (base, counts[base])
    # Rename the first of a register to `name_1` when there are more.
    for w in webs:
        if not w.param:
            base = w.reg.replace('^', '_hi')
    d.decl_inline = {}
    d.decl_top = []
    has_goto = bool(d.goto_targets)
    # Sequence numbers and scopes.
    seq = [0]
    info = {}

    after_label = {}   # id(statement) -> a goto target follows it in its statement list

    def targets_in(ss):
        for s in ss:
            if s.k == 'label' and s.target in d.goto_targets:
                return True
            for sub in (s.body, s.orelse):
                if sub and targets_in(sub):
                    return True
            if s.cases and any(targets_in(b) for _, b in s.cases):
                return True
        return False

    def number(ss, scope):
        for i, s in enumerate(ss):
            after_label[id(s)] = targets_in(ss[i + 1:])
            seq[0] += 1
            s.seq = seq[0]
            s.scope = scope
            if s.body is not None:
                number(s.body, scope + (id(s), 'b'))
            if s.orelse is not None:
                number(s.orelse, scope + (id(s), 'e'))
            if s.cases:
                for i, (_, body) in enumerate(s.cases):
                    number(body, scope + (id(s), i))
            if s.k == 'label' and s.target in d.goto_targets:
                labels_at[s.target] = (s.seq, scope)
            if s.k == 'goto':
                gotos.append((s.seq, scope, s.target))
    labels_at = {}
    gotos = []
    scope_lists = {}

    def lists(ss, scope):
        scope_lists[scope] = ss
        for s in ss:
            if s.body is not None:
                lists(s.body, scope + (id(s), 'b'))
            if s.orelse is not None:
                lists(s.orelse, scope + (id(s), 'e'))
            if s.cases:
                for i, (_, body) in enumerate(s.cases):
                    lists(body, scope + (id(s), i))

    def contains_seq(st, q):
        for sub in (st.body, st.orelse):
            if sub and any(x.seq == q or contains_seq(x, q) for x in sub):
                return True
        if st.cases:
            return any(x.seq == q or contains_seq(x, q) for _, b in st.cases for x in b)
        return False
    number(d.body, ())
    lists(d.body, ())
    d.decl_before = {}
    # Transfers into a scope: a switch case's goto too.

    def bypasses(decl):
        """Whether some goto enters decl's scope past it (C++ forbids that
        for a declaration with an initialiser)."""
        S = decl.scope
        for target, (lseq, lscope) in labels_at.items():
            if lseq <= decl.seq or lscope[:len(S)] != S:
                continue
            for gseq, gscope, gt in gotos:
                if gt == target and not (gseq > decl.seq and gscope[:len(S)] == S):
                    return True
        return False
    defs = defaultdict(list)
    uses = defaultdict(list)

    def collect(ss):
        for s in ss:
            if s.k == 'assign':
                defs[s.web].append(s)
            exprs = list(s.exprs())
            for e in exprs:
                for n in all_nodes(e):
                    if n.k == 'var':
                        uses[n.v].append(s)
            if s.body:
                collect(s.body)
            if s.orelse:
                collect(s.orelse)
            if s.cases:
                for _, body in s.cases:
                    collect(body)
    collect(d.body)
    loops = set()

    def loop_scopes(ss, scope):
        for s in ss:
            if s.k == 'loop':
                loops.add(scope + (id(s), 'b'))
            if s.body is not None:
                loop_scopes(s.body, scope + (id(s), 'b'))
            if s.orelse is not None:
                loop_scopes(s.orelse, scope + (id(s), 'e'))
            if s.cases:
                for i, (_, body) in enumerate(s.cases):
                    loop_scopes(body, scope + (id(s), i))
    loop_scopes(d.body, ())
    for w in sorted(webs, key=lambda w: w.id):
        if w.param:
            continue
        ds = defs.get(w, [])
        us = uses.get(w, [])
        if ds and not w.entry:
            s = min(ds, key=lambda x: x.seq)
            ok = all(u.seq > s.seq and u.scope[:len(s.scope)] == s.scope for u in us + [x for x in ds if x is not s])
            # A goto may not jump past the declaration into its scope.
            if ok and has_goto and after_label.get(id(s)) and bypasses(s):
                ok = False
            # Nor may a later goto back to a label before it reach a use
            # (the declaration is passed again: fine) ... only forward jumps matter.
            if ok:
                d.decl_inline[id(s)] = True
                continue
        # The narrowest scope holding every definition and use: declared
        # just before the first statement there that touches it.
        touch = ds + us
        if touch and not w.entry:
            common = touch[0].scope
            for t in touch[1:]:
                n = 0
                while n < len(common) and n < len(t.scope) and common[n] == t.scope[n]:
                    n += 1
                common = common[:n]
            if len(common) % 2:
                common = common[:-1]
            lst = scope_lists.get(common)
            if lst is not None:
                first = min(touch, key=lambda x: x.seq)
                anchor = None
                for st in lst:
                    if st.seq is not None and (st is first or contains_seq(st, first.seq)):
                        anchor = st
                        break
                if anchor is not None:
                    probe = S('decl', web=w)
                    probe.seq = anchor.seq - 0.5
                    probe.scope = common
                    if not (has_goto and bypasses(probe)):
                        d.decl_before.setdefault(id(anchor), []).append(w)
                        continue
        d.decl_top.append(w)


# ---------------------------------------------------------------------------
# VU0 sequences as ee::vu0::Machine calls
# ---------------------------------------------------------------------------

_LQC2 = re.compile(r'^(\s*)ctx->vu0_vf\[(\d+)\] = _mm_castsi128_ps\(_mm_loadu_si128\(ee::at<__m128i>\((.*)\)\)\); // LQC2$')
_SQC2 = re.compile(r'^(\s*)_mm_storeu_si128\(ee::at<__m128i>\((.*)\), _mm_castps_si128\(ctx->vu0_vf\[(\d+)\]\)\); // SQC2$')
_QMTC2 = re.compile(r'^(\s*)ctx->vu0_vf\[5\] = _mm_castsi128_ps\(_mm_set_epi64x\(static_cast<int64_t>\((.*)\), '
                    r'static_cast<int64_t>\((.*)\)\)\); // QMTC2$')
_T0HI = re.compile(r'^(\s*)((?:const )?(?:uint64_t )?\w+) = _mm_cvtsi128_si64\(_mm_unpackhi_epi64\(_mm_castps_si128\(ctx->vu0_vf\[5\]\), '
                   r'_mm_castps_si128\(ctx->vu0_vf\[5\]\)\)\);$')
_VX5 = 'ee::Float(_mm_cvtss_f32(ctx->vu0_vf[5]))'


def _strip_mask(addr):
    """LQC2 / SQC2's address without the draft's `& ~0xFu` (the Machine masks)."""
    m = re.match(r'^\((.*) & ~0xFu\)$', addr)
    return m.group(1) if m else addr


def _float_of_low(text):
    """QMTC2's low doubleword as the float MFC1 put there, or None."""
    m = re.match(r'^(?:static_cast<int64_t>\()?static_cast<int64_t>\(static_cast<int32_t>\((.*)\.bits\(\)\)\)\)?$', text)
    if m and text.count('(') == text.count(')'):
        inner = m.group(1)
        if inner.startswith('(') and inner.endswith(')') and inner.count('(') == 1:
            inner = inner[1:-1]
        return inner
    m = re.match(r'^(0x[0-9A-Fa-f]+)u?$', text)
    if m:
        v = int(m.group(1), 16)
        if v < 0x100000000:
            f = struct.unpack('<f', struct.pack('<I', v))[0]
            if struct.unpack('<I', struct.pack('<f', float(repr(f))))[0] == v:
                return 'ee::Float(%rf)' % f
    return None


_VU_DEST = {8: 'kX', 4: 'kY', 2: 'kZ', 1: 'kW', 12: 'kXy', 10: 'kXz', 9: 'kXw', 6: 'kYz', 5: 'kYw', 3: 'kZw', 14: 'kXyz',
            13: 'kXyw', 11: 'kXzw', 7: 'kYzw', 15: 'kXyzw'}


def split_top(text, sep):
    """text cut at sep where no bracket is open."""
    parts, depth, start, at = [], 0, 0, 0
    while at < len(text):
        c = text[at]
        depth += c in '([{'
        depth -= c in ')]}'
        if depth == 0 and text.startswith(sep, at):
            parts.append(text[start:at])
            at += len(sep)
            start = at
            continue
        at += 1
    parts.append(text[start:])
    return parts


def fold_vu_expression(text):
    """The recompiled expression through ee::vu0's calls."""
    text = re.sub(r'_mm_shuffle_ps\(([^(),]+\[\d+\]|ctx->vu0_acc), \1, _MM_SHUFFLE\((\d),\2,\2,\2\)\)', r'ee::vu0::lane<\2>(\1)', text)
    return re.sub(r'\bPS2_V(ADD|SUB|MUL)\(', lambda m: 'ee::vu0::%s(' % m.group(1).lower(), text)


def fold_vu_statement(line):
    """An FMAC instruction as the recompiler writes it (`{ __m128 mul_res = ...; __m128 res = ...; [__m128i mask =
    ...;] dst = blend(dst, res, mask); ps2_vu0_fmac_flags(ctx, res, dest); }`) as one call of ee::vu0; any other
    line as it is. The temporaries are the recompiler's, nothing to name (names9)."""
    code = line.strip()
    if not code.startswith('{ __m128 ') or not code.endswith(' }'):
        return line
    parts = [p.strip() for p in split_top(code[2:-2].strip(), ';') if p.strip()]
    values = {}
    while parts and re.match(r'^__m128i? \w+ = ', parts[0]):
        name, value = parts.pop(0).split(' ', 1)[1].split(' = ', 1)
        for known, text in values.items():
            value = re.sub(r'\b%s\b' % known, lambda _: text, value)
        values[name] = value
    if len(parts) != 2 or 'res' not in values:
        return line
    m = re.match(r'^(ctx->vu0_acc|ctx->vu0_vf\[(\d+)\]) = (?:_mm_blendv_ps|PS2_VBLEND)\((.*), res, _mm_castsi128_ps\((.*)\)\)$', parts[0])
    f = re.match(r'^ps2_vu0_fmac_flags\(ctx, res, (\d+)u\)$', parts[1])
    if not m or not f or m.group(3) != m.group(1):
        return line
    dest = int(f.group(1))
    mask = re.match(r'^_mm_set_epi32\((-?\d), (-?\d), (-?\d), (-?\d)\)$', values.get(m.group(4), m.group(4)))
    if dest not in _VU_DEST or not mask or tuple(int(x) for x in mask.groups()) != tuple(-(dest >> i & 1) for i in (0, 1, 2, 3)):
        return line
    result = fold_vu_expression(values['res'])
    target = 'setAcc(ctx, ' if m.group(2) is None else 'setVf(ctx, %s, ' % m.group(2)
    return '%see::vu0::%see::vu0::%s, %s);' % (line[:len(line) - len(line.lstrip())], target, _VU_DEST[dest], result)


def next_io_doubleword(first, second):
    """Whether `second` names the doubleword after `first`: kFifo and kFifoHigh, or two addresses."""
    if second == first + 'High':
        return True
    hex_a, hex_b = (re.match(r'^0x([0-9A-Fa-f]+)u?$', x) for x in (first, second))
    return bool(hex_a and hex_b) and int(hex_b.group(1), 16) == int(hex_a.group(1), 16) + 8


_IO_STORE = re.compile(r'^(\s*)ee::io::store<uint64_t>\((\w+), (.*)\);$')
_IO_LOAD = re.compile(r'^(\s*)(?:const )?uint64_t (\w+) = ee::io::load<uint64_t>\((\w+)\);$')


def join_io_quadwords(lines):
    """An LQ or SQ of an I/O register is lifted as two doublewords: one 16-byte access, as the original makes
    (sceDevVif0PutFifo's store to the FIFO)."""
    out, i = [], 0
    while i < len(lines):
        following = lines[i + 1] if i + 1 < len(lines) else ''
        a, b = _IO_STORE.match(lines[i]), _IO_STORE.match(following)
        if a and b and next_io_doubleword(a.group(2), b.group(2)):
            out.append('%see::io::storeQuadword(%s, ee::Qword{%s, %s});' % (a.group(1), a.group(2), a.group(3), b.group(3)))
            i += 2
            continue
        a, b = _IO_LOAD.match(lines[i]), _IO_LOAD.match(following)
        if a and b and next_io_doubleword(a.group(3), b.group(3)):
            out.append('%sconst ee::Qword %sQuad = ee::io::loadQuadword(%s);' % (a.group(1), a.group(2), a.group(3)))
            out.append('%sconst uint64_t %s = %sQuad.low;' % (a.group(1), a.group(2), a.group(2)))
            out.append('%sconst uint64_t %s = %sQuad.high;' % (a.group(1), b.group(2), a.group(2)))
            i += 2
            continue
        out.append(lines[i])
        i += 1
    return out


def machine_vu0(lines):
    """The draft's VU0 statements (LQC2, the instruction, SQC2) folded into
    ee::vu0::Machine's sequences where they are one: difference, sum, scale,
    cross, dot, squaredLength, normalize, on EE addresses. The Machine runs
    them on the context's registers as the generated code does, so what they
    leave in VU0 (registers, ACC, Q, flags) is the same; a dot product's
    QMFC2 + MTC1 (vf5's x) becomes the call's value. Returns the lines and
    the number of sequences folded."""
    out = []
    n = 0
    names = [0]
    declared = []
    i = 0

    def comment(k):
        return lines[k].strip()[3:] if k < len(lines) and lines[k].strip().startswith('// ') else None

    def instr(k):
        """(the instruction's comment, k past its code) at line k, or None."""
        c = comment(k)
        if c is None or k + 1 >= len(lines):
            return None
        return c, k + 2

    def value_of_vf5(start, name):
        # Uses of vf5's x up to the next line that names vf5 otherwise.
        for k in range(start, len(lines)):
            if _VX5 in lines[k]:
                lines[k] = lines[k].replace(_VX5, name)
            if 'vu0_vf[5]' in lines[k]:
                return
            if lines[k].strip().startswith(('}', 'if', 'else', 'for', 'while', 'case', 'goto')) or lines[k].strip().endswith(':'):
                return

    while i < len(lines):
        m4 = _LQC2.match(lines[i])
        if m4 and m4.group(2) == '4':
            pad, a = m4.group(1), _strip_mask(m4.group(3))
            j = i + 1
            m5 = _LQC2.match(lines[j]) if j < len(lines) else None
            if m5 and m5.group(2) == '5':
                b = _strip_mask(m5.group(3))
                ins = instr(j + 1)
                if ins and ins[0] in ('VSUB.XYZW vf6, vf4, vf5', 'VADD.XYZW vf6, vf4, vf5'):
                    ms = _SQC2.match(lines[ins[1]]) if ins[1] < len(lines) else None
                    if ms and ms.group(3) == '6':
                        op = 'difference' if ins[0].startswith('VSUB') else 'sum'
                        out.append('%svu.%s(%s, %s, %s);' % (pad, op, a, b, _strip_mask(ms.group(2))))
                        i = ins[1] + 1
                        n += 1
                        continue
                if ins and ins[0] == 'VOPMULA.XYZ ACC, vf4, vf5':
                    i2 = instr(ins[1])
                    i3 = instr(i2[1]) if i2 else None
                    if i2 and i3 and i2[0] == 'VOPMSUB.XYZ vf6, vf5, vf4' and i3[0] == 'VSUB.W vf6, vf6, vf6':
                        ms = _SQC2.match(lines[i3[1]]) if i3[1] < len(lines) else None
                        if ms and ms.group(3) == '6':
                            out.append('%svu.cross(%s, %s, %s);' % (pad, a, b, _strip_mask(ms.group(2))))
                            i = i3[1] + 1
                            n += 1
                            continue
                if ins and ins[0] == 'VMUL.XYZ vf5, vf4, vf5':
                    i2 = instr(ins[1])
                    i3 = instr(i2[1]) if i2 else None
                    if i2 and i3 and i2[0] == 'VADDY.X vf5, vf5, vf5y' and i3[0] == 'VADDZ.X vf5, vf5, vf5z':
                        names[0] += 1
                        name = 'dot_%d' % names[0]
                        declared.append(('dot', names[0]))
                        out.append('%s%s = vu.dot(%s, %s);' % (pad, name, a, b))
                        value_of_vf5(i3[1], name)
                        i = i3[1]
                        n += 1
                        continue
            # Statements that do not touch VU0 or memory between the LQC2 and
            # the QMTC2 (a declaration of t0's upper half, a float's load)
            # go before the sequence.
            between = []
            while j < len(lines) and re.match(r'^\s*(const )?(uint64_t|ee::Float|int32_t|uint32_t) \w+ = [^;]*;$', lines[j]) \
                    and 'vu0' not in lines[j] and not re.search(r'\b(ee::call|game::call)', lines[j]):
                between.append(lines[j])
                j += 1
            mq = _QMTC2.match(lines[j]) if j < len(lines) else None
            if not mq:
                j = i + 1
                between = []
            if mq:
                ins = instr(j + 1)
                s_ = _float_of_low(mq.group(3))
                if ins and s_ and ins[0] == 'VMULX.XYZ vf6, vf4, vf5x':
                    ms = _SQC2.match(lines[ins[1]]) if ins[1] < len(lines) else None
                    if ms and ms.group(3) == '6':
                        hi = mq.group(2)
                        for b_ in between:
                            # t0's upper half unknown to the draft (0) is the default.
                            if re.match(r'^\s*uint64_t %s = 0;$' % re.escape(hi), b_) and \
                                    sum(len(re.findall(r'\b%s\b' % re.escape(hi), l)) for l in lines) == 2:
                                hi = '0'
                                continue
                            out.append(b_)
                        extra = '' if hi in ('0', '0u') else ', %s' % hi
                        out.append('%svu.scale(%s, %s, %s%s);' % (pad, a, s_, _strip_mask(ms.group(2)), extra))
                        i = ins[1] + 1
                        n += 1
                        continue
            ins = instr(i + 1)
            if ins and ins[0] == 'VMUL.XYZ vf5, vf4, vf4':
                i2 = instr(ins[1])
                i3 = instr(i2[1]) if i2 else None
                if i2 and i3 and i2[0] == 'VADDY.X vf5, vf5, vf5y' and i3[0] == 'VADDZ.X vf5, vf5, vf5z':
                    seq = []
                    k = i3[1]
                    for want in ('VSQRT Q, vf5x', 'VADDQ.X vf5, vf0, Q', 'VDIV Q, vf0w, vf5x', 'VSUB.XYZW vf6, vf0, vf0'):
                        x = instr(k)
                        if not x or x[0] != want:
                            break
                        seq.append(want)
                        k = x[1]
                    if len(seq) == 4:
                        t0 = None
                        mt = _T0HI.match(lines[k]) if k < len(lines) else None
                        if mt:
                            t0 = lines[k]
                            k += 1
                        x = instr(k)
                        ms = _SQC2.match(lines[x[1]]) if x and x[0] == 'VMULQ.XYZ vf6, vf4, Q' and x[1] < len(lines) else None
                        if ms and ms.group(3) == '6':
                            names[0] += 1
                            name = 'length_%d' % names[0]
                            declared.append(('length', names[0]))
                            out.append('%s%s = vu.normalize(%s, %s);' % (pad, name, a, _strip_mask(ms.group(2))))
                            if t0:
                                out.append(t0.replace(
                                    '_mm_cvtsi128_si64(_mm_unpackhi_epi64(_mm_castps_si128(ctx->vu0_vf[5]), _mm_castps_si128(ctx->vu0_vf[5])))',
                                    'vu.high(5)'))
                            value_of_vf5(x[1] + 1, name)
                            i = x[1] + 1
                            n += 1
                            continue
                    names[0] += 1
                    name = 'squared_%d' % names[0]
                    declared.append(('squared', names[0]))
                    out.append('%s%s = vu.squaredLength(%s);' % (pad, name, a))
                    value_of_vf5(i3[1], name)
                    i = i3[1]
                    n += 1
                    continue
        mt = _T0HI.match(lines[i])
        if mt:
            out.append(lines[i].replace(
                '_mm_cvtsi128_si64(_mm_unpackhi_epi64(_mm_castps_si128(ctx->vu0_vf[5]), _mm_castps_si128(ctx->vu0_vf[5])))',
                'vu.high(5)'))
            i += 1
            continue
        out.append(lines[i])
        i += 1
    # The values are declared at the top (gotos may pass where they are set).
    return out, n, ['%s_%d' % (k, v) for k, v in declared]


def initial_value(w, top=False):
    """A variable's initializer where it is declared ahead of its definitions:
    the caller's register when a path reaches its use with the value the
    function was called with (not a parameter: fn_165230 returns the caller's
    v0 on one path), else 0 (false; a float none)."""
    if getattr(w, 'entry', False) and not getattr(w, 'param', None):
        r = w.reg
        if r in GPR and r != 'zero':
            text = 'ee::callerRegister(%d /* %s */)' % (GPR.index(r), r)
            return ' = ' + (text if w.t == 'u64' else 'static_cast<%s>(%s)' % (CTYPE[w.t], text)) if w.t not in ('f', 'bool') else (' = false' if w.t == 'bool' else '')
        if re.match(r'^f\d+$', r) and w.t == 'f':
            return ' = ee::Float(ee::callerFloat(%s))' % r[1:]
    if w.t == 'bool':
        return ' = false' if top else ''
    return '' if w.t == 'f' else ' = 0'


def render_function(d):
    rend = Renderer(d)
    declare(d, rend)
    body = []
    d.needs_frame = d.lifter.frame_mode
    rend.stmts(d.body, 2, body)
    # Trailing 'return;' of a void function is implicit.
    while body and body[-1].strip() == 'return;' and not d.ret and body[-1].startswith('        return'):
        body.pop()
    body = join_io_quadwords(body)
    lines = []
    gret = d.ghidra_sig[0] if d.ghidra_sig else ''
    ret_t = 'void' if not d.ret else ('ee::Float' if d.ret == 'f0' else 'uint64_t' if getattr(d, 'ret_wide', False) else
                                      ('uint32_t' if '*' in gret or gret.startswith(('uint', 'undefined', 'ulong')) else 'int32_t'))
    d.ret_ctype = ret_t
    params = []
    for r in d.int_params:
        w_t = 'ee::Qword' if r in d.qword_params else ('uint64_t' if r in d.wide_params else 'uint32_t')
        name = d.param_names[r]
        params.append('%s %s' % (w_t, name) if r in d.used_params else '%s /*%s*/' % (w_t, name))
    for r in d.float_params:
        name = d.param_names[r]
        params.append('ee::Float %s' % name if r in d.used_params else 'ee::Float /*%s*/' % name)
    sig = '%s %s(%s)' % (ret_t, d.name, ', '.join(params))
    d.signature = sig
    lines.append('    ' + sig)
    lines.append('    {')
    # The original's frame for its callees' sp and its register saves comes
    # from the frame table (ee/abi.h ee::enterFrame, round 22: every readable
    # function is entered with its original's frame size and prologue saves,
    # from the same generated code), so a draft has a StackFrame only for
    # locals on the stack (a slot whose address it hands on).
    if d.needs_frame or d.lifter.frame_mode:
        lines.append('        ee::StackFrame frame(0x%X); // the original\'s addiu sp, sp, -0x%X' % (d.lifter.frame_size, d.lifter.frame_size))
    folded = 0
    if d.lifter.vu0:
        body, folded, values = machine_vu0(body)
        body = [fold_vu_statement(line) for line in body]
        d.vu0_folded = folded
        if folded:
            lines.append('        ee::vu0::Machine vu; // the engine\'s VU0 sequences, on the context\'s registers')
        for v in values:
            lines.append('        ee::Float %s;' % v)
        if any('ctx->' in l for l in body):
            lines.append('        R5900Context *const ctx = ee::t_ee.frame->ctx; // VU0 as the generated code runs it')
    if d.lifter.uses_runtime:
        lines.append('        uint8_t *const rdram = ee::t_ee.frame->rdram;')
        lines.append('        PS2Runtime *const runtime = ee::t_ee.frame->runtime;')
    for w in d.decl_top:
        lines.append('        %s %s%s;' % (CTYPE[w.t], w.name, initial_value(w, top=True)))
    lines.extend(body)
    lines.append('    }')
    if d.uses_io or d.lifter.uses_io:
        d.includes.add('ee/io.h')
    if d.lifter.uses_mmi or any('ee::mmi::' in l for l in lines):
        d.includes.add('ee/mmi.h')
    return lines


def wait_and_vu0(d):
    ws = d.ws
    waits = d.address in ws.waiting_functions() and not ws.ghidra_names.get(d.address, '') in WAITING_SEEDS
    callees_wait = []
    for b, o in d.lifter.all_ops():
        if o.k == 'call' and o.info.target in ws.waiting_functions():
            callees_wait.append(o.info.target)
    for k in d.lifter.block_keys():
        t = d.lifter.blocks[k].term
        if t[0] == 'tail' and t[1].target in ws.waiting_functions():
            callees_wait.append(t[1].target)
    if d.lifter.syscalls:
        # A kernel call can wait (ee::kernelCall leaves the fiber for it).
        callees_wait.append(None)
    if not callees_wait:
        indirect = [o.pc for _, o in d.lifter.all_ops() if o.k == 'call' and o.info.kind == 'icall']
        if indirect:
            d.todo.append('an indirect call at 0x%x: READABLE_FUNCTION_WAITS if what it calls can wait (messages, '
                          'script commands, file devices: 18 of the 19 functions registered _WAITS that the call graph '
                          'misses wait that way)' % indirect[0])
    vu0 = False
    if d.lifter.vu0:
        vu0 = callers_read_vu0(ws, d.address, d.lifter)
    return bool(callees_wait), vu0


def callers_read_vu0(ws, address, lifter):
    """Whether a caller reads a VU0 register (vf, Q, flags) after calling the
    function before writing it again."""
    written = set()
    for b, o in lifter.all_ops():
        if o.k == 'vld' or o.k == 'vset':
            written.add('vf%d' % o.t)
        if o.k == 'vraw':
            for m in re.finditer(r'ctx->vu0_vf\[(\d+)\]\s*=', o.x.v):
                written.add('vf%d' % int(m.group(1)))
            if 'vu0_q =' in o.x.v:
                written.add('Q')
            if 'fmac_flags' in o.x.v:
                written.add('flags')
    if not written:
        return False
    for caller, pc in ws.jal_sites().get(address, ()):
        if True:
            p = ws.parsed(caller)
            if p is None:
                continue
            a = pc + 8
            seen_w = set()
            for _ in range(64):
                i = p.insns.get(a)
                if i is None:
                    break
                txt = ' '.join(i.code)
                reads = set('vf%d' % int(x) for x in re.findall(r'vu0_vf\[(\d+)\]', txt.split('=', 1)[1] if '=' in txt and txt.startswith('ctx->vu0_vf') else txt))
                if i.op.startswith('sqc2') or i.op.startswith('qmfc2'):
                    reads |= {'vf%d' % int(reg_name(i.args[0 if i.op.startswith('sqc2') else 1])[2:])}
                if 'vu0_q' in txt and not txt.startswith('ctx->vu0_q ='):
                    reads.add('Q')
                if i.op.startswith('cfc2'):
                    reads.add('flags')
                if (reads - seen_w) & written:
                    return True
                for wm in re.finditer(r'ctx->vu0_vf\[(\d+)\]\s*=', txt):
                    seen_w.add('vf%d' % int(wm.group(1)))
                if i.op.startswith('lqc2') or i.op.startswith('qmtc2'):
                    seen_w.add('vf%d' % int(reg_name(i.args[0 if i.op.startswith('lqc2') else 1])[2:]))
                if i.op in ('jal', 'jalr', 'jr', 'j') or i.op.startswith('b'):
                    break
                a += 4
    return False


FLOAT_WRITES_FIRST = ('lwc1', 'mtc1')   # the f register is the destination (lwc1 ft; mtc1 rt, fs)
FLOAT_READS_ALL = ('swc1', 'mfc1', 'cfc1', 'ctc1')


def insn_reg_access(i, reg):
    """('read' | 'write' | None): what instruction i does with reg first
    (reads before its own write), from its operands."""
    args = [reg_name(a) if a.startswith('$') else a for a in i.args]
    mem = [m.group(1) for a in i.args for m in [re.search(r'\((\$\w+)\)', a)] if m]
    if reg in [reg_name(m) for m in mem]:
        return 'read'
    if reg not in args:
        return None
    op = i.op
    if reg.startswith('f'):
        if op == 'lwc1':
            return 'write' if args[0] == reg else None
        if op == 'mtc1':
            return 'write' if args[1] == reg else 'read'
        if op in FLOAT_READS_ALL or op.startswith('c.') or op.startswith('adda.') or op.startswith('madda.') or \
                op.startswith('msuba.') or op.startswith('mula.'):
            return 'read'
        # fd, fs[, ft]: the sources are read first
        return 'read' if reg in args[1:] else 'write'
    # integer: a load's or an arithmetic op's first operand is written; stores and branches read
    if op.startswith('s') and op in ('sb', 'sh', 'sw', 'sd', 'sq', 'swl', 'swr', 'sdl', 'sdr', 'swc1') or \
            op.startswith('b') or op in ('mtc1', 'mthi', 'mtlo', 'jr', 'jalr', 'ctc1', 'qmtc2', 'sqc2'):
        return 'read'
    return 'read' if reg in args[1:] else 'write'


WIDE_LOADS = ('ld', 'ldl', 'ldr', 'lq')


def returns_wide(ws, address, depth=0):
    """Whether the function at address returns a 64-bit value in v0: a
    readable function declared to return uint64_t / int64_t, or generated code
    whose v0 at a return (jr $ra) is set by a 64-bit load, a move of one, or a
    call of such a function (straight-line back from the return, its delay
    slot first)."""
    # (Kept over the batches: the readable entries it read are noted again at
    # every lookup, so the draft cache sees them.)
    cache = ws.__dict__.setdefault('_returns_wide', {})
    if address in cache:
        found, deps = cache[address]
        for a in deps:
            ws.readable.get(a)
        return found
    deps = [address]
    cache[address] = (False, deps)
    r = ws.readable.get(address)
    if r and r.get('signature'):
        found = re.sub(r'\bconst\b', '', r['signature'][0]).strip() in ('uint64_t', 'int64_t', 'std::uint64_t',
                                                                          'std::int64_t')
        cache[address] = (found, deps)
        return found
    p = ws.parsed(address)
    found = False
    if p is not None and depth < 4:
        for a in sorted(p.insns):
            i = p.insns[a]
            if i.op != 'jr' or not i.args or reg_name(i.args[0]) != 'ra':
                continue
            want = 'v0'
            seq = [p.insns.get(a + 4)] + [p.insns.get(a - 4 * k) for k in range(1, 33)]
            for j in seq:
                if j is None:
                    continue   # (a nop the parse leaves out)
                if j is not seq[0] and (j.op in ('jr', 'j', 'b') or j.op.startswith('b')):
                    break
                if j.op == 'jal':
                    if want == 'v0':
                        t = jump_target(j)
                        found = returns_wide(ws, t, depth + 1)
                        deps.extend(cache.get(t, (False, []))[1])
                    break
                args = [reg_name(x) if x.startswith('$') else x for x in j.args]
                if not args or args[0] != want or j.op.startswith('s') and j.op in ('sb', 'sh', 'sw', 'sd', 'sq'):
                    continue
                if j.op in WIDE_LOADS:
                    found = True
                elif j.op in ('daddu', 'or', 'addu') and len(args) == 3 and 'zero' in args[1:] and \
                        j.op != 'addu':
                    want = args[1] if args[2] == 'zero' else args[2]
                    continue
                break
            if found:
                break
    cache[address] = (found, deps)
    return found


def callers_read_after_call(ws, address, reg, depth=0):
    """Whether a caller of `address` (by jal) reads `reg` after the call
    before writing it again: the straight-line code after the call, up to
    its next branch, jump or call (the branch's delay slot included). A
    caller that returns (jr ra) with reg untouched hands it on: then its own
    callers' reads count (FUN_0013B138 calls 0x139D98 and returns; its caller
    FUN_00165430 reads the f0 0x139D98 left, through the wrapper)."""
    cache = ws.__dict__.setdefault('_reads_after_call', {})
    key = (address, reg)
    if key in cache:
        return cache[key]
    cache[key] = False   # (a cycle of wrappers: not read)
    found = False
    for caller, pc in ws.jal_sites().get(address, ()):
        p = ws.parsed(caller)
        if p is None:
            continue
        a = pc + 8
        for _ in range(64):
            i = p.insns.get(a)
            if i is None:
                break
            acc = insn_reg_access(i, reg)
            if acc == 'read':
                found = True
                break
            if acc == 'write':
                break
            if i.op in ('jal', 'jalr', 'jr', 'j') or i.op.startswith('b'):
                # its delay slot runs first
                j = p.insns.get(a + 4)
                acc_j = insn_reg_access(j, reg) if j is not None else None
                if acc_j == 'read':
                    found = True
                elif i.op == 'jr' and i.args and reg_name(i.args[0]) == 'ra' and acc_j is None and depth < 4 and \
                        p.start is not None and callers_read_after_call(ws, p.start, reg, depth + 1):
                    found = True   # handed on to the caller's callers
                break
            a += 4
        if found:
            break
    cache[key] = found
    return found


# ---- v0 no caller reads: a draft returns void ---------------------------------
#
# A draft returns the v0 Ghidra (or the code) gives it; when no caller can read
# it, the draft is void (drafter16 counted 558). "No caller" must hold for every
# way the function is reached: jal sites (their reads followed over branches, not
# only the straight-line scan of the ResultReader table), tail jumps, and calls
# through a pointer (jalr): its address built in code (lui with addiu / ori), a
# word of the ELF's data (the zgui and script tables; pinned in the manifests'
# `elf` column with the `refs` flag, so the build sees them without the ELF), or
# named in the hand-written sources (the host's and readable code's calls by
# address). Any doubt keeps v0.

BRANCH_LIKELY = ('beql', 'bnel', 'blezl', 'bgtzl', 'bltzl', 'bgezl', 'bltzall', 'bgezall', 'bc1tl', 'bc1fl')


def _branch_target(i):
    if i.op == 'j' or i.op == 'jal':
        return ((i.addr + 4) & 0xF0000000) | ((i.raw & 0x03FFFFFF) << 2)
    off = i.raw & 0xFFFF
    if off & 0x8000:
        off -= 0x10000
    return i.addr + 4 + (off << 2)


def _is_branch(i):
    return i.op.startswith('b') and i.op not in ('break',)


def sets_v0(ws, address, depth=0):
    """Whether the function at address sets v0 on every path to its returns
    (its own write, or a call of a function that does, after the last way in):
    a call of it ends a scan for a stale v0. False when unsure."""
    cache = ws.__dict__.setdefault('_sets_v0', {})
    if address in cache:
        return cache[address]
    cache[address] = False
    p = ws.parsed(address)
    if p is None or p.start is None or depth > 3:
        return False
    targets = set()
    for i in p.insns.values():
        if _is_branch(i) or i.op == 'j':
            t = _branch_target(i)
            if i.op == 'j' and not (p.start <= t < p.end):
                return False   # a tail jump: the tail callee's v0
            targets.add(t)
    rets = [a for a, i in p.insns.items() if i.op == 'jr' and i.args and reg_name(i.args[0]) == 'ra']
    if not rets:
        return False
    ok = True
    for r in rets:
        d = p.insns.get(r + 4)
        if d is not None and insn_reg_access(d, 'v0') == 'write':
            continue
        found = False
        x = r - 4
        while x >= p.start and (x + 4) not in targets:
            i = p.insns.get(x)
            if i is None:
                break
            if i.op == 'jal':
                found = sets_v0(ws, _branch_target(i), depth + 1)
                break
            if _is_branch(i) or i.op in ('j', 'jr', 'jalr'):
                break
            acc = insn_reg_access(i, 'v0')
            if acc == 'write':
                found = True
                break
            if acc == 'read':
                break
            x -= 4
        if not found:
            ok = False
            break
    cache[address] = ok
    return ok


def containing_function(ws, pc):
    """The start of the generated function (not an entry_ file) whose range holds pc."""
    if getattr(ws, '_ranges', None) is None:
        ranges = []
        for a in ws.files:
            name = os.path.basename(ws.files[a])
            if name.startswith('entry_'):
                continue
            pp = ws.parsed(a)
            if pp is not None and pp.start is not None:
                ranges.append((pp.start, pp.end))
        ws._ranges = sorted(ranges)
    import bisect
    k = bisect.bisect_right(ws._ranges, (pc, 0xFFFFFFFF)) - 1
    if k >= 0 and ws._ranges[k][0] <= pc < ws._ranges[k][1]:
        return ws._ranges[k][0]
    return None


def tail_sites(ws):
    """{target: [(caller, pc)]} of the jumps (j) out of a function's own range."""
    if getattr(ws, '_tail_sites', None) is None:
        sites = defaultdict(list)
        jump = re.compile(r'// 0x([0-9a-f]+): 0x([0-9a-f]+)\s+j\s')
        rng = re.compile(r'^// Address: 0x([0-9a-f]+) - 0x([0-9a-f]+)', re.M)
        for caller, path in ws.files.items():
            try:
                text = open(path, errors='replace').read()
            except OSError:
                continue
            m = rng.search(text)
            if not m:
                continue
            lo, hi = int(m.group(1), 16), int(m.group(2), 16)
            for mm in jump.finditer(text):
                pc = int(mm.group(1), 16)
                t = (pc & 0xF0000000) | ((int(mm.group(2), 16) & 0x03FFFFFF) << 2)
                if not (lo <= t < hi):
                    sites[t].append((caller, pc))
        ws._tail_sites = sites
    return ws._tail_sites


def code_constants(ws):
    """Every address generated code can build (lui hi with addiu / daddiu / ori
    lo on its register anywhere in the same file, or lui alone): a function
    whose start is one may be called through a pointer."""
    if getattr(ws, '_code_constants', None) is None:
        out = set()
        lui = re.compile(r'// 0x[0-9a-f]+: 0x[0-9a-f]+\s+lui\s+\$(\w+), (0x[0-9A-Fa-f]+|\d+)')
        low = re.compile(r'// 0x[0-9a-f]+: 0x[0-9a-f]+\s+(addiu|daddiu|ori)\s+\$\w+, \$(\w+), (-?0x[0-9A-Fa-f]+|-?\d+)')
        for path in set(ws.files.values()):
            try:
                text = open(path, errors='replace').read()
            except OSError:
                continue
            his = defaultdict(set)
            for m in lui.finditer(text):
                h = int(m.group(2), 0) & 0xFFFF
                his[m.group(1)].add(h)
                out.add(h << 16)
            if not his:
                continue
            for m in low.finditer(text):
                base = m.group(2)
                if base not in his:
                    continue
                lo = int(m.group(3), 0)
                if m.group(1) == 'ori':
                    lo &= 0xFFFF
                    for h in his[base]:
                        out.add((h << 16) | lo)
                else:
                    lo = ((lo & 0xFFFF) ^ 0x8000) - 0x8000
                    for h in his[base]:
                        out.add(((h << 16) + lo) & 0xFFFFFFFF)
        ws._code_constants = out
    return ws._code_constants


def source_mentions(ws, address):
    """Whether a hand-written source (runtime/src, not the sweep's
    manifests or generated batch files) names the address as a literal: the
    host's and readable code's calls by address."""
    if getattr(ws, '_source_literals', None) is None:
        lits = set()
        root = os.path.join(ZAXIS_TREE, 'runtime', 'src')
        hexlit = re.compile(r'\b0x0*([0-9A-Fa-f]{5,6})u?\b')
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d != 'sweep']
            for fn in filenames:
                if not fn.endswith(('.cpp', '.h', '.inc', '.hpp')):
                    continue
                try:
                    text = readable_symbols.literal_view(open(os.path.join(dirpath, fn), errors='replace').read())
                except OSError:
                    continue
                for m in hexlit.finditer(text):
                    lits.add(int(m.group(1), 16))
        ws._source_literals = lits
    # (`srcref` in a manifest row: a source's comment named it by address, as drafts took it)
    found = address in ws._source_literals or address in getattr(ws, 'pinned_srcref', ())
    note_fact('srcref', address, found)
    return found


def data_refs(ws, address):
    """The ELF data words holding the address (a function pointer in a table),
    as a set of their addresses; None when not known (a build whose manifest
    row has no `refs` flag)."""
    pinned = getattr(ws, 'pinned_refs', None)
    if pinned is not None:
        r = pinned.get(address)
        note_fact('refs', address, sorted(r) if r is not None else None)
        return r
    index = elf_data_index(ws)
    if index is None:
        return None
    return set(index.get(address, ()))


def elf_data_index(ws):
    """{word: [addresses]} of the ELF's words outside the generated functions'
    code (data, read-only data, jump tables), read once; None without the ELF."""
    if getattr(ws, '_elf_index', None) is None:
        ws._elf_word(0)   # loads the segments
        if not ws._segments:
            ws._elf_index = False
            return None
        ranges = []
        for a, path in ws.files.items():
            pp = ws.parsed(a)
            if pp is not None and pp.start is not None:
                ranges.append((pp.start, pp.end))
        ranges.sort()
        import bisect
        index = defaultdict(list)
        for vaddr, body in ws._segments:
            for off in range(0, len(body) - 3, 4):
                w = struct.unpack_from('<I', body, off)[0]
                if not (0x00100000 <= w < 0x02000000) or w & 3:
                    continue
                at = vaddr + off
                k = bisect.bisect_right(ranges, (at, 0xFFFFFFFF)) - 1
                if k >= 0 and ranges[k][0] <= at < ranges[k][1]:
                    continue   # an instruction
                index[w].append(at)
        ws._elf_index = index
    return ws._elf_index or None


def v0_read_after_calls(ws, address, depth=0, steps=4000):
    """Whether some caller may read the v0 the function leaves: from each jal
    site, every path (over branches, both ways; a branch-likely's delay slot on
    the taken one) until v0 is written or a call of a function that always sets
    it; a return with v0 untouched hands it on to the caller's own callers; a
    jump out of the caller, a jump through a register, a path off the parsed
    code or past `steps` instructions counts as read. A tail jump to the
    function hands its callers' reads on too."""
    cache = ws.__dict__.setdefault('_v0_read_after', {})
    if address in cache:
        return cache[address]
    cache[address] = True   # (a cycle of wrappers: read, to be safe)
    if depth > 5:
        return True
    if depth > 0 and reached_unseen(ws, address):
        return True   # a wrapper handing v0 on, itself called through a pointer (or by no caller seen)
    found = False
    for caller, pc in tail_sites(ws).get(address, ()):
        f = containing_function(ws, pc)
        if f is None or v0_read_after_calls(ws, f, depth + 1):
            found = True
            break
    sites = [] if found else ws.jal_sites().get(address, ())
    for caller, pc in sites:
        p = ws.parsed(caller)
        if p is None:
            found = True
            break
        owner = p.start if not os.path.basename(p.path).startswith('entry_') else containing_function(ws, pc)
        todo, seen, count = [pc + 8], set(), 0
        while todo and not found:
            a = todo.pop()
            if a in seen:
                continue
            seen.add(a)
            count += 1
            if count > steps:
                found = True
                break
            i = p.insns.get(a)
            if i is None:
                found = True
                break
            acc = insn_reg_access(i, 'v0')
            if acc == 'read':
                found = True
                break
            if acc == 'write':
                continue
            control = i.op in ('j', 'jal', 'jr', 'jalr') or _is_branch(i)
            if not control:
                if i.op in ('syscall', 'eret', 'break', 'sync.p'):
                    found = True
                    break
                todo.append(a + 4)
                continue
            d = p.insns.get(a + 4)
            dacc = insn_reg_access(d, 'v0') if d is not None else None
            if dacc == 'read':
                found = True
                break
            likely = i.op in BRANCH_LIKELY
            if i.op == 'jal':
                if dacc == 'write' or sets_v0(ws, _branch_target(i)):
                    continue
                todo.append(a + 8)
            elif i.op == 'jalr':
                if dacc != 'write':
                    todo.append(a + 8)
            elif i.op == 'jr':
                if dacc == 'write':
                    continue
                if i.args and reg_name(i.args[0]) == 'ra' and owner is not None:
                    if v0_read_after_calls(ws, owner, depth + 1):
                        found = True
                else:
                    found = True
            elif i.op == 'j':
                t = _branch_target(i)
                if dacc == 'write':
                    continue
                if p.start is not None and p.start <= t < p.end and t in p.insns:
                    todo.append(t)
                else:
                    found = True
            else:
                t = _branch_target(i)
                if likely:
                    if dacc != 'write':
                        todo.append(t)
                    todo.append(a + 8)
                elif dacc != 'write':
                    todo.append(t)
                    todo.append(a + 8)
        if found:
            break
    cache[address] = found
    return found


def reached_unseen(ws, address):
    """Whether the function may be called some way the call sites do not show:
    its address built in code, in a data word (or its data words not known
    here), named in a hand-written source, or no jal / j to it at all."""
    if address in code_constants(ws):
        return True
    refs = data_refs(ws, address)
    if refs is None or refs:
        return True
    if source_mentions(ws, address):
        return True
    return not ws.jal_sites().get(address) and not tail_sites(ws).get(address)


def v0_unread(ws, address):
    """Why the function's v0 may be dropped (None: keep it)."""
    if reached_unseen(ws, address):
        return None
    gprs, _ = ws.result_readers().get(address, (0, 0))
    if gprs >> 2 & 1:
        return None
    if v0_read_after_calls(ws, address):
        return None
    return 'no caller reads v0 (jal sites over branches, tail jumps; its address is in no data word, code or source)'


# What a draft says about itself that is settled (the code does it): `// Note:`. Anything else
# is `// TODO(draft):`, and a TODO, an inline /* TODO ... */ or an unknown expression (`/* ? `)
# keeps a function from Verified (OPEN_ITEM, readable_sweep.py's gate).
DRAFT_NOTES = ('Ghidra declares it ', 'returns void: ', 'its name is a low-confidence guess', 'callers read ')
OPEN_ITEM = re.compile(r'\bTODO\b|/\* \? |// \? ')


# Readable callees called by address all the same (readable_draft_check.py: the
# functions of a batch being drafted again, whose registrations are off), and the
# libc and libgcc ports of functions the runtime replaces (sce/libc_stdio.cpp,
# libc_malloc.cpp, __divdi3): the runtime's handlers are the game's.
RUNTIME_PORTS = frozenset((0x0021FA08, 0x00222D60, 0x00222E60, 0x00223068, 0x002232C0, 0x00223ACC, 0x00223B7C, 0x00223D08, 0x00223D48,
                           0x00223DC8, 0x00224400, 0x002278F8, 0x00228E68, 0x00229200, 0x00229320, 0x00229640, 0x0022A1C0))
BY_ADDRESS = set(RUNTIME_PORTS)


DEFAULT_WORKDB = zaxis_paths.get('WORKDB_PY', required=False)


def fetch_cards(addresses, workdb_py):
    """{address: [comment lines]} from the work database (MATCH3.md section 6: `workdb.py card
    ADDR ... --prefix '// ' --width 100`, one process for the batch); {} when the script or the
    database is missing. An address that is not an AI function has no card (the draft is made
    without it)."""
    if not workdb_py or not os.path.exists(workdb_py) or not addresses:
        return {}
    import subprocess
    cards = {}
    # Batches of 200 (a command line's length; each card is about 25 ms).
    for k in range(0, len(addresses), 200):
        chunk = addresses[k:k + 200]
        try:
            p = subprocess.run([sys.executable, '-I', workdb_py, 'card'] + ['0x%08X' % a for a in chunk] +
                               ['--prefix', '// ', '--width', '100'], capture_output=True, text=True, timeout=600)
        except (OSError, subprocess.TimeoutExpired):
            return cards
        current = None
        for line in p.stdout.split('\n'):
            m = re.match(r'^// workdb card: AI PS2 0x([0-9A-Fa-f]+)', line)
            if m:
                current = int(m.group(1), 16)
                cards[current] = []
            if current is not None and line.strip():
                cards[current].append(line)
    return cards


def draft_text(ws, addresses, args):
    text, drafts, failures = draft_batch(ws, addresses, args)
    if failures:
        a = sorted(failures)[0]
        raise Unsupported(failures[a])
    return text, drafts


def draft_batch(ws, addresses, args, skip_failures=False):
    """The drafts of `addresses` in one file: (text, drafts, failures). With
    skip_failures a function that cannot be drafted is left out and its reason
    kept in failures ({address: text}); each draft's lines in the text are
    d.span (first, last; 1-based), for tools that map compiler errors back."""
    callees = Callees(ws)
    drafts = []
    failures = {}
    fixed = getattr(args, 'fixed', None) or {}
    for a in addresses:
        ws.drafting = a
        io_dynamic = getattr(args, 'io_dynamic', None) or ()
        entry = fixed.get(a)
        try:
            d = Draft(ws, a, callees, name=entry['name'] if entry else (args.name if len(addresses) == 1 else None),
                      status=args.status, ghidra=not args.no_ghidra, io_dynamic=io_dynamic is True or a in io_dynamic)
            IO_DYNAMIC[0] = d.io_dynamic
            try:
                d.build()
                d.lines = render_function(d)
                if readable_names:
                    readable_names.rename_draft(ws, d)
            finally:
                IO_DYNAMIC[0] = False
            if entry:
                # A manifest says how it is registered (the call graph's waits
                # and the callers' VU0 reads were found when it was drafted).
                d.waits = entry['macro'] == 'READABLE_FUNCTION_WAITS'
                d.leaves_vu0 = entry['macro'] == 'READABLE_FUNCTION_VU0'
            else:
                d.waits, d.leaves_vu0 = wait_and_vu0(d)
        except Unsupported as e:
            failures[a] = str(e)
            if not skip_failures:
                return '', drafts, failures
            continue
        except Exception as e:  # a generator fault: kept as the function's failure in a batch
            if not skip_failures:
                raise
            failures[a] = 'generator error: %s: %s' % (type(e).__name__, str(e)[:200])
            continue
        drafts.append(d)
    # Names unique in the file (two functions Ghidra names alike).
    seen = {}
    for d in drafts:
        if d.name in seen:
            old = d.name
            d.name = '%s_%06X' % (d.name, d.address)
            d.lines[0] = re.sub(r'\b%s\(' % re.escape(old), d.name + '(', d.lines[0], count=1)
            d.signature = d.lines[0].strip()
        seen[d.name] = d
    workdb_py = getattr(args, 'workdb', None)
    cards = fetch_cards([d.address for d in drafts], workdb_py) if workdb_py else {}
    out = []
    if getattr(args, 'stable', False):
        out.append('// Drafts by runtime/tools/readable_draft.py, lifted from recomp/generated;')
    else:
        today = datetime.date.today().isoformat()
        out.append('// Draft by runtime/tools/readable_draft.py (%s) of %s, lifted from recomp/generated;' %
                   (today, ', '.join('0x%06X' % a for a in addresses)))
    out.append('// calls, EE cycles, the stack frame and float order are the original\'s. Names, types and')
    out.append('// structure are the author\'s; zx-fncheck proves each step (docs/READABLE.md, "Drafts and')
    out.append('// the sweep").')
    out.append('')
    out.append('#include "ee/abi.h"')
    if any(getattr(d, 'vu0_folded', 0) or any('ee::vu0::' in line for line in d.lines) for d in drafts):
        out.append('#include "ee/vu0.h"')
    out.append('#include "ee/readable.h"')
    # (readable_adopt.py's records: the layout table's structs, which the drafts' offsetof lines name)
    records = {'aggressive/game_records.h', 'aggressive/level/edit_records.h', 'zge/engine_records.h'}
    for h in sorted(set().union(*[d.includes for d in drafts]) | {r for r in records if os.path.exists(os.path.join(GAME_SRC, r))}):
        out.append('#include "%s"' % h)
    out.append('')
    out.append('#include <cstdint>')
    if any(d.lifter.vu0 for d in drafts) or any('Unaligned<' in l for d in drafts for l in d.lines):
        out.append('#include <cstring>')
    out.append('')
    decls = sorted(set().union(*[d.decls for d in drafts]))
    if decls:
        out.append('// Readable functions declared only in their own sources (their definitions\' signatures).')
        out.extend(decls)
        out.append('')
    out.append('namespace %s' % args.namespace)
    out.append('{')
    consts = {}
    globs = {}
    taken = {}   # name -> address, over the file (two drafts' kMemset can be two functions)
    typed_globs = {}   # (address, type) -> entry: an address another draft reads as another type
    for d in drafts:
        renames = {}
        for table, source in ((consts, d.constants), (globs, d.globals)):
            for a, entry in source.items():
                name = entry[0]
                if table is globs and a in globs and globs[a][1] != entry[1]:
                    # The same address read as another type by another draft of
                    # the file (fn_1C1720's lb of 0x279668 against a u8 one's):
                    # a declaration of its own, or the file's one type wins.
                    new = '%s_%s' % (name, entry[1])
                    renames[name] = new
                    entry = (new,) + tuple(entry[1:])
                    typed_globs.setdefault((a, entry[1]), entry)
                    continue
                if taken.get(name, a) != a:
                    new = '%s_%06X' % (name, a)
                    renames[name] = new
                    entry = (new,) + tuple(entry[1:])
                taken.setdefault(entry[0], a)
                table[a] = entry
        for old, new in renames.items():
            # (by text: so the old name must mean one address in this draft)
            means = [a for a, e in list(d.constants.items()) + list(d.globals.items()) if e[0] == old]
            if len(set(means)) > 1:
                die('%s: the name %s means %s in one draft; a rename by text would merge them' % (
                    d.name, old, ', '.join('0x%X' % a for a in sorted(set(means)))))
            d.lines = [re.sub(r'\b%s\b' % re.escape(old), new, line) for line in d.lines]
    # (readable_names agent) the struct fields' names the names module gave raw
    # offsets: constants for the layout table's names of padding, and the
    # static_asserts that check a member's offset its comment gives.
    field_consts = {}
    for d in drafts:
        field_consts.update(getattr(d, 'names_field_constants', None) or {})
    field_asserts = sorted(set().union(*[getattr(d, 'names_asserts', None) or set() for d in drafts]))
    if consts or globs or typed_globs or field_consts or field_asserts or \
            any('Unaligned<' in l for d in drafts for l in d.lines):
        out.append('    namespace')
        out.append('    {')
        for a in sorted(consts):
            name, comment = consts[a]
            out.append('        constexpr uint32_t %s = 0x%08Xu; // %s' % (name, a, comment))
        for name in sorted(field_consts):
            off, comment = field_consts[name]
            out.append('        constexpr uint32_t %s = 0x%Xu; // %s' % (name, off, comment))
        for line in field_asserts:
            out.append('        ' + line)
        for a, name, t, comment in sorted([(a,) + tuple(e) for a, e in globs.items()] +
                                          [(a,) + tuple(e) for (a, _), e in typed_globs.items()]):
            out.append('        inline ee::Global<%s, 0x%08Xu> %s; // %s' % ('ee::Float' if t == 'f' else CTYPE[t], a, name, comment))
        unaligned = any('Unaligned<' in l for d in drafts for l in d.lines)
        if unaligned:
            out.append('        template <typename T> T loadUnaligned(uint32_t address) { T v; std::memcpy(&v, ee::at<uint8_t>(address), sizeof(T)); return v; }')
            out.append('        template <typename T> void storeUnaligned(uint32_t address, T v) { std::memcpy(ee::at<uint8_t>(address), &v, sizeof(T)); }')
        out.append('    }')
        out.append('')
    for d in drafts:
        first = len(out) + 1
        card = cards.get(d.address)
        if card:
            out.extend('    ' + line for line in card)
        if d.ghidra:
            dec = ws.decomp(d.address)
            if dec:
                out.append('    // Ghidra (%s):' % dec[0])
                for line in dec[1].split('\n'):
                    if not line.strip():
                        continue
                    out.append('    //   ' + line.replace('\t', '  ').rstrip())
        if d.todo:
            for t in sorted(set(d.todo)):
                out.append('    // %s: %s' % ('Note' if t.startswith(DRAFT_NOTES) or re.match(r'reads \S+, which the function never set', t) else 'TODO(draft)', t))
        out.extend(d.lines)
        macro = 'READABLE_FUNCTION'
        if d.waits:
            macro = 'READABLE_FUNCTION_WAITS'
        elif d.leaves_vu0:
            macro = 'READABLE_FUNCTION_VU0'
        if d.waits and d.leaves_vu0:
            out.append('    // TODO(draft): its callers read the VU0 registers it leaves, and a callee can wait')
        reg = ws.readable.get(d.address)
        if reg and reg['kind'] == 'PLANNED':
            out.append('    // (registered as READABLE_PLANNED "%s" in %s: remove that line)' % (reg['name'], os.path.relpath(reg['file'], GAME_SRC)))
        status = fixed[d.address]['status'] if d.address in fixed else args.status
        out.append('    %s(0x%08X, %s, %s);' % (macro, d.address, d.name, status))
        d.span = (first, len(out))
        d.macro = macro
        out.append('')
    out.append('}')
    return '\n'.join(out) + '\n', drafts, failures



# ---------------------------------------------------------------------------
# Sweep manifests: the swept functions, generated at build time
# ---------------------------------------------------------------------------
#
# The sweep's batch files are generated code (lifted from recomp/generated),
# so they are not committed: a manifest per layer (src/game/<layer>/sweep/
# manifest.txt) pins each function down (status, file, namespace, how it is
# registered, flags, name, Ghidra's signature, the ELF words its jump tables
# needed, why it is Planned) and game.cmake runs `readable_draft.py
# --manifest` over them into the build dir. The output depends only on the
# manifests, this tool, recomp/generated, recomp/ghidra-functions.csv and the
# readable sources: the same inputs give the same bytes.

MANIFEST_COLUMNS = ('address', 'status', 'file', 'namespace', 'macro', 'flags', 'name', 'signature', 'elf', 'reason')
SWEEP_MARKER = '// readable_sweep.py: generated batch file'


def read_manifest(path):
    """(header lines, [entry dicts]) of a manifest: tab-separated columns, `#` header lines."""
    header, entries = [], []
    for line in open(path, encoding='utf-8'):
        line = line.rstrip('\n')
        if not line:
            continue
        if line.startswith('#'):
            header.append(line)
            continue
        cols = line.split('\t')
        cols += [''] * (len(MANIFEST_COLUMNS) - len(cols))
        e = dict(zip(MANIFEST_COLUMNS, cols))
        e['address'] = int(e['address'], 16)
        e['flags'] = set(f for f in e['flags'].split(',') if f and f != '-')
        e['elf'] = {int(a, 16): int(w, 16) for a, w in (x.split(':') for x in e['elf'].split(',') if ':' in x)}
        for k in ('signature', 'reason', 'macro', 'name'):
            if e[k] == '-':
                e[k] = ''
        entries.append(e)
    return header, entries


def write_manifest(path, header, entries):
    lines = list(header)
    for e in sorted(entries, key=lambda e: e['address']):
        cols = ['0x%08X' % e['address'], e['status'], e['file'], e['namespace'], e.get('macro') or '-',
                ','.join(sorted(e.get('flags') or ())) or '-', e.get('name') or '-',
                (e.get('signature') or '-').replace('\t', ' ').replace('\n', ' '),
                ','.join('%X:%X' % kv for kv in sorted((e.get('elf') or {}).items())) or '-',
                (e.get('reason') or '-').replace('\t', ' ').replace('\n', ' ')[:200]]
        lines.append('\t'.join(cols))
    text = '\n'.join(lines) + '\n'
    if not os.path.exists(path) or open(path, encoding='utf-8').read() != text:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        open(path, 'w', encoding='utf-8', newline='\n').write(text)


def pin_data_refs(ws, entries):
    """Pins each entry's data references (the ELF's words holding its address:
    data_refs) into its `elf` column with the `refs` flag, so a build without
    the ELF knows which functions are called through tables. Needs the ELF;
    returns the number of entries with a reference."""
    n = 0
    for e in entries:
        refs = data_refs(ws, e['address'])
        if refs is None:
            continue
        e['flags'] = set(e.get('flags') or ()) | {'refs'}
        e['elf'] = dict(e.get('elf') or {})
        for a in refs:
            e['elf'][a] = e['address']
        n += bool(refs)
    return n


def batch_text(ws, entries, version, comments=False, workdb=None):
    """The batch file of one manifest file's entries: (text, spans {address:
    (first, last)}, failures {address: reason})."""
    ns = entries[0]['namespace']
    drafted = [e for e in entries if e['status'] in ('Draft', 'Tested', 'Verified')]
    args = argparse.Namespace(name=None, status='Draft', no_ghidra=not comments, namespace=ns,
                              workdb=workdb if comments else None, stable=True,
                              io_dynamic={e['address'] for e in drafted if 'io' in e['flags']},
                              fixed={e['address']: e for e in drafted})
    text, drafts, failures = draft_batch(ws, [e['address'] for e in drafted], args, skip_failures=True)
    head = [SWEEP_MARKER + ' (%s), from the manifest; not committed.' % version,
            '// readable_sweep.py keeps the manifest; a function made readable by hand moves to a file of its own.']
    lines = text.rstrip('\n').split('\n') if drafts else ['#include "ee/readable.h"', '', 'namespace %s' % ns, '{', '}']
    planned = []
    for e in entries:
        why = None
        if e['status'] == 'Planned':
            why = e['reason'] or 'not drafted'
        elif e['address'] in failures:
            why = 'not drafted here: ' + failures[e['address']]
        if why is not None:
            planned.append('    // %s: %s' % (e['name'] or 'FUN_%08X' % e['address'], why[:160]))
            planned.append('    READABLE_PLANNED(0x%08X, "%s");' % (e['address'], e['name'] or 'FUN_%08X' % e['address']))
    if planned:
        lines = lines[:-1] + ['    // Not drafted (readable_sweep.py): registered for recording only.'] + planned + ['}']
    spans = {d.address: (d.span[0] + len(head), d.span[1] + len(head)) for d in drafts}
    return '\n'.join(head + lines) + '\n', spans, failures


class DraftCache:
    """The batch files' inputs as they were drafted (<out-dir>/.draft-cache.json):
    per file its manifest rows, the facts it read (FACTS) and a digest of its
    text; for all, a key of this tool, readable_coverage.py, recomp/generated
    (names, sizes and times of its files), the Ghidra map and the manifests'
    generator line. A file whose rows and facts are unchanged, under the same
    key, is the one on disk (its text digest checked)."""

    NAME = '.draft-cache.json'

    def __init__(self, ws, out_dir, version):
        import json
        self.ws = ws
        self.path = os.path.join(out_dir, self.NAME)
        self.key = self.global_key(ws, version)
        self.files = {}
        try:
            data = json.load(open(self.path, encoding='utf-8'))
            if data.get('key') == self.key:
                self.files = data.get('files', {})
        except (OSError, ValueError):
            pass

    @staticmethod
    def global_key(ws, version):
        import hashlib
        h = hashlib.sha1()
        for tool in (os.path.abspath(__file__), os.path.join(HERE, 'readable_coverage.py'),
                     os.path.join(HERE, 'fncheck_traced_gen.py')):
            try:
                h.update(open(tool, 'rb').read())
            except OSError:
                h.update(b'-')
        h.update(repr((version, sys.version_info[:2], GAME_SRC, ws.generated_dir)).encode())
        if readable_names:
            h.update(readable_names.digest().encode())   # its table and the manifests' names
        names = []
        with os.scandir(ws.generated_dir) as it:
            for e in it:
                st = e.stat()
                names.append((e.name, st.st_size, st.st_mtime_ns))
        h.update(repr(sorted(names)).encode())
        csv_path = os.environ.get('READABLE_GHIDRA_CSV') or os.path.join(ws.workspace, 'recomp', 'ghidra-functions.csv')
        try:
            h.update(open(csv_path, 'rb').read())
        except OSError:
            h.update(b'-')
        return h.hexdigest()

    @staticmethod
    def rows_digest(entries):
        return fact_digest([sorted((k, sorted(v) if isinstance(v, set) else sorted(v.items()) if isinstance(v, dict)
                                    else v) for k, v in e.items()) for e in entries])

    def current(self, kind, key):
        """A fact's value now (as FACTS noted it)."""
        ws = self.ws
        if kind in ('readable', 'constants', 'globals'):
            d = getattr(ws, kind)
            if key == '*':
                return fact_digest(sorted(dict.items(d), key=lambda kv: repr(kv[0])))
            return fact_digest(dict.get(d, key))
        if kind == 'hooked':
            return fact_digest(set.__contains__(ws.hooked, key))
        if kind == 'folded':
            return fact_digest(ws._folded(key))
        if kind == 'decomp':
            return fact_digest((ws._decomp or {}).get(key))
        if kind == 'elf':
            return fact_digest((getattr(ws, 'pinned_elf', None) or {}).get(key))
        if kind == 'refs':
            r = (getattr(ws, 'pinned_refs', None) or {}).get(key)
            return fact_digest(sorted(r) if r is not None else None)
        if kind == 'srcref':
            return fact_digest(source_mentions(ws, key))
        return None

    def lookup(self, path, entries, full):
        import hashlib
        c = self.files.get(path)
        if not c or c.get('rows') != self.rows_digest(entries):
            return None
        for kind, key, digest in c['facts']:
            if self.current(kind, key) != digest:
                return None
        try:
            text = open(full, encoding='utf-8', newline='').read()
        except OSError:
            return None
        if hashlib.sha1(text.encode('utf-8')).hexdigest() != c.get('text'):
            return None
        spans = {int(a, 16): tuple(v) for a, v in c['spans'].items()}
        failures = {int(a, 16): why for a, why in c['failures'].items()}
        return text, spans, failures

    def store(self, path, entries, facts, text, spans, failures):
        import hashlib
        self.files[path] = {
            'rows': self.rows_digest(entries),
            'facts': sorted(([kind, key, digest] for (kind, key), digest in facts.items()),
                            key=lambda f: (f[0], repr(f[1]))),
            'text': hashlib.sha1(text.encode('utf-8')).hexdigest(),
            'spans': {'0x%08X' % a: list(v) for a, v in spans.items()},
            'failures': {'0x%08X' % a: why for a, why in failures.items()},
        }

    def save(self, paths):
        import json
        data = {'key': self.key, 'files': {p: c for p, c in self.files.items() if p in paths}}
        text = json.dumps(data, sort_keys=True)
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        tmp = self.path + '.tmp'
        open(tmp, 'w', encoding='utf-8').write(text)
        os.replace(tmp, self.path)


def generate_from_manifests(ws, manifests, out_dir, spans_path=None, comments=False, workdb=None, verbose=False):
    """Writes every manifest's batch files under out_dir (a file only when its
    text changed); the spans as JSON. Returns the number of files written."""
    import json
    groups = {}
    elf = {}
    refs = {}
    signatures = {}
    version = ''
    for m in manifests:
        header, entries = read_manifest(m)
        for h in header:
            mm = re.match(r'^# generator: (.*)$', h)
            if mm:
                version = mm.group(1)
        for e in entries:
            groups.setdefault(e['file'], []).append(e)
            elf.update(e['elf'])
            if 'refs' in e['flags']:
                refs[e['address']] = {a for a, w in e['elf'].items() if w == e['address']}
            if 'srcref' in e['flags']:
                ws.pinned_srcref = getattr(ws, 'pinned_srcref', set()) | {e['address']}
            if e['signature']:
                signatures[e['address']] = (e['name'], e['signature'] + '\n{\n}')
    if not comments:
        ws.pinned_elf = elf
        ws.pinned_refs = refs
        ws._decomp = signatures
    # The cache (DraftCache): a file is drafted again only when its manifest
    # rows, one of the facts it read, or a global input changed.
    cache = DraftCache(ws, out_dir, version) if not comments else None
    written = 0
    drafted = 0
    all_spans = {}
    csv_path = os.environ.get('READABLE_GHIDRA_CSV') or os.path.join(ws.workspace, 'recomp', 'ghidra-functions.csv')
    symbols = readable_symbols.DraftSymbols(ws.parsed, csv_path, out_dir)
    for path in sorted(groups):
        entries = sorted(groups[path], key=lambda e: e['address'])
        full = os.path.join(out_dir, path)
        hit = cache.lookup(path, entries, full) if cache else None
        if hit is not None:
            text, spans, failures = hit
        else:
            FACTS[0] = {} if cache else None
            try:
                text, spans, failures = batch_text(ws, entries, version, comments=comments, workdb=workdb)
            finally:
                facts, FACTS[0] = FACTS[0], None
            drafted += 1
            if cache:
                cache.store(path, entries, facts, text, spans, failures)
        for a, why in failures.items():
            sys.stderr.write('readable_draft.py: 0x%08X (%s) not drafted: %s\n' % (a, path, why))
        # The batch file with symbols, not literals (docs/READABLE.md "Symbols").
        text, spans = symbols.convert(text, path, spans)
        if not os.path.exists(full) or open(full, encoding='utf-8', newline='').read() != text:
            os.makedirs(os.path.dirname(full), exist_ok=True)
            open(full, 'w', encoding='utf-8', newline='\n').write(text)
            written += 1
        all_spans[path] = {'0x%08X' % a: list(s) for a, s in spans.items()}
        if verbose:
            sys.stderr.write('%s: %d functions\n' % (path, len(entries)))
    # (a batch file the manifests no longer have must not be read as a draft)
    for root, _, names in os.walk(out_dir):
        for name in names:
            rel = os.path.relpath(os.path.join(root, name), out_dir).replace(os.sep, '/')
            if name.endswith('.cpp') and rel not in groups:
                os.remove(os.path.join(root, name))
    extra = symbols.save()
    if extra:
        sys.stderr.write('readable_draft.py: %d rows not in tools/readable_symbols.tsv yet (readable_symbols.py generate)\n' % extra)
    if cache:
        cache.save(set(groups))
        sys.stderr.write('readable_draft.py: %d of %d batch files drafted again (the others\' inputs unchanged)\n' %
                         (drafted, len(groups)))
    if spans_path:
        text = json.dumps(all_spans, indent=0, sort_keys=True)
        if not os.path.exists(spans_path) or open(spans_path).read() != text:
            open(spans_path, 'w', newline='\n').write(text)
    return written


def resolve_address(ws, text):
    t = text.strip()
    if re.match(r'^(0x)?[0-9a-fA-F]+$', t):
        return int(t, 16)
    m = re.match(r'^(?:FUN_|fn_)([0-9a-fA-F]+)$', t)
    if m:
        return int(m.group(1), 16)
    for a, r in ws.readable.items():
        if r['name'] == t:
            return a
    for a, n in ws.ghidra_names.items():
        if n == t:
            return a
    die('unknown function ' + text)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('functions', nargs='*')
    ap.add_argument('--workspace', default=zaxis_paths.get('AI_WORKSPACE', required=False))
    ap.add_argument('--generated')
    ap.add_argument('--decomp', default=os.path.join(zaxis_paths.get('DM2_WORKSPACE', required=False) or '', 'ai-decomp.c'))
    ap.add_argument('--namespace', default='draft')
    ap.add_argument('--name')
    ap.add_argument('--status', default='Draft')
    ap.add_argument('--no-ghidra', action='store_true')
    ap.add_argument('--workdb', default=DEFAULT_WORKDB, help='the work database\'s tools/workdb.py: its card for each '
                    'function goes in the draft\'s header (MATCH3.md section 6; default %(default)s, or WORKDB_PY)')
    ap.add_argument('--no-workdb', action='store_true', help='no card')
    ap.add_argument('--io-dynamic', action='store_true', help='memory reached through pointers may be I/O registers: '
                    'each such access decided at run time (ee::io::loadAny / storeAny)')
    ap.add_argument('-o', '--output')
    ap.add_argument('--manifest', action='append', default=[], help='generate the batch files of a sweep manifest '
                    '(repeatable) under --out-dir (game.cmake does at build time)')
    ap.add_argument('--out-dir', help='--manifest: where the batch files go')
    ap.add_argument('--spans', help='--manifest: a JSON file of each draft\'s lines (readable_sweep.py)')
    ap.add_argument('--ghidra-csv', help='recomp/ghidra-functions.csv (default under --workspace)')
    ap.add_argument('--with-comments', action='store_true', help='--manifest: Ghidra\'s decompilation and the '
                    'work database\'s card in each draft (copies to read, never into the source tree)')
    ap.add_argument('--stats', action='store_true')
    ap.add_argument('--saves-table', help='also write, per call the drafts publish callee-saved registers before, '
                    '`FUNCTION SITE CALLEE LINE` (the jal\'s address, the ee::calleeSaves line) to this file')
    ap.add_argument('--pin-refs', action='store_true', help='--manifest: pin each function\'s data references '
                    '(the ELF\'s words holding its address) into the manifests (`elf` column, `refs` flag)')
    args = ap.parse_args()
    main_ws = zaxis_paths.get('AI_WORKSPACE')
    if not os.path.isdir(os.path.join(args.workspace or '', 'recomp')):
        # A worktree's own tooling, without the data links: the main checkout's workspace.
        args.workspace = main_ws
    generated = args.generated or os.environ.get('ZX_RUNNER_GENERATED_DIR') or os.path.join(args.workspace, 'recomp', 'generated')
    if not os.path.isdir(generated):
        die('no recompiled sources at %s (--generated)' % generated)
    if args.ghidra_csv:
        os.environ['READABLE_GHIDRA_CSV'] = args.ghidra_csv
    if args.manifest and args.pin_refs:
        ws = Workspace(args.workspace, generated, None)
        if elf_data_index(ws) is None:
            sys.exit('readable_draft.py: --pin-refs needs the ELF (recomp/disc-root under --workspace)')
        for m in args.manifest:
            header, entries = read_manifest(m)
            n = pin_data_refs(ws, entries)
            write_manifest(m, header, entries)
            sys.stderr.write('%s: %d of %d functions referenced from data\n' % (m, n, len(entries)))
        return
    if args.manifest:
        ws = Workspace(args.workspace, generated, args.decomp if args.with_comments else None)
        n = generate_from_manifests(ws, args.manifest, args.out_dir, args.spans, comments=args.with_comments,
                                    workdb=None if args.no_workdb else args.workdb)
        sys.stderr.write('readable_draft.py: %d batch files written under %s\n' % (n, args.out_dir))
        return
    if not args.functions:
        die('no function given')
    ws = Workspace(args.workspace, generated, args.decomp)
    if args.no_workdb:
        args.workdb = None
    args.io_dynamic = True if args.io_dynamic else ()
    addresses = [resolve_address(ws, f) for f in args.functions]
    text, drafts = draft_text(ws, addresses, args)
    if args.saves_table:
        with open(args.saves_table, 'w') as f:
            for site in sorted(SAVES_AT):
                callee, line = SAVES_AT[site]
                owner = next((a for a in sorted((d.address for d in drafts), reverse=True) if a <= site), 0)
                f.write('0x%06x 0x%06x %s %s\n' % (owner, site, '0x%06x' % callee if callee is not None else '-', line))
    if args.output:
        with open(args.output, 'w') as f:
            f.write(text)
    else:
        sys.stdout.write(text)
    if args.stats:
        for d in drafts:
            gotos = len(d.goto_targets)
            sys.stderr.write('0x%06X %s: %d instructions, %d calls, frame %s, %d gotos, %d not lifted, %d TODO%s%s\n' % (
                d.address, d.name, len(d.lifter.insns), len(d.lifter.calls),
                ('0x%X (StackFrame)' % d.lifter.frame_size) if d.lifter.frame_mode else ('0x%X' % d.lifter.frame_size),
                gotos, len(d.lifter.unsupported), len(set(d.todo)), ', waits' if d.waits else '', ', VU0' if d.leaves_vu0 else ''))


if __name__ == '__main__':
    main()
